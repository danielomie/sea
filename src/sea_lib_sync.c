/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sea_stdlib.h"
#include "sea_string.h"
#include "sea_table.h"
#include "sea_memory.h"
#include <pthread.h>

// ---- Mutex ----

typedef struct {
    SeaObject       obj;
    pthread_mutex_t mu;
} SeaMutex;

static SeaValue mutex_acquire(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_table(args[0])) return SEA_NIL_VAL;
    SeaTable *t = SEA_AS_TABLE(args[0]);
    SeaValue ptr_v = SEA_NIL_VAL;
    sea_table_get_str(t, "__mutex_ptr", &ptr_v);
    if (sea_is_int(ptr_v)) {
        pthread_mutex_t *mu = (pthread_mutex_t*)(uintptr_t)(uint64_t)sea_as_int(ptr_v);
        pthread_mutex_lock(mu);
    }
    return SEA_NIL_VAL;
}

static SeaValue mutex_release(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_table(args[0])) return SEA_NIL_VAL;
    SeaTable *t = SEA_AS_TABLE(args[0]);
    SeaValue ptr_v = SEA_NIL_VAL;
    sea_table_get_str(t, "__mutex_ptr", &ptr_v);
    if (sea_is_int(ptr_v)) {
        pthread_mutex_t *mu = (pthread_mutex_t*)(uintptr_t)(uint64_t)sea_as_int(ptr_v);
        pthread_mutex_unlock(mu);
    }
    return SEA_NIL_VAL;
}

static SeaValue mutex_try_lock(SeaVM *vm, int argc, SeaValue *args) {
    (void)vm;
    if (argc < 1 || !sea_is_table(args[0])) return SEA_FALSE_VAL;
    SeaTable *t = SEA_AS_TABLE(args[0]);
    SeaValue ptr_v = SEA_NIL_VAL;
    sea_table_get_str(t, "__mutex_ptr", &ptr_v);
    if (sea_is_int(ptr_v)) {
        pthread_mutex_t *mu = (pthread_mutex_t*)(uintptr_t)(uint64_t)sea_as_int(ptr_v);
        return sea_bool_val(pthread_mutex_trylock(mu) == 0);
    }
    return SEA_FALSE_VAL;
}

// sync.mutex() -> table with acquire, release, try_lock methods
static SeaValue sync_mutex(SeaVM *vm, int argc, SeaValue *args) {
    (void)argc; (void)args;
    pthread_mutex_t *mu = (pthread_mutex_t*)sea_malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(mu, NULL);

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
    (void)vm;
    if (argc < 1 || !sea_is_table(args[0])) return SEA_NIL_VAL;
    SeaTable *t = SEA_AS_TABLE(args[0]);
    SeaValue ptr_v = SEA_NIL_VAL;
    sea_table_get_str(t, "__wg_ptr", &ptr_v);
    if (!sea_is_int(ptr_v)) return SEA_NIL_VAL;
    WaitGroup *wg = (WaitGroup*)(uintptr_t)(uint64_t)sea_as_int(ptr_v);
    pthread_mutex_lock(&wg->mu);
    while (wg->count > 0) {
        pthread_cond_wait(&wg->cond, &wg->mu);
    }
    pthread_mutex_unlock(&wg->mu);
    return SEA_NIL_VAL;
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
