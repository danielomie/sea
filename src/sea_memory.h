#pragma once
#include "sea_common.h"

/*
 * Memory allocator wrappers.
 * All allocation goes through these functions so we can track usage
 * and handle OOM gracefully.
 */

// Low-level allocator
void* sea_malloc(size_t size);
void* sea_realloc(void *ptr, size_t old_size, size_t new_size);
void  sea_free(void *ptr, size_t size);

// Allocation macros
#define SEA_ALLOC(type)             ((type*)sea_malloc(sizeof(type)))
#define SEA_ALLOC_N(type, n)        ((type*)sea_malloc(sizeof(type) * (size_t)(n)))
#define SEA_REALLOC_N(p, type, o, n) ((type*)sea_realloc((p), sizeof(type)*(size_t)(o), sizeof(type)*(size_t)(n)))
#define SEA_FREE(p, type)           sea_free((p), sizeof(type))
#define SEA_FREE_N(p, type, n)      sea_free((p), sizeof(type) * (size_t)(n))

// Dynamic array helpers
#define SEA_DYNARRAY_PUSH(arr, len, cap, type, val) do { \
    if ((len) >= (cap)) { \
        size_t _old = (size_t)(cap); \
        (cap) = (int)(_old < 8 ? 8 : _old * 2); \
        (arr) = SEA_REALLOC_N(arr, type, _old, (cap)); \
    } \
    (arr)[(len)++] = (val); \
} while(0)

#define SEA_DYNARRAY_FREE(arr, len, type) do { \
    if ((arr)) { sea_free((arr), sizeof(type) * (size_t)(len)); (arr) = NULL; } \
    (len) = 0; \
} while(0)
