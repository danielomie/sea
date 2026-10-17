/*
 * libs/toml/toml.c — TOML 1.0 for Sea.
 *
 * Build:  make -C libs/toml
 * Usage:  local toml = require("../libs/toml/toml")   -- relative to your file
 *
 *   toml.decode(s [, opts]) → table | error
 *   toml.encode(t [, opts]) → string | error
 *   toml.valid(s) → bool
 *   toml.datetime(s) → datetime value | error
 *
 * Tables become Sea tables, arrays arrays; dates and times are strings in
 * RFC 3339 form ("1979-05-27T07:32:00Z"), or datetime values with
 * datetimes = "values" (tables {__toml_datetime = text, kind = "datetime" |
 * "datetime-local" | "date-local" | "time-local"}) that encode writes back
 * as TOML dates. Failures are error values with the position:
 * "toml.decode: duplicate key 'name' at line 4, column 1".
 */

#include "sea.h"
#include "sea_rope.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 256

/* ---- Helpers ---------------------------------------------------------------- */

typedef struct { char *d; size_t len, cap; } Buf;

static void buf_push(Buf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 128;
        while (nc < b->len + n + 1) nc *= 2;
        b->d = realloc(b->d, nc);
        b->cap = nc;
    }
    memcpy(b->d + b->len, s, n);
    b->len += n;
    b->d[b->len] = '\0';
}

static void buf_putc(Buf *b, char c) { buf_push(b, &c, 1); }
static void buf_puts(Buf *b, const char *s) { buf_push(b, s, strlen(s)); }

static void push_utf8(Buf *b, uint32_t cp) {
    char t[4];
    buf_push(b, t, (size_t)sea_utf8_encode(cp, t));
}

static SeaValue new_str(const char *s, size_t n) { return sea_obj_val(&sea_string_new(s, n)->obj); }

static void val_release(SeaValue v) { if (sea_is_obj(v)) sea_obj_release(sea_as_obj(v)); }

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

static bool opt_flag(SeaValue opts, const char *field) {
    SeaValue v;
    return opt_get(opts, field, &v) && sea_is_bool(v) && sea_as_bool(v);
}

static bool opt_is(SeaValue opts, const char *field, const char *want) {
    SeaValue v;
    if (!opt_get(opts, field, &v)) return false;
    SeaString *s = sea_value_get_flat_string(v);
    if (!s) return false;
    bool eq = strcmp(s->data, want) == 0;
    sea_obj_release(&s->obj);
    return eq;
}

/* ======================================================================== */
/* Datetimes                                                                */
/* ======================================================================== */

enum { DT_NONE, DT_OFFSET, DT_LOCAL, DT_DATE, DT_TIME };
static const char *const DT_KIND[] = { "", "datetime", "datetime-local", "date-local", "time-local" };

static bool digits(const char *s, size_t n, size_t at, size_t count) {
    if (at + count > n) return false;
    for (size_t i = 0; i < count; i++) if (s[at + i] < '0' || s[at + i] > '9') return false;
    return true;
}

static int num2(const char *s) { return (s[0] - '0') * 10 + (s[1] - '0'); }

static bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static bool valid_date(const char *s) {
    int y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
    int m = num2(s + 5), d = num2(s + 8);
    static const int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m < 1 || m > 12 || d < 1) return false;
    int max = mdays[m - 1] + (m == 2 && leap(y));
    return d <= max;
}

/* A datetime at s[at..n): its kind and length (DT_NONE if not one). The
 * normalized text (T separator, uppercase Z) goes to out. err set when it
 * looks like a datetime but is malformed. */
static int scan_datetime(const char *s, size_t n, size_t at, size_t *len, Buf *out, const char **err) {
    size_t i = at;
    bool has_date = false, has_time = false;
    *err = NULL;
    if (digits(s, n, i, 4) && i + 4 < n && s[i + 4] == '-') {
        if (!(digits(s, n, i + 5, 2) && i + 7 < n && s[i + 7] == '-' && digits(s, n, i + 8, 2))) { *err = "invalid date"; return DT_NONE; }
        if (!valid_date(s + i)) { *err = "invalid date (no such day)"; return DT_NONE; }
        buf_push(out, s + i, 10);
        i += 10;
        has_date = true;
        /* a time after T, t or a space followed by HH: */
        if (i < n && (s[i] == 'T' || s[i] == 't' || (s[i] == ' ' && digits(s, n, i + 1, 2) && i + 3 < n && s[i + 3] == ':'))) {
            buf_putc(out, 'T');
            i++;
        } else {
            *len = i - at;
            return DT_DATE;
        }
    }
    if (!(digits(s, n, i, 2) && i + 2 < n && s[i + 2] == ':')) {
        if (has_date) { *err = "invalid time"; return DT_NONE; }
        return DT_NONE;
    }
    if (!(digits(s, n, i + 3, 2) && i + 5 < n && s[i + 5] == ':' && digits(s, n, i + 6, 2))) { *err = "invalid time (seconds are required)"; return DT_NONE; }
    int hh = num2(s + i), mm = num2(s + i + 3), ss = num2(s + i + 6);
    if (hh > 23 || mm > 59 || ss > 60) { *err = "invalid time"; return DT_NONE; }
    buf_push(out, s + i, 8);
    i += 8;
    has_time = true;
    if (i < n && s[i] == '.') {
        size_t f = i + 1;
        while (f < n && s[f] >= '0' && s[f] <= '9') f++;
        if (f == i + 1) { *err = "invalid time (digits expected after '.')"; return DT_NONE; }
        buf_push(out, s + i, f - i);
        i = f;
    }
    if (!has_date) { *len = i - at; return DT_TIME; }
    if (i < n && (s[i] == 'Z' || s[i] == 'z')) {
        buf_putc(out, 'Z');
        *len = i + 1 - at;
        return DT_OFFSET;
    }
    if (i < n && (s[i] == '+' || s[i] == '-')) {
        if (!(digits(s, n, i + 1, 2) && i + 3 < n && s[i + 3] == ':' && digits(s, n, i + 4, 2))) { *err = "invalid time offset"; return DT_NONE; }
        if (num2(s + i + 1) > 23 || num2(s + i + 4) > 59) { *err = "invalid time offset"; return DT_NONE; }
        buf_push(out, s + i, 6);
        *len = i + 6 - at;
        return DT_OFFSET;
    }
    (void)has_time;
    *len = i - at;
    return DT_LOCAL;
}

