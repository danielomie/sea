/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * sea_lib_hash.c — the built-in `encoding` and `hash` modules (pure C, no
 * OpenSSL: libs/ssl has the TLS side).
 *
 * encoding: base64 (standard and URL alphabets), base32 (RFC 4648), hex,
 * percent-encoding for URLs, HTML escaping. Decoders return error values.
 *
 * hash: md5, sha1, sha224, sha256, sha384, sha512 (hex digest, or raw bytes
 * with raw = true), hmac(alg, key, data), incremental hashers
 * (hash.new(alg) → h:update(s), h:digest()), crc32, adler32, fnv1a (32-bit),
 * and equal(a, b) in constant time.
 */

#include "sea_array.h"
#include "sea_map.h"
#include "sea_rope.h"
#include "sea_stdlib.h"
#include "sea_struct.h"
#include "sea_vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Helpers ---------------------------------------------------------------- */

static SeaValue new_str(const char *s, size_t n) { return sea_obj_val(&sea_string_new(s, n)->obj); }

static void tset(SeaTable *t, const char *k, SeaValue v) {
    sea_table_set_str(t, k, v);
    if (sea_is_obj(v)) sea_obj_release(sea_as_obj(v));
}

static SeaString *str_arg(SeaVM *vm, const char *mod, const char *fn, int argc, SeaValue *args, int i) {
    SeaString *s = argc > i ? sea_value_get_flat_string(args[i]) : NULL;
    if (!s)
        sea_vm_error_noreturn(vm, "%s.%s: string expected, got %s", mod, fn, argc > i ? sea_type_name(args[i]) : "nothing");
    return s;
}

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

/* opts.field as a bool; a bare bool argument also counts (hash.sha256(s, true)). */
static bool flag_arg(int argc, SeaValue *args, int i, const char *field, bool def) {
    if (argc <= i) return def;
    if (sea_is_bool(args[i])) return sea_as_bool(args[i]);
    SeaValue v;
    if (opt_get(args[i], field, &v) && sea_is_bool(v)) return sea_as_bool(v);
    return def;
}

typedef struct { char *d; size_t len, cap; } Out;

static void out_push(Out *o, const char *s, size_t n) {
    if (o->len + n + 1 > o->cap) {
        size_t nc = o->cap ? o->cap * 2 : 64;
        while (nc < o->len + n + 1) nc *= 2;
        o->d = realloc(o->d, nc);
        o->cap = nc;
    }
    memcpy(o->d + o->len, s, n);
    o->len += n;
}

static void out_putc(Out *o, char c) { out_push(o, &c, 1); }

static SeaValue out_value(Out *o) {
    SeaValue v = new_str(o->d ? o->d : "", o->len);
    free(o->d);
    return v;
}

static SeaValue dec_err(SeaVM *vm, const char *fn, const char *what, size_t at) {
    char msg[160];
    snprintf(msg, sizeof msg, "encoding.%s: %s at byte %zu", fn, what, at + 1);
    return sea_make_error_str(vm, msg);
}

/* ======================================================================== */
/* encoding                                                                 */
/* ======================================================================== */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char B64URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/* base64_encode(s [, opts]): opts url (URL-safe alphabet, no padding unless
 * pad = true), pad (default true for the standard alphabet) */
