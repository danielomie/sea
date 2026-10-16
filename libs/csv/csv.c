/*
 * libs/csv/csv.c — CSV (RFC 4180) for Sea.
 *
 * Build:  make -C libs/csv
 * Usage:  local csv = require("../libs/csv/csv")   -- relative to your file
 *
 *   csv.decode(s [, opts]) → array of rows | error
 *   csv.encode(rows [, opts]) → string | error
 *   csv.each(path, fn [, opts]) → rows read | error     (streams a file)
 *
 * Rows are arrays of strings, or tables keyed by the header's names with
 * header = true. Quoted fields may hold separators, quotes ("" inside) and
 * line breaks; \n, \r\n and \r all end a record. Failures are error values
 * with the line: "csv.decode: unterminated quoted field at line 3".
 */

#include "sea.h"
#include "sea_rope.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Helpers ---------------------------------------------------------------- */

typedef struct { char *d; size_t len, cap; } Buf;

static void buf_push(Buf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->len + n + 1) nc *= 2;
        b->d = realloc(b->d, nc);
        b->cap = nc;
    }
    memcpy(b->d + b->len, s, n);
    b->len += n;
    b->d[b->len] = '\0';
}

static void buf_putc(Buf *b, char c) { buf_push(b, &c, 1); }

static SeaValue new_str(const char *s, size_t n) { return sea_obj_val(&sea_string_new(s, n)->obj); }

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

static int opt_flag(SeaValue opts, const char *field, int def) {
    SeaValue v;
    if (!opt_get(opts, field, &v) || !sea_is_bool(v)) return def;
    return sea_as_bool(v);
}

/* A one-character option (sep, quote, comment); -1: absent, -2: invalid. */
static int opt_char(SeaValue opts, const char *field, int def) {
    SeaValue v;
    if (!opt_get(opts, field, &v)) return def;
    SeaString *s = sea_value_get_flat_string(v);
    if (!s) return -2;
    int c = s->len == 1 ? (unsigned char)s->data[0] : (s->len == 0 ? -1 : -2);
    sea_obj_release(&s->obj);
    return c;
}

/* ======================================================================== */
/* Parser                                                                   */
/* ======================================================================== */

typedef struct {
    int  sep, quote, comment;   /* comment -1: none */
    bool trim, skip_empty, numbers, strict, header;
} Opts;

/* One record's fields (offsets into `text`). */
typedef struct {
    Buf     text;               /* field bytes, back to back */
    size_t *off, *len;
    bool   *quoted;
    size_t  n, cap;
} Record;

static void rec_reset(Record *r) { r->text.len = 0; r->n = 0; }

static void rec_add(Record *r, size_t start, bool quoted) {
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 16;
        r->off = realloc(r->off, r->cap * sizeof *r->off);
        r->len = realloc(r->len, r->cap * sizeof *r->len);
        r->quoted = realloc(r->quoted, r->cap * sizeof *r->quoted);
    }
    r->off[r->n] = start;
    r->len[r->n] = r->text.len - start;
    r->quoted[r->n] = quoted;
    r->n++;
}

static void rec_free(Record *r) {
    free(r->text.d);
    free(r->off);
    free(r->len);
    free(r->quoted);
}

enum { P_OK, P_MORE, P_END, P_ERR, P_SKIP };

/* Parse one record from s[*pos..n). P_MORE: the record continues past n
 * (more input needed, unless eof); P_END: nothing left. *line counts line
 * breaks outside quoted fields and inside them. */
