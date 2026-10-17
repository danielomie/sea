/*
 * Copyright (c) 2026 Daniel
 * SPDX-License-Identifier: MIT
 */

/*
 * sea_lib_bigint.c — the built-in `bigint` module.
 *
 *   bigint.new(v [, base]) → bigint | error   v: int, integral number, string, bigint
 *   bigint.add/sub/mul/div/mod/pow(a, b) → bigint   (div and mod floor, like // and %)
 *   bigint.neg(a), bigint.abs(a) → bigint
 *   bigint.cmp(a, b) → -1 | 0 | 1
 *   bigint.tostring(a [, base]) → string
 *   bigint.toint(a) → int | nil (nil when outside Sea's int range)
 *   bigint.tonumber(a) → number
 *
 * Operands may be bigints, ints or integral numbers; results are bigints.
 * The arithmetic operators do not take bigints; comparisons, ==, tostring,
 * .. and sort do.
 */

#include "sea_bigint.h"
#include "sea_rope.h"
#include "sea_stdlib.h"
#include "sea_vm.h"

#include <math.h>
#include <string.h>

#define SEA_INT_MAX ((INT64_C(1) << 47) - 1)
#define SEA_INT_MIN (-(INT64_C(1) << 47))

static SeaValue big_val(SeaBigInt *b) { return sea_obj_val(&b->obj); }

static void need2(SeaVM *vm, const char *fn, int argc) {
    if (argc < 2) sea_vm_error_noreturn(vm, "bigint.%s: two values expected", fn);
}

static SeaValue arith(SeaVM *vm, const char *fn, char op, int argc, SeaValue *args) {
    need2(vm, fn, argc);
    const char *err = NULL;
    SeaBigInt *r = sea_bigint_arith(op, args[0], args[1], &err);
    if (!r) {
        if (strcmp(err, "integer expected") == 0) {
            SeaValue bad = (sea_is_bigint(args[0]) || sea_is_int(args[0]) ||
                            (sea_is_number(args[0]) && sea_as_number(args[0]) == trunc(sea_as_number(args[0]))))
                           ? args[1] : args[0];
            if (sea_is_number(bad))
                sea_vm_error_noreturn(vm, "bigint.%s: integer expected, got %g", fn, sea_as_number(bad));
            sea_vm_error_noreturn(vm, "bigint.%s: integer expected, got %s", fn, sea_type_name(bad));
        }
        sea_vm_error_noreturn(vm, "bigint.%s: %s", fn, err);
    }
    return big_val(r);
}

static SeaValue bi_add(SeaVM *vm, int argc, SeaValue *args) { return arith(vm, "add", '+', argc, args); }
static SeaValue bi_sub(SeaVM *vm, int argc, SeaValue *args) { return arith(vm, "sub", '-', argc, args); }
static SeaValue bi_mul(SeaVM *vm, int argc, SeaValue *args) { return arith(vm, "mul", '*', argc, args); }
static SeaValue bi_div(SeaVM *vm, int argc, SeaValue *args) { return arith(vm, "div", '/', argc, args); }
static SeaValue bi_mod(SeaVM *vm, int argc, SeaValue *args) { return arith(vm, "mod", '%', argc, args); }
static SeaValue bi_pow(SeaVM *vm, int argc, SeaValue *args) { return arith(vm, "pow", '^', argc, args); }

/* One integer operand as a bigint (+1), or a runtime error. */
static SeaBigInt *one(SeaVM *vm, const char *fn, int argc, SeaValue *args) {
    SeaBigInt *b;
    if (argc < 1 || !sea_bigint_of(args[0], &b)) {
        if (argc >= 1 && sea_is_number(args[0]))
            sea_vm_error_noreturn(vm, "bigint.%s: integer expected, got %g", fn, sea_as_number(args[0]));
        sea_vm_error_noreturn(vm, "bigint.%s: integer expected, got %s", fn,
                              argc < 1 ? "nothing" : sea_type_name(args[0]));
    }
    return b;
}

static SeaValue bi_neg(SeaVM *vm, int argc, SeaValue *args) {
    SeaBigInt *b = one(vm, "neg", argc, args);
    SeaBigInt *r = sea_bigint_negate(b, false);
    sea_obj_release(&b->obj);
    return big_val(r);
}

static SeaValue bi_abs(SeaVM *vm, int argc, SeaValue *args) {
    SeaBigInt *b = one(vm, "abs", argc, args);
    SeaBigInt *r = sea_bigint_negate(b, true);
    sea_obj_release(&b->obj);
    return big_val(r);
}

