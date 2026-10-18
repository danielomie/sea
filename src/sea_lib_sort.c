/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * The `sort` module: sorting and binary search on arrays.
 *
 *   sort.sort(a [, less])          sort in place, stable
 *   sort.by(a, key [, reverse])    sort in place by key (function or field name), stable
 *   sort.sorted(a [, less])        a sorted copy
 *   sort.reverse(a)                reverse in place
 *   sort.is_sorted(a [, less])     bool
 *   sort.search(a, x [, less])     first index whose element is not less than x
 *   sort.asc, sort.desc            ready-made comparators
 *
 * `a` is an array, int_array or float_array. Without `less`, the natural
 * order: numbers by value (ints and floats together; NaN last), strings by
 * bytes. Mixing kinds without a comparator is a runtime error, raised before
 * the array is touched.
 *
 * With a comparator written in Sea, the sort works on a retained copy of the
 * elements and writes the array back only at the end: a panic in the
 * comparator leaves the array as it was, and a comparator that changes the
 * array is a runtime error (never a duplicated or lost element).
 */
#include "sea_stdlib.h"
#include "sea_bigint.h"
#include "sea_string.h"
#include "sea_rope.h"
#include "sea_table.h"
#include "sea_array.h"
#include "sea_struct.h"
#include "sea_map.h"
#include "sea_typed_array.h"
#include "sea_memory.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* sort.asc / sort.desc: recognised by identity, so sorting with them never
 * calls back into Sea */
static SeaObject *g_asc, *g_desc;

/* ---- the natural order ---- */

enum { K_OTHER, K_NUMBER, K_STRING };

static int kind(SeaValue v) {
    if (sea_is_int(v) || sea_is_number(v) || sea_is_bigint(v)) return K_NUMBER;
    if (sea_is_string(v) || sea_is_rope(v)) return K_STRING;
    return K_OTHER;
}

static const char *bytes(SeaValue v, size_t *len) {
    SeaString *s = sea_is_string(v) ? SEA_AS_STRING(v) : sea_rope_flat(SEA_AS_ROPE(v));
    *len = s->len;
    return s->data;
}

/* -1, 0, 1 for two values of the same comparable kind */
static int natural(SeaValue a, SeaValue b) {
    if (sea_is_int(a) && sea_is_int(b)) {
        int64_t x = sea_as_int(a), y = sea_as_int(b);
        return x < y ? -1 : x > y;
    }
    if (sea_is_bigint(a) || sea_is_bigint(b)) {
        int c;
        if (sea_bigint_compare(a, b, &c) == 1) return c;
        /* a NaN: after every number */
        bool na = sea_is_number(a) && isnan(sea_as_number(a)), nb = sea_is_number(b) && isnan(sea_as_number(b));
        return (int)na - (int)nb;
    }
    if (kind(a) == K_NUMBER) {
        double x = sea_is_int(a) ? (double)sea_as_int(a) : sea_as_number(a);
        double y = sea_is_int(b) ? (double)sea_as_int(b) : sea_as_number(b);
        if (isnan(x) || isnan(y)) return isnan(x) - isnan(y);   /* NaN after every number */
        return x < y ? -1 : x > y;
    }
    size_t la, lb;
    const char *pa = bytes(a, &la), *pb = bytes(b, &lb);
    int c = memcmp(pa, pb, la < lb ? la : lb);
    if (c != 0) return c < 0 ? -1 : 1;
    return la < lb ? -1 : la > lb;
}

/* all values comparable in the natural order (one kind, number or string) */
static void check_comparable(SeaVM *vm, const char *fn, const SeaValue *v, size_t n, SeaValue *extra) {
    int k = K_OTHER;
    for (size_t i = 0; i <= n; i++) {
        SeaValue x;
        if (i < n) x = v[i];
        else if (extra) x = *extra;
        else break;
        int kx = kind(x);
        if (kx == K_OTHER)
            sea_vm_error_noreturn(vm, "sort.%s: cannot compare %s values without a comparator", fn, sea_type_name(x));
        if (k == K_OTHER) k = kx;
        else if (kx != k)
            sea_vm_error_noreturn(vm, "sort.%s: cannot compare a number with a string", fn);
    }
}