static int parse_record(const char *s, size_t n, size_t *pos, bool eof, const Opts *o, Record *r,
                        long *line, const char **err) {
    size_t i = *pos;
    long ln = *line;
    rec_reset(r);
    if (i >= n) return eof ? P_END : P_MORE;
    if (o->comment >= 0 && (unsigned char)s[i] == o->comment) {
        /* a comment line: skipped whole, quotes and all */
        while (i < n && s[i] != '\n' && s[i] != '\r') i++;
        if (i >= n && !eof) return P_MORE;
        if (i < n && s[i] == '\r') {
            if (i + 1 >= n && !eof) return P_MORE;
            i++;
            if (i < n && s[i] == '\n') i++;
        } else if (i < n) {
            i++;
        }
        *pos = i;
        *line = ln + 1;
        return P_SKIP;
    }
    for (;;) {
        /* one field */
        size_t start = r->text.len;
        bool quoted = false;
        if (o->trim) while (i < n && (s[i] == ' ' || s[i] == '\t') && s[i] != o->sep) i++;
        if (i < n && s[i] == o->quote) {
            quoted = true;
            i++;
            for (;;) {
                if (i >= n) {
                    if (!eof) return P_MORE;
                    *err = "unterminated quoted field";
                    *line = ln;
                    return P_ERR;
                }
                char c = s[i];
                if (c == o->quote) {
                    if (i + 1 < n && s[i + 1] == o->quote) { buf_putc(&r->text, c); i += 2; continue; }
                    if (i + 1 >= n && !eof) return P_MORE;      /* "" may straddle the chunk */
                    i++;
                    break;
                }
                if (c == '\n' || (c == '\r' && !(i + 1 < n && s[i + 1] == '\n'))) ln++;
                buf_putc(&r->text, c);
                i++;
            }
            /* after the closing quote: separator, line end, or (lenient) more text */
            if (o->trim) while (i < n && (s[i] == ' ' || s[i] == '\t') && s[i] != o->sep) i++;
            if (i < n && s[i] != o->sep && s[i] != '\n' && s[i] != '\r') {
                if (o->strict) {
                    *err = "unexpected character after a closing quote";
                    *line = ln;
                    return P_ERR;
                }
                while (i < n && s[i] != o->sep && s[i] != '\n' && s[i] != '\r') buf_putc(&r->text, s[i++]);
            }
        } else {
            while (i < n && s[i] != o->sep && s[i] != '\n' && s[i] != '\r') {
                if (s[i] == o->quote && o->strict) {
                    *err = "a quote inside an unquoted field";
                    *line = ln;
                    return P_ERR;
                }
                buf_putc(&r->text, s[i++]);
            }
            if (o->trim)
                while (r->text.len > start && (r->text.d[r->text.len - 1] == ' ' || r->text.d[r->text.len - 1] == '\t'))
                    r->text.len--;
        }
        if (i >= n && !eof) return P_MORE;
        rec_add(r, start, quoted);
        if (i < n && s[i] == o->sep) { i++; continue; }
        /* end of record */
        if (i < n && s[i] == '\r') {
            if (i + 1 >= n && !eof) return P_MORE;              /* \r\n may straddle */
            i++;
            if (i < n && s[i] == '\n') i++;
        } else if (i < n && s[i] == '\n') {
            i++;
        }
        ln++;
        *pos = i;
        *line = ln;
        return P_OK;
    }
}

/* Is this record a blank line (one empty unquoted field)? */
static bool skip_record(const Record *r, const Opts *o) {
    return r->n == 1 && r->len[0] == 0 && !r->quoted[0] && o->skip_empty;
}

/* Decimal number syntax: [-+]? (digits [. digits?] | . digits) ([eE] [-+]? digits)?,
 * without a leading zero before another digit ("007" and "0x10" stay text).
 * *is_int: no fraction or exponent. */
static bool decimal_syntax(const char *p, size_t n, bool *is_int) {
    size_t i = 0;
    if (i < n && (p[i] == '-' || p[i] == '+')) i++;
    size_t ds = i;
    while (i < n && p[i] >= '0' && p[i] <= '9') i++;
    size_t idigits = i - ds;
    if (idigits > 1 && p[ds] == '0') return false;
    bool frac = false;
    size_t fdigits = 0;
    if (i < n && p[i] == '.') {
        frac = true;
        i++;
        size_t fs = i;
        while (i < n && p[i] >= '0' && p[i] <= '9') i++;
        fdigits = i - fs;
    }
    if (idigits == 0 && fdigits == 0) return false;
    bool expo = false;
    if (i < n && (p[i] == 'e' || p[i] == 'E')) {
        expo = true;
        i++;
        if (i < n && (p[i] == '-' || p[i] == '+')) i++;
        size_t es = i;
        while (i < n && p[i] >= '0' && p[i] <= '9') i++;
        if (i == es) return false;
    }
    *is_int = !frac && !expo;
    return i == n;
}

