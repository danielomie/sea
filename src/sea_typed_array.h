/*
 * Copyright (c) 2026 Daniel
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once
#include "sea_object.h"
#include "sea_memory.h"

/*
 * SeaIntArray / SeaFloatArray — typed arrays without per-element reference
 * counting.  Ideal for large numeric datasets (ML token arrays, matrices).
 *
 * Memory layout: a single contiguous C array of int64_t / double.
 * No SeaValue boxing/unboxing of individual elements during push/set.
 */

typedef struct {
    SeaObject obj;
    uint32_t  len;
    uint32_t  cap;
    int64_t  *data;
} SeaIntArray;

typedef struct {
    SeaObject obj;
    uint32_t  len;
    uint32_t  cap;
    double   *data;
} SeaFloatArray;

/* --- SeaIntArray --- */
SeaIntArray *sea_int_array_new(uint32_t cap);
void         sea_int_array_push(SeaIntArray *a, int64_t v);
int64_t      sea_int_array_get(SeaIntArray *a, int64_t idx);
void         sea_int_array_set(SeaIntArray *a, int64_t idx, int64_t v);
void         sea_int_array_free(SeaIntArray *a);

/* --- SeaFloatArray --- */
SeaFloatArray *sea_float_array_new(uint32_t cap);
void           sea_float_array_push(SeaFloatArray *a, double v);
double         sea_float_array_get(SeaFloatArray *a, int64_t idx);
void           sea_float_array_set(SeaFloatArray *a, int64_t idx, double v);
void           sea_float_array_free(SeaFloatArray *a);

/* Convenience macros */
#define SEA_AS_INT_ARRAY(v)    ((SeaIntArray*)sea_as_obj(v))
#define SEA_AS_FLOAT_ARRAY(v)  ((SeaFloatArray*)sea_as_obj(v))
#define sea_is_int_array(v)    sea_obj_is_type(v, OBJ_INT_ARRAY)
#define sea_is_float_array(v)  sea_obj_is_type(v, OBJ_FLOAT_ARRAY)