/* ---- comparison ---- */

enum { M_ASC, M_DESC, M_CALL, M_KEYS };

typedef struct {
    SeaVM          *vm;
    int             mode;
    SeaValue        less;     /* M_CALL */
    const SeaValue *keys;     /* M_KEYS: elements are int indexes into keys */
    bool            reverse;  /* M_KEYS */
    bool            failed;   /* a panic escaped the comparator */
} Cmp;

static bool lt(Cmp *c, SeaValue a, SeaValue b) {
    switch (c->mode) {
        case M_ASC:  return natural(a, b) < 0;
        case M_DESC: return natural(b, a) < 0;
        case M_KEYS: {
            SeaValue ka = c->keys[sea_as_int(a)], kb = c->keys[sea_as_int(b)];
            return c->reverse ? natural(kb, ka) < 0 : natural(ka, kb) < 0;
        }
        default: {
            if (c->failed) return false;
            SeaValue args[2] = { a, b };
            SeaValue r = sea_vm_call(c->vm, c->less, args, 2);
            bool yes = sea_is_truthy(r);
            if (sea_is_obj(r)) sea_obj_release(sea_as_obj(r));
            if (c->vm->panic_propagating) {
                c->failed = true;
                return false;
            }
            return yes;
        }
    }
}

/* stable merge sort: insertion sort on runs of 16, then bottom-up merges */
static void merge_sort(Cmp *c, SeaValue *a, size_t n) {
    const size_t RUN = 16;
    for (size_t lo = 0; lo < n && !c->failed; lo += RUN) {
        size_t hi = lo + RUN < n ? lo + RUN : n;
        for (size_t i = lo + 1; i < hi && !c->failed; i++) {
            SeaValue x = a[i];
            size_t j = i;
            while (j > lo && lt(c, x, a[j - 1])) {
                a[j] = a[j - 1];
                j--;
            }
            a[j] = x;
        }
    }
    if (n <= RUN || c->failed) return;
    SeaValue *tmp = (SeaValue *)malloc(sizeof(SeaValue) * n);
    SeaValue *src = a, *dst = tmp;
    for (size_t w = RUN; w < n && !c->failed; w *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * w) {
            size_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                if (lt(c, src[j], src[i])) dst[k++] = src[j++];   /* equal: left first (stable) */
                else dst[k++] = src[i++];
            }
            while (i < mid) dst[k++] = src[i++];
            while (j < hi) dst[k++] = src[j++];
        }
        SeaValue *t = src; src = dst; dst = t;
    }
    if (src != a) memcpy(a, src, sizeof(SeaValue) * n);
    free(tmp);
}

/* the comparator argument: nil, sort.asc, sort.desc or a function */
static void comparator(SeaVM *vm, const char *fn, Cmp *c, int argc, SeaValue *args, int i) {
    memset(c, 0, sizeof *c);
    c->vm = vm;
    c->mode = M_ASC;
    if (argc <= i || sea_is_nil(args[i])) return;
    SeaValue f = args[i];
    if (!sea_obj_is_type(f, OBJ_CLOSURE))
        sea_vm_error_noreturn(vm, "sort.%s: the comparator must be a function, got %s", fn, sea_type_name(f));
    if (sea_as_obj(f) == g_asc) return;
    if (sea_as_obj(f) == g_desc) { c->mode = M_DESC; return; }
    c->mode = M_CALL;
    c->less = f;
}

/* ---- sorting the three kinds of array ---- */

static int cmp_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}
static int cmp_i64_desc(const void *a, const void *b) { return cmp_i64(b, a); }
static int cmp_f64(const void *a, const void *b) {
    return natural(sea_number_val(*(const double *)a), sea_number_val(*(const double *)b));
}
static int cmp_f64_desc(const void *a, const void *b) { return cmp_f64(b, a); }