/* A field as a Sea value: a string, or a number with numbers = true. */
static SeaValue field_value(const Record *r, size_t k, const Opts *o) {
    const char *p = r->text.d + r->off[k];
    size_t n = r->len[k];
    bool is_int;
    if (o->numbers && !r->quoted[k] && n > 0 && n < 64 && decimal_syntax(p, n, &is_int)) {
        char tmp[64];
        memcpy(tmp, p, n);
        tmp[n] = '\0';
        if (is_int) {
            errno = 0;
            long long v = strtoll(tmp, NULL, 10);
            if (errno == 0 && v < (1LL << 47) && v >= -(1LL << 47)) return sea_int_val(v);
        }
        double d = strtod(tmp, NULL);
        if (!isinf(d)) return sea_number_val(d);
    }
    return new_str(p, n);
}

static bool parse_opts(SeaValue opts, Opts *o, const char **err) {
    o->sep = opt_char(opts, "sep", ',');
    o->quote = opt_char(opts, "quote", '"');
    o->comment = opt_char(opts, "comment", -1);
    o->trim = opt_flag(opts, "trim", 0);
    o->skip_empty = opt_flag(opts, "skip_empty", 1);
    o->numbers = opt_flag(opts, "numbers", 0);
    o->strict = opt_flag(opts, "strict", 0);
    o->header = opt_flag(opts, "header", 0);
    if (o->sep < 0 || o->quote < 0 || o->comment == -2) { *err = "sep, quote and comment must be one character"; return false; }
    if (o->sep == '\n' || o->sep == '\r' || o->sep == o->quote) { *err = "invalid separator"; return false; }
    return true;
}

/* Turning records into rows: arrays, or tables keyed by the header. */
typedef struct {
    Opts       o;
    SeaString **names;          /* header names */
    size_t     nnames;
    bool       have_header;
    long       rows;
    size_t     width;           /* fields in the first row (strict) */
    char       err[200];
} Builder;

static void builder_free(Builder *b) {
    for (size_t i = 0; i < b->nnames; i++) sea_obj_release(&b->names[i]->obj);
    free(b->names);
}

/* The row for a record (owned), SEA_NIL_VAL when the record was the header,
 * or false with b->err set. */
static bool build_row(Builder *b, const Record *r, long line, SeaValue *out) {
    *out = SEA_NIL_VAL;
    if (b->o.header && !b->have_header) {
        b->names = calloc(r->n ? r->n : 1, sizeof *b->names);
        for (size_t k = 0; k < r->n; k++) {
            const char *p = r->text.d + r->off[k];
            SeaString *nm = sea_string_find_intern(p, r->len[k]);
            b->names[k] = nm ? nm : sea_string_new(p, r->len[k]);
        }
        b->nnames = r->n;
        b->have_header = true;
        return true;
    }
    if (b->o.header) {
        if (r->n != b->nnames) {
            snprintf(b->err, sizeof b->err, "line %ld has %zu fields, the header has %zu", line, r->n, b->nnames);
            return false;
        }
        SeaTable *t = sea_table_new(TABLE_MAP);
        for (size_t k = 0; k < r->n; k++) {
            SeaValue v = field_value(r, k, &b->o);
            sea_table_set(t, sea_obj_val(&b->names[k]->obj), v);
            if (sea_is_obj(v)) sea_obj_release(sea_as_obj(v));
        }
        *out = sea_obj_val(&t->obj);
    } else {
        if (b->o.strict) {
            if (b->rows == 0) b->width = r->n;
            else if (r->n != b->width) {
                snprintf(b->err, sizeof b->err, "line %ld has %zu fields, the first row has %zu", line, r->n, b->width);
                return false;
            }
        }
        SeaArray *a = sea_array_new((uint32_t)(r->n ? r->n : 1));
        for (size_t k = 0; k < r->n; k++) {
            SeaValue v = field_value(r, k, &b->o);
            sea_array_push(a, v);
            if (sea_is_obj(v)) sea_obj_release(sea_as_obj(v));
        }
        *out = sea_obj_val(&a->obj);
    }
    b->rows++;
    return true;
}