static SeaValue bi_cmp(SeaVM *vm, int argc, SeaValue *args) {
    need2(vm, "cmp", argc);
    int c;
    int k = sea_bigint_compare(args[0], args[1], &c);
    if (k < 0) sea_vm_error_noreturn(vm, "bigint.cmp: cannot compare %s with %s",
                                     sea_type_name(args[0]), sea_type_name(args[1]));
    if (k == 0) return SEA_NIL_VAL;                  /* NaN: unordered */
    return sea_int_val(c);
}

static int base_arg(SeaVM *vm, const char *fn, int argc, SeaValue *args, int i, int def) {
    if (argc <= i || sea_is_nil(args[i])) return def;
    if (!sea_is_int(args[i]) || sea_as_int(args[i]) < 2 || sea_as_int(args[i]) > 36)
        sea_vm_error_noreturn(vm, "bigint.%s: base must be an int from 2 to 36", fn);
    return (int)sea_as_int(args[i]);
}

/* bigint.new(v [, base]) */
static SeaValue bi_new(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1) sea_vm_error_noreturn(vm, "bigint.new: a value expected");
    SeaValue v = args[0];
    if (sea_is_string(v) || sea_is_rope(v)) {
        int base = base_arg(vm, "new", argc, args, 1, 0);
        SeaString *s = sea_value_get_flat_string(v);
        SeaBigInt *b = sea_bigint_parse(s->data, s->len, base);
        SeaValue r;
        if (b) {
            r = big_val(b);
        } else {
            char msg[160];
            snprintf(msg, sizeof msg, "bigint.new: not an integer: \"%.*s\"", s->len > 60 ? 60 : (int)s->len, s->data);
            r = sea_make_error_str(vm, msg);
        }
        sea_obj_release(&s->obj);
        return r;
    }
    if (sea_is_number(v)) {
        double d = sea_as_number(v);
        if (!isfinite(d) || d != trunc(d)) {
            char msg[96];
            snprintf(msg, sizeof msg, "bigint.new: not an integer: %g", d);
            return sea_make_error_str(vm, msg);
        }
    }
    SeaBigInt *b;
    if (!sea_bigint_of(v, &b))
        sea_vm_error_noreturn(vm, "bigint.new: int, number or string expected, got %s", sea_type_name(v));
    return big_val(b);
}

/* bigint.tostring(a [, base]) */
static SeaValue bi_tostring(SeaVM *vm, int argc, SeaValue *args) {
    SeaBigInt *b = one(vm, "tostring", argc, args);
    SeaString *s = sea_bigint_to_string(b, base_arg(vm, "tostring", argc, args, 1, 10));
    sea_obj_release(&b->obj);
    return sea_obj_val(&s->obj);
}

/* bigint.toint(a) → int, or nil when it does not fit Sea's 48-bit ints */
static SeaValue bi_toint(SeaVM *vm, int argc, SeaValue *args) {
    SeaBigInt *b = one(vm, "toint", argc, args);
    int64_t v;
    bool ok = sea_bigint_to_i64(b, &v) && v >= SEA_INT_MIN && v <= SEA_INT_MAX;
    sea_obj_release(&b->obj);
    return ok ? sea_int_val(v) : SEA_NIL_VAL;
}

static SeaValue bi_tonumber(SeaVM *vm, int argc, SeaValue *args) {
    SeaBigInt *b = one(vm, "tonumber", argc, args);
    double d = sea_bigint_to_double(b);
    sea_obj_release(&b->obj);
    return sea_number_val(d);
}

void sea_lib_bigint_register(SeaVM *vm) {
    SeaTable *mod = sea_table_new(TABLE_MAP);
    mod_set(vm, mod, "new",      bi_new);
    mod_set(vm, mod, "add",      bi_add);
    mod_set(vm, mod, "sub",      bi_sub);
    mod_set(vm, mod, "mul",      bi_mul);
    mod_set(vm, mod, "div",      bi_div);
    mod_set(vm, mod, "mod",      bi_mod);
    mod_set(vm, mod, "pow",      bi_pow);
    mod_set(vm, mod, "neg",      bi_neg);
    mod_set(vm, mod, "abs",      bi_abs);
    mod_set(vm, mod, "cmp",      bi_cmp);
    mod_set(vm, mod, "tostring", bi_tostring);
    mod_set(vm, mod, "toint",    bi_toint);
    mod_set(vm, mod, "tonumber", bi_tonumber);
    sea_vm_register_module(vm, "bigint", mod);
    sea_obj_release(&mod->obj);
}