/* Sort an array in place. Returns false if a panic escaped the comparator
 * (the array is then unchanged). */
static bool sort_array(SeaVM *vm, const char *fn, SeaArray *arr, Cmp *c) {
    size_t n = arr->len;
    if (n < 2) return true;
    if (c->mode == M_ASC || c->mode == M_DESC) {
        check_comparable(vm, fn, arr->data, n, NULL);
        merge_sort(c, arr->data, n);             /* no Sea code runs: sort in place */
        return true;
    }
    /* a comparator runs Sea code: sort a retained copy, check the array did
     * not change meanwhile, then write it back */
    SeaValue *orig = (SeaValue *)malloc(sizeof(SeaValue) * n);
    SeaValue *work = (SeaValue *)malloc(sizeof(SeaValue) * n);
    memcpy(orig, arr->data, sizeof(SeaValue) * n);
    memcpy(work, arr->data, sizeof(SeaValue) * n);
    for (size_t i = 0; i < n; i++) if (sea_is_obj(work[i])) sea_obj_retain(sea_as_obj(work[i]));
    merge_sort(c, work, n);
    bool same = !c->failed && arr->len == n && memcmp(orig, arr->data, sizeof(SeaValue) * n) == 0;
    if (same) memcpy(arr->data, work, sizeof(SeaValue) * n);
    for (size_t i = 0; i < n; i++) if (sea_is_obj(orig[i])) sea_obj_release(sea_as_obj(orig[i]));
    free(orig);
    free(work);
    if (c->failed) return false;
    if (!same) sea_vm_error_noreturn(vm, "sort.%s: the array was changed by the comparator", fn);
    return true;
}

/* int_array / float_array: qsort for the natural orders; a comparator works
 * on boxed copies (numbers carry no references) */
static bool sort_typed(SeaVM *vm, SeaValue a, Cmp *c) {
    bool is_int = sea_is_int_array(a);
    size_t n = is_int ? SEA_AS_INT_ARRAY(a)->len : SEA_AS_FLOAT_ARRAY(a)->len;
    if (n < 2) return true;
    if (c->mode == M_ASC || c->mode == M_DESC) {
        if (is_int) qsort(SEA_AS_INT_ARRAY(a)->data, n, sizeof(int64_t), c->mode == M_ASC ? cmp_i64 : cmp_i64_desc);
        else qsort(SEA_AS_FLOAT_ARRAY(a)->data, n, sizeof(double), c->mode == M_ASC ? cmp_f64 : cmp_f64_desc);
        return true;
    }
    SeaValue *work = (SeaValue *)malloc(sizeof(SeaValue) * n);
    for (size_t i = 0; i < n; i++)
        work[i] = is_int ? sea_int_val(SEA_AS_INT_ARRAY(a)->data[i]) : sea_number_val(SEA_AS_FLOAT_ARRAY(a)->data[i]);
    merge_sort(c, work, n);
    size_t now = is_int ? SEA_AS_INT_ARRAY(a)->len : SEA_AS_FLOAT_ARRAY(a)->len;
    if (!c->failed && now == n) {
        for (size_t i = 0; i < n; i++) {
            if (is_int) SEA_AS_INT_ARRAY(a)->data[i] = sea_as_int(work[i]);
            else SEA_AS_FLOAT_ARRAY(a)->data[i] = sea_as_number(work[i]);
        }
    }
    free(work);
    if (c->failed) return false;
    if (now != n) sea_vm_error_noreturn(vm, "sort.sort: the array was changed by the comparator");
    return true;
}

static bool is_sortable(SeaValue a) {
    return sea_is_array(a) || sea_is_int_array(a) || sea_is_float_array(a);
}

static void need_array(SeaVM *vm, const char *fn, int argc, SeaValue *args) {
    if (argc < 1 || !is_sortable(args[0]))
        sea_vm_error_noreturn(vm, "sort.%s: expected an array, got %s", fn,
                              argc < 1 ? "nothing" : sea_type_name(args[0]));
}