static SeaValue fail(SeaVM *vm, const char *fn, const char *msg, long line) {
    char buf[300];
    if (line > 0) snprintf(buf, sizeof buf, "csv.%s: %s at line %ld", fn, msg, line);
    else snprintf(buf, sizeof buf, "csv.%s: %s", fn, msg);
    return sea_make_error_str(vm, buf);
}

/* csv.decode(s [, opts]): opts sep (","), quote ("\""), header (rows as
 * tables), numbers (unquoted numeric fields as numbers), trim, comment
 * (lines starting with it are skipped), skip_empty (true), strict */
static SeaValue fn_decode(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *s = argc > 0 ? sea_value_get_flat_string(args[0]) : NULL;
    if (!s) return sea_make_error_str(vm, "csv.decode: string expected");
    Builder b;
    memset(&b, 0, sizeof b);
    const char *oerr = NULL;
    if (!parse_opts(argc > 1 ? args[1] : SEA_NIL_VAL, &b.o, &oerr)) {
        sea_obj_release(&s->obj);
        return fail(vm, "decode", oerr, 0);
    }
    const char *d = s->data;
    size_t n = s->len, pos = 0;
    if (n >= 3 && memcmp(d, "\xEF\xBB\xBF", 3) == 0) pos = 3;
    SeaArray *rows = sea_array_new(16);
    Record r = { 0 };
    long line = 1;
    SeaValue result = SEA_NIL_VAL;
    for (;;) {
        long rline = line;
        const char *err = NULL;
        int st = parse_record(d, n, &pos, true, &b.o, &r, &line, &err);
        if (st == P_END) break;
        if (st == P_ERR) { result = fail(vm, "decode", err, line); break; }
        if (st == P_SKIP || skip_record(&r, &b.o)) continue;
        SeaValue row;
        if (!build_row(&b, &r, rline, &row)) { result = fail(vm, "decode", b.err, 0); break; }
        if (!sea_is_nil(row)) {
            sea_array_push(rows, row);
            sea_obj_release(sea_as_obj(row));
        }
    }
    rec_free(&r);
    builder_free(&b);
    sea_obj_release(&s->obj);
    if (!sea_is_nil(result)) { sea_obj_release(&rows->obj); return result; }
    return sea_obj_val(&rows->obj);
}

/* csv.each(path, fn [, opts]): fn(row) for each row of the file, reading it
 * in chunks; fn returning false stops. Returns the number of rows. */
