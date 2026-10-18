/*
 * Copyright (c) 2026 Daniel
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * sea_lib_utf8.c — the built-in `utf8` module: strings read as UTF-8.
 *
 * The string module works on bytes; these functions work on characters
 * (code points). Character indexes are 1-based like string.sub's, and
 * negative ones count from the end. An invalid byte counts as one character
 * (utf8.valid / utf8.invalid find them, utf8.sanitize replaces them).
 *
 *   utf8.len(s) → int                     utf8.valid(s) → bool
 *   utf8.invalid(s) → int | nil           (byte position of the first invalid byte)
 *   utf8.char(cp, ...) / utf8.char(array) → string
 *   utf8.codepoint(s [, i]) → int | nil   utf8.codepoints(s) → array
 *   utf8.chars(s) → array of strings      utf8.sub(s, i [, j]) → string
 *   utf8.offset(s, n) → int | nil         (byte position of the n-th character)
 *   utf8.upper(s) / utf8.lower(s) / utf8.reverse(s) → string
 *   utf8.width(s) → int                   (terminal columns)
 *   utf8.sanitize(s [, repl]) → string
 */

#include "sea_utf8.h"
#include "sea_array.h"
#include "sea_rope.h"
#include "sea_typed_array.h"
#include "sea_stdlib.h"
#include "sea_vm.h"

#include <string.h>

/* The string argument i, flattened (+1: release it), or a runtime error. */
static SeaString *str_arg(SeaVM *vm, const char *fn, int argc, SeaValue *args, int i) {
    SeaString *s = argc > i ? sea_value_get_flat_string(args[i]) : NULL;
    if (!s)
        sea_vm_error_noreturn(vm, "utf8.%s: string expected, got %s", fn,
                              argc > i ? sea_type_name(args[i]) : "nothing");
    return s;
}

static int64_t int_arg(SeaVM *vm, const char *fn, int argc, SeaValue *args, int i, int64_t def) {
    if (argc <= i || sea_is_nil(args[i])) return def;
    if (sea_is_int(args[i])) return sea_as_int(args[i]);
    if (sea_is_number(args[i])) {
        double d = sea_as_number(args[i]);
        if (d == (double)(int64_t)d) return (int64_t)d;
    }
    sea_vm_error_noreturn(vm, "utf8.%s: integer expected, got %s", fn, sea_type_name(args[i]));
    return def;
}

static SeaValue new_str(const char *s, size_t n) { return sea_obj_val(&sea_string_new(s, n)->obj); }

/* A growable byte buffer for building results. */
typedef struct { char *data; size_t len, cap; } Out;

static void out_push(Out *o, const char *s, size_t n) {
    if (o->len + n > o->cap) {
        size_t nc = o->cap ? o->cap * 2 : 64;
        while (nc < o->len + n) nc *= 2;
        o->data = SEA_REALLOC_N(o->data, char, o->cap, nc);
        o->cap = nc;
    }
    memcpy(o->data + o->len, s, n);
    o->len += n;
}

static SeaValue out_string(Out *o) {
    SeaValue v = new_str(o->data ? o->data : "", o->len);
    if (o->data) SEA_FREE_N(o->data, char, o->cap);
    return v;
}

/* The code point at byte i (U+FFFD for an invalid byte) and its length. */
static uint32_t cp_at(const SeaString *s, size_t i, size_t *len) {
    uint32_t cp;
    int k = sea_utf8_decode((const unsigned char *)s->data + i, s->len - i, &cp);
    if (!k) { *len = 1; return 0xFFFD; }
    *len = (size_t)k;
    return cp;
}

/* Byte offset of the start of character index c (0-based), or s->len. */
static size_t char_offset(const SeaString *s, size_t c) {
    size_t i = 0;
    while (c-- > 0 && i < s->len) i = sea_utf8_next(s->data, s->len, i);
    return i;
}

static SeaValue u_len(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *s = str_arg(vm, "len", argc, args, 0);
    size_t n = sea_utf8_len(s->data, s->len);
    sea_obj_release(&s->obj);
    return sea_int_val((int64_t)n);
}

static SeaValue u_valid(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *s = str_arg(vm, "valid", argc, args, 0);
    bool ok = sea_utf8_check(s->data, s->len) == s->len;
    sea_obj_release(&s->obj);
    return sea_bool_val(ok);
}

static SeaValue u_invalid(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *s = str_arg(vm, "invalid", argc, args, 0);
    size_t at = sea_utf8_check(s->data, s->len);
    SeaValue r = at == s->len ? SEA_NIL_VAL : sea_int_val((int64_t)at + 1);
    sea_obj_release(&s->obj);
    return r;
}

static void push_cp(SeaVM *vm, Out *o, SeaValue v) {
    char buf[4];
    int64_t cp = -1;
    if (sea_is_int(v)) cp = sea_as_int(v);
    else if (sea_is_number(v) && sea_as_number(v) == (double)(int64_t)sea_as_number(v)) cp = (int64_t)sea_as_number(v);
    else {
        if (o->data) SEA_FREE_N(o->data, char, o->cap);
        sea_vm_error_noreturn(vm, "utf8.char: integer expected, got %s", sea_type_name(v));
    }
    int n = cp >= 0 && cp <= 0x10FFFF ? sea_utf8_encode((uint32_t)cp, buf) : 0;
    if (!n) {
        if (o->data) SEA_FREE_N(o->data, char, o->cap);
        sea_vm_error_noreturn(vm, "utf8.char: invalid code point %lld", (long long)cp);
    }
    out_push(o, buf, (size_t)n);
}