static SeaValue datetime_value(int kind, const char *text, size_t n) {
    SeaTable *t = sea_table_new(TABLE_MAP);
    SeaValue v = new_str(text, n);
    sea_table_set_str(t, "__toml_datetime", v);
    val_release(v);
    v = new_str(DT_KIND[kind], strlen(DT_KIND[kind]));
    sea_table_set_str(t, "kind", v);
    val_release(v);
    return sea_obj_val(&t->obj);
}

/* ======================================================================== */
/* Parser                                                                   */
/* ======================================================================== */

/* How a table or array came to be (TOML's redefinition rules). */
enum { F_EXPLICIT = 1, F_IMPLICIT = 2, F_DOTTED = 4, F_INLINE = 8, F_AOT = 16, F_STATIC = 32 };

typedef struct { SeaObject *o; int flags; } FlagSlot;

typedef struct {
    const char *s;
    size_t      n, i;
    bool        bigints, dt_values;
    const char *err;
    size_t      err_at;
    char        errbuf[200];
    FlagSlot   *fl;
    size_t      flcap, fln;
    int         depth;
} P;

static int fail(P *p, const char *msg, size_t at) {
    if (!p->err) { p->err = msg; p->err_at = at; }
    return 0;
}

static int failf(P *p, size_t at, const char *fmt, const char *key, size_t klen) {
    if (!p->err) {
        snprintf(p->errbuf, sizeof p->errbuf, fmt, (int)(klen > 60 ? 60 : klen), key);
        p->err = p->errbuf;
        p->err_at = at;
    }
    return 0;
}

static size_t fl_hash(SeaObject *o, size_t cap) { return ((uintptr_t)o >> 4) & (cap - 1); }

static void set_flags(P *p, SeaObject *o, int f) {
    if ((p->fln + 1) * 2 > p->flcap) {
        size_t nc = p->flcap ? p->flcap * 2 : 64;
        FlagSlot *ns = calloc(nc, sizeof *ns);
        for (size_t i = 0; i < p->flcap; i++) {
            if (!p->fl[i].o) continue;
            size_t h = fl_hash(p->fl[i].o, nc);
            while (ns[h].o) h = (h + 1) & (nc - 1);
            ns[h] = p->fl[i];
        }
        free(p->fl);
        p->fl = ns;
        p->flcap = nc;
    }
    size_t h = fl_hash(o, p->flcap);
    while (p->fl[h].o && p->fl[h].o != o) h = (h + 1) & (p->flcap - 1);
    if (!p->fl[h].o) { p->fl[h].o = o; p->fln++; }
    p->fl[h].flags = f;
}

static int get_flags(P *p, SeaObject *o) {
    if (!p->flcap) return 0;
    size_t h = fl_hash(o, p->flcap);
    while (p->fl[h].o) {
        if (p->fl[h].o == o) return p->fl[h].flags;
        h = (h + 1) & (p->flcap - 1);
    }
    return 0;
}

static int ch(P *p) { return p->i < p->n ? (unsigned char)p->s[p->i] : -1; }
static int ch_at(P *p, size_t k) { return p->i + k < p->n ? (unsigned char)p->s[p->i + k] : -1; }

static void skip_ws(P *p) {
    while (ch(p) == ' ' || ch(p) == '\t') p->i++;
}

static bool is_ctrl(int c) { return (c >= 0 && c < 0x20 && c != '\t') || c == 0x7f; }

/* A comment, if one starts here (control characters are not allowed in it). */
static int skip_comment(P *p) {
    if (ch(p) != '#') return 1;
    while (p->i < p->n && ch(p) != '\n') {
        int c = ch(p);
        if (c == '\r' && ch_at(p, 1) == '\n') break;
        if (is_ctrl(c)) return fail(p, "control character in a comment", p->i);
        p->i++;
    }
    return 1;
}

/* A line break: \n or \r\n. */
static bool newline(P *p) {
    if (ch(p) == '\n') { p->i++; return true; }
    if (ch(p) == '\r' && ch_at(p, 1) == '\n') { p->i += 2; return true; }
    return false;
}

/* Whitespace, comments and line breaks (inside arrays). */
static int skip_ws_nl(P *p) {
    for (;;) {
        skip_ws(p);
        if (!skip_comment(p)) return 0;
        if (!newline(p)) return 1;
    }
}

/* ---- strings ---------------------------------------------------------------------- */

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int escape(P *p, Buf *b) {
    size_t at = p->i;
    p->i++;                                       /* the backslash */
    int c = ch(p);
    p->i++;
    switch (c) {
        case 'b': buf_putc(b, '\b'); return 1;
        case 't': buf_putc(b, '\t'); return 1;
        case 'n': buf_putc(b, '\n'); return 1;
        case 'f': buf_putc(b, '\f'); return 1;
        case 'r': buf_putc(b, '\r'); return 1;
        case '"': buf_putc(b, '"'); return 1;
        case '\\': buf_putc(b, '\\'); return 1;
        case 'u': case 'U': {
            int k = c == 'u' ? 4 : 8;
            uint32_t v = 0;
            for (int j = 0; j < k; j++) {
                int h = hexval(ch(p));
                if (h < 0) return fail(p, "invalid unicode escape", at);
                v = v * 16 + (uint32_t)h;
                p->i++;
            }
            if ((v >= 0xD800 && v <= 0xDFFF) || v > 0x10FFFF) return fail(p, "invalid unicode escape (not a scalar value)", at);
            push_utf8(b, v);
            return 1;
        }
        default:
            return fail(p, "invalid escape in a string", at);
    }
}