static SeaValue fn_each(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *path = argc > 0 ? sea_value_get_flat_string(args[0]) : NULL;
    if (!path) return sea_make_error_str(vm, "csv.each: path expected");
    if (argc < 2 || !(sea_is_closure(args[1]) || sea_is_function(args[1]))) {
        sea_obj_release(&path->obj);
        return sea_make_error_str(vm, "csv.each: function expected");
    }
    Builder b;
    memset(&b, 0, sizeof b);
    const char *oerr = NULL;
    if (!parse_opts(argc > 2 ? args[2] : SEA_NIL_VAL, &b.o, &oerr)) {
        sea_obj_release(&path->obj);
        return fail(vm, "each", oerr, 0);
    }
    FILE *f = fopen(path->data, "rb");
    if (!f) {
        char msg[300];
        snprintf(msg, sizeof msg, "%s: %s", path->data, strerror(errno));
        sea_obj_release(&path->obj);
        return fail(vm, "each", msg, 0);
    }
    sea_obj_release(&path->obj);
    Buf in = { 0 };
    size_t pos = 0;
    bool eof = false, first = true, stop = false;
    Record r = { 0 };
    long line = 1;
    SeaValue result = SEA_NIL_VAL;
    char chunk[65536];
    while (!stop && sea_is_nil(result)) {
        if (!eof) {
            size_t got = fread(chunk, 1, sizeof chunk, f);
            if (got) buf_push(&in, chunk, got);
            if (got < sizeof chunk) eof = true;
            if (first && in.len >= 3 && memcmp(in.d, "\xEF\xBB\xBF", 3) == 0) pos = 3;
            if (first && (in.len >= 3 || eof)) first = false;
        }
        for (;;) {
            size_t start = pos;
            long rline = line, save_line = line;
            const char *err = NULL;
            int st = parse_record(in.d ? in.d : "", in.len, &pos, eof, &b.o, &r, &line, &err);
            if (st == P_MORE) { pos = start; line = save_line; break; }
            if (st == P_END) { stop = true; break; }
            if (st == P_ERR) { result = fail(vm, "each", err, line); break; }
            if (st == P_SKIP || skip_record(&r, &b.o)) continue;
            SeaValue row;
            if (!build_row(&b, &r, rline, &row)) { result = fail(vm, "each", b.err, 0); break; }
            if (sea_is_nil(row)) continue;
            SeaValue ret = sea_vm_call(vm, args[1], &row, 1);
            sea_obj_release(sea_as_obj(row));
            bool halt = sea_is_bool(ret) && !sea_as_bool(ret);
            if (sea_is_obj(ret)) sea_obj_release(sea_as_obj(ret));
            if (vm->panic_propagating) { result = SEA_FALSE_VAL; stop = true; break; }
            if (halt) { stop = true; break; }
        }
        /* drop what was consumed */
        if (pos > 0 && pos <= in.len) {
            memmove(in.d, in.d + pos, in.len - pos);
            in.len -= pos;
            pos = 0;
        }
        if (eof && in.len == 0) stop = true;
    }
    fclose(f);
    free(in.d);
    rec_free(&r);
    long count = b.rows;
    builder_free(&b);
    if (result == SEA_FALSE_VAL) return SEA_NIL_VAL;    /* a panic is on its way */
    if (!sea_is_nil(result)) return result;
    return sea_int_val(count);
}

/* ======================================================================== */
/* Encoder                                                                  */
/* ======================================================================== */

typedef struct {
    int  sep, quote;
    bool crlf, quote_all;
    Buf  out;
    char err[200];
} Enc;

static void write_field(Enc *e, const char *p, size_t n) {
    bool need = e->quote_all;
    for (size_t i = 0; i < n && !need; i++)
        if (p[i] == e->sep || p[i] == e->quote || p[i] == '\n' || p[i] == '\r') need = true;
    if (!need && n > 0 && (p[0] == ' ' || p[n - 1] == ' ')) need = true;   /* readers that trim keep them */
    if (!need) { buf_push(&e->out, p, n); return; }
    buf_putc(&e->out, (char)e->quote);
    for (size_t i = 0; i < n; i++) {
        if (p[i] == e->quote) buf_putc(&e->out, (char)e->quote);
        buf_putc(&e->out, p[i]);
    }
    buf_putc(&e->out, (char)e->quote);
}

static int fmt_double(char *out, double d) {
    int n = snprintf(out, 32, "%.15g", d);
    if (strtod(out, NULL) != d) {
        n = snprintf(out, 32, "%.16g", d);
        if (strtod(out, NULL) != d) n = snprintf(out, 32, "%.17g", d);
    }
    return n;
}