/* utf8.char(cp, ...) or utf8.char(array) */
static SeaValue u_fromcp(SeaVM *vm, int argc, SeaValue *args) {
    Out o = { 0 };
    if (argc == 1 && sea_is_array(args[0])) {
        SeaArray *a = SEA_AS_ARRAY(args[0]);
        for (uint32_t i = 0; i < sea_array_len(a); i++) push_cp(vm, &o, sea_array_get(a, (int64_t)i));
    } else if (argc == 1 && sea_is_int_array(args[0])) {
        SeaIntArray *a = SEA_AS_INT_ARRAY(args[0]);
        for (uint32_t i = 0; i < a->len; i++) push_cp(vm, &o, sea_int_val(a->data[i]));
    } else {
        for (int i = 0; i < argc; i++) push_cp(vm, &o, args[i]);
    }
    return out_string(&o);
}

/* Character index i (1-based, negative from the end) → 0-based, or -1 when
 * outside [1, n]. */
static int64_t norm_index(int64_t i, size_t n) {
    if (i < 0) i += (int64_t)n + 1;
    if (i < 1 || i > (int64_t)n) return -1;
    return i - 1;
}

/* utf8.codepoint(s [, i]) → the code point of character i (default 1), or nil */
static SeaValue u_codepoint(SeaVM *vm, int argc, SeaValue *args) {
    int64_t i = int_arg(vm, "codepoint", argc, args, 1, 1);
    SeaString *s = str_arg(vm, "codepoint", argc, args, 0);
    SeaValue r = SEA_NIL_VAL;
    int64_t c = i < 0 ? norm_index(i, sea_utf8_len(s->data, s->len)) : i - 1;
    if (c >= 0) {
        size_t at = char_offset(s, (size_t)c);
        if (at < s->len) {
            uint32_t cp;
            if (sea_utf8_decode((const unsigned char *)s->data + at, s->len - at, &cp)) r = sea_int_val(cp);
        }
    }
    sea_obj_release(&s->obj);
    return r;
}

/* utf8.codepoints(s) → array of code points (U+FFFD for invalid bytes) */
static SeaValue u_codepoints(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *s = str_arg(vm, "codepoints", argc, args, 0);
    SeaArray *a = sea_array_new(s->len < 16 ? 4 : (uint32_t)(s->len / 2));
    for (size_t i = 0, k; i < s->len; i += k) sea_array_push(a, sea_int_val(cp_at(s, i, &k)));
    sea_obj_release(&s->obj);
    return sea_obj_val(&a->obj);
}

/* utf8.chars(s) → array of one-character strings */
static SeaValue u_fromcps(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *s = str_arg(vm, "chars", argc, args, 0);
    SeaArray *a = sea_array_new(s->len < 16 ? 4 : (uint32_t)(s->len / 2));
    for (size_t i = 0; i < s->len;) {
        size_t j = sea_utf8_next(s->data, s->len, i);
        SeaValue c = new_str(s->data + i, j - i);
        sea_array_push(a, c);
        sea_obj_release(sea_as_obj(c));
        i = j;
    }
    sea_obj_release(&s->obj);
    return sea_obj_val(&a->obj);
}

/* utf8.sub(s, i [, j]): characters i..j (inclusive, 1-based, negative from the end) */
static SeaValue u_sub(SeaVM *vm, int argc, SeaValue *args) {
    int64_t i = int_arg(vm, "sub", argc, args, 1, 1);
    int64_t j = int_arg(vm, "sub", argc, args, 2, -1);
    SeaString *s = str_arg(vm, "sub", argc, args, 0);
    int64_t n = (int64_t)sea_utf8_len(s->data, s->len);
    if (i < 0) i += n + 1;
    if (j < 0) j += n + 1;
    if (i < 1) i = 1;
    if (j > n) j = n;
    SeaValue r;
    if (i > j) {
        r = new_str("", 0);
    } else {
        size_t from = char_offset(s, (size_t)(i - 1));
        size_t to = from;
        for (int64_t k = i; k <= j; k++) to = sea_utf8_next(s->data, s->len, to);
        if (from == 0 && to == s->len) r = sea_val_ref(sea_obj_val(&s->obj));
        else r = new_str(s->data + from, to - from);
    }
    sea_obj_release(&s->obj);
    return r;
}

/* utf8.offset(s, n) → byte position (1-based) where character n starts;
 * n = len + 1 gives the position just past the end; nil when out of range */
