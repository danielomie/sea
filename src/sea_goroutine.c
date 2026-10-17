/*
 * Copyright (c) 2026 Daniel
 * SPDX-License-Identifier: MIT
 */

#include "sea_goroutine.h"
#include "sea_memory.h"
#include "sea_stdlib.h"
#include "sea_map.h"
#include "sea_sched.h"
#include "sea_object.h"

/*
 * Fase 3: goroutines are lightweight — no pthread_create per goroutine.
 * Instead, the goroutine is enqueued on the global scheduler, and a pool
 * of N worker threads executes them.
 */

SeaGoroutine* sea_goroutine_spawn(SeaVM *parent, SeaValue fn, SeaValue *args, int argc) {
    /* Try to reuse a goroutine from the pool before allocating */
    SeaGoroutine *g = sea_sched_pool_get(sea_global_sched);
    if (!g) {
        g = SEA_ALLOC(SeaGoroutine);
        g->obj.type    = OBJ_GOROUTINE;
        g->obj.gc = 0;
        g->vm          = NULL;
    }

    g->obj.refcount     = 1;
    g->done             = false;
    g->result           = SEA_NIL_VAL;
    g->fn               = fn;
    g->argc             = argc;
    g->status           = GORO_RUNNABLE;
    g->sched_next       = NULL;
    g->block_chan        = NULL;
    g->block_val         = SEA_NIL_VAL;
    g->has_pending_recv  = false;
    g->park_state        = GORO_PARK_RUNNING;
    g->io_waiting        = false;
    g->io_fd             = -1;
    g->io_deadline       = 0;
    g->io_heap           = -1;

    if (argc > 0) {
        g->args = SEA_ALLOC_N(SeaValue, argc);
        memcpy(g->args, args, sizeof(SeaValue) * (size_t)argc);
    } else {
        g->args = NULL;
    }

    g->parent_shared = parent->shared;

    /* Mark fn and all args as shared: they'll be accessed by the new goroutine
     * which may run on a different OS thread concurrently. */
    sea_val_mark_shared(fn);
    for (int i = 0; i < argc; i++) sea_val_mark_shared(args[i]);

    sea_sched_spawn(sea_global_sched, g);
    return g;
}

void sea_goroutine_join(SeaGoroutine *g) {
    /* With the scheduler, we can't busy-wait on a specific goroutine.
     * This is now handled by sea_sched_drain() or sync primitives. */
    (void)g;
}
