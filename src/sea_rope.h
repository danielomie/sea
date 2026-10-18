/*
 * Copyright (c) 2026 Daniel
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once
#include "sea_object.h"
#include "sea_string.h"
#include "sea_value.h"
#include "sea_memory.h"

/*
 * Flat lazy rope: O(1) amortized append, O(total_chars) single flatten.
 * Used by OP_CONCAT to avoid O(N²) copying in accumulation loops.
 *
 * RC model: the rope retains every part via sea_obj_retain.
 *           sea_rope_free releases all parts and frees the parts array.
 */

#define SEA_ROPE_THRESHOLD  64   /* results ≤ 64 bytes stay flat */
#define SEA_ROPE_INIT_CAP    8

typedef struct {
    SeaObject   obj;        /* type=OBJ_ROPE, refcount */
    uint32_t    total_len;  /* sum of all parts' lengths */
    uint32_t    count;      /* number of parts in use */
    uint32_t    cap;        /* allocated capacity of parts[] */
    uint32_t    _pad;
    SeaString **parts;      /* malloc'd array; each entry is retained */
    SeaString  *flat;       /* cached flattened copy (owned) or NULL; see
                               sea_rope_flat. Kept last so the offsets above
                               stay stable for already-built native libs. */
} SeaRope;

SeaRope   *sea_rope_new(void);
void       sea_rope_append(SeaRope *r, SeaString *s);   /* retains s */
SeaString *sea_rope_flatten(SeaRope *r);                 /* returns RC=1 */
/* Flattened copy cached in the rope (borrowed: valid while the rope lives,
 * do not release). Computed once; safe to call from several threads. */
SeaString *sea_rope_flat(SeaRope *r);
void       sea_rope_free(SeaRope *r);                    /* release all parts */

#define SEA_AS_ROPE(v)  ((SeaRope*)sea_as_obj(v))
#define sea_is_rope(v)  sea_obj_is_type(v, OBJ_ROPE)

/* A rope used as a table/map key stands for its string content: hash and
 * compare its cached flat copy (borrowed; containers retain what they store). */
static inline SeaValue sea_rope_key(SeaValue v) {
    return sea_obj_val(&sea_rope_flat(SEA_AS_ROPE(v))->obj);
}

/*
 * Get a flat SeaString from a SeaValue that may be a string or a rope.
 * Always returns RC=1 — caller must call sea_obj_release() when done.
 * Returns NULL if v is not a string or rope.
 */
static inline SeaString *sea_value_get_flat_string(SeaValue v) {
    if (sea_is_string(v)) {
        SeaString *s = SEA_AS_STRING(v);
        sea_obj_retain(&s->obj);
        return s;
    }
    if (sea_is_rope(v)) {
        return sea_rope_flatten(SEA_AS_ROPE(v));
    }
    return NULL;
}
