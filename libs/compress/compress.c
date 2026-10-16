/*
 * libs/compress/compress.c — gzip / zlib / raw DEFLATE for Sea (on zlib).
 *
 * Build:  make -C libs/compress        (needs zlib's headers: zlib1g-dev)
 * Usage:  local compress = require("../libs/compress/compress")
 *
 *   compress.gzip(data [, opts]) → bytes          compress.gunzip(data [, opts]) → string | error
 *   compress.deflate(data [, opts]) → bytes       compress.inflate(data [, opts]) → string | error
 *   compress.gzip_file(src, dst [, opts]) → bytes written | error
 *   compress.gunzip_file(src, dst [, opts]) → bytes written | error
 *
 * opts: level (0-9, default 6), format ("zlib", "raw", "gzip"; inflate also
 * "auto": gzip or zlib by the header), max (largest output in bytes, default
 * 1 GiB: a guard against decompression bombs).
 */

#include "sea.h"
#include "sea_rope.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define DEFAULT_MAX (INT64_C(1) << 30)

/* ---- Helpers ---------------------------------------------------------------- */

static bool opt_get(SeaValue opts, const char *field, SeaValue *out) {
    *out = SEA_NIL_VAL;
    if (sea_is_table(opts)) return sea_table_get_str(SEA_AS_TABLE(opts), field, out) && !sea_is_nil(*out);
    if (sea_is_struct(opts)) {
        SeaString *k = sea_string_intern(field, strlen(field));
        *out = sea_struct_get(SEA_AS_STRUCT(opts), k);
        sea_obj_release(&k->obj);
        return !sea_is_nil(*out);
    }
    if (sea_is_map(opts)) {
        SeaString *k = sea_string_intern(field, strlen(field));
        bool found = sea_map_get(SEA_AS_MAP(opts), sea_obj_val(&k->obj), out);
        sea_obj_release(&k->obj);
        return found && !sea_is_nil(*out);
    }
    return false;
}

static int64_t opt_int(SeaValue opts, const char *field, int64_t def) {
    SeaValue v;
    if (sea_is_int(opts) && strcmp(field, "level") == 0) return sea_as_int(opts);   /* gzip(data, 9) */
    if (!opt_get(opts, field, &v)) return def;
    if (sea_is_int(v)) return sea_as_int(v);
    if (sea_is_number(v)) return (int64_t)sea_as_number(v);
    return def;
}

enum { F_ZLIB, F_RAW, F_GZIP, F_AUTO };

/* The window bits zlib wants for a format. */
static int window_bits(int format) {
    switch (format) {
        case F_RAW:  return -15;
        case F_GZIP: return 15 + 16;
        case F_AUTO: return 15 + 32;
        default:     return 15;
    }
}

/* opts.format, or def; -1 if not a known name. */
static int opt_format(SeaValue opts, int def, bool allow_auto) {
    SeaValue v;
    if (!opt_get(opts, "format", &v)) return def;
    SeaString *s = sea_value_get_flat_string(v);
    if (!s) return -1;
    int f = -1;
    if (!strcmp(s->data, "zlib")) f = F_ZLIB;
    else if (!strcmp(s->data, "raw")) f = F_RAW;
    else if (!strcmp(s->data, "gzip")) f = F_GZIP;
    else if (allow_auto && !strcmp(s->data, "auto")) f = F_AUTO;
    sea_obj_release(&s->obj);
    return f;
}

static SeaValue err(SeaVM *vm, const char *fn, const char *msg) {
    char buf[300];
    snprintf(buf, sizeof buf, "compress.%s: %s", fn, msg);
    return sea_make_error_str(vm, buf);
}

typedef struct { unsigned char *d; size_t len, cap; } Buf;

static bool buf_room(Buf *b, size_t n) {
    if (b->len + n <= b->cap) return true;
    size_t nc = b->cap ? b->cap : 4096;
    while (nc < b->len + n) nc *= 2;
    unsigned char *p = realloc(b->d, nc);
    if (!p) return false;
    b->d = p;
    b->cap = nc;
    return true;
}

/* ---- in memory --------------------------------------------------------------- */

