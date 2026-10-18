/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once
#include "sea_vm.h"

/*
 * Cycle collector (docs/memory.md). Reference counting frees everything
 * except objects that keep each other alive in a cycle. A container whose
 * count is decremented to a non-zero value is buffered as a possible root
 * (sea_gc_possible_root, sea_object.h); a collection subtracts the references
 * internal to the subgraphs reachable from the roots (trial deletion, Bacon &
 * Rajan 2001): what drops to zero is held only by cycles and is freed.
 *
 * Each goroutine's VM has its own buffer and collects only objects that are
 * not shared (never seen by another goroutine), so no locking is needed;
 * shared objects count as external references. Collections run at safe
 * points of the interpreter (allocations, returns), in some JIT helpers, when
 * a goroutine finishes, and on gc.collect().
 */

/* The VM of the goroutine running on this thread: where possible roots are
 * buffered (NULL: nothing is buffered). Set by sea_vm_exec and the scheduler. */
extern __thread SeaVM *sea_gc_vm;

/* Run a collection on vm's buffer; returns the number of objects freed. */
uint32_t sea_gc_collect(SeaVM *vm);

/* A goroutine is done: collect what is left in its buffer, free the buffer. */
void sea_gc_vm_finish(SeaVM *vm);

/* Safe-point check: collect once the buffer is past its threshold. */
static inline void sea_gc_maybe_collect(SeaVM *vm) {
    if (SEA_UNLIKELY(vm->gc_nroots > vm->gc_threshold)) sea_gc_collect(vm);
}

/* sea_obj_free of a buffered object: release its contents now, keep the
 * header (the buffer points to it) until the collector frees it. */
void sea_gc_release_contents(SeaObject *o);
bool sea_gc_unbuffer_dying(SeaObject *o);
void sea_gc_free_header(SeaObject *o);

/* The `gc` module: gc.collect(), gc.stats(). */
void sea_lib_gc_register(SeaVM *vm);
