/*
 * Copyright (c) 2026 Daniel
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sea_struct.h"
#include "sea_string.h"
#include <string.h>
#include "sea_memory.h"

static void val_retain(SeaValue v) {
    if (sea_is_obj(v)) sea_obj_retain(sea_as_obj(v));
}
static void val_release(SeaValue v) {
    if (sea_is_obj(v)) sea_obj_release(sea_as_obj(v));
}

static size_t struct_alloc_size(uint32_t n) {
    return sizeof(SeaStruct) + (size_t)n * (sizeof(SeaString*) + sizeof(SeaValue));
}

/* Embedded free-list pool for small structs (field_cap 1..POOL_MAX_CAP).
 * The freed struct's first names-array slot is repurposed as a "next" pointer.
 * Total cached nodes is bounded to avoid unbounded memory growth. */
#define STRUCT_POOL_MAX_CAP   8       /* pool covers 1..8 field structs   */
#define STRUCT_POOL_MAX_TOTAL 65536   /* max cached nodes per thread      */

typedef struct FreeNode { struct FreeNode *next; } FreeNode;
/* Per thread: goroutines on different scheduler threads allocate and free
 * structs concurrently (a node freed by one thread just joins its pool). */
static __thread FreeNode *struct_freelist[STRUCT_POOL_MAX_CAP + 1];
static __thread int       struct_pool_total;

static SeaStruct *pool_alloc(uint32_t n) {
    if (n == 0 || n > STRUCT_POOL_MAX_CAP) return NULL;
    FreeNode *node = struct_freelist[n];
    if (!node) return NULL;
    struct_freelist[n] = node->next;
    struct_pool_total--;
    return (SeaStruct*)node;
}

static bool pool_free(SeaStruct *s) {
    uint32_t n = s->field_cap;
    if (n == 0 || n > STRUCT_POOL_MAX_CAP) return false;
    if (struct_pool_total >= STRUCT_POOL_MAX_TOTAL) return false;
    /* Embed the next pointer in the struct's first names-array slot. */
    FreeNode *node = (FreeNode*)s;
    node->next = struct_freelist[n];
    struct_freelist[n] = node;
    struct_pool_total++;
    return true;
}

SeaStruct *sea_struct_new(uint32_t n) {
    SeaStruct *s = pool_alloc(n);
    if (!s) {
        s = sea_malloc(struct_alloc_size(n));
        s->field_cap = n;
        s->_pad      = 0;
    }
    s->obj.type     = OBJ_STRUCT;
    s->obj.gc = 0;
    s->obj.refcount = 1;
    s->field_count  = 0;

    if (n > 0) {
        SeaString **names  = sea_struct_names(s);
        SeaValue   *values = sea_struct_values(s);
        for (uint32_t i = 0; i < n; i++) {
            names[i]  = NULL;
            values[i] = SEA_NIL_VAL;
        }
        s->building_remaining = (int32_t)n;
    } else {
        s->building_remaining = -1;
    }
    return s;
}

void sea_struct_init_field(SeaStruct *s, SeaString *key, SeaValue v) {
    uint32_t idx = s->field_count;
    SeaString **names  = sea_struct_names(s);
    SeaValue   *values = sea_struct_values(s);
    /* Field names are always interned string constants retained by the chunk;
     * the struct borrows them without an additional retain. */
    names[idx]  = key;
    values[idx] = v;
    sea_store_barrier(&s->obj, v);
    val_retain(v);
    s->field_count++;
    s->building_remaining--;
    if (s->building_remaining == 0)
        s->building_remaining = -1;
}

void sea_struct_init_field_borrow(SeaStruct *s, SeaString *key, SeaValue v) {
    uint32_t idx = s->field_count;
    SeaString **names  = sea_struct_names(s);
    SeaValue   *values = sea_struct_values(s);
    names[idx]  = key;
    values[idx] = v;
    sea_store_barrier(&s->obj, v);
    /* No val_retain: the caller hands over its reference (SET_FIELD_MOVE). */
    s->field_count++;
    s->building_remaining--;
    if (s->building_remaining == 0)
        s->building_remaining = -1;
}

/* Field names are interned constants, so a constant key matches by pointer.
 * A key built at run time (s[k] with k from string ops) is a different
 * object with the same bytes: compare contents when the pointers miss. */
static int struct_find(SeaStruct *s, SeaString *key) {
    SeaString **names = sea_struct_names(s);
    for (uint32_t i = 0; i < s->field_count; i++)
        if (names[i] == key) return (int)i;
    if (sea_string_is_interned(key)) return -1;   /* interned: pointer is identity */
    for (uint32_t i = 0; i < s->field_count; i++)
        if (names[i] && names[i]->len == key->len &&
            memcmp(names[i]->data, key->data, key->len) == 0) return (int)i;
    return -1;
}

SeaValue sea_struct_get(SeaStruct *s, SeaString *key) {
    int i = struct_find(s, key);
    return i >= 0 ? sea_struct_values(s)[i] : SEA_NIL_VAL;
}

bool sea_struct_set(SeaStruct *s, SeaString *key, SeaValue v) {
    int i = struct_find(s, key);
    if (i < 0) return false;
    SeaValue *values = sea_struct_values(s);
    sea_store_barrier(&s->obj, v);
    SeaValue old = values[i];
    val_retain(v);
    values[i] = v;
    val_release(old);   /* after the store (as SETLOCAL) */
    return true;
}

/* As sea_struct_set, taking over the caller's reference to v. */
bool sea_struct_set_move(SeaStruct *s, SeaString *key, SeaValue v) {
    int i = struct_find(s, key);
    if (i < 0) return false;
    SeaValue *values = sea_struct_values(s);
    sea_store_barrier(&s->obj, v);
    SeaValue old = values[i];
    values[i] = v;
    val_release(old);
    return true;
}

bool sea_struct_next(SeaStruct *s, uint32_t *iter, SeaValue *k, SeaValue *v) {
    SeaString **names  = sea_struct_names(s);
    SeaValue   *values = sea_struct_values(s);
    while (*iter < s->field_count) {
        uint32_t i = (*iter)++;
        if (names[i]) {
            *k = sea_obj_val(&names[i]->obj);
            *v = values[i];
            return true;
        }
    }
    return false;
}

void sea_struct_free(SeaStruct *s) {
    SeaValue *values = sea_struct_values(s);
    for (uint32_t i = 0; i < s->field_count; i++) {
        val_release(values[i]);
    }
    sea_struct_free_block(s);
}

void sea_struct_free_block(SeaStruct *s) {
    if (!pool_free(s))
        sea_free(s, struct_alloc_size(s->field_cap));
}
