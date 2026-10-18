/*
 * Copyright (c) 2026 Daniel
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once
#include "sea_object.h"
#include "sea_memory.h"
#include <pthread.h>

/*
 * Sea hash table - used for both maps and struct-like tables.
 * Uses open addressing with linear probing.
 * Capacity is always a power of 2.
 */

typedef struct {
    SeaValue key;
    SeaValue value;
} SeaTableEntry;

// Table mode
typedef enum {
    TABLE_MAP    = 0,   // general-purpose key-value map
    TABLE_STRUCT = 1,   // struct: string-keyed, field cache enabled
} SeaTableMode;

struct SeaTable {
    SeaObject     obj;
    SeaTableMode  mode;
    uint32_t      count;
    uint32_t      cap;      // always power of 2
    SeaTableEntry *entries;
};

// Create a new table
SeaTable* sea_table_new(SeaTableMode mode);

/* Inline cache for a lookup site (one per LOAD_GLOBAL / GET_FIELD
 * instruction): where the key was found last time. A hit is validated against
 * the table's current entries array, the index bound and the stored key, so
 * a stale cache (resize, delete, another table, a torn update from another
 * thread) just misses and is refilled. */
typedef struct {
    SeaTableEntry *entries;
    uint32_t       idx;
} SeaTableIC;

bool sea_table_get_ic_slow(SeaTable *t, SeaValue key, SeaTableIC *ic, SeaValue *out);

/* cap is loaded before entries (acquire): a table published with
 * sea_table_set_sync stores entries before cap, so a reader that sees the new
 * cap also sees the new entries, and one that sees the old cap indexes either
 * array in bounds. On ordinary tables these are plain loads. */
static inline bool sea_table_ic_hit(SeaTable *t, SeaValue key, SeaTableIC *ic, SeaValue *out) {
    SeaTableEntry *e = __atomic_load_n(&ic->entries, __ATOMIC_RELAXED);
    uint32_t i = __atomic_load_n(&ic->idx, __ATOMIC_RELAXED);
    uint32_t cap = __atomic_load_n(&t->cap, __ATOMIC_ACQUIRE);
    if (e && e == __atomic_load_n(&t->entries, __ATOMIC_RELAXED) && i < cap &&
        __atomic_load_n(&e[i].key, __ATOMIC_RELAXED) == key) {
        *out = __atomic_load_n(&e[i].value, __ATOMIC_RELAXED);
        return true;
    }
    return false;
}

static inline bool sea_table_get_ic(SeaTable *t, SeaValue key, SeaTableIC *ic, SeaValue *out) {
    if (sea_table_ic_hit(t, key, ic, out)) return true;
    return sea_table_get_ic_slow(t, key, ic, out);
}

/* ---- Tables read by many threads without a lock (the globals) -----------
 * Writers take `mu` and run a seqlock (`seq` odd while a write is under
 * way); readers never lock: they retry if `seq` moved. A resize publishes the
 * new array before the new cap and keeps the old array on `retired` (freed by
 * sea_table_sync_destroy) because a reader may still be probing it; growth is
 * geometric, so the retired arrays add up to less than the live one. Readers
 * must use sea_table_get_sync / sea_table_get_ic (never sea_table_get), and
 * all writes must go through sea_table_set_sync. */
typedef struct {
    pthread_mutex_t mu;
    uint32_t        seq;
    SeaTableEntry **retired;
    uint32_t       *retired_cap;   /* entry count of each retired array */
    uint32_t        n_retired, retired_size;
} SeaTableSync;

void sea_table_sync_init(SeaTableSync *ts);
void sea_table_sync_destroy(SeaTableSync *ts);
void sea_table_set_sync(SeaTable *t, SeaTableSync *ts, SeaValue key, SeaValue val);
/* ic may be NULL */
bool sea_table_get_sync(SeaTable *t, SeaTableSync *ts, SeaValue key, SeaTableIC *ic, SeaValue *out);

static inline bool sea_table_get_ic_sync(SeaTable *t, SeaTableSync *ts, SeaValue key,
                                         SeaTableIC *ic, SeaValue *out) {
    if (sea_table_ic_hit(t, key, ic, out)) return true;
    return sea_table_get_sync(t, ts, key, ic, out);
}

// Get a value by key (returns false if not found)
bool sea_table_get(SeaTable *t, SeaValue key, SeaValue *out);

// Set a key-value pair
void sea_table_set(SeaTable *t, SeaValue key, SeaValue val);
// Set, taking over the caller's reference to val (no retain)
void sea_table_set_move(SeaTable *t, SeaValue key, SeaValue val);

// Delete a key (returns true if key existed)
bool sea_table_delete(SeaTable *t, SeaValue key);

// Iterate: *iter starts at 0, set to next index after call
// Returns false when iteration is done
bool sea_table_next(SeaTable *t, uint32_t *iter, SeaValue *key, SeaValue *val);

// Copy all entries from src to dst
void sea_table_merge(SeaTable *dst, SeaTable *src);

// Get length (number of entries)
uint32_t sea_table_count(SeaTable *t);

// Free internal storage
void sea_table_destroy(SeaTable *t);
// Free internal storage without releasing the entries (cycle collector)
void sea_table_free_storage(SeaTable *t);

// Key sentinels
// We use SEA_NIL_VAL as tombstone marker for deleted entries
// and a special TOMBSTONE value distinct from nil

// Convenience: set with string key (common operation)
void sea_table_set_str(SeaTable *t, const char *key, SeaValue val);
bool sea_table_get_str(SeaTable *t, const char *key, SeaValue *out);

// For each string key that starts with an uppercase letter, add an alias
// with the key fully lowercased (only if the lowercase key isn't already set).
// Called automatically by sea_vm_register_module.
void sea_table_add_lowercase_aliases(SeaTable *t);
