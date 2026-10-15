#pragma once
#include "sea_object.h"
#include "sea_channel.h"
#include "sea_vm.h"
#include "sea_sched.h"
#include <pthread.h>

/*
 * Sea goroutine - lightweight concurrent execution unit.
 * Each goroutine has its own VM instance.
 *
 * sched_next  — intrusive link used by SeaRunQueue (Fase 1+)
 * status      — lifecycle state machine (Fase 1+)
 */

struct SeaGoroutine {
    SeaObject     obj;
    SeaVM        *vm;          /* goroutine's own VM                      */
    pthread_t     thread;
    bool          done;
    SeaValue      result;
    SeaValue      fn;
    SeaValue     *args;
    int           argc;

    /* Scheduler fields (Fase 1) */
    GoroStatus    status;      /* current lifecycle state                 */
    SeaGoroutine *sched_next;  /* next in run queue OR channel wait list  */

    /* Shared state from parent (Fase 2) — held until child VM is created */
    SeaShared    *parent_shared;   /* borrowed; cleared after VM init */

    /* Channel blocking state (Fase 4) */
    SeaChannel   *block_chan;       /* channel this goroutine is blocked on  */
    SeaValue      block_val;        /* value to send (BLOCKED_SEND) or
                                       pre-loaded recv value (direct handoff) */
    bool          has_pending_recv; /* true: block_val holds pre-loaded recv  */

    /* Park handshake (sea_sched.c): GORO_PARK_RUNNING while a worker runs the
     * goroutine, GORO_PARK_PARKED once that worker has completely finished
     * suspending it, GORO_PARK_WOKEN when a waker called sea_sched_requeue
     * before that. Whichever side arrives second pushes it on a run queue, so
     * the goroutine never runs on two workers at once. Accessed atomically. */
    int           park_state;
};

#define GORO_PARK_RUNNING 0
#define GORO_PARK_PARKED  1
#define GORO_PARK_WOKEN   2

// Spawn a goroutine (non-blocking)
SeaGoroutine* sea_goroutine_spawn(SeaVM *parent, SeaValue fn, SeaValue *args, int argc);

// Wait for a goroutine to complete
void sea_goroutine_join(SeaGoroutine *g);