/* A string of any of the four kinds at p->i (its opening quote). */
static int parse_string(P *p, Buf *b, bool allow_multiline) {
    size_t at = p->i;
    char q = p->s[p->i];
    bool basic = q == '"';
    bool ml = ch_at(p, 1) == q && ch_at(p, 2) == q;
    if (ml && !allow_multiline) return fail(p, "a multi-line string cannot be a key", at);
    if (ml) {
        p->i += 3;
        newline(p);                               /* a line break right after the opening is trimmed */
        for (;;) {
            if (p->i >= p->n) return fail(p, "unterminated multi-line string", at);
            int c = ch(p);
            if (c == q && ch_at(p, 1) == q && ch_at(p, 2) == q) {
                size_t k = 3;
                while (ch_at(p, k) == q) k++;
                if (k > 5) return fail(p, "too many quotes at the end of a multi-line string", p->i);
                for (size_t j = 3; j < k; j++) buf_putc(b, q);
                p->i += k;
                return 1;
            }
            if (basic && c == '\\') {
                /* a line-ending backslash trims the break and the whitespace after it */
                size_t j = p->i + 1;
                while (j < p->n && (p->s[j] == ' ' || p->s[j] == '\t')) j++;
                if (j < p->n && (p->s[j] == '\n' || (p->s[j] == '\r' && j + 1 < p->n && p->s[j + 1] == '\n'))) {
                    p->i = j;
                    for (;;) {
                        skip_ws(p);
                        if (!newline(p)) break;
                    }
                    continue;
                }
                if (!escape(p, b)) return 0;
                continue;
            }
            if (c == '\r' && ch_at(p, 1) == '\n') { buf_putc(b, '\n'); p->i += 2; continue; }
            if (c == '\n') { buf_putc(b, '\n'); p->i++; continue; }
            if (is_ctrl(c)) return fail(p, "control character in a string", p->i);
            buf_putc(b, (char)c);
            p->i++;
        }
    }
    p->i++;
    for (;;) {
        if (p->i >= p->n) return fail(p, "unterminated string", at);
        int c = ch(p);
        if (c == q) { p->i++; return 1; }
        if (c == '\n' || c == '\r') return fail(p, "line break in a single-line string", p->i);
        if (basic && c == '\\') { if (!escape(p, b)) return 0; continue; }
        if (is_ctrl(c)) return fail(p, "control character in a string", p->i);
        buf_putc(b, (char)c);
        p->i++;
    }
}

/* ---- keys ------------------------------------------------------------------------ */

typedef struct { SeaString *k[64]; size_t n; size_t at[64]; } Key;

static void key_free(Key *k) {
    for (size_t i = 0; i < k->n; i++) sea_obj_release(&k->k[i]->obj);
    k->n = 0;
}

static bool bare_char(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static int parse_key(P *p, Key *key) {
    key->n = 0;
    for (;;) {
        skip_ws(p);
        if (key->n >= 64) return fail(p, "key has too many parts", p->i);
        size_t at = p->i;
        int c = ch(p);
        Buf b = { 0 };
        if (c == '"' || c == '\'') {
            if (!parse_string(p, &b, false)) { free(b.d); return 0; }
        } else if (bare_char(c)) {
            while (bare_char(ch(p))) p->i++;
            buf_push(&b, p->s + at, p->i - at);
        } else {
            free(b.d);
            return fail(p, c < 0 || c == '\n' || c == '\r' ? "expected a key" : "invalid character in a key", at);
        }
        key->k[key->n] = sea_string_new(b.d ? b.d : "", b.len);
        key->at[key->n] = at;
        key->n++;
        free(b.d);
        skip_ws(p);
        if (ch(p) != '.') return 1;
        p->i++;
    }
}

/* ---- values ------------------------------------------------------------------------- */

static int parse_value(P *p, SeaValue *out);

static bool value_end(int c) {
    return c < 0 || c == ' ' || c == '\t' || c == ',' || c == ']' || c == '}' || c == '#' || c == '\n' || c == '\r';
}

/* digit groups with single underscores between digits; base 10/16/8/2 */
static bool int_body(const char *s, size_t n, int base) {
    if (n == 0) return false;
    bool prev_digit = false;
    for (size_t i = 0; i < n; i++) {
        int c = (unsigned char)s[i];
        if (c == '_') {
            if (!prev_digit) return false;
            prev_digit = false;
            continue;
        }
        int d = hexval(c);
        if (d < 0 || d >= base) return false;
        prev_digit = true;
    }
    return prev_digit;
}

static int parse_number(P *p, SeaValue *out) {
    size_t at = p->i;
    size_t e = p->i;
    while (e < p->n && (bare_char((unsigned char)p->s[e]) || p->s[e] == '+' || p->s[e] == '.')) e++;
    const char *s = p->s + at;
    size_t n = e - at;
    p->i = e;
    /* inf / nan */
    size_t sgn = (n && (s[0] == '+' || s[0] == '-')) ? 1 : 0;
    if (n - sgn == 3 && (memcmp(s + sgn, "inf", 3) == 0 || memcmp(s + sgn, "nan", 3) == 0)) {
        if (s[sgn] == 'i') *out = sea_number_val(s[0] == '-' ? -INFINITY : INFINITY);
        else *out = sea_number_val(NAN);
        return 1;
    }
    /* hex, octal, binary */
    if (n > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'o' || s[1] == 'b')) {
        int base = s[1] == 'x' ? 16 : s[1] == 'o' ? 8 : 2;
        if (!int_body(s + 2, n - 2, base)) return fail(p, "invalid integer", at);
        uint64_t v = 0;
        for (size_t i = 2; i < n; i++) {
            if (s[i] == '_') continue;
            uint64_t d = (uint64_t)hexval((unsigned char)s[i]);
            if (v > (UINT64_MAX - d) / (uint64_t)base) return fail(p, "integer out of range (64 bits)", at);
            v = v * (uint64_t)base + d;
        }
        if (v > (uint64_t)INT64_MAX) return fail(p, "integer out of range (64 bits)", at);
        int64_t x = (int64_t)v;
        if (x < (INT64_C(1) << 47)) *out = sea_int_val(x);
        else if (p->bigints) *out = sea_obj_val(&sea_bigint_from_i64(x)->obj);
        else *out = sea_number_val((double)x);
        return 1;
    }
    /* decimal integer or float: split at '.' / 'e' */
    size_t i = sgn;
    size_t int_start = i;
    while (i < n && s[i] != '.' && s[i] != 'e' && s[i] != 'E') i++;
    size_t int_len = i - int_start;
    if (!int_body(s + int_start, int_len, 10)) return fail(p, "invalid number", at);
    if (int_len > 1 && s[int_start] == '0') return fail(p, "leading zeros are not allowed", at);
    bool is_float = false;
    if (i < n && s[i] == '.') {
        is_float = true;
        size_t fs = ++i;
        while (i < n && s[i] != 'e' && s[i] != 'E') i++;
        if (!int_body(s + fs, i - fs, 10)) return fail(p, "invalid float (digits expected after '.')", at);
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        is_float = true;
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-')) i++;
        if (!int_body(s + i, n - i, 10)) return fail(p, "invalid float exponent", at);
        i = n;
    }
    if (i != n) return fail(p, "invalid number", at);
    char tmp[128];
    size_t k = 0;
    for (size_t j = 0; j < n && k < sizeof tmp - 1; j++) if (s[j] != '_') tmp[k++] = s[j];
    tmp[k] = '\0';
    if (k >= sizeof tmp - 1) return fail(p, "number too long", at);
    if (is_float) {
        *out = sea_number_val(strtod(tmp, NULL));
        return 1;
    }
    /* int64 range */
    bool neg = tmp[0] == '-';
    const char *d = tmp + ((tmp[0] == '-' || tmp[0] == '+') ? 1 : 0);
    uint64_t v = 0;
    for (; *d; d++) {
        uint64_t x = (uint64_t)(*d - '0');
        if (v > (UINT64_MAX - x) / 10) return fail(p, "integer out of range (64 bits)", at);
        v = v * 10 + x;
    }
    if ((!neg && v > (uint64_t)INT64_MAX) || (neg && v > (uint64_t)INT64_MAX + 1)) return fail(p, "integer out of range (64 bits)", at);
    int64_t x = neg ? (int64_t)(0 - v) : (int64_t)v;
    if (x >= -(INT64_C(1) << 47) && x < (INT64_C(1) << 47)) *out = sea_int_val(x);
    else if (p->bigints) *out = sea_obj_val(&sea_bigint_from_i64(x)->obj);
    else *out = sea_number_val((double)x);
    return 1;
}

