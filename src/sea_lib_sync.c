/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sea_stdlib.h"
#include "sea_netpoll.h"
#include "sea_string.h"
#include "sea_table.h"
#include "sea_memory.h"
#include <pthread.h>
#include <sched.h>
#include <time.h>

/* A goroutine that has to wait (for a mutex, for a waitgroup) parks for a
 * millisecond and tries again, so its scheduler thread runs other goroutines
 * meanwhile. Blocking the thread instead (pthread_mutex_lock) deadlocked as
 * soon as the waiters outnumbered the threads while the holder was parked
 * (sleeping, doing I/O): it had no thread left to resume on. A goroutine also
 * moves between threads, so the lock cannot be a pthread mutex (unlocking one
 * from another thread is undefined). Returns SEA_PARK_VAL to park, or
 * SEA_NIL_VAL when the caller should just try again (outside a goroutine, or
 * in a native callback, where parking is impossible: there it sleeps briefly). */
static SeaValue wait_a_little(SeaVM *vm) {
    if (vm->goroutine && vm->native_depth == 0) {
        if (vm->io_call.active) sea_io_end(vm);   /* the previous wait is over */
        sea_io_begin(vm, 1);
        if (sea_io_wait(vm, -1, 0) == SEA_IO_PARK) return SEA_PARK_VAL;
        sea_io_end(vm);
        return SEA_NIL_VAL;
    }
    struct timespec ts = {0, 200000};   /* 0.2 ms */
    nanosleep(&ts, NULL);
    return SEA_NIL_VAL;
}

/* The wait is over: drop the I/O call a park left open. */
static void done_waiting(SeaVM *vm) {
    if (vm->io_call.active) sea_io_end(vm);
}

// ---- Mutex ----

/* 0 = free, 1 = held; any goroutine may release it */
static int *mutex_flag(int argc, SeaValue *args) {
    if (argc < 1 || !sea_is_table(args[0])) return NULL;
    SeaValue ptr_v = SEA_NIL_VAL;
    sea_table_get_str(SEA_AS_TABLE(args[0]), "__mutex_ptr", &ptr_v);
    return sea_is_int(ptr_v) ? (int*)(uintptr_t)(uint64_t)sea_as_int(ptr_v) : NULL;
}

static bool mutex_take(int *flag) {
    int free_ = 0;
    return __atomic_compare_exchange_n(flag, &free_, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static SeaValue mutex_acquire(SeaVM *vm, int argc, SeaValue *args) {
    int *flag = mutex_flag(argc, args);
    if (!flag) return SEA_NIL_VAL;
    for (;;) {
        if (mutex_take(flag)) {
            done_waiting(vm);
            return SEA_NIL_VAL;
        }
        SeaValue w = wait_a_little(vm);
        if (w == SEA_PARK_VAL) return w;   /* this CALL runs again after the wait */
    }
}

static SeaValue mutex_release(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    int *flag = mutex_flag(argc, args);
    if (flag) __atomic_store_n(flag, 0, __ATOMIC_RELEASE);
    return SEA_NIL_VAL;
}

static SeaValue mutex_try_lock(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    int *flag = mutex_flag(argc, args);
    return sea_bool_val(flag && mutex_take(flag));
}

// sync.mutex() -> table with acquire, release, try_lock methods
static SeaValue sync_mutex(SeaVM *vm, int argc, SeaValue *args) {
    (void)argc; (void)args;
    int *mu = (int*)sea_malloc(sizeof(int));
    *mu = 0;

    SeaTable *t = sea_table_new(TABLE_STRUCT);
    // Store pointer as int
    sea_table_set_str(t, "__mutex_ptr", sea_int_val((int64_t)(uintptr_t)mu));
    mod_set(vm, t, "acquire",  mutex_acquire);
    mod_set(vm, t, "release",  mutex_release);
    mod_set(vm, t, "try_lock", mutex_try_lock);

    /* Return rc=1; caller owns */
    return sea_obj_val(&t->obj);
}

// ---- WaitGroup ----

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t  cond;
    int             count;
} WaitGroup;

static SeaValue wg_add(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 2 || !sea_is_table(args[0])) return SEA_NIL_VAL;
    SeaTable *t = SEA_AS_TABLE(args[0]);
    SeaValue ptr_v = SEA_NIL_VAL;
    sea_table_get_str(t, "__wg_ptr", &ptr_v);
    if (!sea_is_int(ptr_v)) return SEA_NIL_VAL;
    WaitGroup *wg = (WaitGroup*)(uintptr_t)(uint64_t)sea_as_int(ptr_v);
    int64_t delta = sea_is_int(args[1]) ? sea_as_int(args[1]) : (int64_t)sea_as_number(args[1]);
    pthread_mutex_lock(&wg->mu);
    wg->count += (int)delta;
    if (wg->count <= 0) pthread_cond_broadcast(&wg->cond);
    pthread_mutex_unlock(&wg->mu);
    return SEA_NIL_VAL;
}