/* sort.sort(a [, less]) → a */
static SeaValue sort_sort(SeaVM *vm, int argc, SeaValue *args) {
    need_array(vm, "sort", argc, args);
    Cmp c;
    comparator(vm, "sort", &c, argc, args, 1);
    bool ok = sea_is_array(args[0]) ? sort_array(vm, "sort", SEA_AS_ARRAY(args[0]), &c) : sort_typed(vm, args[0], &c);
    if (!ok) return SEA_NIL_VAL;               /* the panic unwinds from here */
    return sea_val_ref(args[0]);
}

/* sort.sorted(a [, less]) → a new sorted array (of the same kind) */
static SeaValue sort_sorted(SeaVM *vm, int argc, SeaValue *args) {
    need_array(vm, "sorted", argc, args);
    Cmp c;
    comparator(vm, "sorted", &c, argc, args, 1);
    SeaValue copy;
    if (sea_is_array(args[0])) {
        SeaArray *src = SEA_AS_ARRAY(args[0]);
        SeaArray *dst = sea_array_new(src->len);
        for (uint32_t i = 0; i < src->len; i++) sea_array_push(dst, src->data[i]);
        copy = sea_obj_val(&dst->obj);
    } else if (sea_is_int_array(args[0])) {
        SeaIntArray *src = SEA_AS_INT_ARRAY(args[0]);
        SeaIntArray *dst = sea_int_array_new(src->len);
        for (uint32_t i = 0; i < src->len; i++) sea_int_array_push(dst, src->data[i]);
        copy = sea_obj_val(&dst->obj);
    } else {
        SeaFloatArray *src = SEA_AS_FLOAT_ARRAY(args[0]);
        SeaFloatArray *dst = sea_float_array_new(src->len);
        for (uint32_t i = 0; i < src->len; i++) sea_float_array_push(dst, src->data[i]);
        copy = sea_obj_val(&dst->obj);
    }
    bool ok = sea_is_array(copy) ? sort_array(vm, "sorted", SEA_AS_ARRAY(copy), &c) : sort_typed(vm, copy, &c);
    if (!ok) {
        sea_obj_release(sea_as_obj(copy));
        return SEA_NIL_VAL;
    }
    return copy;
}

/* a field of a table, struct or map, borrowed (nil if absent) */
static SeaValue field(SeaVM *vm, SeaValue v, SeaValue name) {
    SeaValue out = SEA_NIL_VAL;
    if (sea_is_table(v)) {
        sea_table_get(SEA_AS_TABLE(v), name, &out);
        return out;
    }
    if (sea_is_map(v)) {
        sea_map_get(SEA_AS_MAP(v), name, &out);
        return out;
    }
    if (sea_is_struct(v)) return sea_struct_get(SEA_AS_STRUCT(v), SEA_AS_STRING(name));
    sea_vm_error_noreturn(vm, "sort.by: a %s has no fields", sea_type_name(v));
}

/* sort.by(a, key [, reverse]) → a: key is a function (called once per
 * element) or a field name; the keys are compared in the natural order */
