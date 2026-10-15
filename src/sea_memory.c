#include "sea_memory.h"

/*
 * Memory allocation primitives.
 * On OOM, we print an error and abort -- no recovery.
 *
 * Build with -DUSE_JEMALLOC (make jemalloc) to route all allocations through
 * jemalloc. Including <jemalloc/jemalloc.h> macros malloc/realloc/free to
 * jemalloc's implementations, giving per-thread caches and better multi-arena
 * scaling for goroutine workloads.
 * Requires: sudo apt install libjemalloc-dev
 */

#ifdef USE_JEMALLOC
#  include <jemalloc/jemalloc.h>
#endif

void* sea_malloc(size_t size) {
    if (size == 0) return NULL;
    void *ptr = malloc(size);
    if (SEA_UNLIKELY(!ptr)) {
        fprintf(stderr, "sea: out of memory (requested %zu bytes)\n", size);
        abort();
    }
    return ptr;
}

void* sea_realloc(void *ptr, size_t old_size, size_t new_size) {
    (void)old_size;
    if (new_size == 0) {
        free(ptr);
        return NULL;
    }
    void *new_ptr = realloc(ptr, new_size);
    if (SEA_UNLIKELY(!new_ptr)) {
        fprintf(stderr, "sea: out of memory (realloc %zu -> %zu bytes)\n",
                old_size, new_size);
        abort();
    }
    return new_ptr;
}

void sea_free(void *ptr, size_t size) {
    (void)size;
    free(ptr);
}
