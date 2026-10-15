#include "sea_string.h"
#include <math.h>
#include <inttypes.h>
#include <pthread.h>

/*
 * String implementation with interning.
 * Uses a simple open-addressing hash table for the intern pool.
 */

// ---- FNV-1a hash ----

uint32_t sea_string_hash(const char *data, size_t len) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint8_t)data[i];
        hash *= 16777619u;
    }
    return hash;
}

// ---- Intern table ----

typedef struct {
    SeaString **entries;
    uint32_t   cap;
    uint32_t   count;
} InternTable;

static InternTable g_interns = {NULL, 0, 0};

/* The intern table is process-global and goroutines run on several scheduler
 * threads (natives call sea_table_get_str -> sea_string_intern), so the public
 * entry points (sea_string_intern, sea_string_remove_intern,
 * sea_string_intern_free) hold this lock; the static helpers assume it held. */
static pthread_mutex_t g_interns_mu = PTHREAD_MUTEX_INITIALIZER;

void sea_string_intern_init(void) {
    g_interns.cap = 64;
    g_interns.entries = (SeaString**)sea_malloc(sizeof(SeaString*) * 64);
    memset(g_interns.entries, 0, sizeof(SeaString*) * 64);
    g_interns.count = 0;
}

void sea_string_intern_free(void) {
    /* Release the intern table's own reference to each string.
     * If rc drops to 0 here, it means all callers already released their refs
     * and this is the final release. (The interned flag is cleared first, so
     * the free path's sea_string_remove_intern returns without locking.) */
    pthread_mutex_lock(&g_interns_mu);
    if (g_interns.entries) {
        for (uint32_t i = 0; i < g_interns.cap; i++) {
            SeaString *s = g_interns.entries[i];
            if (s) {
                /* Clear interned flag first so sea_string_remove_intern is a no-op */
                sea_string_clr_interned(s);
                if (s->obj.refcount & SEA_RC_PINNED_BIT)   /* pinned chunk constant */
                    s->obj.refcount = 1;
                sea_obj_release(&s->obj);
            }
        }
        sea_free(g_interns.entries, sizeof(SeaString*) * g_interns.cap);
    }
    g_interns.entries = NULL;
    g_interns.cap = 0;
    g_interns.count = 0;
    pthread_mutex_unlock(&g_interns_mu);
}

static void intern_resize(void) {
    uint32_t new_cap = g_interns.cap * 2;
    SeaString **new_entries = (SeaString**)sea_malloc(sizeof(SeaString*) * new_cap);
    memset(new_entries, 0, sizeof(SeaString*) * new_cap);
    // Rehash
    for (uint32_t i = 0; i < g_interns.cap; i++) {
        SeaString *s = g_interns.entries[i];
        if (!s) continue;
        uint32_t idx = sea_string_pure_hash(s) & (new_cap - 1);
        while (new_entries[idx]) {
            idx = (idx + 1) & (new_cap - 1);
        }
        new_entries[idx] = s;
    }
    sea_free(g_interns.entries, sizeof(SeaString*) * g_interns.cap);
    g_interns.entries = new_entries;
    g_interns.cap = new_cap;
}

static SeaString* intern_find(const char *data, size_t len, uint32_t hash) {
    if (!g_interns.entries) return NULL;
    uint32_t idx = hash & (g_interns.cap - 1);
    while (g_interns.entries[idx]) {
        SeaString *s = g_interns.entries[idx];
        if (sea_string_pure_hash(s) == hash && s->len == len &&
            memcmp(s->data, data, len) == 0) {
            return s;
        }
        idx = (idx + 1) & (g_interns.cap - 1);
    }
    return NULL;
}

static void intern_insert(SeaString *s) {
    if (!g_interns.entries) sea_string_intern_init();
    if (g_interns.count * 3 >= g_interns.cap * 2) {
        intern_resize();
    }
    uint32_t idx = sea_string_pure_hash(s) & (g_interns.cap - 1);
    while (g_interns.entries[idx]) {
        idx = (idx + 1) & (g_interns.cap - 1);
    }
    g_interns.entries[idx] = s;
    g_interns.count++;
}