static SeaValue e_b64_encode(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *s = str_arg(vm, "encoding", "base64_encode", argc, args, 0);
    SeaValue opts = argc > 1 ? args[1] : SEA_NIL_VAL;
    SeaValue v;
    bool url = opt_get(opts, "url", &v) && sea_is_bool(v) && sea_as_bool(v);
    bool pad = !url;
    if (opt_get(opts, "pad", &v) && sea_is_bool(v)) pad = sea_as_bool(v);
    const char *A = url ? B64URL : B64;
    const unsigned char *u = (const unsigned char *)s->data;
    size_t n = s->len;
    size_t outlen = pad ? (n + 2) / 3 * 4 : (n * 4 + 2) / 3;
    SeaString *r = sea_string_alloc(outlen);
    char *w = r->data;
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t x = (uint32_t)u[i] << 16 | (uint32_t)u[i + 1] << 8 | u[i + 2];
        *w++ = A[x >> 18]; *w++ = A[(x >> 12) & 63]; *w++ = A[(x >> 6) & 63]; *w++ = A[x & 63];
    }
    if (i < n) {
        uint32_t x = (uint32_t)u[i] << 16 | (i + 1 < n ? (uint32_t)u[i + 1] << 8 : 0);
        *w++ = A[x >> 18];
        *w++ = A[(x >> 12) & 63];
        if (i + 1 < n) *w++ = A[(x >> 6) & 63];
        else if (pad) *w++ = '=';
        if (pad) *w++ = '=';
    }
    *w = '\0';
    sea_obj_release(&s->obj);
    return sea_obj_val(&r->obj);
}

/* base64_decode(s): either alphabet, padding optional, whitespace ignored */
static SeaValue e_b64_decode(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *s = str_arg(vm, "encoding", "base64_decode", argc, args, 0);
    Out o = { 0 };
    uint32_t acc = 0;
    int bits = 0, pad = 0;
    size_t i;
    SeaValue err = SEA_NIL_VAL;
    for (i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)s->data[i];
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == '=') { pad++; continue; }
        else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        else { err = dec_err(vm, "base64_decode", "invalid character", i); break; }
        if (pad) { err = dec_err(vm, "base64_decode", "data after padding", i); break; }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out_putc(&o, (char)((acc >> bits) & 0xFF));
        }
    }
    if (sea_is_nil(err) && (bits >= 6 || pad > 2))
        err = dec_err(vm, "base64_decode", "truncated input", s->len ? s->len - 1 : 0);
    sea_obj_release(&s->obj);
    if (!sea_is_nil(err)) { free(o.d); return err; }
    return out_value(&o);
}

static const char B32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

static SeaValue e_b32_encode(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *s = str_arg(vm, "encoding", "base32_encode", argc, args, 0);
    bool pad = flag_arg(argc, args, 1, "pad", true);
    Out o = { 0 };
    const unsigned char *u = (const unsigned char *)s->data;
    uint64_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < s->len; i++) {
        acc = (acc << 8) | u[i];
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out_putc(&o, B32[(acc >> bits) & 31]);
        }
    }
    if (bits > 0) out_putc(&o, B32[(acc << (5 - bits)) & 31]);
    if (pad) while (o.len % 8) out_putc(&o, '=');
    sea_obj_release(&s->obj);
    return out_value(&o);
}

static SeaValue e_b32_decode(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *s = str_arg(vm, "encoding", "base32_decode", argc, args, 0);
    Out o = { 0 };
    uint64_t acc = 0;
    int bits = 0;
    bool padded = false;
    SeaValue err = SEA_NIL_VAL;
    for (size_t i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)s->data[i];
        int v;
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= '2' && c <= '7') v = c - '2' + 26;
        else if (c == '=') { padded = true; continue; }
        else if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '-') continue;
        else { err = dec_err(vm, "base32_decode", "invalid character", i); break; }
        if (padded) { err = dec_err(vm, "base32_decode", "data after padding", i); break; }
        acc = (acc << 5) | (uint64_t)v;
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            out_putc(&o, (char)((acc >> bits) & 0xFF));
        }
    }
    sea_obj_release(&s->obj);
    if (!sea_is_nil(err)) { free(o.d); return err; }
    return out_value(&o);
}

static const char HEXL[] = "0123456789abcdef";
static const char HEXU[] = "0123456789ABCDEF";

static void hex_into(char *w, const unsigned char *u, size_t n, bool upper) {
    const char *H = upper ? HEXU : HEXL;
    for (size_t i = 0; i < n; i++) { w[2 * i] = H[u[i] >> 4]; w[2 * i + 1] = H[u[i] & 15]; }
}

