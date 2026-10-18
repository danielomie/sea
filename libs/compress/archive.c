/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * libs/compress/archive.c — ZIP and tar archives (part of compress.so).
 *
 *   compress.zip_write(dst, entries [, opts]) → count | error
 *   compress.zip_list(src) → [{name, type, size, compressed, mode, mtime}] | error
 *   compress.zip_read(src, name) → string | error
 *   compress.zip_extract(src, dir [, opts]) → [names] | error
 *   compress.tar_write / tar_list / tar_read / tar_extract: the same for tar
 *   (ustar with PAX long names; gzip when dst ends in .gz / .tgz or
 *   opts.gzip; read back either way)
 *
 * entries: an array of paths (a directory adds the files under it, in name
 * order) and tables {name = "in/archive", data = "..."} or {name = ...,
 * path = "file"}; a single path is also accepted. opts: base (paths are
 * read relative to it, names are as given), level (0-9), mtime (one time
 * for every entry: reproducible archives), max (largest member read, in
 * bytes, default 1 GiB), names (extract only these).
 *
 * Archives are read whole into memory. Extraction refuses names that are
 * absolute or contain "..", and skips links. No ZIP64, no encrypted ZIPs.
 */

#include "sea.h"
#include "sea_rope.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <zlib.h>

#define AR_DEFAULT_MAX (INT64_C(1) << 30)

/* ---- helpers ---- */

static bool ar_opt(SeaValue opts, const char *field, SeaValue *out) {
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

static int64_t ar_opt_int(SeaValue opts, const char *field, int64_t def) {
    SeaValue v;
    if (!ar_opt(opts, field, &v)) return def;
    if (sea_is_int(v)) return sea_as_int(v);
    if (sea_is_number(v)) return (int64_t)sea_as_number(v);
    return def;
}

/* opts.field as a C string (malloc'd), or NULL */
static char *ar_opt_str(SeaValue opts, const char *field) {
    SeaValue v;
    if (!ar_opt(opts, field, &v)) return NULL;
    SeaString *s = sea_value_get_flat_string(v);
    if (!s) return NULL;
    char *r = strdup(s->data);
    sea_obj_release(&s->obj);
    return r;
}

typedef struct { unsigned char *d; size_t len, cap; } ABuf;

static void ab_put(ABuf *b, const void *p, size_t n) {
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap : 65536;
        while (nc < b->len + n) nc *= 2;
        b->d = realloc(b->d, nc);
        b->cap = nc;
    }
    if (n) memcpy(b->d + b->len, p, n);
    b->len += n;
}
static void ab_zero(ABuf *b, size_t n) {
    static const unsigned char z[512];
    while (n) { size_t k = n < sizeof z ? n : sizeof z; ab_put(b, z, k); n -= k; }
}
static void ab_u16(ABuf *b, unsigned v) { unsigned char x[2] = { (unsigned char)v, (unsigned char)(v >> 8) }; ab_put(b, x, 2); }
static void ab_u32(ABuf *b, uint32_t v) {
    unsigned char x[4] = { (unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
    ab_put(b, x, 4);
}
static unsigned le16(const unsigned char *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static uint32_t le32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static bool read_file(const char *path, ABuf *out, char *err, size_t errlen) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errlen, "%s: %s", path, strerror(errno)); return false; }
    unsigned char tmp[65536];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) ab_put(out, tmp, n);
    bool bad = ferror(f);
    fclose(f);
    if (bad) { snprintf(err, errlen, "%s: read error", path); return false; }
    return true;
}

static bool write_file(const char *path, const void *d, size_t n, unsigned mode, char *err, size_t errlen) {
    FILE *f = fopen(path, "wb");
    if (!f) { snprintf(err, errlen, "%s: %s", path, strerror(errno)); return false; }
    bool ok = fwrite(d, 1, n, f) == n;
    if (fclose(f) != 0) ok = false;
    if (!ok) { snprintf(err, errlen, "%s: write error", path); return false; }
    if (mode) chmod(path, mode & 07777 & ~(unsigned)S_ISUID & ~(unsigned)S_ISGID);
    return true;
}

static char *join(const char *a, const char *b) {
    if (!a || !*a) return strdup(b);
    size_t la = strlen(a), lb = strlen(b);
    char *r = malloc(la + lb + 2);
    memcpy(r, a, la);
    size_t k = la;
    if (a[la - 1] != '/') r[k++] = '/';
    memcpy(r + k, b, lb + 1);
    return r;
}

/* A name inside an archive: '/'-separated, relative, no "." or ".." parts.
 * Returns a malloc'd normal form, or NULL. */
static char *clean_name(const char *s) {
    size_t n = strlen(s);
    char *out = malloc(n + 1);
    size_t k = 0;
    const char *p = s;
    while (*p) {
        while (*p == '/') p++;
        const char *e = p;
        while (*e && *e != '/') e++;
        size_t len = (size_t)(e - p);
        if (len == 0) break;
        if (len == 2 && p[0] == '.' && p[1] == '.') { free(out); return NULL; }
        if (!(len == 1 && p[0] == '.')) {
            if (k) out[k++] = '/';
            memcpy(out + k, p, len);
            k += len;
        }
        p = e;
    }
    out[k] = '\0';
    if (k == 0) { free(out); return NULL; }
    return out;
}

/* Whether a name read from an archive is safe to extract under a directory */
static bool safe_name(const char *s) {
    if (!*s || *s == '/') return false;
    const char *p = s;
    while (*p) {
        const char *e = strchr(p, '/');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == 2 && p[0] == '.' && p[1] == '.') return false;
        if (!e) break;
        p = e + 1;
    }
    return true;
}