// Remove a string from intern table (called on free)
void sea_string_remove_intern(SeaString *s) {
    if (!sea_string_is_interned(s)) return;
    pthread_mutex_lock(&g_interns_mu);
    if (!g_interns.entries) { pthread_mutex_unlock(&g_interns_mu); return; }
    uint32_t idx = sea_string_pure_hash(s) & (g_interns.cap - 1);
    while (g_interns.entries[idx]) {
        if (g_interns.entries[idx] == s) {
            g_interns.entries[idx] = NULL;
            g_interns.count--;
            // Rehash subsequent entries
            uint32_t j = (idx + 1) & (g_interns.cap - 1);
            while (g_interns.entries[j]) {
                SeaString *rehash = g_interns.entries[j];
                g_interns.entries[j] = NULL;
                g_interns.count--;
                intern_insert(rehash);
                j = (j + 1) & (g_interns.cap - 1);
            }
            pthread_mutex_unlock(&g_interns_mu);
            return;
        }
        idx = (idx + 1) & (g_interns.cap - 1);
    }
    pthread_mutex_unlock(&g_interns_mu);
}

// ---- String constructors ----

/* Allocate a SeaString header + len+1 bytes; caller fills data[].
 * hash not yet computed (SEA_HASH_UNCOMPUTED), not interned, rc=1. */
SeaString* sea_string_alloc(size_t len) {
    SeaString *s = (SeaString*)sea_malloc(sizeof(SeaString) + len + 1);
    s->obj.type     = OBJ_STRING;
    s->obj.gc = 0;
    s->obj.refcount = 1;
    s->len          = (uint32_t)len;
    s->hash         = SEA_HASH_UNCOMPUTED;  // interned bit is 0 by default
    return s;
}

/* Compute and cache the hash if not already computed. */
void sea_string_ensure_hash(SeaString *s) {
    if (s->hash & SEA_HASH_UNCOMPUTED) {
        uint32_t h = sea_string_hash(s->data, s->len);
        /* Preserve the INTERNED flag (bit 30); clear UNCOMPUTED (bit 31) and mask
         * the computed hash to bits 29..0 so neither flag bit is accidentally set. */
        uint32_t interned_bit = s->hash & SEA_STRING_INTERNED;
        s->hash = (h & SEA_STRING_HASH_MASK) | interned_bit;
    }
}

SeaString* sea_string_new(const char *data, size_t len) {
    SeaString *s = sea_string_alloc(len);
    s->hash       = sea_string_hash(data, len) & ~SEA_HASH_UNCOMPUTED;
    if (data) memcpy(s->data, data, len);
    s->data[len] = '\0';
    return s;
}

SeaString* sea_string_from_cstr(const char *cstr) {
    if (!cstr) return sea_string_new("", 0);
    return sea_string_new(cstr, strlen(cstr));
}

/* The interned string with these bytes (+1), or NULL. Never inserts: for
 * data from outside (e.g. JSON keys), which must not grow the intern table,
 * but can share an instance the program already interned (a constant). */
SeaString* sea_string_find_intern(const char *data, size_t len) {
    uint32_t hash = sea_string_hash(data, len) & SEA_STRING_HASH_MASK;
    pthread_mutex_lock(&g_interns_mu);
    SeaString *s = intern_find(data, len, hash);
    if (s) sea_obj_retain(&s->obj);
    pthread_mutex_unlock(&g_interns_mu);
    return s;
}

SeaString* sea_string_intern(const char *data, size_t len) {
    uint32_t hash = sea_string_hash(data, len) & SEA_STRING_HASH_MASK;
    pthread_mutex_lock(&g_interns_mu);
    if (!g_interns.entries) sea_string_intern_init();
    SeaString *existing = intern_find(data, len, hash);
    if (existing) {
        sea_obj_retain(&existing->obj);
        pthread_mutex_unlock(&g_interns_mu);
        return existing;
    }
    /* New interned string: rc=2 (one for intern table, one for caller).
     * Born shared: an interned string is one object reachable from every
     * thread (chunk constants, table keys, natives' sea_table_get_str), so its
     * refcount must always be updated atomically. */
    SeaString *s = sea_string_alloc(len);
    s->hash         = hash;
    if (data) memcpy(s->data, data, len);
    s->data[len]    = '\0';
    s->obj.refcount = 2 | SEA_RC_SHARED_BIT;
    sea_string_set_interned(s);
    intern_insert(s);
    pthread_mutex_unlock(&g_interns_mu);
    return s;
}

/* Append b to a in place via realloc.
 * Caller guarantees a->obj.refcount == 1 and !a->interned. */
SeaString* sea_string_append_inplace(SeaString *a, SeaString *b) {
    size_t old_len = a->len;
    size_t new_len = old_len + (size_t)b->len;
    SeaString *s = (SeaString*)sea_realloc(a,
        sizeof(SeaString) + old_len + 1,
        sizeof(SeaString) + new_len + 1);
    memcpy(s->data + old_len, b->data, b->len);
    s->len  = (uint32_t)new_len;
    s->data[new_len] = '\0';
    s->hash = SEA_HASH_UNCOMPUTED;
    return s;
}