static int insert_dotted(P *p, SeaTable *t, Key *key, SeaValue v, bool inline_ctx);

static int parse_array(P *p, SeaValue *out) {
    size_t at = p->i++;
    if (++p->depth > MAX_DEPTH) return fail(p, "nesting too deep", at);
    SeaArray *a = sea_array_new(4);
    set_flags(p, &a->obj, F_STATIC);
    for (;;) {
        if (!skip_ws_nl(p)) break;
        if (ch(p) == ']') {
            p->i++;
            p->depth--;
            *out = sea_obj_val(&a->obj);
            return 1;
        }
        SeaValue v;
        if (!parse_value(p, &v)) break;
        sea_array_push(a, v);
        val_release(v);
        if (!skip_ws_nl(p)) break;
        if (ch(p) == ',') { p->i++; continue; }
        if (ch(p) == ']') continue;
        fail(p, p->i >= p->n ? "unterminated array" : "expected ',' or ']' in an array", p->i);
        break;
    }
    sea_obj_release(&a->obj);
    return 0;
}

static int parse_inline_table(P *p, SeaValue *out) {
    size_t at = p->i++;
    if (++p->depth > MAX_DEPTH) return fail(p, "nesting too deep", at);
    SeaTable *t = sea_table_new(TABLE_MAP);
    set_flags(p, &t->obj, F_INLINE);
    skip_ws(p);
    if (ch(p) == '}') {
        p->i++;
        p->depth--;
        *out = sea_obj_val(&t->obj);
        return 1;
    }
    for (;;) {
        Key key;
        if (!parse_key(p, &key)) break;
        if (ch(p) != '=') { key_free(&key); fail(p, "expected '=' after a key", p->i); break; }
        p->i++;
        skip_ws(p);
        SeaValue v;
        if (!parse_value(p, &v)) { key_free(&key); break; }
        int ok = insert_dotted(p, t, &key, v, true);
        key_free(&key);
        if (!ok) break;
        skip_ws(p);
        if (ch(p) == ',') {
            p->i++;
            skip_ws(p);
            if (ch(p) == '}') { fail(p, "trailing comma in an inline table", p->i); break; }
            continue;
        }
        if (ch(p) == '}') {
            p->i++;
            p->depth--;
            *out = sea_obj_val(&t->obj);
            return 1;
        }
        fail(p, (ch(p) == '\n' || ch(p) == '\r') ? "line break in an inline table" : "expected ',' or '}' in an inline table", p->i);
        break;
    }
    sea_obj_release(&t->obj);
    return 0;
}

static int parse_value(P *p, SeaValue *out) {
    int c = ch(p);
    size_t at = p->i;
    if (c == '"' || c == '\'') {
        Buf b = { 0 };
        if (!parse_string(p, &b, true)) { free(b.d); return 0; }
        *out = new_str(b.d ? b.d : "", b.len);
        free(b.d);
        return 1;
    }
    if (c == '[') return parse_array(p, out);
    if (c == '{') return parse_inline_table(p, out);
    if (c == 't' && p->n - p->i >= 4 && memcmp(p->s + p->i, "true", 4) == 0 && value_end(ch_at(p, 4))) {
        p->i += 4;
        *out = SEA_TRUE_VAL;
        return 1;
    }
    if (c == 'f' && p->n - p->i >= 5 && memcmp(p->s + p->i, "false", 5) == 0 && value_end(ch_at(p, 5))) {
        p->i += 5;
        *out = SEA_FALSE_VAL;
        return 1;
    }
    if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == 'i' || c == 'n') {
        if (c >= '0' && c <= '9') {
            Buf b = { 0 };
            size_t len;
            const char *err;
            int kind = scan_datetime(p->s, p->n, p->i, &len, &b, &err);
            if (err) { free(b.d); return fail(p, err, at); }
            if (kind != DT_NONE) {
                p->i += len;
                if (!value_end(ch(p))) { free(b.d); return fail(p, "invalid date or time", at); }
                *out = p->dt_values ? datetime_value(kind, b.d, b.len) : new_str(b.d, b.len);
                free(b.d);
                return 1;
            }
            free(b.d);
        }
        if (!parse_number(p, out)) return 0;
        if (!value_end(ch(p))) return fail(p, "invalid value", at);
        return 1;
    }
    if (c < 0 || c == '\n' || c == '\r') return fail(p, "expected a value", at);
    return fail(p, "invalid value", at);
}