static SeaValue e_hex_encode(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *s = str_arg(vm, "encoding", "hex_encode", argc, args, 0);
    bool upper = flag_arg(argc, args, 1, "upper", false);
    SeaString *r = sea_string_alloc((size_t)s->len * 2);
    hex_into(r->data, (const unsigned char *)s->data, s->len, upper);
    r->data[(size_t)s->len * 2] = '\0';
    sea_obj_release(&s->obj);
    return sea_obj_val(&r->obj);
}

static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static SeaValue e_hex_decode(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *s = str_arg(vm, "encoding", "hex_decode", argc, args, 0);
    Out o = { 0 };
    SeaValue err = SEA_NIL_VAL;
    int hi = -1;
    size_t i;
    for (i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)s->data[i];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == ':') {
            if (hi >= 0) { err = dec_err(vm, "hex_decode", "odd number of digits", i); break; }
            continue;
        }
        int v = hexv(c);
        if (v < 0) { err = dec_err(vm, "hex_decode", "invalid hex digit", i); break; }
        if (hi < 0) hi = v;
        else { out_putc(&o, (char)(hi << 4 | v)); hi = -1; }
    }
    if (sea_is_nil(err) && hi >= 0) err = dec_err(vm, "hex_decode", "odd number of digits", s->len - 1);
    sea_obj_release(&s->obj);
    if (!sea_is_nil(err)) { free(o.d); return err; }
    return out_value(&o);
}

/* url_encode(s [, opts]): percent-encode all but the RFC 3986 unreserved
 * characters (A-Z a-z 0-9 - . _ ~); opts.plus: spaces as '+' (forms) */
static SeaValue e_url_encode(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *s = str_arg(vm, "encoding", "url_encode", argc, args, 0);
    bool plus = flag_arg(argc, args, 1, "plus", false);
    Out o = { 0 };
    for (size_t i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)s->data[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '.' || c == '_' || c == '~') out_putc(&o, (char)c);
        else if (c == ' ' && plus) out_putc(&o, '+');
        else {
            char t[3] = { '%', HEXU[c >> 4], HEXU[c & 15] };
            out_push(&o, t, 3);
        }
    }
    sea_obj_release(&s->obj);
    return out_value(&o);
}

/* url_decode(s [, opts]): %XX sequences; opts.plus: '+' is a space */
static SeaValue e_url_decode(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *s = str_arg(vm, "encoding", "url_decode", argc, args, 0);
    bool plus = flag_arg(argc, args, 1, "plus", false);
    Out o = { 0 };
    SeaValue err = SEA_NIL_VAL;
    for (size_t i = 0; i < s->len; i++) {
        char c = s->data[i];
        if (c == '%') {
            int h = i + 2 < s->len ? hexv((unsigned char)s->data[i + 1]) : -1;
            int l = h >= 0 ? hexv((unsigned char)s->data[i + 2]) : -1;
            if (l < 0) { err = dec_err(vm, "url_decode", "invalid %-escape", i); break; }
            out_putc(&o, (char)(h << 4 | l));
            i += 2;
        } else if (c == '+' && plus) {
            out_putc(&o, ' ');
        } else {
            out_putc(&o, c);
        }
    }
    sea_obj_release(&s->obj);
    if (!sea_is_nil(err)) { free(o.d); return err; }
    return out_value(&o);
}

/* html_escape(s): & < > " ' as entities */
static SeaValue e_html_escape(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *s = str_arg(vm, "encoding", "html_escape", argc, args, 0);
    Out o = { 0 };
    for (size_t i = 0; i < s->len; i++) {
        switch (s->data[i]) {
            case '&':  out_push(&o, "&amp;", 5); break;
            case '<':  out_push(&o, "&lt;", 4); break;
            case '>':  out_push(&o, "&gt;", 4); break;
            case '"':  out_push(&o, "&quot;", 6); break;
            case '\'': out_push(&o, "&#39;", 5); break;
            default:   out_putc(&o, s->data[i]); break;
        }
    }
    sea_obj_release(&s->obj);
    return out_value(&o);
}