/* Binary concat: ONE allocation, no intermediate copy, lazy hash.
 * Hot path for s = s .. x. */
SeaString* sea_string_concat(SeaString *a, SeaString *b) {
    size_t total = (size_t)a->len + (size_t)b->len;
    SeaString *s  = sea_string_alloc(total);
    memcpy(s->data,           a->data, a->len);
    memcpy(s->data + a->len,  b->data, b->len);
    s->data[total] = '\0';
    /* hash_valid stays false — only compute on demand */
    return s;
}

/* Concat N strings in one allocation.
 * Used by the OP_CONCAT handler when there are >2 operands. */
SeaString* sea_string_concat_n(SeaString **parts, int n) {
    size_t total = 0;
    for (int i = 0; i < n; i++) total += parts[i]->len;
    SeaString *s = sea_string_alloc(total);
    char *p = s->data;
    for (int i = 0; i < n; i++) {
        memcpy(p, parts[i]->data, parts[i]->len);
        p += parts[i]->len;
    }
    *p = '\0';
    return s;
}

bool sea_string_equal(SeaString *a, SeaString *b) {
    if (a == b) return true;
    if (a->len != b->len) return false;
    /* Only use hash shortcut when both are already computed */
    bool a_ready = !(a->hash & SEA_HASH_UNCOMPUTED);
    bool b_ready = !(b->hash & SEA_HASH_UNCOMPUTED);
    if (a_ready && b_ready && sea_string_pure_hash(a) != sea_string_pure_hash(b)) return false;
    return memcmp(a->data, b->data, a->len) == 0;
}

const char* sea_string_cstr(SeaString *s) {
    return s->data;
}

SeaString* sea_number_to_string(double n) {
    char buf[64];
    // Check if it's an integer value
    if (n == (double)(int64_t)n && n >= -1e15 && n <= 1e15) {
        snprintf(buf, sizeof(buf), "%" PRId64, (int64_t)n);
    } else {
        snprintf(buf, sizeof(buf), "%.14g", n);
    }
    return sea_string_new(buf, strlen(buf));
}

/* ---- Small-integer string cache ------------------------------------------ */

static SeaString *g_int_cache[SEA_INT_CACHE_MAX];

void sea_string_int_cache_init(void) {
    for (int i = 0; i < SEA_INT_CACHE_MAX; i++) {
        char buf[8];
        int len = snprintf(buf, sizeof(buf), "%d", i);
        g_int_cache[i] = sea_string_new(buf, (size_t)len);
        sea_obj_pin(&g_int_cache[i]->obj);   /* read by every thread: no RC */
        /* RC=1: the cache holds the permanent reference. */
    }
}

void sea_string_int_cache_free(void) {
    for (int i = 0; i < SEA_INT_CACHE_MAX; i++) {
        if (g_int_cache[i]) {
            g_int_cache[i]->obj.refcount = 1;   /* unpin */
            sea_obj_release(&g_int_cache[i]->obj);
            g_int_cache[i] = NULL;
        }
    }
}

SeaString* sea_int_to_string(int64_t i) {
    if (i >= 0 && i < SEA_INT_CACHE_MAX) {
        SeaString *s = g_int_cache[i];
        sea_obj_retain(&s->obj);
        return s;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%" PRId64, i);
    return sea_string_new(buf, strlen(buf));
}

/* ---- Single-byte string cache -------------------------------------------- */
/* 256 pre-allocated strings, one per possible byte value.
 * Thread-safe: created once at init and pinned (no refcounting).
 * Avoids a malloc per string.sub(s,i,i) call in tight loops. */

static SeaString *g_byte_cache[256];

void sea_string_cache_init(void) {
    for (int i = 0; i < 256; i++) {
        SeaString *s = sea_string_alloc(1);
        s->data[0]      = (char)(unsigned char)i;
        s->data[1]      = '\0';
        s->hash         = sea_string_hash(s->data, 1) & ~SEA_HASH_UNCOMPUTED;
        sea_obj_pin(&s->obj);   /* shared by every thread: retain/release are no-ops */
        g_byte_cache[i] = s;
    }
}

void sea_string_cache_free(void) {
    for (int i = 0; i < 256; i++) {
        if (g_byte_cache[i]) {
            g_byte_cache[i]->obj.refcount = 1;   /* unpin */
            sea_obj_release(&g_byte_cache[i]->obj);
            g_byte_cache[i] = NULL;
        }
    }
}

SeaString* sea_string_get_byte(unsigned char c) {
    SeaString *s = g_byte_cache[(int)c];
    sea_obj_retain(&s->obj);
    return s;
}
