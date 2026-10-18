/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once
#include "sea_object.h"
#include "sea_memory.h"

/*
 * Sea dynamic array type.
 * Supports negative indexing (Python-style: -1 = last element).
 */

struct SeaArray {
    SeaObject  obj;    // must be first
    uint32_t   len;
    uint32_t   cap;
    SeaValue  *data;
};

/* A small array (sea_array_new with a capacity up to SEA_ARRAY_INLINE_MAX)
 * keeps its elements in the same allocation, right after the header: one
 * malloc instead of two. Growing moves them to a buffer of their own. Code
 * that frees or reallocates `data` must check this first. */
#define SEA_ARRAY_INLINE_MAX 8
#define SEA_ARRAY_IS_INLINE(a) ((a)->data == (SeaValue *)((a) + 1))

// Create a new array with optional initial capacity
SeaArray* sea_array_new(uint32_t initial_cap);

// Initialize an already-allocated SeaArray header (e.g. from the frame arena).
// The header itself is NOT owned by this function; only the heap-allocated data
// buffer is created. refcount is left for the caller to set (arena bit, etc.).
void sea_array_init_header(SeaArray *a, uint32_t initial_cap);

// Push a value to end
void sea_array_push(SeaArray *a, SeaValue v);

// Get element (supports negative indices, returns nil for out-of-range)
SeaValue sea_array_get(SeaArray *a, int64_t idx);

// Set element (supports negative indices, no-op for out-of-range)
void sea_array_set(SeaArray *a, int64_t idx, SeaValue v);

// Get length
uint32_t sea_array_len(SeaArray *a);

// Pop last element (returns nil if empty)
SeaValue sea_array_pop(SeaArray *a);

// Delete element at idx (supports negative indices), shifts remaining left
void sea_array_del(SeaArray *a, int64_t idx);

// Free array contents (but not the SeaArray struct itself - handled by obj_release)
void sea_array_destroy(SeaArray *a);
void sea_array_push_move(SeaArray *a, SeaValue v);   /* takes over v's reference */
