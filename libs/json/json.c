/*
 * libs/json/json.c — JSON encode/decode for Sea.
 *
 * Build:  cd libs/json && make
 * Usage:  local json = require("./libs/json/json")
 *
 * Exports:
 *   json.encode(value)  → string  (nil on unencodable / circular)
 *   json.decode(string) → value   (nil on parse error)
 */

#include "sea.h"
#include "sea_map.h"
#include "sea_rope.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Dynamic write buffer ------------------------------------------------- */

typedef struct { char *data; size_t len; size_t cap; } Buf;

static void buf_init(Buf *b) { b->data = NULL; b->len = 0; b->cap = 0; }
static void buf_free(Buf *b) { free(b->data); buf_init(b); }

static int buf_push(Buf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        while (nc < b->len + n + 1) nc *= 2;
        char *p = realloc(b->data, nc);
        if (!p) return 0;
        b->data = p; b->cap = nc;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 1;
}

static int buf_push_c(Buf *b, char c) { return buf_push(b, &c, 1); }

/* ---- Encoder -------------------------------------------------------------- */

static int encode_value(Buf *b, SeaValue v, int depth);

static int encode_string_data(Buf *b, const char *data, size_t len) {
    buf_push_c(b, '"');
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        switch (c) {
            case '"':  buf_push(b, "\\\"", 2); break;
            case '\\': buf_push(b, "\\\\", 2); break;
            case '\n': buf_push(b, "\\n",  2); break;
            case '\r': buf_push(b, "\\r",  2); break;
            case '\t': buf_push(b, "\\t",  2); break;
            case '\b': buf_push(b, "\\b",  2); break;
            case '\f': buf_push(b, "\\f",  2); break;
            default:
                if (c < 0x20) {
                    char esc[7];
                    snprintf(esc, sizeof(esc), "\\u%04x", c);
                    buf_push(b, esc, 6);
                } else {
                    buf_push_c(b, (char)c);
                }
                break;
        }
    }
    buf_push_c(b, '"');
    return 1;
}

static int encode_array(Buf *b, SeaArray *a, int depth) {
    buf_push_c(b, '[');
    uint32_t n = sea_array_len(a);
    for (uint32_t i = 0; i < n; i++) {
        if (i > 0) buf_push_c(b, ',');
        if (!encode_value(b, sea_array_get(a, (int64_t)i), depth + 1))
            return 0;
    }
    buf_push_c(b, ']');
    return 1;
}

/* Returns 1 if t has exactly count keys: integers 0..count-1 (Sea array-literal layout). */
static int is_array_table(SeaTable *t) {
    uint32_t count = sea_table_count(t);
    if (count == 0) return 0;
    uint32_t iter = 0; SeaValue k, v;
    while (sea_table_next(t, &iter, &k, &v)) {
        if (!sea_is_int(k)) return 0;
        int64_t idx = sea_as_int(k);
        if (idx < 0 || (uint64_t)idx >= (uint64_t)count) return 0;
    }
    return 1;
}

static int encode_array_table(Buf *b, SeaTable *t, int depth) {
    uint32_t count = sea_table_count(t);
    buf_push_c(b, '[');
    for (uint32_t i = 0; i < count; i++) {
        if (i > 0) buf_push_c(b, ',');
        SeaValue v;
        if (!sea_table_get(t, sea_int_val((int64_t)i), &v)) v = SEA_NIL_VAL;
        if (!encode_value(b, v, depth + 1)) return 0;
    }
    buf_push_c(b, ']');
    return 1;
}

static int encode_table(Buf *b, SeaTable *t, int depth) {
    if (is_array_table(t)) return encode_array_table(b, t, depth);
    buf_push_c(b, '{');
    uint32_t iter = 0;
    SeaValue k, v;
    int first = 1;
    while (sea_table_next(t, &iter, &k, &v)) {
        if (!sea_is_string(k)) continue;
        SeaString *ks = SEA_AS_STRING(k);
        if (!first) buf_push_c(b, ',');
        first = 0;
        encode_string_data(b, ks->data, ks->len);
        buf_push_c(b, ':');
        if (!encode_value(b, v, depth + 1)) return 0;
    }
    buf_push_c(b, '}');
    return 1;
}

