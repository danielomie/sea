/*
 * Copyright (c) 2026 Daniel
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sea_array.h"
#include "sea_object.h"
#include <string.h>

SEA_INLINE void arr_val_retain(SeaValue v) {
    if (sea_is_obj(v)) sea_obj_retain(sea_as_obj(v));
}
SEA_INLINE void arr_val_release(SeaValue v) {
    if (sea_is_obj(v)) sea_obj_release(sea_as_obj(v));
}

SeaArray* sea_array_new(uint32_t initial_cap) {
    SeaArray *a = SEA_ALLOC(SeaArray);
    a->obj.type = OBJ_ARRAY;
    a->obj.gc = 0;
    a->obj.refcount = 1;
    a->len = 0;
    a->cap = initial_cap < 4 ? 4 : initial_cap;
    a->data = SEA_ALLOC_N(SeaValue, a->cap);
    return a;
}

void sea_array_init_header(SeaArray *a, uint32_t initial_cap) {
    a->obj.type = OBJ_ARRAY;
    a->obj.gc = 0;
    /* refcount set by caller (e.g. SEA_RC_ARENA_BIT for arena-allocated headers) */
    a->len = 0;
    a->cap = initial_cap < 4 ? 4 : initial_cap;
    a->data = SEA_ALLOC_N(SeaValue, a->cap);
}

void sea_array_push(SeaArray *a, SeaValue v) {
    if (a->len >= a->cap) {
        uint32_t new_cap = a->cap * 2;
        a->data = SEA_REALLOC_N(a->data, SeaValue, a->cap, new_cap);
        a->cap = new_cap;
    }
    sea_store_barrier(&a->obj, v);
    arr_val_retain(v);
    a->data[a->len++] = v;
}

/* Append, taking over the caller's reference to v (no retain). */
void sea_array_push_move(SeaArray *a, SeaValue v) {
    if (a->len >= a->cap) {
        uint32_t new_cap = a->cap * 2;
        a->data = SEA_REALLOC_N(a->data, SeaValue, a->cap, new_cap);
        a->cap = new_cap;
    }
    sea_store_barrier(&a->obj, v);
    a->data[a->len++] = v;
}

SeaValue sea_array_get(SeaArray *a, int64_t idx) {
    int64_t len = (int64_t)a->len;
    if (idx < 0) idx += len;
    if (idx < 0 || idx >= len) return SEA_NIL_VAL;
    return a->data[(uint32_t)idx];
}

void sea_array_set(SeaArray *a, int64_t idx, SeaValue v) {
    int64_t len = (int64_t)a->len;
    if (idx < 0) idx += len;
    if (idx < 0 || idx >= len) return;
    sea_store_barrier(&a->obj, v);
    SeaValue old = a->data[(uint32_t)idx];
    arr_val_retain(v);
    a->data[(uint32_t)idx] = v;
    arr_val_release(old);   /* after the store: v may be reachable only through old */
}

uint32_t sea_array_len(SeaArray *a) {
    return a->len;
}

SeaValue sea_array_pop(SeaArray *a) {
    if (a->len == 0) return SEA_NIL_VAL;
    /* Caller gets the reference that was held by the array */
    return a->data[--a->len];
}

void sea_array_del(SeaArray *a, int64_t idx) {
    int64_t len = (int64_t)a->len;
    if (idx < 0) idx += len;
    if (idx < 0 || idx >= len) return;
    arr_val_release(a->data[(uint32_t)idx]);
    memmove(&a->data[idx], &a->data[idx + 1],
            (size_t)(len - idx - 1) * sizeof(SeaValue));
    a->len--;
}

void sea_array_destroy(SeaArray *a) {
    if (a->data) {
        /* Release all held references */
        for (uint32_t i = 0; i < a->len; i++) {
            arr_val_release(a->data[i]);
        }
        sea_free(a->data, sizeof(SeaValue) * a->cap);
        a->data = NULL;
    }
    a->len = 0;
    a->cap = 0;
}
