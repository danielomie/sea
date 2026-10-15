#pragma once
#include "sea_object.h"
#include "sea_memory.h"

/*
 * Swiss-table hash map.
 * Uses a control byte array for fast metadata probing:
 *   0x80 = EMPTY, 0xFE = DELETED (tombstone),
 *   0x00..0x7F = live entry (low 7 bits of key hash, bit 7 = 0).
 * h1 = hash >> 7  → slot index
 * h2 = hash & 0x7F → ctrl match byte
 * Load factor kept below 75%.
 */

#define SMAP_CTRL_EMPTY   ((uint8_t)0x80)
#define SMAP_CTRL_DELETED ((uint8_t)0xFE)

typedef struct {
    SeaValue key;
    SeaValue value;
} SeaMapEntry;

struct SeaMap {
    SeaObject    obj;    // must be first
    uint32_t     count;  // live (non-empty, non-deleted) entries
    uint32_t     cap;    // always power of 2
    uint8_t     *ctrl;   // cap bytes, one per slot
    SeaMapEntry *slots;  // cap entries, parallel to ctrl
};

SeaMap*  sea_map_new(void);
bool     sea_map_get(SeaMap *m, SeaValue key, SeaValue *out);
void     sea_map_set(SeaMap *m, SeaValue key, SeaValue val);
bool     sea_map_delete(SeaMap *m, SeaValue key);
bool     sea_map_next(SeaMap *m, uint32_t *iter, SeaValue *key, SeaValue *val);
uint32_t sea_map_count(SeaMap *m);
void     sea_map_destroy(SeaMap *m);
void     sea_map_free_storage(SeaMap *m);   /* no key/value release (cycle collector) */