/* ======================================================================== */
/* Digests                                                                  */
/* ======================================================================== */

static inline uint32_t rol32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static inline uint32_t ror32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static inline uint64_t ror64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

static inline uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline uint32_t le32(const unsigned char *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static inline uint64_t be64(const unsigned char *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

enum { H_MD5, H_SHA1, H_SHA224, H_SHA256, H_SHA384, H_SHA512 };

static const struct { const char *name; int block, size; } ALGS[] = {
    { "md5", 64, 16 }, { "sha1", 64, 20 }, { "sha224", 64, 28 },
    { "sha256", 64, 32 }, { "sha384", 128, 48 }, { "sha512", 128, 64 },
};

/* One state for every algorithm: plain data, so a hasher can live in a string. */
typedef struct {
    uint32_t magic;
    int32_t  alg;
    uint64_t total;          /* bytes hashed */
    uint32_t h32[8];
    uint64_t h64[8];
    unsigned char buf[128];
    uint32_t buflen;
} HashState;

#define HASH_MAGIC 0x48415348u   /* "HASH" */

static void md5_block(HashState *st, const unsigned char *p) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
    static const int R[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                               5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                               4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                               6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
    uint32_t M[16];
    for (int i = 0; i < 16; i++) M[i] = le32(p + 4 * i);
    uint32_t a = st->h32[0], b = st->h32[1], c = st->h32[2], d = st->h32[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
        else { f = c ^ (b | ~d); g = (7 * i) % 16; }
        uint32_t t = d;
        d = c;
        c = b;
        b = b + rol32(a + f + K[i] + M[g], R[i]);
        a = t;
    }
    st->h32[0] += a; st->h32[1] += b; st->h32[2] += c; st->h32[3] += d;
}

static void sha1_block(HashState *st, const unsigned char *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = be32(p + 4 * i);
    for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = st->h32[0], b = st->h32[1], c = st->h32[2], d = st->h32[3], e = st->h32[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol32(b, 30); b = a; a = t;
    }
    st->h32[0] += a; st->h32[1] += b; st->h32[2] += c; st->h32[3] += d; st->h32[4] += e;
}

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

static void sha256_block(HashState *st, const unsigned char *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = be32(p + 4 * i);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = st->h32[0], b = st->h32[1], c = st->h32[2], d = st->h32[3];
    uint32_t e = st->h32[4], f = st->h32[5], g = st->h32[6], h = st->h32[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    st->h32[0] += a; st->h32[1] += b; st->h32[2] += c; st->h32[3] += d;
    st->h32[4] += e; st->h32[5] += f; st->h32[6] += g; st->h32[7] += h;
}

static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL };

static void sha512_block(HashState *st, const unsigned char *p) {
    uint64_t w[80];
    for (int i = 0; i < 16; i++) w[i] = be64(p + 8 * i);
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = st->h64[0], b = st->h64[1], c = st->h64[2], d = st->h64[3];
    uint64_t e = st->h64[4], f = st->h64[5], g = st->h64[6], h = st->h64[7];
    for (int i = 0; i < 80; i++) {
        uint64_t S1 = ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41);
        uint64_t ch = (e & f) ^ (~e & g);
        uint64_t t1 = h + S1 + ch + K512[i] + w[i];
        uint64_t S0 = ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39);
        uint64_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint64_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    st->h64[0] += a; st->h64[1] += b; st->h64[2] += c; st->h64[3] += d;
    st->h64[4] += e; st->h64[5] += f; st->h64[6] += g; st->h64[7] += h;
}

static void hash_init(HashState *st, int alg) {
    memset(st, 0, sizeof *st);
    st->magic = HASH_MAGIC;
    st->alg = alg;
    static const uint32_t I256[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    static const uint32_t I224[8] = { 0xc1059ed8, 0x367cd507, 0x3070dd17, 0xf70e5939, 0xffc00b31, 0x68581511, 0x64f98fa7, 0xbefa4fa4 };
    static const uint64_t I512[8] = { 0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
                                      0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL };
    static const uint64_t I384[8] = { 0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL, 0x152fecd8f70e5939ULL,
                                      0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL, 0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL };
    switch (alg) {
        case H_MD5:    st->h32[0] = 0x67452301; st->h32[1] = 0xefcdab89; st->h32[2] = 0x98badcfe; st->h32[3] = 0x10325476; break;
        case H_SHA1:   st->h32[0] = 0x67452301; st->h32[1] = 0xEFCDAB89; st->h32[2] = 0x98BADCFE; st->h32[3] = 0x10325476; st->h32[4] = 0xC3D2E1F0; break;
        case H_SHA224: memcpy(st->h32, I224, sizeof I224); break;
        case H_SHA256: memcpy(st->h32, I256, sizeof I256); break;
        case H_SHA384: memcpy(st->h64, I384, sizeof I384); break;
        case H_SHA512: memcpy(st->h64, I512, sizeof I512); break;
    }
}

static void hash_block(HashState *st, const unsigned char *p) {
    switch (st->alg) {
        case H_MD5: md5_block(st, p); break;
        case H_SHA1: sha1_block(st, p); break;
        case H_SHA224: case H_SHA256: sha256_block(st, p); break;
        default: sha512_block(st, p); break;
    }
}

static void hash_update(HashState *st, const unsigned char *p, size_t n) {
    size_t bs = (size_t)ALGS[st->alg].block;
    st->total += n;
    if (st->buflen) {
        size_t take = bs - st->buflen < n ? bs - st->buflen : n;
        memcpy(st->buf + st->buflen, p, take);
        st->buflen += (uint32_t)take;
        p += take;
        n -= take;
        if (st->buflen == bs) { hash_block(st, st->buf); st->buflen = 0; }
    }
    while (n >= bs) { hash_block(st, p); p += bs; n -= bs; }
    if (n) { memcpy(st->buf, p, n); st->buflen = (uint32_t)n; }
}

/* Finish a copy of st into out (ALGS[alg].size bytes). */
static void hash_final(const HashState *in, unsigned char *out) {
    HashState st = *in;
    size_t bs = (size_t)ALGS[st.alg].block;
    size_t lenbytes = bs == 128 ? 16 : 8;
    uint64_t bits = st.total * 8;
    unsigned char pad[256];
    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    size_t padlen = (st.buflen < bs - lenbytes) ? bs - lenbytes - st.buflen : 2 * bs - lenbytes - st.buflen;
    unsigned char lenb[16];
    memset(lenb, 0, sizeof lenb);
    if (st.alg == H_MD5) for (int i = 0; i < 8; i++) lenb[i] = (unsigned char)(bits >> (8 * i));
    else for (int i = 0; i < 8; i++) lenb[lenbytes - 1 - (size_t)i] = (unsigned char)(bits >> (8 * i));
    uint64_t total = st.total;
    hash_update(&st, pad, padlen);
    hash_update(&st, lenb, lenbytes);
    (void)total;
    int size = ALGS[st.alg].size;
    if (st.alg == H_MD5) {
        for (int i = 0; i < 4; i++) for (int k = 0; k < 4; k++) out[4 * i + k] = (unsigned char)(st.h32[i] >> (8 * k));
    } else if (st.alg <= H_SHA256) {
        for (int i = 0; i < size / 4; i++) for (int k = 0; k < 4; k++) out[4 * i + k] = (unsigned char)(st.h32[i] >> (24 - 8 * k));
    } else {
        for (int i = 0; i < size / 8; i++) for (int k = 0; k < 8; k++) out[8 * i + k] = (unsigned char)(st.h64[i] >> (56 - 8 * k));
    }
}

static int alg_of(const char *name) {
    for (int i = 0; i < (int)(sizeof ALGS / sizeof *ALGS); i++) {
        const char *a = ALGS[i].name;
        /* "sha256", "SHA256" and "sha-256" all name it */
        size_t j = 0, k = 0;
        bool ok = true;
        while (name[j] || a[k]) {
            char c = name[j];
            if (c == '-') { j++; continue; }
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            if (c != a[k]) { ok = false; break; }
            j++;
            k++;
        }
        if (ok) return i;
    }
    return -1;
}

static SeaValue digest_value(const unsigned char *d, int size, bool raw) {
    if (raw) return new_str((const char *)d, (size_t)size);
    char hex[129];
    hex_into(hex, d, (size_t)size, false);
    return new_str(hex, (size_t)size * 2);
}

static SeaValue digest_of(SeaVM *vm, int alg, int argc, SeaValue *args) {
    SeaString *s = str_arg(vm, "hash", ALGS[alg].name, argc, args, 0);
    bool raw = flag_arg(argc, args, 1, "raw", false);
    HashState st;
    hash_init(&st, alg);
    hash_update(&st, (const unsigned char *)s->data, s->len);
    unsigned char d[64];
    hash_final(&st, d);
    sea_obj_release(&s->obj);
    return digest_value(d, ALGS[alg].size, raw);
}

static SeaValue h_md5(SeaVM *vm, int argc, SeaValue *args)    { return digest_of(vm, H_MD5, argc, args); }
static SeaValue h_sha1(SeaVM *vm, int argc, SeaValue *args)   { return digest_of(vm, H_SHA1, argc, args); }
static SeaValue h_sha224(SeaVM *vm, int argc, SeaValue *args) { return digest_of(vm, H_SHA224, argc, args); }
static SeaValue h_sha256(SeaVM *vm, int argc, SeaValue *args) { return digest_of(vm, H_SHA256, argc, args); }
static SeaValue h_sha384(SeaVM *vm, int argc, SeaValue *args) { return digest_of(vm, H_SHA384, argc, args); }
static SeaValue h_sha512(SeaVM *vm, int argc, SeaValue *args) { return digest_of(vm, H_SHA512, argc, args); }

static int alg_arg(SeaVM *vm, const char *fn, int argc, SeaValue *args, int i) {
    SeaString *n = argc > i ? sea_value_get_flat_string(args[i]) : NULL;
    int alg = n ? alg_of(n->data) : -1;
    if (n) sea_obj_release(&n->obj);
    if (alg < 0) sea_vm_error_noreturn(vm, "hash.%s: unknown algorithm (md5, sha1, sha224, sha256, sha384, sha512)", fn);
    return alg;
}

/* hash.hmac(alg, key, data [, raw]) (RFC 2104) */
static SeaValue h_hmac(SeaVM *vm, int argc, SeaValue *args) {
    int alg = alg_arg(vm, "hmac", argc, args, 0);
    SeaString *key = str_arg(vm, "hash", "hmac", argc, args, 1);
    SeaString *data = argc > 2 ? sea_value_get_flat_string(args[2]) : NULL;
    if (!data) { sea_obj_release(&key->obj); sea_vm_error_noreturn(vm, "hash.hmac: data (string) expected"); }
    bool raw = flag_arg(argc, args, 3, "raw", false);
    size_t bs = (size_t)ALGS[alg].block;
    unsigned char k[128], ipad[128], opad[128], inner[64], out[64];
    memset(k, 0, sizeof k);
    if (key->len > bs) {
        HashState kh;
        hash_init(&kh, alg);
        hash_update(&kh, (const unsigned char *)key->data, key->len);
        hash_final(&kh, k);
    } else {
        memcpy(k, key->data, key->len);
    }
    for (size_t i = 0; i < bs; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    HashState st;
    hash_init(&st, alg);
    hash_update(&st, ipad, bs);
    hash_update(&st, (const unsigned char *)data->data, data->len);
    hash_final(&st, inner);
    hash_init(&st, alg);
    hash_update(&st, opad, bs);
    hash_update(&st, inner, (size_t)ALGS[alg].size);
    hash_final(&st, out);
    sea_obj_release(&key->obj);
    sea_obj_release(&data->obj);
    return digest_value(out, ALGS[alg].size, raw);
}

/* ---- incremental hashers: the state lives in a string field -------------- */

static HashState *state_of(SeaVM *vm, const char *fn, int argc, SeaValue *args, SeaString **blob) {
    SeaValue v = SEA_NIL_VAL;
    if (argc < 1 || !sea_is_table(args[0]) || !sea_table_get_str(SEA_AS_TABLE(args[0]), "__state", &v) ||
        !sea_is_string(v) || SEA_AS_STRING(v)->len != sizeof(HashState))
        sea_vm_error_noreturn(vm, "hash.%s: not a hasher from hash.new", fn);
    *blob = SEA_AS_STRING(v);
    HashState *st = (HashState *)(void *)(*blob)->data;
    if (st->magic != HASH_MAGIC || st->alg < 0 || st->alg > H_SHA512 || st->buflen >= 128)
        sea_vm_error_noreturn(vm, "hash.%s: not a hasher from hash.new", fn);
    return st;
}

static SeaValue hs_update(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *blob;
    HashState *cur = state_of(vm, "update", argc, args, &blob);
    SeaString *s = str_arg(vm, "hash", "update", argc, args, 1);
    SeaString *next = sea_string_alloc(sizeof(HashState));
    memcpy(next->data, cur, sizeof(HashState));
    next->data[sizeof(HashState)] = '\0';
    hash_update((HashState *)(void *)next->data, (const unsigned char *)s->data, s->len);
    sea_obj_release(&s->obj);
    tset(SEA_AS_TABLE(args[0]), "__state", sea_obj_val(&next->obj));
    return sea_val_ref(args[0]);                       /* chainable: h:update(a):update(b) */
}

static SeaValue hs_digest(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *blob;
    HashState *st = state_of(vm, "digest", argc, args, &blob);
    bool raw = flag_arg(argc, args, 1, "raw", false);
    unsigned char d[64];
    hash_final(st, d);
    return digest_value(d, ALGS[st->alg].size, raw);
}

/* hash.new(alg) → hasher: h:update(s) (returns h), h:digest([raw]) (the
 * hasher can go on after a digest) */
static SeaValue h_new(SeaVM *vm, int argc, SeaValue *args) {
    int alg = alg_arg(vm, "new", argc, args, 0);
    SeaString *blob = sea_string_alloc(sizeof(HashState));
    hash_init((HashState *)(void *)blob->data, alg);
    blob->data[sizeof(HashState)] = '\0';
    SeaTable *t = sea_table_new(TABLE_MAP);
    tset(t, "algorithm", new_str(ALGS[alg].name, strlen(ALGS[alg].name)));
    tset(t, "size", sea_int_val(ALGS[alg].size));
    tset(t, "__state", sea_obj_val(&blob->obj));
    mod_set(vm, t, "update", hs_update);
    mod_set(vm, t, "digest", hs_digest);
    return sea_obj_val(&t->obj);
}

/* ---- checksums ------------------------------------------------------------------ */

static SeaValue h_crc32(SeaVM *vm, int argc, SeaValue *args) {
    static uint32_t table[256];
    static int ready;
    if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        __atomic_store_n(&ready, 1, __ATOMIC_RELEASE);
    }
    SeaString *s = str_arg(vm, "hash", "crc32", argc, args, 0);
    uint32_t crc = 0xFFFFFFFFu;
    if (argc > 1 && sea_is_int(args[1])) crc = ~(uint32_t)sea_as_int(args[1]);   /* continue a running crc */
    for (size_t i = 0; i < s->len; i++) crc = table[(crc ^ (unsigned char)s->data[i]) & 0xFF] ^ (crc >> 8);
    sea_obj_release(&s->obj);
    return sea_int_val((int64_t)(crc ^ 0xFFFFFFFFu));
}

static SeaValue h_adler32(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *s = str_arg(vm, "hash", "adler32", argc, args, 0);
    uint32_t a = 1, b = 0;
    if (argc > 1 && sea_is_int(args[1])) { a = (uint32_t)sea_as_int(args[1]) & 0xFFFF; b = ((uint32_t)sea_as_int(args[1]) >> 16) & 0xFFFF; }
    for (size_t i = 0; i < s->len; i++) {
        a = (a + (unsigned char)s->data[i]) % 65521;
        b = (b + a) % 65521;
    }
    sea_obj_release(&s->obj);
    return sea_int_val((int64_t)(b << 16 | a));
}

static SeaValue h_fnv1a(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *s = str_arg(vm, "hash", "fnv1a", argc, args, 0);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < s->len; i++) { h ^= (unsigned char)s->data[i]; h *= 16777619u; }
    sea_obj_release(&s->obj);
    return sea_int_val((int64_t)h);
}

/* hash.equal(a, b): compare in time that depends only on the lengths (for
 * checking MACs and tokens) */
static SeaValue h_equal(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *a = str_arg(vm, "hash", "equal", argc, args, 0);
    SeaString *b = argc > 1 ? sea_value_get_flat_string(args[1]) : NULL;
    if (!b) { sea_obj_release(&a->obj); sea_vm_error_noreturn(vm, "hash.equal: two strings expected"); }
    unsigned char diff = a->len != b->len;
    size_t n = a->len < b->len ? a->len : b->len;
    for (size_t i = 0; i < n; i++) diff |= (unsigned char)(a->data[i] ^ b->data[i]);
    sea_obj_release(&a->obj);
    sea_obj_release(&b->obj);
    return sea_bool_val(diff == 0);
}

/* ---- Registration ------------------------------------------------------------ */

void sea_lib_hash_register(SeaVM *vm) {
    SeaTable *enc = sea_table_new(TABLE_MAP);
    mod_set(vm, enc, "base64_encode", e_b64_encode);
    mod_set(vm, enc, "base64_decode", e_b64_decode);
    mod_set(vm, enc, "base32_encode", e_b32_encode);
    mod_set(vm, enc, "base32_decode", e_b32_decode);
    mod_set(vm, enc, "hex_encode",    e_hex_encode);
    mod_set(vm, enc, "hex_decode",    e_hex_decode);
    mod_set(vm, enc, "url_encode",    e_url_encode);
    mod_set(vm, enc, "url_decode",    e_url_decode);
    mod_set(vm, enc, "html_escape",   e_html_escape);
    sea_vm_register_module(vm, "encoding", enc);
    sea_obj_release(&enc->obj);

    SeaTable *h = sea_table_new(TABLE_MAP);
    mod_set(vm, h, "md5",     h_md5);
    mod_set(vm, h, "sha1",    h_sha1);
    mod_set(vm, h, "sha224",  h_sha224);
    mod_set(vm, h, "sha256",  h_sha256);
    mod_set(vm, h, "sha384",  h_sha384);
    mod_set(vm, h, "sha512",  h_sha512);
    mod_set(vm, h, "hmac",    h_hmac);
    mod_set(vm, h, "new",     h_new);
    mod_set(vm, h, "crc32",   h_crc32);
    mod_set(vm, h, "adler32", h_adler32);
    mod_set(vm, h, "fnv1a",   h_fnv1a);
    mod_set(vm, h, "equal",   h_equal);
    sea_vm_register_module(vm, "hash", h);
    sea_obj_release(&h->obj);
}