static int encode_value(Buf *b, SeaValue v, int depth) {
    if (depth > 256) return 0;

    if (sea_is_nil(v))  { buf_push(b, "null",  4); return 1; }
    if (sea_is_bool(v)) {
        if (sea_as_bool(v)) buf_push(b, "true", 4);
        else                buf_push(b, "false", 5);
        return 1;
    }
    if (sea_is_int(v)) {
        char tmp[32];
        int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)sea_as_int(v));
        return buf_push(b, tmp, (size_t)n);
    }
    if (sea_is_number(v)) {
        double d = sea_as_number(v);
        if (isnan(d) || isinf(d)) { buf_push(b, "null", 4); return 1; }
        char tmp[32];
        /* Use shortest representation that round-trips. */
        int n = snprintf(tmp, sizeof(tmp), "%.17g", d);
        return buf_push(b, tmp, (size_t)n);
    }
    if (sea_is_string(v)) {
        SeaString *s = SEA_AS_STRING(v);
        return encode_string_data(b, s->data, s->len);
    }
    if (sea_is_rope(v)) {
        SeaString *s = sea_rope_flatten(SEA_AS_ROPE(v));
        int r = encode_string_data(b, s->data, s->len);
        sea_obj_release(&s->obj);
        return r;
    }
    if (sea_is_array(v))  return encode_array(b, SEA_AS_ARRAY(v), depth);
    if (sea_is_table(v))  return encode_table(b, SEA_AS_TABLE(v), depth);
    if (sea_is_map(v)) {
        SeaMap *m = SEA_AS_MAP(v);
        buf_push_c(b, '{');
        uint32_t iter = 0;
        SeaValue k, mv;
        int first = 1;
        while (sea_map_next(m, &iter, &k, &mv)) {
            SeaString *ks = NULL;
            SeaString *ks_owned = NULL;
            if (sea_is_string(k))      ks = SEA_AS_STRING(k);
            else if (sea_is_rope(k)) { ks = sea_rope_flatten(SEA_AS_ROPE(k)); ks_owned = ks; }
            if (!ks) continue;
            if (!first) buf_push_c(b, ',');
            first = 0;
            encode_string_data(b, ks->data, ks->len);
            buf_push_c(b, ':');
            if (!encode_value(b, mv, depth + 1)) { if (ks_owned) sea_obj_release(&ks_owned->obj); return 0; }
            if (ks_owned) sea_obj_release(&ks_owned->obj);
        }
        buf_push_c(b, '}');
        return 1;
    }
    if (sea_is_struct(v)) {
        SeaStruct *st = SEA_AS_STRUCT(v);
        buf_push_c(b, '{');
        uint32_t iter = 0;
        SeaValue k, fv;
        int first = 1;
        while (sea_struct_next(st, &iter, &k, &fv)) {
            if (!sea_is_string(k)) continue;
            SeaString *ks = SEA_AS_STRING(k);
            if (!first) buf_push_c(b, ',');
            first = 0;
            encode_string_data(b, ks->data, ks->len);
            buf_push_c(b, ':');
            if (!encode_value(b, fv, depth + 1)) return 0;
        }
        buf_push_c(b, '}');
        return 1;
    }

    /* functions, channels, etc. → null */
    buf_push(b, "null", 4);
    return 1;
}

static SeaValue fn_encode(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    Buf b; buf_init(&b);
    if (!encode_value(&b, args[0], 0)) { buf_free(&b); return SEA_NIL_VAL; }
    SeaValue r = sea_obj_val(&sea_string_new(b.data ? b.data : "null", b.len)->obj);
    buf_free(&b);
    return r;
}

/* ---- Decoder -------------------------------------------------------------- */

/* Sentinel: all-ones is not a valid Sea value (no tag uses bits 50:48 = 7). */
#define JSON_ERR UINT64_C(0xFFFFFFFFFFFFFFFF)
static int json_is_err(SeaValue v) { return v == JSON_ERR; }

typedef struct {
    const char *src;
    size_t      len;
    size_t      pos;
} JP;  /* JSON parser state */

static SeaValue jp_value(JP *p);

static void jp_skip_ws(JP *p) {
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->pos++;
        else break;
    }
}

static SeaValue jp_literal(JP *p, const char *lit, size_t n, SeaValue result) {
    if (p->pos + n <= p->len && memcmp(p->src + p->pos, lit, n) == 0) {
        p->pos += n;
        return result;
    }
    return JSON_ERR;
}

