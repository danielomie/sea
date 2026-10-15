#pragma once
#include "sea_object.h"
#include "sea_memory.h"

/*
 * Sea string type.
 * Strings are immutable heap objects with FNV-1a hashing.
 * A global interning table deduplicates identical strings.
 */

// Bit 31 of hash: hash not yet computed (lazy).
// Bit 30 of hash: string is in the global intern table.
// Bits 29..0: actual FNV-1a hash value.
#define SEA_HASH_UNCOMPUTED  UINT32_C(0x80000000)
#define SEA_STRING_INTERNED  UINT32_C(0x40000000)
#define SEA_STRING_HASH_MASK (~(SEA_HASH_UNCOMPUTED | SEA_STRING_INTERNED))

#define sea_string_is_interned(s) (!!((s)->hash & SEA_STRING_INTERNED))
#define sea_string_set_interned(s)   ((s)->hash |=  SEA_STRING_INTERNED)
#define sea_string_clr_interned(s)   ((s)->hash &= ~SEA_STRING_INTERNED)
// Pure hash value (no flag bits):
#define sea_string_pure_hash(s)   ((s)->hash & SEA_STRING_HASH_MASK)

struct SeaString {
    SeaObject  obj;        // must be first
    uint32_t   len;
    uint32_t   hash;       // bits 29..0 = FNV-1a hash; bit 30 = INTERNED; bit 31 = UNCOMPUTED
    char       data[];     // flexible array member, null-terminated
};

// Create a new string (copies data, computes hash eagerly)
SeaString* sea_string_new(const char *data, size_t len);

// Allocate a string of given length with uninitialized data[]; hash_valid=false.
// Caller must fill data[0..len] and optionally call sea_string_ensure_hash().
SeaString* sea_string_alloc(size_t len);

// Create a string from a null-terminated C string
SeaString* sea_string_from_cstr(const char *cstr);

// Concatenate two strings into a new string (hash computed lazily)
SeaString* sea_string_concat(SeaString *a, SeaString *b);

// Append b to a IN PLACE (a must have refcount==1 and not be interned).
// Returns the (possibly relocated) string; old pointer to a must be discarded.
SeaString* sea_string_append_inplace(SeaString *a, SeaString *b);

// Concatenate N strings into one allocation (hash computed lazily)
SeaString* sea_string_concat_n(SeaString **parts, int n);

// Ensure the hash field is valid (computes if hash_valid == false)
void sea_string_ensure_hash(SeaString *s);

// Intern a string (returns existing if identical string is interned)
SeaString* sea_string_intern(const char *data, size_t len);
SeaString* sea_string_find_intern(const char *data, size_t len);   /* lookup only, +1 or NULL */

// String comparison (content equality)
bool sea_string_equal(SeaString *a, SeaString *b);

// Get null-terminated C string pointer
const char* sea_string_cstr(SeaString *s);

// FNV-1a hash computation
uint32_t sea_string_hash(const char *data, size_t len);

// Initialize/free the global interning table
void sea_string_intern_init(void);
void sea_string_intern_free(void);

// Single-byte string cache: 256 pre-allocated strings (thread-safe, read-only after init)
void       sea_string_cache_init(void);
void       sea_string_cache_free(void);
SeaString* sea_string_get_byte(unsigned char c);  // retain + return cached string

// Small-integer string cache: integers [0, SEA_INT_CACHE_MAX)
#define SEA_INT_CACHE_MAX 10000
void       sea_string_int_cache_init(void);
void       sea_string_int_cache_free(void);

// Number to string conversion
SeaString* sea_number_to_string(double n);
SeaString* sea_int_to_string(int64_t i);
