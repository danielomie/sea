#pragma once
#include "sea_object.h"
#include "sea_string.h"
#include "sea_memory.h"

/*
 * SeaStruct — fixed-layout object created from {k=v, ...} table literals.
 *
 * All data (header, field name pointers, field values) lives in a single
 * allocation:
 *
 *   [SeaStruct header (24B)] [SeaString* names × N] [SeaValue values × N]
 *
 * field_cap and building_remaining are the only book-keeping fields.
 * Field names and values are accessed via sea_struct_names() / sea_struct_values().
 *
 * Field names are interned strings; comparison uses pointer equality (O(1)).
 * Linear scan over field_count is fast for typical struct sizes (< 16 fields).
 *
 * Lifecycle:
 *   1. sea_struct_new(N)              — single alloc, building_remaining=N
 *   2. sea_struct_init_field(s,k,v)   — fills slots in order (called N times)
 *      After the N-th call, building_remaining → -1 (struct is sealed).
 *   3. sea_struct_get / sea_struct_set — normal access (sealed struct only)
 *   4. sea_struct_free                — release refs and free single block
 */

typedef struct {
    SeaObject  obj;
    uint32_t   field_count;          /* number of initialized fields        */
    uint32_t   field_cap;            /* allocated capacity (= N from NEW)   */
    int32_t    building_remaining;   /* >=0 during init; -1 when sealed     */
    uint32_t   _pad;                 /* keep sizeof(SeaStruct) == 24 (8-aligned) */
    /* Inline arrays follow immediately in the same allocation:
     *   SeaString *names [field_cap]  at offset sizeof(SeaStruct)
     *   SeaValue   values[field_cap]  at offset sizeof(SeaStruct) + field_cap*8
     */
} SeaStruct;

static inline SeaString **sea_struct_names(SeaStruct *s) {
    return (SeaString**)((char*)s + sizeof(SeaStruct));
}
static inline SeaValue *sea_struct_values(SeaStruct *s) {
    return (SeaValue*)((char*)s + sizeof(SeaStruct) + (size_t)s->field_cap * sizeof(SeaString*));
}

SeaStruct *sea_struct_new(uint32_t field_count);

/* Called once per field during construction (building_remaining > 0). */
void sea_struct_init_field(SeaStruct *s, SeaString *key, SeaValue v);

/* Borrow variant: store without retaining v (used for arena-allocated structs
 * where the owning local already holds the RC). */
void sea_struct_init_field_borrow(SeaStruct *s, SeaString *key, SeaValue v);

/* Read a field; returns SEA_NIL_VAL if not found. */
SeaValue sea_struct_get(SeaStruct *s, SeaString *key);

/* Update an existing field; returns false if field not found (sealed). */
bool sea_struct_set(SeaStruct *s, SeaString *key, SeaValue v);
bool sea_struct_set_move(SeaStruct *s, SeaString *key, SeaValue v);   /* takes over v */

/* For-in iteration; *iter starts at 0. Returns false when exhausted. */
bool sea_struct_next(SeaStruct *s, uint32_t *iter, SeaValue *k, SeaValue *v);

/* Release all refs and free memory. */
void sea_struct_free(SeaStruct *s);
/* Free the block only (field values already released or handed elsewhere). */
void sea_struct_free_block(SeaStruct *s);

/* Convenience macros */
#define SEA_AS_STRUCT(v)  ((SeaStruct*)sea_as_obj(v))
#define sea_is_struct(v)  sea_obj_is_type(v, OBJ_STRUCT)