/* ---- inserting keys and tables ---------------------------------------------------- */

static bool table_get(SeaTable *t, SeaString *k, SeaValue *out) {
    return sea_table_get(t, sea_obj_val(&k->obj), out);
}

static void table_put(SeaTable *t, SeaString *k, SeaValue v) {
    sea_table_set(t, sea_obj_val(&k->obj), v);
}

/* a.b.c = v into t: a and b are tables made (or extended) by dotted keys. */
static int insert_dotted(P *p, SeaTable *t, Key *key, SeaValue v, bool inline_ctx) {
    for (size_t j = 0; j + 1 < key->n; j++) {
        SeaValue cur;
        SeaString *k = key->k[j];
        if (!table_get(t, k, &cur)) {
            SeaTable *nt = sea_table_new(TABLE_MAP);
            set_flags(p, &nt->obj, F_DOTTED | (inline_ctx ? F_INLINE : 0));
            table_put(t, k, sea_obj_val(&nt->obj));
            sea_obj_release(&nt->obj);
            t = nt;
            continue;
        }
        if (!sea_is_table(cur)) {
            val_release(v);
            return failf(p, key->at[j], "key '%.*s' is already defined (not a table)", k->data, k->len);
        }
        int f = get_flags(p, sea_as_obj(cur));
        if (!(f & F_DOTTED) || ((f & F_INLINE) && !inline_ctx)) {
            val_release(v);
            return failf(p, key->at[j], "cannot add keys to table '%.*s' with dotted keys: it is defined elsewhere", k->data, k->len);
        }
        t = SEA_AS_TABLE(cur);
    }
    SeaString *last = key->k[key->n - 1];
    SeaValue old;
    if (table_get(t, last, &old)) {
        val_release(v);
        return failf(p, key->at[key->n - 1], "duplicate key '%.*s'", last->data, last->len);
    }
    table_put(t, last, v);
    val_release(v);
    return 1;
}

/* Walk a header's path (all but its last key) from root. */
static SeaTable *header_parent(P *p, SeaTable *root, Key *key) {
    SeaTable *t = root;
    for (size_t j = 0; j + 1 < key->n; j++) {
        SeaValue cur;
        SeaString *k = key->k[j];
        if (!table_get(t, k, &cur)) {
            SeaTable *nt = sea_table_new(TABLE_MAP);
            set_flags(p, &nt->obj, F_IMPLICIT);
            table_put(t, k, sea_obj_val(&nt->obj));
            sea_obj_release(&nt->obj);
            t = nt;
            continue;
        }
        if (sea_is_table(cur)) {
            if (get_flags(p, sea_as_obj(cur)) & F_INLINE) {
                failf(p, key->at[j], "cannot extend inline table '%.*s'", k->data, k->len);
                return NULL;
            }
            t = SEA_AS_TABLE(cur);
            continue;
        }
        if (sea_is_array(cur) && (get_flags(p, sea_as_obj(cur)) & F_AOT)) {
            SeaArray *a = SEA_AS_ARRAY(cur);
            t = SEA_AS_TABLE(sea_array_get(a, (int64_t)sea_array_len(a) - 1));
            continue;
        }
        failf(p, key->at[j], "key '%.*s' is already defined (not a table)", k->data, k->len);
        return NULL;
    }
    return t;
}

static SeaTable *header(P *p, SeaTable *root, Key *key, bool aot) {
    SeaTable *t = header_parent(p, root, key);
    if (!t) return NULL;
    SeaString *k = key->k[key->n - 1];
    size_t at = key->at[key->n - 1];
    SeaValue cur;
    bool exists = table_get(t, k, &cur);
    if (aot) {
        SeaArray *a;
        if (!exists) {
            a = sea_array_new(4);
            set_flags(p, &a->obj, F_AOT);
            table_put(t, k, sea_obj_val(&a->obj));
            sea_obj_release(&a->obj);
        } else if (sea_is_array(cur) && (get_flags(p, sea_as_obj(cur)) & F_AOT)) {
            a = SEA_AS_ARRAY(cur);
        } else {
            failf(p, at, sea_is_array(cur) ? "cannot append to static array '%.*s' with [[ ]]" :
                         "key '%.*s' is already defined (not an array of tables)", k->data, k->len);
            return NULL;
        }
        SeaTable *nt = sea_table_new(TABLE_MAP);
        set_flags(p, &nt->obj, F_EXPLICIT);
        sea_array_push(a, sea_obj_val(&nt->obj));
        sea_obj_release(&nt->obj);
        return nt;
    }
    if (!exists) {
        SeaTable *nt = sea_table_new(TABLE_MAP);
        set_flags(p, &nt->obj, F_EXPLICIT);
        table_put(t, k, sea_obj_val(&nt->obj));
        sea_obj_release(&nt->obj);
        return nt;
    }
    if (!sea_is_table(cur)) {
        failf(p, at, sea_is_array(cur) ? "table '%.*s' is already an array" : "key '%.*s' is already defined (not a table)", k->data, k->len);
        return NULL;
    }
    int f = get_flags(p, sea_as_obj(cur));
    if (f & F_INLINE) { failf(p, at, "cannot redefine inline table '%.*s'", k->data, k->len); return NULL; }
    if (f & F_EXPLICIT) { failf(p, at, "table '%.*s' defined twice", k->data, k->len); return NULL; }
    if (f & F_DOTTED) { failf(p, at, "table '%.*s' is already defined by dotted keys", k->data, k->len); return NULL; }
    set_flags(p, sea_as_obj(cur), F_EXPLICIT);
    return SEA_AS_TABLE(cur);
}