static SeaValue sort_by(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1 || !sea_is_array(args[0]))
        sea_vm_error_noreturn(vm, "sort.by: expected an array, got %s", argc < 1 ? "nothing" : sea_type_name(args[0]));
    if (argc < 2 || (!sea_obj_is_type(args[1], OBJ_CLOSURE) && !sea_is_string(args[1])))
        sea_vm_error_noreturn(vm, "sort.by: the key must be a function or a field name");
    SeaArray *arr = SEA_AS_ARRAY(args[0]);
    size_t n = arr->len;
    bool reverse = argc > 2 && sea_is_truthy(args[2]);
    if (n < 2) return sea_val_ref(args[0]);

    SeaValue *orig = (SeaValue *)malloc(sizeof(SeaValue) * n);
    SeaValue *keys = (SeaValue *)calloc(n, sizeof(SeaValue));
    memcpy(orig, arr->data, sizeof(SeaValue) * n);
    for (size_t i = 0; i < n; i++) if (sea_is_obj(orig[i])) sea_obj_retain(sea_as_obj(orig[i]));
    bool is_fn = sea_obj_is_type(args[1], OBJ_CLOSURE);
    size_t got = 0;
    for (; got < n; got++) {
        SeaValue k;
        if (is_fn) {
            k = sea_vm_call(vm, args[1], &orig[got], 1);          /* owned */
            if (vm->panic_propagating) {
                if (sea_is_obj(k)) sea_obj_release(sea_as_obj(k));
                break;
            }
        } else {
            k = sea_val_ref(field(vm, orig[got], args[1]));
        }
        keys[got] = k;
    }
    bool failed = got < n;
    int bad = K_OTHER;
    if (!failed) {
        /* the keys must be comparable (checked here, so that an error does
         * not jump past the cleanup) */
        int k0 = K_OTHER;
        for (size_t i = 0; i < n && bad == K_OTHER; i++) {
            int ki = kind(keys[i]);
            if (ki == K_OTHER) bad = -1;
            else if (k0 == K_OTHER) k0 = ki;
            else if (ki != k0) bad = -2;
        }
    }
    bool same = false;
    if (!failed && bad == K_OTHER) {
        SeaValue *idx = (SeaValue *)malloc(sizeof(SeaValue) * n);
        for (size_t i = 0; i < n; i++) idx[i] = sea_int_val((int64_t)i);
        Cmp c = { .vm = vm, .mode = M_KEYS, .keys = keys, .reverse = reverse };
        merge_sort(&c, idx, n);
        same = arr->len == n && memcmp(orig, arr->data, sizeof(SeaValue) * n) == 0;
        if (same)
            for (size_t i = 0; i < n; i++) arr->data[i] = orig[sea_as_int(idx[i])];
        free(idx);
    }
    for (size_t i = 0; i < got; i++) if (sea_is_obj(keys[i])) sea_obj_release(sea_as_obj(keys[i]));
    for (size_t i = 0; i < n; i++) if (sea_is_obj(orig[i])) sea_obj_release(sea_as_obj(orig[i]));
    free(keys);
    free(orig);
    if (failed) return SEA_NIL_VAL;
    if (bad == -1) sea_vm_error_noreturn(vm, "sort.by: the keys must be numbers or strings");
    if (bad == -2) sea_vm_error_noreturn(vm, "sort.by: cannot compare a number key with a string key");
    if (!same) sea_vm_error_noreturn(vm, "sort.by: the array was changed by the key function");
    return sea_val_ref(args[0]);
}

/* sort.reverse(a) → a */
static SeaValue sort_reverse(SeaVM *vm, int argc, SeaValue *args) {
    need_array(vm, "reverse", argc, args);
    SeaValue a = args[0];
    if (sea_is_array(a)) {
        SeaArray *arr = SEA_AS_ARRAY(a);
        for (uint32_t i = 0, j = arr->len; i + 1 < j; i++, j--) {
            SeaValue t = arr->data[i]; arr->data[i] = arr->data[j - 1]; arr->data[j - 1] = t;
        }
    } else if (sea_is_int_array(a)) {
        SeaIntArray *arr = SEA_AS_INT_ARRAY(a);
        for (uint32_t i = 0, j = arr->len; i + 1 < j; i++, j--) {
            int64_t t = arr->data[i]; arr->data[i] = arr->data[j - 1]; arr->data[j - 1] = t;
        }
    } else {
        SeaFloatArray *arr = SEA_AS_FLOAT_ARRAY(a);
        for (uint32_t i = 0, j = arr->len; i + 1 < j; i++, j--) {
            double t = arr->data[i]; arr->data[i] = arr->data[j - 1]; arr->data[j - 1] = t;
        }
    }
    return sea_val_ref(a);
}