/* A value as field text; false for a type with no CSV form. */
static bool write_value(Enc *e, SeaValue v) {
    char t[48];
    if (sea_is_nil(v)) return true;
    if (sea_is_string(v)) { write_field(e, SEA_AS_STRING(v)->data, SEA_AS_STRING(v)->len); return true; }
    if (sea_is_rope(v)) {
        SeaString *s = sea_rope_flatten(SEA_AS_ROPE(v));
        write_field(e, s->data, s->len);
        sea_obj_release(&s->obj);
        return true;
    }
    if (sea_is_int(v)) { int n = snprintf(t, sizeof t, "%lld", (long long)sea_as_int(v)); write_field(e, t, (size_t)n); return true; }
    if (sea_is_number(v)) {
        double d = sea_as_number(v);
        if (isnan(d) || isinf(d)) return true;            /* no CSV form: empty */
        write_field(e, t, (size_t)fmt_double(t, d));
        return true;
    }
    if (sea_is_bool(v)) { write_field(e, sea_as_bool(v) ? "true" : "false", sea_as_bool(v) ? 4 : 5); return true; }
    if (sea_is_bigint(v)) {
        SeaString *s = sea_bigint_to_string(SEA_AS_BIGINT(v), 10);
        write_field(e, s->data, s->len);
        sea_obj_release(&s->obj);
        return true;
    }
    snprintf(e->err, sizeof e->err, "cannot write a value of type %s in a field", sea_type_name(v));
    return false;
}

static void end_line(Enc *e) {
    if (e->crlf) buf_push(&e->out, "\r\n", 2);
    else buf_putc(&e->out, '\n');
}

static uint32_t seq_len(SeaValue v) {
    if (sea_is_array(v)) return sea_array_len(SEA_AS_ARRAY(v));
    return sea_table_count(SEA_AS_TABLE(v));
}

static SeaValue seq_get(SeaValue v, uint32_t i) {
    if (sea_is_array(v)) return sea_array_get(SEA_AS_ARRAY(v), (int64_t)i);
    SeaValue x;
    if (!sea_table_get(SEA_AS_TABLE(v), sea_int_val((int64_t)i), &x)) x = SEA_NIL_VAL;
    return x;
}

/* A table whose keys are exactly 0..n-1 is a list. */
static bool is_list(SeaValue v) {
    if (sea_is_array(v)) return true;
    if (!sea_is_table(v)) return false;
    uint32_t count = sea_table_count(SEA_AS_TABLE(v)), it = 0;
    SeaValue k, x;
    while (sea_table_next(SEA_AS_TABLE(v), &it, &k, &x))
        if (!sea_is_int(k) || sea_as_int(k) < 0 || (uint64_t)sea_as_int(k) >= count) return false;
    return true;
}

static bool record_get(SeaValue row, SeaValue key, SeaValue *out) {
    *out = SEA_NIL_VAL;
    if (sea_is_table(row)) return sea_table_get(SEA_AS_TABLE(row), key, out);
    if (sea_is_map(row)) return sea_map_get(SEA_AS_MAP(row), key, out);
    if (sea_is_struct(row) && sea_is_string(key)) { *out = sea_struct_get(SEA_AS_STRUCT(row), SEA_AS_STRING(key)); return true; }
    return false;
}

static int str_cmp(const void *a, const void *b) {
    const SeaString *x = *(SeaString *const *)a, *y = *(SeaString *const *)b;
    size_t n = x->len < y->len ? x->len : y->len;
    int c = memcmp(x->data, y->data, n);
    return c ? c : (x->len > y->len) - (x->len < y->len);
}

/* csv.encode(rows [, opts]): rows are lists (written in order) or records
 * (tables, maps, structs) written in the order of opts.columns (default:
 * every key, sorted) under a header line (opts.header = false omits it).
 * opts: sep, quote, crlf (line ends "\r\n"), quote_all */