static SeaValue jp_number(JP *p) {
    size_t start = p->pos;
    int is_float = 0;

    if (p->pos < p->len && p->src[p->pos] == '-') p->pos++;

    while (p->pos < p->len && p->src[p->pos] >= '0' && p->src[p->pos] <= '9')
        p->pos++;

    if (p->pos < p->len && p->src[p->pos] == '.') {
        is_float = 1; p->pos++;
        while (p->pos < p->len && p->src[p->pos] >= '0' && p->src[p->pos] <= '9')
            p->pos++;
    }
    if (p->pos < p->len && (p->src[p->pos] == 'e' || p->src[p->pos] == 'E')) {
        is_float = 1; p->pos++;
        if (p->pos < p->len && (p->src[p->pos] == '+' || p->src[p->pos] == '-'))
            p->pos++;
        while (p->pos < p->len && p->src[p->pos] >= '0' && p->src[p->pos] <= '9')
            p->pos++;
    }

    if (p->pos == start) return JSON_ERR;

    size_t num_len = p->pos - start;
    char tmp[64];
    if (num_len >= sizeof(tmp)) return JSON_ERR;
    memcpy(tmp, p->src + start, num_len);
    tmp[num_len] = '\0';

    if (!is_float) {
        long long i = strtoll(tmp, NULL, 10);
        /* Fits in Sea's 48-bit signed int? */
        if (i >= -(1LL << 47) && i < (1LL << 47))
            return sea_int_val((int64_t)i);
        return sea_number_val((double)i);
    }
    return sea_number_val(strtod(tmp, NULL));
}