static SeaValue u_offset(SeaVM *vm, int argc, SeaValue *args) {
    int64_t c = int_arg(vm, "offset", argc, args, 1, 1);
    SeaString *s = str_arg(vm, "offset", argc, args, 0);
    int64_t n = (int64_t)sea_utf8_len(s->data, s->len);
    if (c < 0) c += n + 1;
    SeaValue r = SEA_NIL_VAL;
    if (c >= 1 && c <= n + 1) r = sea_int_val((int64_t)char_offset(s, (size_t)(c - 1)) + 1);
    sea_obj_release(&s->obj);
    return r;
}

static SeaValue map_case(SeaVM *vm, const char *fn, int argc, SeaValue *args, bool upper) {
    SeaString *s = str_arg(vm, fn, argc, args, 0);
    Out o = { 0 };
    for (size_t i = 0, k; i < s->len; i += k) {
        uint32_t cp;
        int d = sea_utf8_decode((const unsigned char *)s->data + i, s->len - i, &cp);
        if (!d) { k = 1; out_push(&o, s->data + i, 1); continue; }      /* invalid byte: kept */
        k = (size_t)d;
        uint32_t m = upper ? sea_utf8_upper(cp) : sea_utf8_lower(cp);
        char buf[4];
        if (m == cp) out_push(&o, s->data + i, k);
        else out_push(&o, buf, (size_t)sea_utf8_encode(m, buf));
    }
    sea_obj_release(&s->obj);
    return out_string(&o);
}

static SeaValue u_upper(SeaVM *vm, int argc, SeaValue *args) { return map_case(vm, "upper", argc, args, true); }
static SeaValue u_lower(SeaVM *vm, int argc, SeaValue *args) { return map_case(vm, "lower", argc, args, false); }

static SeaValue u_reverse(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *s = str_arg(vm, "reverse", argc, args, 0);
    SeaString *r = sea_string_alloc(s->len);
    size_t w = s->len;
    for (size_t i = 0; i < s->len;) {
        size_t j = sea_utf8_next(s->data, s->len, i);
        w -= j - i;
        memcpy(r->data + w, s->data + i, j - i);
        i = j;
    }
    r->data[s->len] = '\0';
    sea_obj_release(&s->obj);
    return sea_obj_val(&r->obj);
}

/* utf8.width(s): terminal columns (wide characters 2, combining marks 0,
 * control characters and invalid bytes 0) */
static SeaValue u_width(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *s = str_arg(vm, "width", argc, args, 0);
    int64_t w = 0;
    for (size_t i = 0, k; i < s->len; i += k) {
        uint32_t cp;
        int d = sea_utf8_decode((const unsigned char *)s->data + i, s->len - i, &cp);
        k = d ? (size_t)d : 1;
        if (!d) continue;
        int cw = sea_utf8_cpwidth(cp);
        if (cw > 0) w += cw;
    }
    sea_obj_release(&s->obj);
    return sea_int_val(w);
}

/* utf8.sanitize(s [, repl]): every invalid byte replaced by repl (default U+FFFD) */
static SeaValue u_sanitize(SeaVM *vm, int argc, SeaValue *args) {
    SeaString *repl = argc > 1 && !sea_is_nil(args[1]) ? str_arg(vm, "sanitize", argc, args, 1) : NULL;
    SeaString *s = str_arg(vm, "sanitize", argc, args, 0);    /* (an error here leaks repl: rare misuse) */
    size_t bad = sea_utf8_check(s->data, s->len);
    if (bad == s->len) {                                      /* valid: the same string (+1 already) */
        if (repl) sea_obj_release(&repl->obj);
        return sea_obj_val(&s->obj);
    }
    const char *rp = repl ? repl->data : "\xEF\xBF\xBD";
    size_t rn = repl ? repl->len : 3;
    Out o = { 0 };
    out_push(&o, s->data, bad);
    for (size_t i = bad; i < s->len;) {
        uint32_t cp;
        int d = sea_utf8_decode((const unsigned char *)s->data + i, s->len - i, &cp);
        if (d) { out_push(&o, s->data + i, (size_t)d); i += (size_t)d; }
        else { out_push(&o, rp, rn); i++; }
    }
    if (repl) sea_obj_release(&repl->obj);
    sea_obj_release(&s->obj);
    return out_string(&o);
}

void sea_lib_utf8_register(SeaVM *vm) {
    SeaTable *mod = sea_table_new(TABLE_MAP);
    mod_set(vm, mod, "len",        u_len);
    mod_set(vm, mod, "valid",      u_valid);
    mod_set(vm, mod, "invalid",    u_invalid);
    mod_set(vm, mod, "char",       u_fromcp);
    mod_set(vm, mod, "codepoint",  u_codepoint);
    mod_set(vm, mod, "codepoints", u_codepoints);
    mod_set(vm, mod, "chars",      u_fromcps);
    mod_set(vm, mod, "sub",        u_sub);
    mod_set(vm, mod, "offset",     u_offset);
    mod_set(vm, mod, "upper",      u_upper);
    mod_set(vm, mod, "lower",      u_lower);
    mod_set(vm, mod, "reverse",    u_reverse);
    mod_set(vm, mod, "width",      u_width);
    mod_set(vm, mod, "sanitize",   u_sanitize);
    sea_vm_register_module(vm, "utf8", mod);
    sea_obj_release(&mod->obj);
}