static SeaValue fn_encode(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1 || !is_list(args[0]))
        return sea_make_error_str(vm, "csv.encode: an array of rows expected");
    SeaValue opts = argc > 1 ? args[1] : SEA_NIL_VAL;
    Enc e;
    memset(&e, 0, sizeof e);
    e.sep = opt_char(opts, "sep", ',');
    e.quote = opt_char(opts, "quote", '"');
    e.crlf = opt_flag(opts, "crlf", 0);
    e.quote_all = opt_flag(opts, "quote_all", 0);
    if (e.sep < 0 || e.quote < 0) return sea_make_error_str(vm, "csv.encode: sep and quote must be one character");
    SeaValue rows = args[0];
    uint32_t n = seq_len(rows);
    /* columns for records */
    SeaString **cols = NULL;
    size_t ncols = 0;
    bool records = false;
    for (uint32_t i = 0; i < n; i++) {
        SeaValue row = seq_get(rows, i);
        if (!is_list(row)) { records = true; break; }
    }
    SeaValue colv;
    bool ok = true;
    if (records) {
        if (opt_get(opts, "columns", &colv) && is_list(colv)) {
            ncols = seq_len(colv);
            cols = calloc(ncols ? ncols : 1, sizeof *cols);
            for (size_t c = 0; c < ncols; c++) {
                SeaString *s = sea_value_get_flat_string(seq_get(colv, (uint32_t)c));
                if (!s) { ok = false; snprintf(e.err, sizeof e.err, "columns must be strings"); ncols = c; break; }
                cols[c] = s;
            }
        } else {
            /* the union of the records' string keys, sorted */
            size_t cap = 16;
            cols = calloc(cap, sizeof *cols);
            for (uint32_t i = 0; i < n && ok; i++) {
                SeaValue row = seq_get(rows, i);
                uint32_t it = 0;
                SeaValue k, v;
                for (;;) {
                    bool more = sea_is_table(row) ? sea_table_next(SEA_AS_TABLE(row), &it, &k, &v)
                              : sea_is_map(row) ? sea_map_next(SEA_AS_MAP(row), &it, &k, &v)
                              : sea_is_struct(row) ? sea_struct_next(SEA_AS_STRUCT(row), &it, &k, &v) : false;
                    if (!more) break;
                    if (!sea_is_string(k)) continue;
                    bool seen = false;
                    for (size_t c = 0; c < ncols; c++) if (cols[c]->len == SEA_AS_STRING(k)->len &&
                        memcmp(cols[c]->data, SEA_AS_STRING(k)->data, cols[c]->len) == 0) { seen = true; break; }
                    if (seen) continue;
                    if (ncols == cap) { cap *= 2; cols = realloc(cols, cap * sizeof *cols); }
                    sea_obj_retain(sea_as_obj(k));
                    cols[ncols++] = SEA_AS_STRING(k);
                }
            }
            qsort(cols, ncols, sizeof *cols, str_cmp);
        }
        if (ok && opt_flag(opts, "header", 1)) {
            for (size_t c = 0; c < ncols; c++) {
                if (c) buf_putc(&e.out, (char)e.sep);
                write_field(&e, cols[c]->data, cols[c]->len);
            }
            end_line(&e);
        }
    }
    for (uint32_t i = 0; i < n && ok; i++) {
        SeaValue row = seq_get(rows, i);
        if (is_list(row)) {
            uint32_t m = seq_len(row);
            for (uint32_t c = 0; c < m && ok; c++) {
                if (c) buf_putc(&e.out, (char)e.sep);
                ok = write_value(&e, seq_get(row, c));
            }
        } else if (sea_is_table(row) || sea_is_map(row) || sea_is_struct(row)) {
            for (size_t c = 0; c < ncols && ok; c++) {
                if (c) buf_putc(&e.out, (char)e.sep);
                SeaValue v;
                record_get(row, sea_obj_val(&cols[c]->obj), &v);
                ok = write_value(&e, v);
            }
        } else {
            snprintf(e.err, sizeof e.err, "row %u is a %s, not a list or a record", i, sea_type_name(row));
            ok = false;
        }
        if (ok) end_line(&e);
    }
    for (size_t c = 0; c < ncols; c++) sea_obj_release(&cols[c]->obj);
    free(cols);
    SeaValue r;
    if (!ok) {
        char msg[260];
        snprintf(msg, sizeof msg, "csv.encode: %s", e.err);
        r = sea_make_error_str(vm, msg);
    } else {
        r = new_str(e.out.d ? e.out.d : "", e.out.len);
    }
    free(e.out.d);
    return r;
}

SEA_EXPORT SeaValue sea_open(SeaVM *vm) {
    (void)vm;
    SeaTable *mod = sea_mod_new();
    sea_mod_set(mod, "decode", fn_decode);
    sea_mod_set(mod, "encode", fn_encode);
    sea_mod_set(mod, "each",   fn_each);
    return sea_obj_val(&mod->obj);
}