static bool mkdir_p(const char *path, char *err, size_t errlen) {
    char *tmp = strdup(path);
    for (char *p = tmp + 1; ; p++) {
        if (*p == '/' || *p == '\0') {
            char c = *p;
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                snprintf(err, errlen, "%s: %s", tmp, strerror(errno));
                free(tmp);
                return false;
            }
            struct stat st;
            if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) {
                snprintf(err, errlen, "%s: not a directory", tmp);
                free(tmp);
                return false;
            }
            *p = c;
            if (!c) break;
        }
    }
    free(tmp);
    return true;
}

/* ---- the entries to write ---- */

typedef struct {
    char      *name;   /* in the archive */
    char      *path;   /* to read, or NULL */
    SeaString *data;   /* or this (+1) */
    unsigned   mode;
    int64_t    mtime;
    bool       dir;
} Ent;

typedef struct { Ent *v; size_t n, cap; } Ents;

static void ents_add(Ents *e, Ent x) {
    if (e->n == e->cap) {
        e->cap = e->cap ? e->cap * 2 : 16;
        e->v = realloc(e->v, e->cap * sizeof *e->v);
    }
    e->v[e->n++] = x;
}

static void ents_free(Ents *e) {
    for (size_t i = 0; i < e->n; i++) {
        free(e->v[i].name);
        free(e->v[i].path);
        if (e->v[i].data) sea_obj_release(&e->v[i].data->obj);
    }
    free(e->v);
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* A file or a directory (recursively) on disk, as entries named under name */
static bool add_path(Ents *e, const char *path, const char *name, char *err, size_t errlen, int depth) {
    struct stat st;
    if (stat(path, &st) != 0) { snprintf(err, errlen, "%s: %s", path, strerror(errno)); return false; }
    if (S_ISREG(st.st_mode)) {
        ents_add(e, (Ent){ strdup(name), strdup(path), NULL, st.st_mode & 0777, (int64_t)st.st_mtime, false });
        return true;
    }
    if (!S_ISDIR(st.st_mode)) return true;     /* sockets, devices: skipped */
    if (depth > 64) { snprintf(err, errlen, "%s: directories nested too deep", path); return false; }
    DIR *d = opendir(path);
    if (!d) { snprintf(err, errlen, "%s: %s", path, strerror(errno)); return false; }
    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (n == cap) { cap = cap ? cap * 2 : 16; names = realloc(names, cap * sizeof *names); }
        names[n++] = strdup(de->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof *names, cmp_str);
    bool ok = true;
    if (n == 0)   /* an empty directory: kept as such */
        ents_add(e, (Ent){ strdup(name), NULL, NULL, st.st_mode & 0777, (int64_t)st.st_mtime, true });
    for (size_t i = 0; i < n; i++) {
        if (ok) {
            char *p = join(path, names[i]), *nm = join(name, names[i]);
            ok = add_path(e, p, nm, err, errlen, depth + 1);
            free(p);
            free(nm);
        }
        free(names[i]);
    }
    free(names);
    return ok;
}

static bool collect(SeaValue entries, const char *base, Ents *e, char *err, size_t errlen) {
    SeaValue one[1] = { entries };
    const SeaValue *items = one;
    size_t count = 1;
    if (sea_is_array(entries)) {
        items = SEA_AS_ARRAY(entries)->data;
        count = SEA_AS_ARRAY(entries)->len;
    }
    for (size_t i = 0; i < count; i++) {
        SeaValue it = items[i];
        SeaString *s = sea_value_get_flat_string(it);
        if (s) {
            char *name = clean_name(s->data);
            if (!name) {
                snprintf(err, errlen, "entry %zu: '%.200s' is not a usable name (empty or with \"..\")", i, s->data);
                sea_obj_release(&s->obj);
                return false;
            }
            char *path = s->data[0] == '/' ? strdup(s->data) : join(base, s->data);
            sea_obj_release(&s->obj);
            bool ok = add_path(e, path, name, err, errlen, 0);
            free(path);
            free(name);
            if (!ok) return false;
            continue;
        }
        if (!sea_is_table(it) && !sea_is_struct(it) && !sea_is_map(it)) {
            snprintf(err, errlen, "entry %zu: expected a path or {name, data | path}, got %s", i, sea_type_name(it));
            return false;
        }
        char *nm = ar_opt_str(it, "name");
        char *name = nm ? clean_name(nm) : NULL;
        if (!name) {
            snprintf(err, errlen, "entry %zu: %s", i, nm ? "the name is empty or has \"..\"" : "a name is needed");
            free(nm);
            return false;
        }
        free(nm);
        SeaValue data;
        int64_t now = (int64_t)time(NULL);
        unsigned mode = (unsigned)ar_opt_int(it, "mode", 0644);
        if (ar_opt(it, "data", &data)) {
            SeaString *ds = sea_value_get_flat_string(data);
            if (!ds) {
                snprintf(err, errlen, "entry %zu: data must be a string", i);
                free(name);
                return false;
            }
            ents_add(e, (Ent){ name, NULL, ds, mode, now, false });
            continue;
        }
        char *p = ar_opt_str(it, "path");
        if (!p) {
            snprintf(err, errlen, "entry %zu ('%.200s'): data or path is needed", i, name);
            free(name);
            return false;
        }
        char *full = p[0] == '/' ? strdup(p) : join(base, p);
        free(p);
        bool ok = add_path(e, full, name, err, errlen, 0);
        free(full);
        free(name);
        if (!ok) return false;
    }
    return true;
}

/* An entry's bytes: its data, or its file read into tmp */
static bool ent_bytes(const Ent *x, ABuf *tmp, const unsigned char **d, size_t *n, char *err, size_t errlen) {
    if (x->data) { *d = (const unsigned char *)x->data->data; *n = x->data->len; return true; }
    tmp->len = 0;
    if (x->dir) { *d = (const unsigned char *)""; *n = 0; return true; }
    if (!read_file(x->path, tmp, err, errlen)) return false;
    *d = tmp->d ? tmp->d : (const unsigned char *)"";
    *n = tmp->len;
    return true;
}

static bool deflate_raw(const unsigned char *d, size_t n, int level, ABuf *out) {
    z_stream z;
    memset(&z, 0, sizeof z);
    if (deflateInit2(&z, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return false;
    out->len = 0;
    size_t bound = deflateBound(&z, (uLong)n);
    ab_put(out, NULL, 0);
    if (out->cap < bound) { out->d = realloc(out->d, bound); out->cap = bound; }
    z.next_in = (unsigned char *)d;
    z.avail_in = (uInt)n;
    z.next_out = out->d;
    z.avail_out = (uInt)out->cap;
    int rc = deflate(&z, Z_FINISH);
    out->len = z.total_out;
    deflateEnd(&z);
    return rc == Z_STREAM_END;
}

static bool inflate_to(const unsigned char *d, size_t n, int wbits, int64_t expect, int64_t max, ABuf *out, char *err, size_t errlen) {
    z_stream z;
    memset(&z, 0, sizeof z);
    if (inflateInit2(&z, wbits) != Z_OK) { snprintf(err, errlen, "could not start the decompressor"); return false; }
    out->len = 0;
    z.next_in = (unsigned char *)d;
    z.avail_in = (uInt)n;
    unsigned char tmp[65536];
    int rc;
    do {
        z.next_out = tmp;
        z.avail_out = sizeof tmp;
        rc = inflate(&z, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) break;
        ab_put(out, tmp, sizeof tmp - z.avail_out);
        if ((int64_t)out->len > max) { inflateEnd(&z); snprintf(err, errlen, "a member is larger than the limit (opts.max)"); return false; }
        if (rc == Z_OK && z.avail_in == 0 && z.avail_out != 0) break;
    } while (rc != Z_STREAM_END);
    inflateEnd(&z);
    if (rc != Z_STREAM_END || (expect >= 0 && (int64_t)out->len != expect)) {
        snprintf(err, errlen, "corrupt compressed data");
        return false;
    }
    return true;
}

/* ---- reading: the members of an archive ---- */

enum { T_FILE, T_DIR, T_SYMLINK, T_LINK, T_OTHER };
static const char *const TYPE_NAMES[] = { "file", "dir", "symlink", "link", "other" };

typedef struct {
    char    *name;
    char    *link;
    int      type;
    int64_t  size, csize, mtime;
    unsigned mode;
    size_t   off;       /* data offset in the archive buffer */
    int      method;    /* zip: 0 stored, 8 deflate */
    uint32_t crc;
} Member;

typedef struct {
    bool    zip;
    ABuf    buf;        /* the archive (decompressed, for a .tar.gz) */
    Member *m;
    size_t  n, cap;
} Arch;

static void arch_add(Arch *a, Member x) {
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 32;
        a->m = realloc(a->m, a->cap * sizeof *a->m);
    }
    a->m[a->n++] = x;
}

static void arch_free(Arch *a) {
    for (size_t i = 0; i < a->n; i++) { free(a->m[i].name); free(a->m[i].link); }
    free(a->m);
    free(a->buf.d);
}

static char *strndup_s(const unsigned char *p, size_t n) {
    char *r = malloc(n + 1);
    memcpy(r, p, n);
    r[n] = '\0';
    return r;
}

/* DOS date and time (local time, as ZIP stores them) */
static void dos_time(int64_t t, unsigned *dtime, unsigned *ddate) {
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    if (tm.tm_year < 80) { *dtime = 0; *ddate = (1 << 5) | 1; return; }
    *dtime = (unsigned)(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
    *ddate = (unsigned)((tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
}

static int64_t from_dos(unsigned dtime, unsigned ddate) {
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = (int)(ddate >> 9) + 80;
    tm.tm_mon = (int)((ddate >> 5) & 15) - 1;
    tm.tm_mday = (int)(ddate & 31);
    tm.tm_hour = (int)(dtime >> 11);
    tm.tm_min = (int)((dtime >> 5) & 63);
    tm.tm_sec = (int)(dtime & 31) * 2;
    tm.tm_isdst = -1;
    return (int64_t)mktime(&tm);
}

static bool zip_parse(Arch *a, char *err, size_t errlen) {
    const unsigned char *b = a->buf.d;
    size_t len = a->buf.len;
    if (len < 22) { snprintf(err, errlen, "not a ZIP archive"); return false; }
    size_t eocd = (size_t)-1;
    size_t lo = len > 22 + 65535 ? len - 22 - 65535 : 0;
    for (size_t i = len - 22 + 1; i-- > lo;) {
        if (le32(b + i) == 0x06054b50 && i + 22 + le16(b + i + 20) <= len) { eocd = i; break; }
    }
    if (eocd == (size_t)-1) { snprintf(err, errlen, "not a ZIP archive (no end of central directory)"); return false; }
    unsigned count = le16(b + eocd + 10);
    uint32_t cdsize = le32(b + eocd + 12), cdoff = le32(b + eocd + 16);
    if (count == 0xFFFF || cdoff == 0xFFFFFFFFu) { snprintf(err, errlen, "ZIP64 archives are not supported"); return false; }
    if ((size_t)cdoff + cdsize > len) { snprintf(err, errlen, "corrupt ZIP archive (central directory)"); return false; }
    size_t p = cdoff;
    for (unsigned i = 0; i < count; i++) {
        if (p + 46 > len || le32(b + p) != 0x02014b50) { snprintf(err, errlen, "corrupt ZIP archive (entry %u)", i); return false; }
        unsigned made = le16(b + p + 4), flags = le16(b + p + 8), method = le16(b + p + 10);
        unsigned nlen = le16(b + p + 28), elen = le16(b + p + 30), clen = le16(b + p + 32);
        uint32_t crc = le32(b + p + 16), csize = le32(b + p + 20), usize = le32(b + p + 24);
        uint32_t eattr = le32(b + p + 38), lho = le32(b + p + 42);
        if (p + 46 + nlen + elen + clen > len) { snprintf(err, errlen, "corrupt ZIP archive (entry %u)", i); return false; }
        if (csize == 0xFFFFFFFFu || usize == 0xFFFFFFFFu || lho == 0xFFFFFFFFu) { snprintf(err, errlen, "ZIP64 archives are not supported"); return false; }
        if ((size_t)lho + 30 > len || le32(b + lho) != 0x04034b50) { snprintf(err, errlen, "corrupt ZIP archive (local header %u)", i); return false; }
        size_t off = (size_t)lho + 30 + le16(b + lho + 26) + le16(b + lho + 28);
        if (off + csize > len) { snprintf(err, errlen, "corrupt ZIP archive (data %u)", i); return false; }
        Member m;
        memset(&m, 0, sizeof m);
        m.name = strndup_s(b + p + 46, nlen);
        size_t nl = strlen(m.name);
        m.type = nl && m.name[nl - 1] == '/' ? T_DIR : T_FILE;
        if (m.type == T_DIR) m.name[nl - 1] = '\0';
        unsigned umode = (made >> 8) == 3 ? eattr >> 16 : 0;
        if (umode && S_ISLNK(umode)) m.type = T_SYMLINK;
        m.mode = umode & 07777 ? umode & 0777 : (m.type == T_DIR ? 0755 : 0644);
        m.size = usize;
        m.csize = csize;
        m.mtime = from_dos(le16(b + p + 12), le16(b + p + 14));
        m.off = off;
        m.method = (flags & 1) ? -1 : (int)method;    /* -1: encrypted */
        m.crc = crc;
        arch_add(a, m);
        p += 46 + nlen + elen + clen;
    }
    return true;
}

static int64_t tar_num(const unsigned char *p, size_t n) {
    if (p[0] & 0x80) {          /* base-256 (GNU, for big sizes) */
        int64_t v = p[0] & 0x3f;
        for (size_t i = 1; i < n; i++) v = v << 8 | p[i];
        return v;
    }
    int64_t v = 0;
    size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == '\0')) i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = v * 8 + (p[i] - '0');
    return v;
}

static bool tar_parse(Arch *a, int64_t max, char *err, size_t errlen) {
    if (a->buf.len >= 2 && a->buf.d[0] == 0x1f && a->buf.d[1] == 0x8b) {
        ABuf plain = { 0 };
        if (!inflate_to(a->buf.d, a->buf.len, 15 + 16, -1, max, &plain, err, errlen)) { free(plain.d); return false; }
        free(a->buf.d);
        a->buf = plain;
    }
    const unsigned char *b = a->buf.d;
    size_t len = a->buf.len, p = 0;
    if (len < 512) { snprintf(err, errlen, "not a tar archive"); return false; }
    char *long_name = NULL, *long_link = NULL;
    while (p + 512 <= len) {
        const unsigned char *h = b + p;
        bool zero = true;
        for (int i = 0; i < 512 && zero; i++) if (h[i]) zero = false;
        if (zero) break;
        unsigned sum = 0;
        for (int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? ' ' : h[i];
        if ((int64_t)sum != tar_num(h + 148, 8)) {
            free(long_name);
            free(long_link);
            snprintf(err, errlen, p == 0 ? "not a tar archive" : "corrupt tar archive (header checksum)");
            return false;
        }
        int64_t size = tar_num(h + 124, 12);
        char type = (char)h[156];
        size_t data = p + 512;
        if (size < 0 || data + (size_t)size > len) {
            free(long_name);
            free(long_link);
            snprintf(err, errlen, "corrupt tar archive (truncated)");
            return false;
        }
        p = data + (((size_t)size + 511) & ~(size_t)511);
        if (type == 'x' || type == 'L' || type == 'K') {
            if (type == 'L') { free(long_name); long_name = strndup_s(b + data, (size_t)size); continue; }
            if (type == 'K') { free(long_link); long_link = strndup_s(b + data, (size_t)size); continue; }
            /* PAX records: "len key=value\n" */
            size_t q = data, end = data + (size_t)size;
            while (q < end) {
                size_t rl = 0, k = q;
                while (k < end && b[k] >= '0' && b[k] <= '9') rl = rl * 10 + (b[k++] - '0');
                if (rl == 0 || q + rl > end || k >= end || b[k] != ' ') break;
                const unsigned char *kv = b + k + 1;
                size_t kvlen = q + rl - (k + 1) - 1;    /* without the \n */
                if (kvlen > 5 && !memcmp(kv, "path=", 5)) { free(long_name); long_name = strndup_s(kv + 5, kvlen - 5); }
                if (kvlen > 9 && !memcmp(kv, "linkpath=", 9)) { free(long_link); long_link = strndup_s(kv + 9, kvlen - 9); }
                q += rl;
            }
            continue;
        }
        if (type == 'g') continue;
        Member m;
        memset(&m, 0, sizeof m);
        if (long_name) {
            m.name = long_name;
            long_name = NULL;
        } else {
            char name[256 + 2];
            size_t nl = strnlen((const char *)h, 100);
            bool ustar = !memcmp(h + 257, "ustar", 5);
            size_t pl = ustar ? strnlen((const char *)h + 345, 155) : 0;
            size_t k = 0;
            if (pl) { memcpy(name, h + 345, pl); k = pl; name[k++] = '/'; }
            memcpy(name + k, h, nl);
            name[k + nl] = '\0';
            m.name = strdup(name);
        }
        if (long_link) { m.link = long_link; long_link = NULL; }
        else m.link = strndup_s(h + 157, strnlen((const char *)h + 157, 100));
        size_t nl = strlen(m.name);
        while (nl > 1 && m.name[nl - 1] == '/') m.name[--nl] = '\0';
        m.type = (type == '0' || type == '\0' || type == '7') ? T_FILE : type == '5' ? T_DIR :
                 type == '2' ? T_SYMLINK : type == '1' ? T_LINK : T_OTHER;
        m.size = m.type == T_FILE ? size : 0;
        m.csize = m.size;
        m.mode = (unsigned)tar_num(h + 100, 8) & 0777;
        m.mtime = tar_num(h + 136, 12);
        m.off = data;
        arch_add(a, m);
    }
    free(long_name);
    free(long_link);
    return true;
}

static bool arch_open(const char *path, bool zip, int64_t max, Arch *a, char *err, size_t errlen) {
    memset(a, 0, sizeof *a);
    a->zip = zip;
    if (!read_file(path, &a->buf, err, errlen)) return false;
    return zip ? zip_parse(a, err, errlen) : tar_parse(a, max, err, errlen);
}

/* A member's bytes (into out, or pointed at in the archive) */
static bool member_bytes(Arch *a, const Member *m, int64_t max, ABuf *out, const unsigned char **d, size_t *n,
                         char *err, size_t errlen) {
    const unsigned char *src = a->buf.d + m->off;
    if (!a->zip || m->method == 0) {
        if (m->size > max) { snprintf(err, errlen, "%s: larger than the limit (opts.max)", m->name); return false; }
        *d = src;
        *n = (size_t)(a->zip ? m->csize : m->size);
    } else if (m->method == 8) {
        char e2[200];
        if (!inflate_to(src, (size_t)m->csize, -15, m->size, max, out, e2, sizeof e2)) {
            snprintf(err, errlen, "%s: %s", m->name, e2);
            return false;
        }
        *d = out->d ? out->d : (const unsigned char *)"";
        *n = out->len;
    } else {
        snprintf(err, errlen, "%s: %s", m->name, m->method < 0 ? "encrypted entries are not supported" : "unsupported compression method");
        return false;
    }
    if (a->zip && (uint32_t)crc32(0, *d, (uInt)*n) != m->crc) {
        snprintf(err, errlen, "%s: CRC mismatch (corrupt data)", m->name);
        return false;
    }
    return true;
}

/* ---- the Sea functions ---- */

static SeaValue ar_err(SeaVM *vm, const char *fn, const char *msg) {
    char buf[700];
    snprintf(buf, sizeof buf, "compress.%s: %s", fn, msg);
    return sea_make_error_str(vm, buf);
}

static void tset_str(SeaTable *t, const char *k, const char *v) {
    SeaString *s = sea_string_new(v, strlen(v));
    sea_table_set_str(t, k, sea_obj_val(&s->obj));
    sea_obj_release(&s->obj);
}

static SeaValue write_out(SeaVM *vm, const char *fn, const char *dst, ABuf *b, size_t count) {
    char err[600];
    SeaValue r = write_file(dst, b->d ? b->d : (unsigned char *)"", b->len, 0, err, sizeof err) ? sea_int_val((int64_t)count)
                                                                                               : ar_err(vm, fn, err);
    free(b->d);
    return r;
}

static SeaValue fn_zip_write(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *dst = argc > 0 ? sea_value_get_flat_string(args[0]) : NULL;
    if (!dst || argc < 2) {
        if (dst) sea_obj_release(&dst->obj);
        return ar_err(vm, "zip_write", "expected a destination path and the entries");
    }
    SeaValue opts = argc > 2 ? args[2] : SEA_NIL_VAL;
    char *base = ar_opt_str(opts, "base");
    int level = (int)ar_opt_int(opts, "level", 6);
    int64_t mtime = ar_opt_int(opts, "mtime", -1);
    char err[600];
    Ents e = { 0 };
    ABuf out = { 0 }, tmp = { 0 }, comp = { 0 }, cd = { 0 };
    SeaValue r = SEA_NIL_VAL;
    if (level < 0 || level > 9) r = ar_err(vm, "zip_write", "level is 0-9");
    else if (!collect(args[1], base, &e, err, sizeof err)) r = ar_err(vm, "zip_write", err);
    else if (e.n > 65535) r = ar_err(vm, "zip_write", "more than 65535 entries (ZIP64 is not supported)");
    for (size_t i = 0; i < e.n && sea_is_nil(r); i++) {
        Ent *x = &e.v[i];
        const unsigned char *d;
        size_t n;
        if (!ent_bytes(x, &tmp, &d, &n, err, sizeof err)) { r = ar_err(vm, "zip_write", err); break; }
        uint32_t crc = (uint32_t)crc32(0, d, (uInt)n);
        const unsigned char *cdata = d;
        size_t csize = n;
        unsigned method = 0;
        if (n > 0 && level > 0 && deflate_raw(d, n, level, &comp) && comp.len < n) {
            cdata = comp.d;
            csize = comp.len;
            method = 8;
        }
        if (n > 0xFFFFFFFEu || out.len > 0xFFFFFFFEu - csize) { r = ar_err(vm, "zip_write", "archive larger than 4 GB (ZIP64 is not supported)"); break; }
        unsigned dt, dd;
        dos_time(mtime >= 0 ? mtime : x->mtime, &dt, &dd);
        char *name = x->dir ? join(x->name, "") : strdup(x->name);     /* a directory ends in '/' */
        size_t nlen = strlen(name);
        uint32_t lho = (uint32_t)out.len;
        ab_u32(&out, 0x04034b50); ab_u16(&out, 20); ab_u16(&out, 0x0800); ab_u16(&out, method);
        ab_u16(&out, dt); ab_u16(&out, dd); ab_u32(&out, crc); ab_u32(&out, (uint32_t)csize); ab_u32(&out, (uint32_t)n);
        ab_u16(&out, (unsigned)nlen); ab_u16(&out, 0);
        ab_put(&out, name, nlen);
        ab_put(&out, cdata, csize);
        unsigned umode = (x->dir ? S_IFDIR : S_IFREG) | (x->mode & 0777);
        ab_u32(&cd, 0x02014b50); ab_u16(&cd, 3 << 8 | 20); ab_u16(&cd, 20); ab_u16(&cd, 0x0800); ab_u16(&cd, method);
        ab_u16(&cd, dt); ab_u16(&cd, dd); ab_u32(&cd, crc); ab_u32(&cd, (uint32_t)csize); ab_u32(&cd, (uint32_t)n);
        ab_u16(&cd, (unsigned)nlen); ab_u16(&cd, 0); ab_u16(&cd, 0); ab_u16(&cd, 0); ab_u16(&cd, 0);
        ab_u32(&cd, (uint32_t)umode << 16 | (x->dir ? 0x10 : 0)); ab_u32(&cd, lho);
        ab_put(&cd, name, nlen);
        free(name);
    }
    if (sea_is_nil(r)) {
        uint32_t cdoff = (uint32_t)out.len;
        ab_put(&out, cd.d, cd.len);
        ab_u32(&out, 0x06054b50); ab_u16(&out, 0); ab_u16(&out, 0);
        ab_u16(&out, (unsigned)e.n); ab_u16(&out, (unsigned)e.n);
        ab_u32(&out, (uint32_t)cd.len); ab_u32(&out, cdoff); ab_u16(&out, 0);
        r = write_out(vm, "zip_write", dst->data, &out, e.n);
    } else {
        free(out.d);
    }
    free(tmp.d);
    free(comp.d);
    free(cd.d);
    free(base);
    ents_free(&e);
    sea_obj_release(&dst->obj);
    return r;
}

/* the tar header fields */
static void octal(unsigned char *p, size_t n, int64_t v) {
    char tmp[24];
    snprintf(tmp, sizeof tmp, "%0*llo", (int)(n - 1), (long long)v);
    memcpy(p, tmp, n - 1);
    p[n - 1] = '\0';
}

static void tar_header(ABuf *out, const char *name, size_t size, unsigned mode, int64_t mtime, char type) {
    unsigned char h[512];
    memset(h, 0, sizeof h);
    size_t nl = strlen(name);
    if (nl <= 100) {
        memcpy(h, name, nl);
    } else {
        /* prefix (155) / name (100), split at a '/' */
        const char *cut = NULL;
        for (const char *s = name + nl - 1; s > name; s--)
            if (*s == '/' && (size_t)(s - name) <= 155 && nl - (size_t)(s - name) - 1 <= 100) { cut = s; break; }
        if (cut) {
            memcpy(h + 345, name, (size_t)(cut - name));
            memcpy(h, cut + 1, nl - (size_t)(cut - name) - 1);
        } else {
            memcpy(h, name, 100);   /* the PAX record before this header holds the full name */
        }
    }
    octal(h + 100, 8, mode & 07777);
    octal(h + 108, 8, 0);
    octal(h + 116, 8, 0);
    if (size < ((size_t)1 << 33)) octal(h + 124, 12, (int64_t)size);
    else { h[124] = 0x80; for (int i = 0; i < 8; i++) h[135 - i] = (unsigned char)(size >> (8 * i)); }
    octal(h + 136, 12, mtime < 0 ? 0 : mtime);
    h[156] = (unsigned char)type;
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    memset(h + 148, ' ', 8);
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += h[i];
    char cs[8];
    snprintf(cs, sizeof cs, "%06o", sum);
    memcpy(h + 148, cs, 7);
    ab_put(out, h, 512);
}

static bool needs_pax(const char *name) {
    size_t nl = strlen(name);
    if (nl <= 100) return false;
    for (const char *s = name + nl - 1; s > name; s--)
        if (*s == '/' && (size_t)(s - name) <= 155 && nl - (size_t)(s - name) - 1 <= 100) return false;
    return true;
}

static SeaValue fn_tar_write(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *dst = argc > 0 ? sea_value_get_flat_string(args[0]) : NULL;
    if (!dst || argc < 2) {
        if (dst) sea_obj_release(&dst->obj);
        return ar_err(vm, "tar_write", "expected a destination path and the entries");
    }
    SeaValue opts = argc > 2 ? args[2] : SEA_NIL_VAL;
    char *base = ar_opt_str(opts, "base");
    int level = (int)ar_opt_int(opts, "level", 6);
    int64_t mtime = ar_opt_int(opts, "mtime", -1);
    SeaValue gz;
    size_t dl = dst->len;
    bool gzip = ar_opt(opts, "gzip", &gz) ? sea_is_truthy(gz)
              : (dl > 3 && !strcmp(dst->data + dl - 3, ".gz")) || (dl > 4 && !strcmp(dst->data + dl - 4, ".tgz"));
    char err[600];
    Ents e = { 0 };
    ABuf out = { 0 }, tmp = { 0 };
    SeaValue r = SEA_NIL_VAL;
    if (level < 0 || level > 9) r = ar_err(vm, "tar_write", "level is 0-9");
    else if (!collect(args[1], base, &e, err, sizeof err)) r = ar_err(vm, "tar_write", err);
    for (size_t i = 0; i < e.n && sea_is_nil(r); i++) {
        Ent *x = &e.v[i];
        const unsigned char *d;
        size_t n;
        if (!ent_bytes(x, &tmp, &d, &n, err, sizeof err)) { r = ar_err(vm, "tar_write", err); break; }
        int64_t mt = mtime >= 0 ? mtime : x->mtime;
        if (needs_pax(x->name)) {
            /* "len path=name\n", where len counts itself */
            size_t body = strlen(x->name) + 7;   /* " path=" + "\n" */
            size_t total = body + 1;
            while (snprintf(NULL, 0, "%zu", total) + body != total) total++;
            char *rec = malloc(total + 1);
            snprintf(rec, total + 1, "%zu path=%s\n", total, x->name);
            tar_header(&out, "././@PaxHeader", total, 0644, mt, 'x');
            ab_put(&out, rec, total);
            ab_zero(&out, (512 - total % 512) % 512);
            free(rec);
        }
        tar_header(&out, x->name, x->dir ? 0 : n, x->dir ? (x->mode ? x->mode : 0755) : x->mode, mt, x->dir ? '5' : '0');
        if (!x->dir) {
            ab_put(&out, d, n);
            ab_zero(&out, (512 - n % 512) % 512);
        }
    }
    if (sea_is_nil(r)) {
        ab_zero(&out, 1024);
        if (gzip) {
            z_stream z;
            memset(&z, 0, sizeof z);
            ABuf g = { 0 };
            if (deflateInit2(&z, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
                r = ar_err(vm, "tar_write", "could not start the compressor");
            } else {
                size_t bound = deflateBound(&z, (uLong)out.len);
                g.d = malloc(bound);
                g.cap = bound;
                z.next_in = out.d;
                z.avail_in = (uInt)out.len;
                z.next_out = g.d;
                z.avail_out = (uInt)bound;
                deflate(&z, Z_FINISH);
                g.len = z.total_out;
                deflateEnd(&z);
                free(out.d);
                out = g;
            }
        }
        if (sea_is_nil(r)) r = write_out(vm, "tar_write", dst->data, &out, e.n);
        else free(out.d);
    } else {
        free(out.d);
    }
    free(tmp.d);
    free(base);
    ents_free(&e);
    sea_obj_release(&dst->obj);
    return r;
}

static SeaValue list_impl(SeaVM *vm, const char *fn, bool zip, int argc, SeaValue *args) {
    SeaString *src = argc > 0 ? sea_value_get_flat_string(args[0]) : NULL;
    if (!src) return ar_err(vm, fn, "expected the archive's path");
    int64_t max = ar_opt_int(argc > 1 ? args[1] : SEA_NIL_VAL, "max", AR_DEFAULT_MAX);
    char err[600];
    Arch a;
    SeaValue r;
    if (!arch_open(src->data, zip, max, &a, err, sizeof err)) {
        r = ar_err(vm, fn, err);
    } else {
        SeaArray *out = sea_array_new((uint32_t)a.n);
        for (size_t i = 0; i < a.n; i++) {
            Member *m = &a.m[i];
            SeaTable *t = sea_table_new(TABLE_MAP);
            tset_str(t, "name", m->name);
            tset_str(t, "type", TYPE_NAMES[m->type]);
            sea_table_set_str(t, "size", sea_int_val(m->size));
            if (zip) sea_table_set_str(t, "compressed", sea_int_val(m->csize));
            sea_table_set_str(t, "mode", sea_int_val(m->mode));
            sea_table_set_str(t, "mtime", sea_int_val(m->mtime));
            if ((m->type == T_SYMLINK || m->type == T_LINK) && m->link) tset_str(t, "link", m->link);
            sea_array_push(out, sea_obj_val(&t->obj));
            sea_obj_release(&t->obj);
        }
        r = sea_obj_val(&out->obj);
    }
    arch_free(&a);
    sea_obj_release(&src->obj);
    return r;
}

static SeaValue read_impl(SeaVM *vm, const char *fn, bool zip, int argc, SeaValue *args) {
    SeaString *src = argc > 0 ? sea_value_get_flat_string(args[0]) : NULL;
    SeaString *name = argc > 1 ? sea_value_get_flat_string(args[1]) : NULL;
    if (!src || !name) {
        if (src) sea_obj_release(&src->obj);
        if (name) sea_obj_release(&name->obj);
        return ar_err(vm, fn, "expected the archive's path and a member name");
    }
    int64_t max = ar_opt_int(argc > 2 ? args[2] : SEA_NIL_VAL, "max", AR_DEFAULT_MAX);
    char err[600];
    Arch a;
    SeaValue r = SEA_NIL_VAL;
    if (!arch_open(src->data, zip, max, &a, err, sizeof err)) {
        r = ar_err(vm, fn, err);
    } else {
        Member *found = NULL;
        for (size_t i = a.n; i-- > 0;)      /* tar: a later copy wins */
            if (a.m[i].type == T_FILE && !strcmp(a.m[i].name, name->data)) { found = &a.m[i]; break; }
        if (!found) {
            snprintf(err, sizeof err, "'%.200s' is not in %.300s", name->data, src->data);
            r = ar_err(vm, fn, err);
        } else {
            ABuf tmp = { 0 };
            const unsigned char *d;
            size_t n;
            if (member_bytes(&a, found, max, &tmp, &d, &n, err, sizeof err))
                r = sea_obj_val(&sea_string_new((const char *)d, n)->obj);
            else
                r = ar_err(vm, fn, err);
            free(tmp.d);
        }
    }
    arch_free(&a);
    sea_obj_release(&src->obj);
    sea_obj_release(&name->obj);
    return r;
}

/* whether a member is among opts.names (all when there is no such list) */
static bool wanted(SeaValue names, const char *name) {
    if (!sea_is_array(names)) return true;
    SeaArray *arr = SEA_AS_ARRAY(names);
    for (uint32_t i = 0; i < arr->len; i++) {
        SeaString *s = sea_value_get_flat_string(arr->data[i]);
        if (!s) continue;
        bool eq = !strcmp(s->data, name);
        sea_obj_release(&s->obj);
        if (eq) return true;
    }
    return false;
}

static SeaValue extract_impl(SeaVM *vm, const char *fn, bool zip, int argc, SeaValue *args) {
    SeaString *src = argc > 0 ? sea_value_get_flat_string(args[0]) : NULL;
    SeaString *dir = argc > 1 ? sea_value_get_flat_string(args[1]) : NULL;
    if (!src || !dir) {
        if (src) sea_obj_release(&src->obj);
        if (dir) sea_obj_release(&dir->obj);
        return ar_err(vm, fn, "expected the archive's path and a directory");
    }
    SeaValue opts = argc > 2 ? args[2] : SEA_NIL_VAL;
    int64_t max = ar_opt_int(opts, "max", AR_DEFAULT_MAX);
    SeaValue names = SEA_NIL_VAL;
    ar_opt(opts, "names", &names);
    char err[600];
    Arch a;
    SeaValue r = SEA_NIL_VAL;
    if (!arch_open(src->data, zip, max, &a, err, sizeof err)) {
        r = ar_err(vm, fn, err);
        goto done;
    }
    /* check every name first: nothing is written from a bad archive */
    for (size_t i = 0; i < a.n; i++) {
        if (!safe_name(a.m[i].name) && wanted(names, a.m[i].name)) {
            snprintf(err, sizeof err, "unsafe name '%.200s' (absolute or with \"..\"): nothing extracted", a.m[i].name);
            r = ar_err(vm, fn, err);
            goto done;
        }
    }
    if (!mkdir_p(dir->data, err, sizeof err)) { r = ar_err(vm, fn, err); goto done; }
    SeaArray *out = sea_array_new(16);
    ABuf tmp = { 0 };
    for (size_t i = 0; i < a.n; i++) {
        Member *m = &a.m[i];
        if ((m->type != T_FILE && m->type != T_DIR) || !wanted(names, m->name)) continue;
        char *path = join(dir->data, m->name);
        bool ok = true;
        if (m->type == T_DIR) {
            ok = mkdir_p(path, err, sizeof err);
        } else {
            char *slash = strrchr(path, '/');
            if (slash && slash != path) {
                *slash = '\0';
                ok = mkdir_p(path, err, sizeof err);
                *slash = '/';
            }
            const unsigned char *d;
            size_t n;
            tmp.len = 0;
            ok = ok && member_bytes(&a, m, max, &tmp, &d, &n, err, sizeof err) &&
                 write_file(path, d, n, m->mode ? m->mode : 0644, err, sizeof err);
        }
        if (ok && m->mtime > 0) {
            struct timeval tv[2] = { { (time_t)m->mtime, 0 }, { (time_t)m->mtime, 0 } };
            utimes(path, tv);
        }
        free(path);
        if (!ok) {
            free(tmp.d);
            sea_obj_release(&out->obj);
            r = ar_err(vm, fn, err);
            goto done;
        }
        SeaString *s = sea_string_new(m->name, strlen(m->name));
        sea_array_push(out, sea_obj_val(&s->obj));
        sea_obj_release(&s->obj);
    }
    free(tmp.d);
    r = sea_obj_val(&out->obj);
done:
    arch_free(&a);
    sea_obj_release(&src->obj);
    sea_obj_release(&dir->obj);
    return r;
}

static SeaValue fn_zip_list(SeaVM *vm, int argc, SeaValue *args) { return list_impl(vm, "zip_list", true, argc, args); }
static SeaValue fn_tar_list(SeaVM *vm, int argc, SeaValue *args) { return list_impl(vm, "tar_list", false, argc, args); }
static SeaValue fn_zip_read(SeaVM *vm, int argc, SeaValue *args) { return read_impl(vm, "zip_read", true, argc, args); }
static SeaValue fn_tar_read(SeaVM *vm, int argc, SeaValue *args) { return read_impl(vm, "tar_read", false, argc, args); }
static SeaValue fn_zip_extract(SeaVM *vm, int argc, SeaValue *args) { return extract_impl(vm, "zip_extract", true, argc, args); }
static SeaValue fn_tar_extract(SeaVM *vm, int argc, SeaValue *args) { return extract_impl(vm, "tar_extract", false, argc, args); }

void compress_archive_register(SeaTable *mod) {
    sea_mod_set(mod, "zip_write",   fn_zip_write);
    sea_mod_set(mod, "zip_list",    fn_zip_list);
    sea_mod_set(mod, "zip_read",    fn_zip_read);
    sea_mod_set(mod, "zip_extract", fn_zip_extract);
    sea_mod_set(mod, "tar_write",   fn_tar_write);
    sea_mod_set(mod, "tar_list",    fn_tar_list);
    sea_mod_set(mod, "tar_read",    fn_tar_read);
    sea_mod_set(mod, "tar_extract", fn_tar_extract);
}