static int parse_document(P *p, SeaTable *root) {
    SeaTable *current = root;
    if (p->n >= 3 && memcmp(p->s, "\xEF\xBB\xBF", 3) == 0) p->i = 3;
    while (p->i < p->n) {
        skip_ws(p);
        if (!skip_comment(p)) return 0;
        if (newline(p)) continue;
        if (p->i >= p->n) break;
        int c = ch(p);
        if (c == '[') {
            size_t at = p->i;
            bool aot = ch_at(p, 1) == '[';
            p->i += aot ? 2 : 1;
            Key key;
            if (!parse_key(p, &key)) return 0;
            if (ch(p) != ']' || (aot && ch_at(p, 1) != ']')) {
                key_free(&key);
                return fail(p, aot ? "expected ']]' to end the array-of-tables header" : "expected ']' to end the table header", p->i);
            }
            p->i += aot ? 2 : 1;
            current = header(p, root, &key, aot);
            key_free(&key);
            if (!current) return 0;
            (void)at;
        } else if (c == '\r') {
            return fail(p, "a carriage return must be followed by a line feed", p->i);
        } else {
            Key key;
            if (!parse_key(p, &key)) return 0;
            if (ch(p) != '=') { key_free(&key); return fail(p, "expected '=' after a key", p->i); }
            p->i++;
            skip_ws(p);
            SeaValue v;
            if (!parse_value(p, &v)) { key_free(&key); return 0; }
            int ok = insert_dotted(p, current, &key, v, false);
            key_free(&key);
            if (!ok) return 0;
        }
        skip_ws(p);
        if (!skip_comment(p)) return 0;
        if (p->i < p->n && !newline(p)) return fail(p, "expected a line break after the value", p->i);
    }
    return 1;
}

static SeaValue decode_impl(SeaVM *vm, const char *fn, SeaString *s, SeaValue opts, bool keep) {
    size_t bad = sea_utf8_check(s->data, s->len);
    if (bad < s->len) {
        char msg[120];
        snprintf(msg, sizeof msg, "%s: invalid UTF-8 at byte %zu", fn, bad + 1);
        return sea_make_error_str(vm, msg);
    }
    P p;
    memset(&p, 0, sizeof p);
    p.s = s->data;
    p.n = s->len;
    p.bigints = opt_flag(opts, "bigint");
    p.dt_values = opt_is(opts, "datetimes", "values");
    SeaTable *root = sea_table_new(TABLE_MAP);
    set_flags(&p, &root->obj, F_EXPLICIT);
    int ok = parse_document(&p, root);
    free(p.fl);
    if (!ok) {
        sea_obj_release(&root->obj);
        size_t line = 1, col = 1;
        for (size_t i = 0; i < p.err_at && i < p.n; i++) {
            if (p.s[i] == '\n') { line++; col = 1; }
            else if (((unsigned char)p.s[i] & 0xC0) != 0x80) col++;
        }
        char msg[320];
        snprintf(msg, sizeof msg, "%s: %s at line %zu, column %zu", fn, p.err, line, col);
        return sea_make_error_str(vm, msg);
    }
    if (!keep) { sea_obj_release(&root->obj); return SEA_TRUE_VAL; }
    return sea_obj_val(&root->obj);
}

static SeaValue fn_decode(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *s = argc > 0 ? sea_value_get_flat_string(args[0]) : NULL;
    if (!s) return sea_make_error_str(vm, "toml.decode: string expected");
    SeaValue r = decode_impl(vm, "toml.decode", s, argc > 1 ? args[1] : SEA_NIL_VAL, true);
    sea_obj_release(&s->obj);
    return r;
}

static SeaValue fn_valid(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *s = argc > 0 ? sea_value_get_flat_string(args[0]) : NULL;
    if (!s) return SEA_FALSE_VAL;
    SeaValue r = decode_impl(vm, "toml.valid", s, SEA_NIL_VAL, false);
    sea_obj_release(&s->obj);
    bool ok = r == SEA_TRUE_VAL;
    val_release(r);
    return sea_bool_val(ok);
}

/* toml.datetime(s): a datetime value from its text */
static SeaValue fn_datetime(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *s = argc > 0 ? sea_value_get_flat_string(args[0]) : NULL;
    if (!s) return sea_make_error_str(vm, "toml.datetime: string expected");
    Buf b = { 0 };
    size_t len = 0;
    const char *err = NULL;
    int kind = scan_datetime(s->data, s->len, 0, &len, &b, &err);
    SeaValue r;
    if (kind == DT_NONE || len != s->len) {
        char msg[160];
        snprintf(msg, sizeof msg, "toml.datetime: not a TOML date or time: \"%.60s\"%s%s", s->data, err ? " — " : "", err ? err : "");
        r = sea_make_error_str(vm, msg);
    } else {
        r = datetime_value(kind, b.d, b.len);
    }
    free(b.d);
    sea_obj_release(&s->obj);
    return r;
}

/* ======================================================================== */
/* Encoder                                                                  */
/* ======================================================================== */

typedef struct {
    Buf   out;
    bool  sort_keys;
    int   depth;
    char  err[200];
} Enc;

static bool is_record(SeaValue v) { return sea_is_table(v) || sea_is_map(v) || sea_is_struct(v); }

static bool record_next(SeaValue v, uint32_t *it, SeaValue *k, SeaValue *x) {
    if (sea_is_table(v)) return sea_table_next(SEA_AS_TABLE(v), it, k, x);
    if (sea_is_map(v)) return sea_map_next(SEA_AS_MAP(v), it, k, x);
    return sea_struct_next(SEA_AS_STRUCT(v), it, k, x);
}