/* Emit Unicode codepoint as UTF-8 into buf. */
static void push_utf8(Buf *b, unsigned int cp) {
    if (cp < 0x80) {
        buf_push_c(b, (char)cp);
    } else if (cp < 0x800) {
        buf_push_c(b, (char)(0xC0 | (cp >> 6)));
        buf_push_c(b, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        buf_push_c(b, (char)(0xE0 | (cp >> 12)));
        buf_push_c(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        buf_push_c(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        buf_push_c(b, (char)(0xF0 | (cp >> 18)));
        buf_push_c(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
        buf_push_c(b, (char)(0x80 | ((cp >> 6)  & 0x3F)));
        buf_push_c(b, (char)(0x80 | (cp & 0x3F)));
    }
}

static SeaValue jp_string(JP *p) {
    p->pos++;  /* skip opening '"' */
    Buf b; buf_init(&b);

    while (p->pos < p->len && p->src[p->pos] != '"') {
        unsigned char c = (unsigned char)p->src[p->pos];
        if (c == '\\') {
            p->pos++;
            if (p->pos >= p->len) { buf_free(&b); return JSON_ERR; }
            unsigned char esc = (unsigned char)p->src[p->pos];
            switch (esc) {
                case '"':  buf_push_c(&b, '"');  break;
                case '\\': buf_push_c(&b, '\\'); break;
                case '/':  buf_push_c(&b, '/');  break;
                case 'n':  buf_push_c(&b, '\n'); break;
                case 'r':  buf_push_c(&b, '\r'); break;
                case 't':  buf_push_c(&b, '\t'); break;
                case 'b':  buf_push_c(&b, '\b'); break;
                case 'f':  buf_push_c(&b, '\f'); break;
                case 'u': {
                    if (p->pos + 4 >= p->len) { buf_free(&b); return JSON_ERR; }
                    p->pos++;
                    char hex[5];
                    memcpy(hex, p->src + p->pos, 4); hex[4] = '\0';
                    unsigned int cp = (unsigned int)strtoul(hex, NULL, 16);
                    p->pos += 3;  /* loop will +1 */
                    /* Handle surrogate pair: \uD800–\uDBFF followed by \uDC00–\uDFFF */
                    if (cp >= 0xD800 && cp <= 0xDBFF &&
                        p->pos + 1 < p->len &&
                        p->src[p->pos + 1] == '\\' &&
                        p->pos + 2 < p->len &&
                        p->src[p->pos + 2] == 'u') {
                        p->pos += 3;
                        if (p->pos + 3 >= p->len) { buf_free(&b); return JSON_ERR; }
                        char hex2[5];
                        memcpy(hex2, p->src + p->pos, 4); hex2[4] = '\0';
                        unsigned int lo = (unsigned int)strtoul(hex2, NULL, 16);
                        p->pos += 3;
                        if (lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    push_utf8(&b, cp);
                    break;
                }
                default:
                    buf_push_c(&b, (char)esc);
                    break;
            }
        } else {
            buf_push_c(&b, (char)c);
        }
        p->pos++;
    }

    if (p->pos >= p->len) { buf_free(&b); return JSON_ERR; }
    p->pos++;  /* skip closing '"' */

    SeaValue r = sea_obj_val(&sea_string_new(b.data ? b.data : "", b.len)->obj);
    buf_free(&b);
    return r;
}

/* Release a SeaValue returned by jp_* if it's an object. */
static void jp_release(SeaValue v) {
    if (!json_is_err(v) && sea_is_obj(v))
        sea_obj_release(sea_as_obj(v));
}

static SeaValue jp_array(JP *p) {
    p->pos++;  /* skip '[' */
    jp_skip_ws(p);

    SeaArray *a = sea_array_new(4);

    if (p->pos < p->len && p->src[p->pos] == ']') {
        p->pos++;
        return sea_obj_val(&a->obj);  /* rc=1, caller owns */
    }

    while (p->pos < p->len) {
        jp_skip_ws(p);
        SeaValue v = jp_value(p);
        if (json_is_err(v)) { sea_obj_release(&a->obj); return JSON_ERR; }

        sea_array_push(a, v);
        jp_release(v);  /* array retained it; release our ref */

        jp_skip_ws(p);
        if (p->pos >= p->len) break;
        char c = p->src[p->pos];
        if (c == ']') { p->pos++; goto done; }
        if (c != ',') { sea_obj_release(&a->obj); return JSON_ERR; }
        p->pos++;
    }
    sea_obj_release(&a->obj);
    return JSON_ERR;

done:
    return sea_obj_val(&a->obj);
}

static SeaValue jp_object(JP *p) {
    p->pos++;  /* skip '{' */
    jp_skip_ws(p);

    SeaTable *t = sea_table_new(TABLE_MAP);

    if (p->pos < p->len && p->src[p->pos] == '}') {
        p->pos++;
        return sea_obj_val(&t->obj);
    }

    while (p->pos < p->len) {
        jp_skip_ws(p);
        if (p->pos >= p->len || p->src[p->pos] != '"') {
            sea_obj_release(&t->obj); return JSON_ERR;
        }

        SeaValue key = jp_string(p);
        if (json_is_err(key)) { sea_obj_release(&t->obj); return JSON_ERR; }

        jp_skip_ws(p);
        if (p->pos >= p->len || p->src[p->pos] != ':') {
            jp_release(key); sea_obj_release(&t->obj); return JSON_ERR;
        }
        p->pos++;  /* skip ':' */
        jp_skip_ws(p);

        SeaValue val = jp_value(p);
        if (json_is_err(val)) {
            jp_release(key); sea_obj_release(&t->obj); return JSON_ERR;
        }

        sea_table_set(t, key, val);
        jp_release(key);  /* table retained key */
        jp_release(val);  /* table retained val */

        jp_skip_ws(p);
        if (p->pos >= p->len) break;
        char c = p->src[p->pos];
        if (c == '}') { p->pos++; goto done; }
        if (c != ',') { sea_obj_release(&t->obj); return JSON_ERR; }
        p->pos++;
    }
    sea_obj_release(&t->obj);
    return JSON_ERR;

done:
    return sea_obj_val(&t->obj);
}

static SeaValue jp_value(JP *p) {
    jp_skip_ws(p);
    if (p->pos >= p->len) return JSON_ERR;

    char c = p->src[p->pos];
    if (c == 'n') return jp_literal(p, "null",  4, SEA_NIL_VAL);
    if (c == 't') return jp_literal(p, "true",  4, SEA_TRUE_VAL);
    if (c == 'f') return jp_literal(p, "false", 5, SEA_FALSE_VAL);
    if (c == '"') return jp_string(p);
    if (c == '[') return jp_array(p);
    if (c == '{') return jp_object(p);
    if (c == '-' || (c >= '0' && c <= '9')) return jp_number(p);
    return JSON_ERR;
}

static SeaValue fn_decode(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1) return SEA_NIL_VAL;
    SeaString *s = sea_value_get_flat_string(args[0]);
    if (!s) return SEA_NIL_VAL;
    JP p = { s->data, s->len, 0 };
    SeaValue v = jp_value(&p);
    jp_skip_ws(&p);
    bool ok = !json_is_err(v) && p.pos == p.len;
    sea_obj_release(&s->obj);
    if (!ok) { if (!json_is_err(v)) jp_release(v); return SEA_NIL_VAL; }
    return v;
}

/* ---- Module entry point --------------------------------------------------- */

SEA_EXPORT SeaValue sea_open(SeaVM *vm) {
    (void)vm;
    SeaTable *mod = sea_mod_new();  /* rc=1; require retains after return */
    sea_mod_set(mod, "encode", fn_encode);
    sea_mod_set(mod, "decode", fn_decode);
    return sea_obj_val(&mod->obj);
}