static SeaValue do_compress(SeaVM *vm, const char *fn, int argc, SeaValue *args, int def_format) {
    SeaString *s = argc > 0 ? sea_value_get_flat_string(args[0]) : NULL;
    if (!s) return err(vm, fn, "data (string) expected");
    SeaValue opts = argc > 1 ? args[1] : SEA_NIL_VAL;
    int level = (int)opt_int(opts, "level", 6);
    int format = def_format == F_GZIP ? F_GZIP : opt_format(opts, def_format, false);
    if (level < 0 || level > 9 || format < 0) {
        sea_obj_release(&s->obj);
        return err(vm, fn, "level is 0-9 and format \"zlib\", \"raw\" or \"gzip\"");
    }
    z_stream z;
    memset(&z, 0, sizeof z);
    if (deflateInit2(&z, level, Z_DEFLATED, window_bits(format), 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        sea_obj_release(&s->obj);
        return err(vm, fn, "could not start the compressor");
    }
    uLong bound = deflateBound(&z, s->len);
    SeaString *out = sea_string_alloc(bound);
    z.next_in = (Bytef *)s->data;
    z.avail_in = s->len;
    z.next_out = (Bytef *)out->data;
    z.avail_out = (uInt)bound;
    int rc = deflate(&z, Z_FINISH);
    size_t n = z.total_out;
    deflateEnd(&z);
    sea_obj_release(&s->obj);
    if (rc != Z_STREAM_END) {
        sea_obj_release(&out->obj);
        return err(vm, fn, "compression failed");
    }
    /* the exact size: a copy (the bound is usually a little larger) */
    SeaString *r = sea_string_new(out->data, n);
    sea_obj_release(&out->obj);
    return sea_obj_val(&r->obj);
}

static SeaValue do_decompress(SeaVM *vm, const char *fn, int argc, SeaValue *args, int def_format) {
    SeaString *s = argc > 0 ? sea_value_get_flat_string(args[0]) : NULL;
    if (!s) return err(vm, fn, "data (string) expected");
    SeaValue opts = argc > 1 ? args[1] : SEA_NIL_VAL;
    int format = opt_format(opts, def_format, true);
    int64_t max = opt_int(opts, "max", DEFAULT_MAX);
    if (format < 0) { sea_obj_release(&s->obj); return err(vm, fn, "format is \"zlib\", \"raw\", \"gzip\" or \"auto\""); }
    Buf b = { 0 };
    const unsigned char *in = (const unsigned char *)s->data;
    size_t in_len = s->len, pos = 0;
    const char *bad = NULL;
    bool first = true;
    /* gzip allows several members back to back: inflate each */
    while (pos < in_len || first) {
        first = false;
        z_stream z;
        memset(&z, 0, sizeof z);
        if (inflateInit2(&z, window_bits(format)) != Z_OK) { bad = "could not start the decompressor"; break; }
        z.next_in = (Bytef *)(in + pos);
        z.avail_in = (uInt)(in_len - pos);
        int rc;
        do {
            if (!buf_room(&b, 65536)) { bad = "out of memory"; break; }
            z.next_out = b.d + b.len;
            z.avail_out = 65536;
            rc = inflate(&z, Z_NO_FLUSH);
            b.len = (size_t)(z.next_out - b.d);
            if ((int64_t)b.len > max) { bad = "output larger than the limit (opts.max)"; break; }
            if (rc == Z_NEED_DICT || rc == Z_DATA_ERROR) { bad = z.msg ? z.msg : "invalid compressed data"; break; }
            if (rc == Z_MEM_ERROR) { bad = "out of memory"; break; }
            if (rc == Z_BUF_ERROR && z.avail_in == 0) { bad = "truncated compressed data"; break; }
        } while (rc != Z_STREAM_END);
        size_t used = (size_t)(z.next_in - (in + pos));
        inflateEnd(&z);
        if (bad) break;
        pos += used;
        /* more data: another gzip member, or trailing garbage */
        if (pos < in_len) {
            bool gzip_member = (format == F_GZIP || format == F_AUTO) && in_len - pos >= 2 &&
                               in[pos] == 0x1f && in[pos + 1] == 0x8b;
            if (!gzip_member) { bad = "unexpected data after the compressed stream"; break; }
        }
    }
    sea_obj_release(&s->obj);
    if (bad) { free(b.d); return err(vm, fn, bad); }
    SeaValue r = sea_obj_val(&sea_string_new((const char *)(b.d ? b.d : (unsigned char *)""), b.len)->obj);
    free(b.d);
    return r;
}

static SeaValue fn_gzip(SeaVM *vm, int argc, SeaValue *args) { return do_compress(vm, "gzip", argc, args, F_GZIP); }
static SeaValue fn_gunzip(SeaVM *vm, int argc, SeaValue *args) { return do_decompress(vm, "gunzip", argc, args, F_GZIP); }
static SeaValue fn_deflate(SeaVM *vm, int argc, SeaValue *args) { return do_compress(vm, "deflate", argc, args, F_ZLIB); }
static SeaValue fn_inflate(SeaVM *vm, int argc, SeaValue *args) { return do_decompress(vm, "inflate", argc, args, F_AUTO); }

/* ---- files, streamed ----------------------------------------------------------- */

static SeaValue file_err(SeaVM *vm, const char *fn, const char *path, int e) {
    char buf[600];
    snprintf(buf, sizeof buf, "compress.%s: %s: %s", fn, path, strerror(e));
    return sea_make_error_str(vm, buf);
}

/* gzip_file(src, dst [, opts]): compress src into dst in 64 KB steps */
static SeaValue fn_gzip_file(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *src = argc > 0 ? sea_value_get_flat_string(args[0]) : NULL;
    SeaString *dst = argc > 1 ? sea_value_get_flat_string(args[1]) : NULL;
    if (!src || !dst) {
        if (src) sea_obj_release(&src->obj);
        if (dst) sea_obj_release(&dst->obj);
        return err(vm, "gzip_file", "source and destination paths expected");
    }
    SeaValue opts = argc > 2 ? args[2] : SEA_NIL_VAL;
    int level = (int)opt_int(opts, "level", 6);
    SeaValue r = SEA_NIL_VAL;
    FILE *in = fopen(src->data, "rb");
    FILE *out = in ? fopen(dst->data, "wb") : NULL;
    if (!in) r = file_err(vm, "gzip_file", src->data, errno);
    else if (!out) r = file_err(vm, "gzip_file", dst->data, errno);
    else if (level < 0 || level > 9) r = err(vm, "gzip_file", "level is 0-9");
    z_stream z;
    memset(&z, 0, sizeof z);
    if (sea_is_nil(r) && deflateInit2(&z, level, Z_DEFLATED, window_bits(F_GZIP), 8, Z_DEFAULT_STRATEGY) != Z_OK)
        r = err(vm, "gzip_file", "could not start the compressor");
    else if (sea_is_nil(r)) {
        unsigned char ibuf[65536], obuf[65536];
        int flush = Z_NO_FLUSH;
        do {
            size_t n = fread(ibuf, 1, sizeof ibuf, in);
            if (ferror(in)) { r = file_err(vm, "gzip_file", src->data, errno); break; }
            flush = feof(in) ? Z_FINISH : Z_NO_FLUSH;
            z.next_in = ibuf;
            z.avail_in = (uInt)n;
            do {
                z.next_out = obuf;
                z.avail_out = sizeof obuf;
                deflate(&z, flush);
                size_t have = sizeof obuf - z.avail_out;
                if (have && fwrite(obuf, 1, have, out) != have) { r = file_err(vm, "gzip_file", dst->data, errno); break; }
            } while (z.avail_out == 0);
        } while (flush != Z_FINISH && sea_is_nil(r));
        if (sea_is_nil(r)) r = sea_int_val((int64_t)z.total_out);
        deflateEnd(&z);
    }
    if (in) fclose(in);
    if (out && fclose(out) != 0 && !sea_is_error(r)) r = file_err(vm, "gzip_file", dst->data, errno);
    sea_obj_release(&src->obj);
    sea_obj_release(&dst->obj);
    return r;
}

/* gunzip_file(src, dst [, opts]): decompress src into dst in 64 KB steps
 * (several gzip members are joined); opts.max limits the output */
static SeaValue fn_gunzip_file(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *src = argc > 0 ? sea_value_get_flat_string(args[0]) : NULL;
    SeaString *dst = argc > 1 ? sea_value_get_flat_string(args[1]) : NULL;
    if (!src || !dst) {
        if (src) sea_obj_release(&src->obj);
        if (dst) sea_obj_release(&dst->obj);
        return err(vm, "gunzip_file", "source and destination paths expected");
    }
    SeaValue opts = argc > 2 ? args[2] : SEA_NIL_VAL;
    int64_t max = opt_int(opts, "max", INT64_MAX);
    SeaValue r = SEA_NIL_VAL;
    FILE *in = fopen(src->data, "rb");
    FILE *out = in ? fopen(dst->data, "wb") : NULL;
    if (!in) r = file_err(vm, "gunzip_file", src->data, errno);
    else if (!out) r = file_err(vm, "gunzip_file", dst->data, errno);
    z_stream z;
    memset(&z, 0, sizeof z);
    if (sea_is_nil(r) && inflateInit2(&z, window_bits(F_GZIP)) != Z_OK)
        r = err(vm, "gunzip_file", "could not start the decompressor");
    else if (sea_is_nil(r)) {
        unsigned char ibuf[65536], obuf[65536];
        int64_t total = 0;
        int rc = Z_OK;
        bool ended = false;
        for (;;) {
            if (z.avail_in == 0) {
                size_t n = fread(ibuf, 1, sizeof ibuf, in);
                if (ferror(in)) { r = file_err(vm, "gunzip_file", src->data, errno); break; }
                if (n == 0) {
                    if (!ended) r = err(vm, "gunzip_file", "truncated compressed data");
                    break;
                }
                z.next_in = ibuf;
                z.avail_in = (uInt)n;
            }
            if (ended) {
                /* another member? */
                if (z.avail_in >= 1 && z.next_in[0] != 0x1f) { r = err(vm, "gunzip_file", "unexpected data after the compressed stream"); break; }
                inflateReset(&z);
                ended = false;
            }
            z.next_out = obuf;
            z.avail_out = sizeof obuf;
            rc = inflate(&z, Z_NO_FLUSH);
            size_t have = sizeof obuf - z.avail_out;
            total += (int64_t)have;
            if (total > max) { r = err(vm, "gunzip_file", "output larger than the limit (opts.max)"); break; }
            if (have && fwrite(obuf, 1, have, out) != have) { r = file_err(vm, "gunzip_file", dst->data, errno); break; }
            if (rc == Z_STREAM_END) ended = true;
            else if (rc != Z_OK && rc != Z_BUF_ERROR) { r = err(vm, "gunzip_file", z.msg ? z.msg : "invalid compressed data"); break; }
        }
        if (sea_is_nil(r)) r = sea_int_val(total);
        inflateEnd(&z);
    }
    if (in) fclose(in);
    if (out && fclose(out) != 0 && !sea_is_error(r)) r = file_err(vm, "gunzip_file", dst->data, errno);
    sea_obj_release(&src->obj);
    sea_obj_release(&dst->obj);
    return r;
}

static SeaValue fn_version(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm; (void)argc; (void)args;
    const char *v = zlibVersion();
    return sea_obj_val(&sea_string_new(v, strlen(v))->obj);
}

SEA_EXPORT SeaValue sea_open(SeaVM *vm) {
    (void)vm;
    SeaTable *mod = sea_mod_new();
    sea_mod_set(mod, "gzip",        fn_gzip);
    sea_mod_set(mod, "gunzip",      fn_gunzip);
    sea_mod_set(mod, "deflate",     fn_deflate);
    sea_mod_set(mod, "inflate",     fn_inflate);
    sea_mod_set(mod, "gzip_file",   fn_gzip_file);
    sea_mod_set(mod, "gunzip_file", fn_gunzip_file);
    sea_mod_set(mod, "version",     fn_version);
    return sea_obj_val(&mod->obj);
}