static SeaValue record_field(SeaValue v, const char *name) {
    SeaValue x = SEA_NIL_VAL;
    if (sea_is_table(v)) sea_table_get_str(SEA_AS_TABLE(v), name, &x);
    else if (sea_is_struct(v)) {
        SeaString *k = sea_string_find_intern(name, strlen(name));
        if (k) { x = sea_struct_get(SEA_AS_STRUCT(v), k); sea_obj_release(&k->obj); }
    }
    return x;
}

/* A datetime value from toml.datetime / datetimes = "values". */
static SeaString *datetime_text(SeaValue v) {
    if (!sea_is_table(v)) return NULL;
    SeaValue t = record_field(v, "__toml_datetime");
    return sea_is_string(t) ? SEA_AS_STRING(t) : NULL;
}

/* A table whose keys are 0..n-1 is a list. */
static bool is_seq_table(SeaValue v) {
    if (!sea_is_table(v)) return false;
    uint32_t count = sea_table_count(SEA_AS_TABLE(v)), it = 0;
    if (!count) return false;
    SeaValue k, x;
    while (sea_table_next(SEA_AS_TABLE(v), &it, &k, &x))
        if (!sea_is_int(k) || sea_as_int(k) < 0 || (uint64_t)sea_as_int(k) >= count) return false;
    return true;
}

static bool is_list(SeaValue v) {
    return sea_is_array(v) || sea_is_int_array(v) || sea_is_float_array(v) || is_seq_table(v);
}

static uint32_t list_len(SeaValue v) {
    if (sea_is_array(v)) return sea_array_len(SEA_AS_ARRAY(v));
    if (sea_is_int_array(v)) return SEA_AS_INT_ARRAY(v)->len;
    if (sea_is_float_array(v)) return SEA_AS_FLOAT_ARRAY(v)->len;
    return sea_table_count(SEA_AS_TABLE(v));
}

static SeaValue list_get(SeaValue v, uint32_t i) {
    if (sea_is_array(v)) return sea_array_get(SEA_AS_ARRAY(v), (int64_t)i);
    if (sea_is_int_array(v)) return sea_int_val(SEA_AS_INT_ARRAY(v)->data[i]);
    if (sea_is_float_array(v)) return sea_number_val(SEA_AS_FLOAT_ARRAY(v)->data[i]);
    SeaValue x;
    if (!sea_table_get(SEA_AS_TABLE(v), sea_int_val((int64_t)i), &x)) x = SEA_NIL_VAL;
    return x;
}

/* A table section ([a]) holds records; an array of tables ([[a]]) is a
 * non-empty list of records only. */
static bool is_table_value(SeaValue v) { return is_record(v) && !datetime_text(v) && !is_seq_table(v); }

static bool is_aot(SeaValue v) {
    if (!is_list(v) || list_len(v) == 0) return false;
    for (uint32_t i = 0; i < list_len(v); i++)
        if (!is_table_value(list_get(v, i))) return false;
    return true;
}

static void enc_string(Enc *e, const char *s, size_t n) {
    Buf *b = &e->out;
    buf_putc(b, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"': buf_puts(b, "\\\""); break;
            case '\\': buf_puts(b, "\\\\"); break;
            case '\n': buf_puts(b, "\\n"); break;
            case '\t': buf_puts(b, "\\t"); break;
            case '\r': buf_puts(b, "\\r"); break;
            case '\b': buf_puts(b, "\\b"); break;
            case '\f': buf_puts(b, "\\f"); break;
            default:
                if (c < 0x20 || c == 0x7f) {
                    char t[8];
                    snprintf(t, sizeof t, "\\u%04X", c);
                    buf_puts(b, t);
                } else {
                    buf_putc(b, (char)c);
                }
        }
    }
    buf_putc(b, '"');
}

static bool bare_key(const char *s, size_t n) {
    if (n == 0) return false;
    for (size_t i = 0; i < n; i++) if (!bare_char((unsigned char)s[i])) return false;
    return true;
}

static void enc_key(Enc *e, SeaString *k) {
    if (bare_key(k->data, k->len)) buf_push(&e->out, k->data, k->len);
    else enc_string(e, k->data, k->len);
}

/* A key as a string (ints, numbers and bools become their text); NULL for
 * other keys, which are skipped. +1 */
static SeaString *key_string(SeaValue k) {
    char t[48];
    if (sea_is_string(k)) { sea_obj_retain(sea_as_obj(k)); return SEA_AS_STRING(k); }
    if (sea_is_rope(k)) return sea_rope_flatten(SEA_AS_ROPE(k));
    if (sea_is_int(k)) { int n = snprintf(t, sizeof t, "%lld", (long long)sea_as_int(k)); return sea_string_new(t, (size_t)n); }
    if (sea_is_bool(k)) return sea_string_new(sea_as_bool(k) ? "true" : "false", sea_as_bool(k) ? 4 : 5);
    return NULL;
}

static int fmt_double(char *out, double d) {
    if (isnan(d)) return snprintf(out, 8, "nan");
    if (isinf(d)) return snprintf(out, 8, d < 0 ? "-inf" : "inf");
    int n = snprintf(out, 32, "%.15g", d);
    if (strtod(out, NULL) != d) {
        n = snprintf(out, 32, "%.16g", d);
        if (strtod(out, NULL) != d) n = snprintf(out, 32, "%.17g", d);
    }
    if (!strpbrk(out, ".eEn")) { out[n++] = '.'; out[n++] = '0'; out[n] = '\0'; }
    return n;
}

static int enc_inline(Enc *e, SeaValue v);

/* Members of a record, as (key string, value) sorted if asked. */
typedef struct { SeaString *k; SeaValue v; } Member;

static int member_cmp(const void *a, const void *b) {
    const SeaString *x = ((const Member *)a)->k, *y = ((const Member *)b)->k;
    size_t n = x->len < y->len ? x->len : y->len;
    int c = memcmp(x->data, y->data, n);
    return c ? c : (x->len > y->len) - (x->len < y->len);
}

