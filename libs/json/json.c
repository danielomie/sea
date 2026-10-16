/*
 * libs/json/json.c — JSON (RFC 8259) encode/decode for Sea.
 *
 * Build:  make -C libs/json
 * Usage:  local json = require("../libs/json/json")   -- relative to your file
 *
 *   json.encode(value [, opts]) → string | error
 *       opts: indent (spaces, or a string such as "\t"), sort_keys (bool),
 *             ascii (bool: escape non-ASCII as \uXXXX), max_depth (512)
 *   json.decode(string [, opts]) → value | error
 *       opts: max_depth (512), bigint (bool: integers past ±2^47 as exact bigints)
 *   json.valid(string) → bool
 *
 * Failures are error values (is_error(e), e.message), e.g.
 * "json.decode: invalid number at line 3, column 14", so `json.decode(s)?`
 * propagates them and a JSON null (→ nil) is never mistaken for a failure.
 */

#include "sea.h"
#include "sea_rope.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 512

/* ---- Dynamic write buffer ------------------------------------------------- */

typedef struct { char *data; size_t len, cap; int oom; } Buf;

static void buf_free(Buf *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

static int buf_grow(Buf *b, size_t need) {
    size_t nc = b->cap ? b->cap : 256;
    while (nc < b->len + need + 1) nc *= 2;
    char *p = realloc(b->data, nc);
    if (!p) { b->oom = 1; return 0; }
    b->data = p;
    b->cap = nc;
    return 1;
}

static inline int buf_reserve(Buf *b, size_t n) {
    if (b->len + n + 1 <= b->cap) return 1;
    return buf_grow(b, n);
}

static inline void buf_push(Buf *b, const char *s, size_t n) {
    if (!buf_reserve(b, n)) return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
}

static inline void buf_putc(Buf *b, char c) {
    if (!buf_reserve(b, 1)) return;
    b->data[b->len++] = c;
}

/* ---- Options (table, map or struct) --------------------------------------- */

static bool opt_get(SeaValue opts, const char *field, SeaValue *out) {
    *out = SEA_NIL_VAL;
    if (sea_is_table(opts)) return sea_table_get_str(SEA_AS_TABLE(opts), field, out) && !sea_is_nil(*out);
    if (sea_is_map(opts)) {
        SeaString *k = sea_string_intern(field, strlen(field));
        bool found = sea_map_get(SEA_AS_MAP(opts), sea_obj_val(&k->obj), out);
        sea_obj_release(&k->obj);
        return found && !sea_is_nil(*out);
    }
    if (sea_is_struct(opts)) {
        SeaString *k = sea_string_intern(field, strlen(field));
        *out = sea_struct_get(SEA_AS_STRUCT(opts), k);
        sea_obj_release(&k->obj);
        return !sea_is_nil(*out);
    }
    return false;
}

static bool opt_bool(SeaValue opts, const char *field) {
    SeaValue v;
    return opt_get(opts, field, &v) && sea_is_bool(v) && sea_as_bool(v);
}

static int opt_int(SeaValue opts, const char *field, int def) {
    SeaValue v;
    if (!opt_get(opts, field, &v)) return def;
    if (sea_is_int(v)) return (int)sea_as_int(v);
    if (sea_is_number(v)) return (int)sea_as_number(v);
    return def;
}

/* ---- Number formatting ---------------------------------------------------- */

static int fmt_int(char *out, int64_t v) {
    char tmp[24];
    int n = 0;
    uint64_t u = v < 0 ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
    do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    int o = 0;
    if (v < 0) out[o++] = '-';
    while (n) out[o++] = tmp[--n];
    out[o] = '\0';
    return o;
}

/* The shortest of %.15g / %.16g / %.17g that reads back as d; a value that
 * prints like an integer gets ".0" so it decodes back as a float. */
static int fmt_double(char *out, double d) {
    int n = snprintf(out, 32, "%.15g", d);
    if (strtod(out, NULL) != d) {
        n = snprintf(out, 32, "%.16g", d);
        if (strtod(out, NULL) != d) n = snprintf(out, 32, "%.17g", d);
    }
    if (!strpbrk(out, ".eE")) { out[n++] = '.'; out[n++] = '0'; out[n] = '\0'; }
    return n;
}

/* ---- UTF-8 ------------------------------------------------------------------ */

/* Length of the valid UTF-8 sequence at s (n bytes available) and its code
 * point, or 0 if it is not valid (overlong, surrogate, > U+10FFFF, truncated). */
static int utf8_seq(const unsigned char *s, size_t n, uint32_t *cp) {
    unsigned char c = s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if (c >= 0xC2 && c <= 0xDF) {
        if (n < 2 || (s[1] & 0xC0) != 0x80) return 0;
        *cp = ((uint32_t)(c & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }
    if (c >= 0xE0 && c <= 0xEF) {
        if (n < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
        uint32_t v = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        if (v < 0x800 || (v >= 0xD800 && v <= 0xDFFF)) return 0;
        *cp = v;
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (n < 4 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return 0;
        uint32_t v = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
                     ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        if (v < 0x10000 || v > 0x10FFFF) return 0;
        *cp = v;
        return 4;
    }
    return 0;
}

static void push_utf8(Buf *b, uint32_t cp) {
    char t[4];
    if (cp < 0x80) { buf_putc(b, (char)cp); return; }
    if (cp < 0x800) {
        t[0] = (char)(0xC0 | (cp >> 6));
        t[1] = (char)(0x80 | (cp & 0x3F));
        buf_push(b, t, 2);
    } else if (cp < 0x10000) {
        t[0] = (char)(0xE0 | (cp >> 12));
        t[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        t[2] = (char)(0x80 | (cp & 0x3F));
        buf_push(b, t, 3);
    } else {
        t[0] = (char)(0xF0 | (cp >> 18));
        t[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        t[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        t[3] = (char)(0x80 | (cp & 0x3F));
        buf_push(b, t, 4);
    }
}

/* ======================================================================== */
/* Encoder                                                                  */
/* ======================================================================== */

typedef struct {
    Buf         b;
    const char *indent;      /* NULL: compact */
    size_t      indent_len;
    bool        sort_keys;
    bool        ascii;
    int         max_depth;
    int         depth;
    SeaObject  *open[JSON_MAX_DEPTH + 1];   /* containers being encoded (cycles) */
    const char *err;
    char        errbuf[96];
} Enc;

static int enc_value(Enc *e, SeaValue v);

static void enc_newline(Enc *e, int level) {
    if (!e->indent) return;
    buf_putc(&e->b, '\n');
    for (int i = 0; i < level; i++) buf_push(&e->b, e->indent, e->indent_len);
}

static void push_u_escape(Buf *b, uint32_t u) {
    static const char hx[] = "0123456789abcdef";
    char t[6] = { '\\', 'u', hx[(u >> 12) & 15], hx[(u >> 8) & 15], hx[(u >> 4) & 15], hx[u & 15] };
    buf_push(b, t, 6);
}

/* A JSON string. Invalid UTF-8 becomes U+FFFD; with ascii, every non-ASCII
 * code point is written as \uXXXX (a surrogate pair above U+FFFF). */
static void enc_string(Enc *e, const char *data, size_t len) {
    Buf *b = &e->b;
    const unsigned char *s = (const unsigned char *)data;
    buf_reserve(b, len + 2);
    buf_putc(b, '"');
    size_t start = 0, i = 0;
    while (i < len) {
        unsigned char c = s[i];
        if (c >= 0x20 && c < 0x80 && c != '"' && c != '\\') { i++; continue; }
        if (c >= 0x80) {
            uint32_t cp = 0;
            int n = utf8_seq(s + i, len - i, &cp);
            if (n && !e->ascii) { i += (size_t)n; continue; }   /* valid: keep as is */
            buf_push(b, data + start, i - start);
            if (!n) {
                if (e->ascii) push_u_escape(b, 0xFFFD);
                else buf_push(b, "\xEF\xBF\xBD", 3);
                i++;
            } else {
                if (cp >= 0x10000) {
                    cp -= 0x10000;
                    push_u_escape(b, 0xD800 + (cp >> 10));
                    push_u_escape(b, 0xDC00 + (cp & 0x3FF));
                } else {
                    push_u_escape(b, cp);
                }
                i += (size_t)n;
            }
            start = i;
            continue;
        }
        buf_push(b, data + start, i - start);
        switch (c) {
            case '"':  buf_push(b, "\\\"", 2); break;
            case '\\': buf_push(b, "\\\\", 2); break;
            case '\n': buf_push(b, "\\n", 2);  break;
            case '\r': buf_push(b, "\\r", 2);  break;
            case '\t': buf_push(b, "\\t", 2);  break;
            case '\b': buf_push(b, "\\b", 2);  break;
            case '\f': buf_push(b, "\\f", 2);  break;
            default:   push_u_escape(b, c);    break;
        }
        i++;
        start = i;
    }
    buf_push(b, data + start, len - start);
    buf_putc(b, '"');
}

/* Values JSON can represent; anything else (functions, channels, errors,
 * goroutines) is left out of objects and written as null in arrays. */
static bool encodable(SeaValue v) {
    if (sea_is_nil(v) || sea_is_bool(v) || sea_is_int(v) || sea_is_number(v) || sea_is_bigint(v)) return true;
    return sea_is_string(v) || sea_is_rope(v) || sea_is_array(v) || sea_is_table(v) ||
           sea_is_map(v) || sea_is_struct(v) || sea_is_int_array(v) || sea_is_float_array(v);
}

static int enc_fail(Enc *e, const char *msg) {
    if (!e->err) e->err = msg;
    return 0;
}

/* Track a container on the path being encoded: depth limit and cycles. */
static int enc_enter(Enc *e, SeaValue v) {
    SeaObject *o = sea_as_obj(v);
    for (int i = 0; i < e->depth; i++)
        if (e->open[i] == o) return enc_fail(e, "cycle detected (a value contains itself)");
    if (e->depth >= e->max_depth) return enc_fail(e, "nesting too deep");
    e->open[e->depth++] = o;
    return 1;
}

static void enc_leave(Enc *e) { e->depth--; }

static int enc_array_items(Enc *e, SeaValue v) {
    Buf *b = &e->b;
    uint32_t n;
    if (sea_is_array(v))            n = sea_array_len(SEA_AS_ARRAY(v));
    else if (sea_is_int_array(v))   n = SEA_AS_INT_ARRAY(v)->len;
    else if (sea_is_float_array(v)) n = SEA_AS_FLOAT_ARRAY(v)->len;
    else                            n = sea_table_count(SEA_AS_TABLE(v));   /* 0..n-1 table */
    buf_putc(b, '[');
    for (uint32_t i = 0; i < n; i++) {
        if (i) buf_putc(b, ',');
        enc_newline(e, e->depth);
        if (sea_is_int_array(v)) {
            char t[24];
            buf_push(b, t, (size_t)fmt_int(t, SEA_AS_INT_ARRAY(v)->data[i]));
            continue;
        }
        if (sea_is_float_array(v)) {
            double d = SEA_AS_FLOAT_ARRAY(v)->data[i];
            if (isnan(d) || isinf(d)) { buf_push(b, "null", 4); continue; }
            char t[40];
            buf_push(b, t, (size_t)fmt_double(t, d));
            continue;
        }
        SeaValue item;
        if (sea_is_array(v)) item = sea_array_get(SEA_AS_ARRAY(v), (int64_t)i);
        else if (!sea_table_get(SEA_AS_TABLE(v), sea_int_val((int64_t)i), &item)) item = SEA_NIL_VAL;
        if (!encodable(item)) { buf_push(b, "null", 4); continue; }
        if (!enc_value(e, item)) return 0;
    }
    if (n) enc_newline(e, e->depth - 1);
    buf_putc(b, ']');
    return 1;
}

/* One object member: the key as a string (borrowed or owned) and the value. */
typedef struct { SeaString *key; bool owned; SeaValue val; } Member;

typedef struct { Member *m; size_t n, cap; Member small[16]; } Members;

static void members_add(Members *ms, SeaValue k, SeaValue v) {
    if (!encodable(v)) return;
    SeaString *ks = NULL;
    bool owned = false;
    char t[40];
    if (sea_is_string(k)) {
        ks = SEA_AS_STRING(k);
    } else if (sea_is_rope(k)) {
        ks = sea_rope_flatten(SEA_AS_ROPE(k));
        owned = true;
    } else if (sea_is_int(k)) {                 /* {[1] = "a"} → {"1": "a"} */
        ks = sea_string_new(t, (size_t)fmt_int(t, sea_as_int(k)));
        owned = true;
    } else if (sea_is_number(k)) {
        double d = sea_as_number(k);
        if (isnan(d) || isinf(d)) return;
        ks = sea_string_new(t, (size_t)fmt_double(t, d));
        owned = true;
    } else if (sea_is_bool(k)) {
        ks = sea_string_from_cstr(sea_as_bool(k) ? "true" : "false");
        owned = true;
    } else if (sea_is_bigint(k)) {
        ks = sea_bigint_to_string(SEA_AS_BIGINT(k), 10);
        owned = true;
    } else {
        return;                                 /* object keys: no JSON form */
    }
    if (ms->n == ms->cap) {
        size_t nc = ms->cap * 2;
        Member *nm = malloc(nc * sizeof *nm);
        if (!nm) { if (owned) sea_obj_release(&ks->obj); return; }
        memcpy(nm, ms->m, ms->n * sizeof *nm);
        if (ms->m != ms->small) free(ms->m);
        ms->m = nm;
        ms->cap = nc;
    }
    ms->m[ms->n++] = (Member){ ks, owned, v };
}

static void members_free(Members *ms) {
    for (size_t i = 0; i < ms->n; i++)
        if (ms->m[i].owned) sea_obj_release(&ms->m[i].key->obj);
    if (ms->m != ms->small) free(ms->m);
}

static int member_cmp(const void *a, const void *b) {
    const SeaString *x = ((const Member *)a)->key, *y = ((const Member *)b)->key;
    size_t n = x->len < y->len ? x->len : y->len;
    int c = memcmp(x->data, y->data, n);
    if (c) return c;
    return (x->len > y->len) - (x->len < y->len);
}

static int enc_object(Enc *e, SeaValue v) {
    Members ms;
    ms.m = ms.small;
    ms.n = 0;
    ms.cap = sizeof ms.small / sizeof ms.small[0];
    uint32_t iter = 0;
    SeaValue k, val;
    if (sea_is_table(v)) {
        while (sea_table_next(SEA_AS_TABLE(v), &iter, &k, &val)) members_add(&ms, k, val);
    } else if (sea_is_map(v)) {
        while (sea_map_next(SEA_AS_MAP(v), &iter, &k, &val)) members_add(&ms, k, val);
    } else {
        while (sea_struct_next(SEA_AS_STRUCT(v), &iter, &k, &val)) members_add(&ms, k, val);
    }
    if (e->sort_keys && ms.n > 1) qsort(ms.m, ms.n, sizeof *ms.m, member_cmp);

    Buf *b = &e->b;
    buf_putc(b, '{');
    int ok = 1;
    for (size_t i = 0; i < ms.n && ok; i++) {
        if (i) buf_putc(b, ',');
        enc_newline(e, e->depth);
        enc_string(e, ms.m[i].key->data, ms.m[i].key->len);
        buf_putc(b, ':');
        if (e->indent) buf_putc(b, ' ');
        ok = enc_value(e, ms.m[i].val);
    }
    if (ok && ms.n) enc_newline(e, e->depth - 1);
    buf_putc(b, '}');
    members_free(&ms);
    return ok;
}

/* A table whose keys are exactly the integers 0..n-1 (a positional table
 * literal {a, b, c}) is a JSON array. */
static bool is_array_table(SeaTable *t) {
    uint32_t count = sea_table_count(t);
    if (count == 0) return false;
    uint32_t iter = 0;
    SeaValue k, v;
    while (sea_table_next(t, &iter, &k, &v)) {
        if (!sea_is_int(k)) return false;
        int64_t i = sea_as_int(k);
        if (i < 0 || (uint64_t)i >= count) return false;
    }
    return true;
}

static int enc_value(Enc *e, SeaValue v) {
    Buf *b = &e->b;
    if (sea_is_nil(v))  { buf_push(b, "null", 4); return 1; }
    if (sea_is_bool(v)) {
        if (sea_as_bool(v)) buf_push(b, "true", 4); else buf_push(b, "false", 5);
        return 1;
    }
    if (sea_is_int(v)) {
        char t[24];
        buf_push(b, t, (size_t)fmt_int(t, sea_as_int(v)));
        return 1;
    }
    if (sea_is_bigint(v)) {                     /* every digit, exactly */
        SeaString *s = sea_bigint_to_string(SEA_AS_BIGINT(v), 10);
        buf_push(b, s->data, s->len);
        sea_obj_release(&s->obj);
        return 1;
    }
    if (sea_is_number(v)) {
        double d = sea_as_number(v);
        if (isnan(d) || isinf(d)) { buf_push(b, "null", 4); return 1; }
        char t[40];
        buf_push(b, t, (size_t)fmt_double(t, d));
        return 1;
    }
    if (sea_is_string(v)) {
        SeaString *s = SEA_AS_STRING(v);
        enc_string(e, s->data, s->len);
        return 1;
    }
    if (sea_is_rope(v)) {
        SeaString *s = sea_rope_flatten(SEA_AS_ROPE(v));
        enc_string(e, s->data, s->len);
        sea_obj_release(&s->obj);
        return 1;
    }
    if (!encodable(v)) {
        snprintf(e->errbuf, sizeof e->errbuf, "cannot encode a value of type %s", sea_type_name(v));
        return enc_fail(e, e->errbuf);
    }
    if (!enc_enter(e, v)) return 0;
    int ok;
    if (sea_is_array(v) || sea_is_int_array(v) || sea_is_float_array(v) ||
        (sea_is_table(v) && is_array_table(SEA_AS_TABLE(v))))
        ok = enc_array_items(e, v);
    else
        ok = enc_object(e, v);
    enc_leave(e);
    return ok;
}

static SeaValue fn_encode(SeaVM *vm, int argc, SeaValue *args) {
    SeaValue opts = argc > 1 ? args[1] : SEA_NIL_VAL;
    Enc *e = calloc(1, sizeof *e);
    if (!e) return sea_make_error_str(vm, "json.encode: out of memory");
    e->max_depth = opt_int(opts, "max_depth", JSON_MAX_DEPTH);
    if (e->max_depth < 1 || e->max_depth > JSON_MAX_DEPTH) e->max_depth = JSON_MAX_DEPTH;
    e->sort_keys = opt_bool(opts, "sort_keys");
    e->ascii = opt_bool(opts, "ascii");
    static const char spaces[] = "                ";   /* up to 16 */
    SeaValue ind;
    SeaString *ind_s = NULL;
    if (opt_get(opts, "indent", &ind)) {
        if (sea_is_int(ind) && sea_as_int(ind) > 0) {
            int64_t n = sea_as_int(ind);
            e->indent = spaces;
            e->indent_len = (size_t)(n > 16 ? 16 : n);
        } else if ((ind_s = sea_value_get_flat_string(ind)) != NULL) {
            e->indent = ind_s->data;
            e->indent_len = ind_s->len;
        }
    }

    SeaValue r;
    if (argc < 1) {
        r = sea_make_error_str(vm, "json.encode: value expected");
    } else if (!enc_value(e, args[0]) || e->b.oom) {
        char msg[160];
        snprintf(msg, sizeof msg, "json.encode: %s", e->b.oom ? "out of memory" : e->err);
        r = sea_make_error_str(vm, msg);
    } else {
        r = sea_obj_val(&sea_string_new(e->b.data ? e->b.data : "", e->b.len)->obj);
    }
    if (ind_s) sea_obj_release(&ind_s->obj);
    buf_free(&e->b);
    free(e);
    return r;
}

/* ======================================================================== */
/* Decoder                                                                  */
/* ======================================================================== */

/* Not a valid Sea value (no tag uses these bits): "parse error, see p->err". */
#define JSON_ERR UINT64_C(0xFFFFFFFFFFFFFFFF)

/* Object keys: one string per distinct key per decode (an array of a
 * thousand records shares its "id"), reusing the program's interned instance
 * when there is one, so `obj.id` compares pointers. Never interns new keys:
 * input must not grow the global intern table. */
#define KEYCACHE_SIZE  512
#define KEYCACHE_PROBE 8

typedef struct {
    const char *src;
    size_t      len, pos;
    int         depth, max_depth;
    bool        build;            /* false: json.valid, no values are made */
    bool        bigints;          /* integers past Sea's int range → bigint */
    const char *err;
    size_t      err_pos;
    SeaString **keys;             /* KEYCACHE_SIZE slots, allocated on first key */
} JP;

static SeaValue jp_value(JP *p);

static SeaValue jp_fail(JP *p, const char *msg, size_t at) {
    if (!p->err) { p->err = msg; p->err_pos = at; }
    return JSON_ERR;
}

static void jp_skip_ws(JP *p) {
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') p->pos++;
        else break;
    }
}

static SeaString *jp_key(JP *p, const char *data, size_t len) {
    if (!p->keys) p->keys = calloc(KEYCACHE_SIZE, sizeof *p->keys);
    uint32_t h = sea_string_hash(data, len);
    if (p->keys) {
        for (int i = 0; i < KEYCACHE_PROBE; i++) {
            SeaString **slot = &p->keys[(h + (uint32_t)i) & (KEYCACHE_SIZE - 1)];
            SeaString *s = *slot;
            if (s && s->len == len && memcmp(s->data, data, len) == 0) {
                sea_obj_retain(&s->obj);
                return s;
            }
            if (!s) {
                s = sea_string_find_intern(data, len);
                if (!s) s = sea_string_new(data, len);
                *slot = s;                       /* the cache's reference */
                sea_obj_retain(&s->obj);         /* the caller's */
                return s;
            }
        }
    }
    SeaString *s = sea_string_find_intern(data, len);
    return s ? s : sea_string_new(data, len);
}

static SeaValue jp_make_string(JP *p, const char *data, size_t len, bool key) {
    if (!p->build) return SEA_NIL_VAL;
    if (key) return sea_obj_val(&jp_key(p, data, len)->obj);
    SeaString *s = sea_string_alloc(len);                  /* hash computed lazily */
    memcpy(s->data, data, len);
    s->data[len] = '\0';
    return sea_obj_val(&s->obj);
}

static int hex4(const char *s) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

static SeaValue jp_string(JP *p, bool key) {
    size_t open = p->pos++;                     /* the opening quote */
    size_t start = p->pos;
    const char *s = p->src;
    while (p->pos < p->len) {
        unsigned char c = (unsigned char)s[p->pos];
        if (c == '"' || c == '\\' || c < 0x20) break;
        p->pos++;
    }
    if (p->pos >= p->len) return jp_fail(p, "unterminated string", open);
    if (s[p->pos] == '"') {                     /* no escapes: straight from the input */
        SeaValue r = jp_make_string(p, s + start, p->pos - start, key);
        p->pos++;
        return r;
    }

    Buf b = { 0 };
    if (p->build) buf_push(&b, s + start, p->pos - start);
    while (p->pos < p->len) {
        unsigned char c = (unsigned char)s[p->pos];
        if (c == '"') break;
        if (c < 0x20) { buf_free(&b); return jp_fail(p, "control character in string", p->pos); }
        if (c != '\\') {
            size_t run = p->pos;
            while (p->pos < p->len) {
                unsigned char d = (unsigned char)s[p->pos];
                if (d == '"' || d == '\\' || d < 0x20) break;
                p->pos++;
            }
            if (p->build) buf_push(&b, s + run, p->pos - run);
            continue;
        }
        size_t esc_at = p->pos++;
        if (p->pos >= p->len) break;
        char esc = s[p->pos++];
        char out;
        switch (esc) {
            case '"':  out = '"';  break;
            case '\\': out = '\\'; break;
            case '/':  out = '/';  break;
            case 'b':  out = '\b'; break;
            case 'f':  out = '\f'; break;
            case 'n':  out = '\n'; break;
            case 'r':  out = '\r'; break;
            case 't':  out = '\t'; break;
            case 'u': {
                int u = p->pos + 4 <= p->len ? hex4(s + p->pos) : -1;
                if (u < 0) { buf_free(&b); return jp_fail(p, "invalid \\u escape", esc_at); }
                p->pos += 4;
                uint32_t cp = (uint32_t)u;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    /* a high surrogate pairs with a following \uDC00-\uDFFF */
                    int lo = (p->pos + 6 <= p->len && s[p->pos] == '\\' && s[p->pos + 1] == 'u')
                             ? hex4(s + p->pos + 2) : -1;
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + ((uint32_t)lo - 0xDC00);
                        p->pos += 6;
                    } else {
                        cp = 0xFFFD;            /* lone surrogate */
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                if (p->build) push_utf8(&b, cp);
                continue;
            }
            default:
                buf_free(&b);
                return jp_fail(p, "invalid escape in string", esc_at);
        }
        if (p->build) buf_putc(&b, out);
    }
    if (p->pos >= p->len) { buf_free(&b); return jp_fail(p, "unterminated string", open); }
    p->pos++;                                   /* the closing quote */
    if (b.oom) { buf_free(&b); return jp_fail(p, "out of memory", open); }
    SeaValue r = jp_make_string(p, b.data ? b.data : "", b.len, key);
    buf_free(&b);
    return r;
}

static SeaValue jp_number(JP *p) {
    const char *s = p->src;
    size_t start = p->pos;
    bool neg = false, is_float = false, big = false;
    uint64_t acc = 0;
    if (s[p->pos] == '-') { neg = true; p->pos++; }
    if (p->pos >= p->len || s[p->pos] < '0' || s[p->pos] > '9')
        return jp_fail(p, "invalid number", start);
    if (s[p->pos] == '0') {
        p->pos++;
        if (p->pos < p->len && s[p->pos] >= '0' && s[p->pos] <= '9')
            return jp_fail(p, "invalid number (leading zero)", start);
    } else {
        while (p->pos < p->len && s[p->pos] >= '0' && s[p->pos] <= '9') {
            if (acc > (UINT64_MAX - 9) / 10) big = true;
            else acc = acc * 10 + (uint64_t)(s[p->pos] - '0');
            p->pos++;
        }
    }
    if (p->pos < p->len && s[p->pos] == '.') {
        is_float = true;
        p->pos++;
        if (p->pos >= p->len || s[p->pos] < '0' || s[p->pos] > '9')
            return jp_fail(p, "invalid number (digit expected after '.')", start);
        while (p->pos < p->len && s[p->pos] >= '0' && s[p->pos] <= '9') p->pos++;
    }
    if (p->pos < p->len && (s[p->pos] == 'e' || s[p->pos] == 'E')) {
        is_float = true;
        p->pos++;
        if (p->pos < p->len && (s[p->pos] == '+' || s[p->pos] == '-')) p->pos++;
        if (p->pos >= p->len || s[p->pos] < '0' || s[p->pos] > '9')
            return jp_fail(p, "invalid number (digit expected in exponent)", start);
        while (p->pos < p->len && s[p->pos] >= '0' && s[p->pos] <= '9') p->pos++;
    }
    if (!p->build) return SEA_NIL_VAL;

    /* integers within Sea's 48-bit int range stay ints */
    if (!is_float && !big) {
        if (!neg && acc < (UINT64_C(1) << 47)) return sea_int_val((int64_t)acc);
        if (neg && acc <= (UINT64_C(1) << 47)) return sea_int_val(-(int64_t)acc);
    }
    if (!is_float && p->bigints) {
        SeaBigInt *b = sea_bigint_parse(s + start, p->pos - start, 10);
        if (!b) return jp_fail(p, "out of memory", start);
        return sea_obj_val(&b->obj);
    }
    size_t n = p->pos - start;
    char small[64];
    char *tmp = n < sizeof small ? small : malloc(n + 1);
    if (!tmp) return jp_fail(p, "out of memory", start);
    memcpy(tmp, s + start, n);
    tmp[n] = '\0';
    double d = strtod(tmp, NULL);
    if (tmp != small) free(tmp);
    return sea_number_val(d);
}

static SeaValue jp_array(JP *p) {
    size_t open = p->pos++;
    if (++p->depth > p->max_depth) return jp_fail(p, "nesting too deep", open);
    SeaArray *a = p->build ? sea_array_new(4) : NULL;
    jp_skip_ws(p);
    if (p->pos < p->len && p->src[p->pos] == ']') {
        p->pos++;
        p->depth--;
        return a ? sea_obj_val(&a->obj) : SEA_NIL_VAL;
    }
    for (;;) {
        SeaValue v = jp_value(p);
        if (v == JSON_ERR) break;
        if (a) sea_array_push_move(a, v);      /* takes our reference */
        jp_skip_ws(p);
        if (p->pos >= p->len) { jp_fail(p, "unexpected end of input (missing ']')", open); break; }
        char c = p->src[p->pos];
        if (c == ']') {
            p->pos++;
            p->depth--;
            return a ? sea_obj_val(&a->obj) : SEA_NIL_VAL;
        }
        if (c != ',') { jp_fail(p, "expected ',' or ']'", p->pos); break; }
        p->pos++;
    }
    if (a) sea_obj_release(&a->obj);
    return JSON_ERR;
}

static SeaValue jp_object(JP *p) {
    size_t open = p->pos++;
    if (++p->depth > p->max_depth) return jp_fail(p, "nesting too deep", open);
    SeaTable *t = p->build ? sea_table_new(TABLE_MAP) : NULL;
    jp_skip_ws(p);
    if (p->pos < p->len && p->src[p->pos] == '}') {
        p->pos++;
        p->depth--;
        return t ? sea_obj_val(&t->obj) : SEA_NIL_VAL;
    }
    for (;;) {
        jp_skip_ws(p);
        if (p->pos >= p->len) { jp_fail(p, "unexpected end of input (missing '}')", open); break; }
        if (p->src[p->pos] != '"') { jp_fail(p, "expected a string key", p->pos); break; }
        SeaValue key = jp_string(p, true);
        if (key == JSON_ERR) break;
        jp_skip_ws(p);
        if (p->pos >= p->len || p->src[p->pos] != ':') {
            if (t) sea_obj_release(sea_as_obj(key));
            jp_fail(p, "expected ':' after key", p->pos);
            break;
        }
        p->pos++;
        SeaValue val = jp_value(p);
        if (val == JSON_ERR) { if (t) sea_obj_release(sea_as_obj(key)); break; }
        if (t) {
            sea_table_set_move(t, key, val);     /* takes val; retains key */
            sea_obj_release(sea_as_obj(key));
        }
        jp_skip_ws(p);
        if (p->pos >= p->len) { jp_fail(p, "unexpected end of input (missing '}')", open); break; }
        char c = p->src[p->pos];
        if (c == '}') {
            p->pos++;
            p->depth--;
            return t ? sea_obj_val(&t->obj) : SEA_NIL_VAL;
        }
        if (c != ',') { jp_fail(p, "expected ',' or '}'", p->pos); break; }
        p->pos++;
    }
    if (t) sea_obj_release(&t->obj);
    return JSON_ERR;
}

static SeaValue jp_literal(JP *p, const char *lit, size_t n, SeaValue v) {
    if (p->pos + n <= p->len && memcmp(p->src + p->pos, lit, n) == 0) {
        p->pos += n;
        return v;
    }
    return jp_fail(p, "unexpected character", p->pos);
}

static SeaValue jp_value(JP *p) {
    jp_skip_ws(p);
    if (p->pos >= p->len) return jp_fail(p, "unexpected end of input", p->pos);
    char c = p->src[p->pos];
    switch (c) {
        case '{': return jp_object(p);
        case '[': return jp_array(p);
        case '"': return jp_string(p, false);
        case 't': return jp_literal(p, "true", 4, SEA_TRUE_VAL);
        case 'f': return jp_literal(p, "false", 5, SEA_FALSE_VAL);
        case 'n': return jp_literal(p, "null", 4, SEA_NIL_VAL);
        default:
            if (c == '-' || (c >= '0' && c <= '9')) return jp_number(p);
            return jp_fail(p, "unexpected character", p->pos);
    }
}

/* Parse a whole document; on failure *err gets a message with the position. */
static SeaValue jp_document(JP *p, char *err, size_t errlen) {
    if (p->len >= 3 && memcmp(p->src, "\xEF\xBB\xBF", 3) == 0) p->pos = 3;   /* UTF-8 BOM */
    SeaValue v = jp_value(p);
    if (v != JSON_ERR) {
        jp_skip_ws(p);
        if (p->pos != p->len) {
            if (p->build && sea_is_obj(v)) sea_obj_release(sea_as_obj(v));
            v = jp_fail(p, "unexpected data after the JSON value", p->pos);
        }
    }
    if (p->keys) {
        for (int i = 0; i < KEYCACHE_SIZE; i++)
            if (p->keys[i]) sea_obj_release(&p->keys[i]->obj);
        free(p->keys);
        p->keys = NULL;
    }
    if (v == JSON_ERR && err) {
        size_t line = 1, col = 1;
        for (size_t i = 0; i < p->err_pos && i < p->len; i++) {
            if (p->src[i] == '\n') { line++; col = 1; } else col++;
        }
        char what[24] = "";
        unsigned char ch = p->err_pos < p->len ? (unsigned char)p->src[p->err_pos] : 0;
        if (strcmp(p->err, "unexpected character") == 0 && ch >= 0x20 && ch < 0x7F)
            snprintf(what, sizeof what, " '%c'", ch);
        snprintf(err, errlen, "json.decode: %s%s at line %zu, column %zu", p->err, what, line, col);
    }
    return v;
}

static SeaValue fn_decode(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) return sea_make_error_str(vm, "json.decode: string expected");
    SeaString *s = sea_value_get_flat_string(args[0]);
    if (!s) return sea_make_error_str(vm, "json.decode: string expected");
    JP p = { 0 };
    p.src = s->data;
    p.len = s->len;
    p.build = true;
    p.max_depth = opt_int(argc > 1 ? args[1] : SEA_NIL_VAL, "max_depth", JSON_MAX_DEPTH);
    p.bigints = opt_bool(argc > 1 ? args[1] : SEA_NIL_VAL, "bigint");
    if (p.max_depth < 1) p.max_depth = JSON_MAX_DEPTH;
    if (p.max_depth > 10000) p.max_depth = 10000;     /* bounded by the C stack */
    char err[160];
    SeaValue v = jp_document(&p, err, sizeof err);
    sea_obj_release(&s->obj);
    if (v == JSON_ERR) return sea_make_error_str(vm, err);
    return v;
}

static SeaValue fn_valid(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_FALSE_VAL;
    SeaString *s = sea_value_get_flat_string(args[0]);
    if (!s) return SEA_FALSE_VAL;
    JP p = { 0 };
    p.src = s->data;
    p.len = s->len;
    p.build = false;
    p.max_depth = JSON_MAX_DEPTH;
    SeaValue v = jp_document(&p, NULL, 0);
    sea_obj_release(&s->obj);
    return sea_bool_val(v != JSON_ERR);
}

/* ---- Module entry point --------------------------------------------------- */

SEA_EXPORT SeaValue sea_open(SeaVM *vm) {
    (void)vm;
    SeaTable *mod = sea_mod_new();
    sea_mod_set(mod, "encode", fn_encode);
    sea_mod_set(mod, "decode", fn_decode);
    sea_mod_set(mod, "valid",  fn_valid);
    return sea_obj_val(&mod->obj);
}