/* element i of any of the three kinds, as a value (borrowed) */
static SeaValue elem(SeaValue a, size_t i) {
    if (sea_is_array(a)) return SEA_AS_ARRAY(a)->data[i];
    if (sea_is_int_array(a)) return sea_int_val(SEA_AS_INT_ARRAY(a)->data[i]);
    return sea_number_val(SEA_AS_FLOAT_ARRAY(a)->data[i]);
}

static size_t count(SeaValue a) {
    if (sea_is_array(a)) return SEA_AS_ARRAY(a)->len;
    if (sea_is_int_array(a)) return SEA_AS_INT_ARRAY(a)->len;
    return SEA_AS_FLOAT_ARRAY(a)->len;
}

/* sort.is_sorted(a [, less]) → bool */
static SeaValue sort_is_sorted(SeaVM *vm, int argc, SeaValue *args) {
    need_array(vm, "is_sorted", argc, args);
    Cmp c;
    comparator(vm, "is_sorted", &c, argc, args, 1);
    SeaValue a = args[0];
    if (sea_is_array(a) && c.mode != M_CALL) check_comparable(vm, "is_sorted", SEA_AS_ARRAY(a)->data, count(a), NULL);
    for (size_t i = 1; i < count(a); i++) {
        if (lt(&c, elem(a, i), elem(a, i - 1))) return SEA_FALSE_VAL;
        if (c.failed) return SEA_NIL_VAL;
    }
    return SEA_TRUE_VAL;
}

/* sort.search(a, x [, less]) → the first index i with not less(a[i], x)
 * (len(a) if none): where x is, or would go, in a sorted array */
static SeaValue sort_search(SeaVM *vm, int argc, SeaValue *args) {
    need_array(vm, "search", argc, args);
    if (argc < 2) sea_vm_error_noreturn(vm, "sort.search: expected a value to search for");
    Cmp c;
    comparator(vm, "search", &c, argc, args, 2);
    SeaValue a = args[0], x = args[1];
    if (c.mode != M_CALL) {
        if (sea_is_array(a)) check_comparable(vm, "search", SEA_AS_ARRAY(a)->data, count(a), &x);
        else check_comparable(vm, "search", &x, 1, NULL);
    }
    size_t lo = 0, hi = count(a);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        bool less = lt(&c, elem(a, mid), x);
        if (c.failed) return SEA_NIL_VAL;
        if (less) lo = mid + 1; else hi = mid;
    }
    return sea_int_val((int64_t)lo);
}

/* sort.asc(a, b) / sort.desc(a, b): the natural order and its reverse */
static SeaValue sort_asc(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2) sea_vm_error_noreturn(vm, "sort.asc: expected two values");
    check_comparable(vm, "asc", args, 2, NULL);
    return sea_bool_val(natural(args[0], args[1]) < 0);
}

static SeaValue sort_desc(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2) sea_vm_error_noreturn(vm, "sort.desc: expected two values");
    check_comparable(vm, "desc", args, 2, NULL);
    return sea_bool_val(natural(args[1], args[0]) < 0);
}

void sea_lib_sort_register(SeaVM *vm) {
    SeaTable *mod = sea_table_new(TABLE_MAP);
    mod_set(vm, mod, "sort",      sort_sort);
    mod_set(vm, mod, "by",        sort_by);
    mod_set(vm, mod, "sorted",    sort_sorted);
    mod_set(vm, mod, "reverse",   sort_reverse);
    mod_set(vm, mod, "is_sorted", sort_is_sorted);
    mod_set(vm, mod, "search",    sort_search);
    mod_set(vm, mod, "asc",       sort_asc);
    mod_set(vm, mod, "desc",      sort_desc);
    SeaValue v = SEA_NIL_VAL;
    sea_table_get_str(mod, "asc", &v);
    g_asc = sea_as_obj(v);
    sea_table_get_str(mod, "desc", &v);
    g_desc = sea_as_obj(v);
    sea_vm_register_module(vm, "sort", mod);
    sea_obj_release(&mod->obj);
}