static Member *members(Enc *e, SeaValue v, size_t *count) {
    size_t cap = 16, n = 0;
    Member *m = malloc(cap * sizeof *m);
    uint32_t it = 0;
    SeaValue k, x;
    while (record_next(v, &it, &k, &x)) {
        if (sea_is_nil(x)) continue;                    /* TOML has no null: left out */
        SeaString *ks = key_string(k);
        if (!ks) continue;
        if (n == cap) { cap *= 2; m = realloc(m, cap * sizeof *m); }
        m[n].k = ks;
        m[n].v = x;
        n++;
    }
    if (e->sort_keys && n > 1) qsort(m, n, sizeof *m, member_cmp);
    *count = n;
    return m;
}

static void members_free(Member *m, size_t n) {
    for (size_t i = 0; i < n; i++) sea_obj_release(&m[i].k->obj);
    free(m);
}

static int enc_scalar(Enc *e, SeaValue v) {
    char t[48];
    if (sea_is_bool(v)) { buf_puts(&e->out, sea_as_bool(v) ? "true" : "false"); return 1; }
    if (sea_is_int(v)) { snprintf(t, sizeof t, "%lld", (long long)sea_as_int(v)); buf_puts(&e->out, t); return 1; }
    if (sea_is_number(v)) { fmt_double(t, sea_as_number(v)); buf_puts(&e->out, t); return 1; }
    if (sea_is_bigint(v)) {
        int64_t x;
        if (!sea_bigint_to_i64(SEA_AS_BIGINT(v), &x)) { snprintf(e->err, sizeof e->err, "integer out of TOML's 64-bit range"); return 0; }
        snprintf(t, sizeof t, "%lld", (long long)x);
        buf_puts(&e->out, t);
        return 1;
    }
    if (sea_is_string(v) || sea_is_rope(v)) {
        SeaString *s = sea_value_get_flat_string(v);
        enc_string(e, s->data, s->len);
        sea_obj_release(&s->obj);
        return 1;
    }
    SeaString *dt = datetime_text(v);
    if (dt) { buf_push(&e->out, dt->data, dt->len); return 1; }
    if (sea_is_nil(v)) { snprintf(e->err, sizeof e->err, "TOML has no null (a nil in an array)"); return 0; }
    snprintf(e->err, sizeof e->err, "cannot encode a value of type %s", sea_type_name(v));
    return 0;
}

static int enc_inline(Enc *e, SeaValue v) {
    if (++e->depth > MAX_DEPTH) { snprintf(e->err, sizeof e->err, "nesting too deep (or a cycle)"); return 0; }
    int ok = 1;
    if (is_list(v)) {
        buf_putc(&e->out, '[');
        for (uint32_t i = 0; i < list_len(v) && ok; i++) {
            if (i) buf_puts(&e->out, ", ");
            ok = enc_inline(e, list_get(v, i));
        }
        buf_putc(&e->out, ']');
    } else if (is_table_value(v)) {
        size_t n;
        Member *m = members(e, v, &n);
        buf_putc(&e->out, '{');
        for (size_t i = 0; i < n && ok; i++) {
            buf_puts(&e->out, i ? ", " : " ");
            enc_key(e, m[i].k);
            buf_puts(&e->out, " = ");
            ok = enc_inline(e, m[i].v);
        }
        buf_puts(&e->out, n ? " }" : "}");
        members_free(m, n);
    } else {
        ok = enc_scalar(e, v);
    }
    e->depth--;
    return ok;
}

static void enc_path(Enc *e, SeaString **path, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (i) buf_putc(&e->out, '.');
        enc_key(e, path[i]);
    }
}

/* A table's body: its plain keys, then its sub-tables and arrays of tables. */
static int enc_table(Enc *e, SeaValue v, SeaString **path, size_t plen, bool header, bool aot) {
    if (++e->depth > MAX_DEPTH || plen >= MAX_DEPTH) { snprintf(e->err, sizeof e->err, "nesting too deep (or a cycle)"); return 0; }
    size_t n;
    Member *m = members(e, v, &n);
    int ok = 1;
    if (header) {
        if (e->out.len) buf_putc(&e->out, '\n');
        buf_puts(&e->out, aot ? "[[" : "[");
        enc_path(e, path, plen);
        buf_puts(&e->out, aot ? "]]\n" : "]\n");
    }
    for (size_t i = 0; i < n && ok; i++) {
        if (is_table_value(m[i].v) || is_aot(m[i].v)) continue;
        enc_key(e, m[i].k);
        buf_puts(&e->out, " = ");
        ok = enc_inline(e, m[i].v);
        buf_putc(&e->out, '\n');
    }
    for (size_t i = 0; i < n && ok; i++) {
        path[plen] = m[i].k;
        if (is_table_value(m[i].v)) {
            ok = enc_table(e, m[i].v, path, plen + 1, true, false);
        } else if (is_aot(m[i].v)) {
            for (uint32_t j = 0; j < list_len(m[i].v) && ok; j++)
                ok = enc_table(e, list_get(m[i].v, j), path, plen + 1, true, true);
        }
    }
    members_free(m, n);
    e->depth--;
    return ok;
}

/* toml.encode(t [, opts]): opts.sort_keys */
static SeaValue fn_encode(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1 || !is_table_value(args[0]))
        return sea_make_error_str(vm, "toml.encode: a table expected (a TOML document is a table)");
    Enc e;
    memset(&e, 0, sizeof e);
    e.sort_keys = opt_flag(argc > 1 ? args[1] : SEA_NIL_VAL, "sort_keys");
    SeaString **path = calloc(MAX_DEPTH + 1, sizeof *path);
    int ok = enc_table(&e, args[0], path, 0, false, false);
    free(path);
    SeaValue r;
    if (!ok) {
        char msg[260];
        snprintf(msg, sizeof msg, "toml.encode: %s", e.err);
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
    sea_mod_set(mod, "decode",   fn_decode);
    sea_mod_set(mod, "encode",   fn_encode);
    sea_mod_set(mod, "valid",    fn_valid);
    sea_mod_set(mod, "datetime", fn_datetime);
    return sea_obj_val(&mod->obj);
}