static SeaValue wg_done(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_table(args[0])) return SEA_NIL_VAL;
    SeaTable *t = SEA_AS_TABLE(args[0]);
    SeaValue ptr_v = SEA_NIL_VAL;
    sea_table_get_str(t, "__wg_ptr", &ptr_v);
    if (!sea_is_int(ptr_v)) return SEA_NIL_VAL;
    WaitGroup *wg = (WaitGroup*)(uintptr_t)(uint64_t)sea_as_int(ptr_v);
    pthread_mutex_lock(&wg->mu);
    wg->count--;
    if (wg->count <= 0) pthread_cond_broadcast(&wg->cond);
    pthread_mutex_unlock(&wg->mu);
    return SEA_NIL_VAL;
}

static SeaValue wg_wait(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 1 || !sea_is_table(args[0])) return SEA_NIL_VAL;
    SeaTable *t = SEA_AS_TABLE(args[0]);
    SeaValue ptr_v = SEA_NIL_VAL;
    sea_table_get_str(t, "__wg_ptr", &ptr_v);
    if (!sea_is_int(ptr_v)) return SEA_NIL_VAL;
    WaitGroup *wg = (WaitGroup*)(uintptr_t)(uint64_t)sea_as_int(ptr_v);
    if (!vm->goroutine || vm->native_depth > 0) {
        /* cannot park (the main thread, a native callback): block on the condition */
        pthread_mutex_lock(&wg->mu);
        while (wg->count > 0) pthread_cond_wait(&wg->cond, &wg->mu);
        pthread_mutex_unlock(&wg->mu);
        return SEA_NIL_VAL;
    }
    for (;;) {
        pthread_mutex_lock(&wg->mu);
        bool zero = wg->count <= 0;
        pthread_mutex_unlock(&wg->mu);
        if (zero) {
            done_waiting(vm);
            return SEA_NIL_VAL;
        }
        SeaValue w = wait_a_little(vm);   /* see mutex_acquire */
        if (w == SEA_PARK_VAL) return w;
    }
}

static SeaValue sync_waitgroup(SeaVM *vm, int argc, SeaValue *args) {
    (void)argc; (void)args;
    WaitGroup *wg = (WaitGroup*)sea_malloc(sizeof(WaitGroup));
    pthread_mutex_init(&wg->mu, NULL);
    pthread_cond_init(&wg->cond, NULL);
    wg->count = 0;

    SeaTable *t = sea_table_new(TABLE_STRUCT);
    sea_table_set_str(t, "__wg_ptr", sea_int_val((int64_t)(uintptr_t)wg));
    mod_set(vm, t, "add",  wg_add);
    mod_set(vm, t, "done", wg_done);
    mod_set(vm, t, "wait", wg_wait);

    /* Return rc=1; caller owns */
    return sea_obj_val(&t->obj);
}

// ---- Once ----

typedef struct {
    pthread_mutex_t mu;
    bool done;
} Once;

static SeaValue once_do(SeaVM *vm, int argc, SeaValue *args) {
    if (argc < 2 || !sea_is_table(args[0])) return SEA_NIL_VAL;
    SeaTable *t = SEA_AS_TABLE(args[0]);
    SeaValue ptr_v = SEA_NIL_VAL;
    sea_table_get_str(t, "__once_ptr", &ptr_v);
    if (!sea_is_int(ptr_v)) return SEA_NIL_VAL;
    Once *once = (Once*)(uintptr_t)(uint64_t)sea_as_int(ptr_v);
    pthread_mutex_lock(&once->mu);
    if (!once->done) {
        once->done = true;
        pthread_mutex_unlock(&once->mu);
        // Call the function (its result is owned: drop it)
        SeaValue r = sea_vm_call(vm, args[1], NULL, 0);
        if (sea_is_obj(r)) sea_obj_release(sea_as_obj(r));
    } else {
        pthread_mutex_unlock(&once->mu);
    }
    return SEA_NIL_VAL;
}

static SeaValue sync_once(SeaVM *vm, int argc, SeaValue *args) {
    (void)argc; (void)args;
    Once *once = (Once*)sea_malloc(sizeof(Once));
    pthread_mutex_init(&once->mu, NULL);
    once->done = false;

    SeaTable *t = sea_table_new(TABLE_STRUCT);
    sea_table_set_str(t, "__once_ptr", sea_int_val((int64_t)(uintptr_t)once));
    mod_set(vm, t, "do_once", once_do);

    /* Return rc=1; caller owns */
    return sea_obj_val(&t->obj);
}

void sea_lib_sync_register(SeaVM *vm) {
    SeaTable *mod = sea_table_new(TABLE_MAP);

    mod_set(vm, mod, "mutex",     sync_mutex);
    mod_set(vm, mod, "waitgroup", sync_waitgroup);
    mod_set(vm, mod, "once",      sync_once);

    sea_vm_register_module(vm, "sync", mod);
    sea_obj_release(&mod->obj);
}
