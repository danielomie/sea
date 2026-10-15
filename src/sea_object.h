#pragma once
#include "sea_common.h"
#include "sea_value.h"

/*
 * All heap-allocated Sea objects share this header.
 * Reference counting is used for memory management.
 *
 * RC model — inspired by Rust Rc<T> vs Arc<T>:
 *   Bit 31 of refcount is the "shared" flag.
 *   - 0 (goroutine-local): plain ++/-- (no memory barrier, ~5× faster)
 *   - 1 (shared across goroutines): __atomic_fetch_add/sub
 * Objects are born local (bit 31 = 0). sea_obj_mark_shared() sets the flag
 * recursively before a value crosses a goroutine boundary (channel send,
 * goroutine spawn, global write).
 */

#define SEA_RC_SHARED_BIT  UINT32_C(0x80000000)
#define SEA_RC_ARENA_BIT   UINT32_C(0x40000000)  /* header in a frame arena: counted as
                                                    usual, but reaching zero tears down
                                                    the contents (the arena owns the header) */
#define SEA_RC_PINNED_BIT  UINT32_C(0x20000000)  /* pinned (chunk constants): no RC at all */
#define SEA_RC_FLAG_BITS   (SEA_RC_SHARED_BIT | SEA_RC_ARENA_BIT | SEA_RC_PINNED_BIT)
#define SEA_RC_COUNT_MASK  UINT32_C(0x1FFFFFFF)

// Object type tags
typedef enum {
    OBJ_STRING    = 0,
    OBJ_ARRAY     = 1,
    OBJ_TABLE     = 2,   // used for both maps and structs
    OBJ_FUNCTION  = 3,
    OBJ_CLOSURE   = 4,
    OBJ_UPVALUE   = 5,
    OBJ_ERROR     = 6,
    OBJ_CHANNEL   = 7,
    OBJ_GOROUTINE = 8,
    OBJ_MUTEX     = 9,
    OBJ_WAITGROUP = 10,
    OBJ_MAP       = 11,
    OBJ_ROPE      = 12,
    OBJ_STRUCT    = 13,  /* fixed-layout struct from {k=v,...} literals */
    OBJ_INT_ARRAY = 14,  /* typed int64_t array, no per-element RC       */
    OBJ_FLOAT_ARRAY= 15, /* typed double array, no per-element RC        */
} SeaObjType;

struct SeaObject {
    SeaObjType type;
    uint32_t   refcount;   /* bits 0–30: reference count; bit 31: shared flag */
};

/* Unconditionally free the object (called when RC drops to zero). */
void sea_obj_free(SeaObject *obj);

/* An arena object's count reached zero: release what it owns (an array's
 * elements and data buffer, a struct's field values); the header stays in the
 * frame arena, reclaimed in bulk when the frame returns. */
void sea_obj_arena_teardown(SeaObject *obj);

/* Mark obj (and all objects reachable from it) as shared so atomic RC ops are
 * used going forward. Call before transferring a value across goroutine boundaries
 * (channel send, goroutine spawn, global write). */
void sea_obj_mark_shared(SeaObject *obj);

static inline void sea_val_mark_shared(SeaValue v) {
    if (sea_is_obj(v)) sea_obj_mark_shared(sea_as_obj(v));
}

/* Inline retain/release: hot paths benefit from inlining; sea_object.c provides
 * the out-of-line (extern inline) copy so dlopen'd .so libraries can link them. */
__attribute__((always_inline))
inline void sea_obj_retain(SeaObject *obj) {
    if (!obj) return;
    /* Relaxed atomic load (a plain mov on x86): other threads may be updating
     * the count of a shared object concurrently; we only test the flag bits,
     * which never change once set. */
    uint32_t rc = __atomic_load_n(&obj->refcount, __ATOMIC_RELAXED);
    if (SEA_UNLIKELY(rc & SEA_RC_FLAG_BITS)) {
        if (rc & SEA_RC_PINNED_BIT) return;             /* pinned: no RC */
        if (rc & SEA_RC_SHARED_BIT) __atomic_fetch_add(&obj->refcount, 1u, __ATOMIC_RELAXED);
        else obj->refcount++;                           /* arena: frame-local */
    } else {
        obj->refcount++;
    }
}

__attribute__((always_inline))
inline void sea_obj_release(SeaObject *obj) {
    if (!obj) return;
    uint32_t rc = __atomic_load_n(&obj->refcount, __ATOMIC_RELAXED);   /* see retain */
    if (SEA_UNLIKELY(rc & SEA_RC_FLAG_BITS)) {
        if (rc & SEA_RC_PINNED_BIT) return;             /* pinned: no RC */
        if (rc & SEA_RC_SHARED_BIT) {
            uint32_t prev = __atomic_fetch_sub(&obj->refcount, 1u, __ATOMIC_ACQ_REL);
            if (SEA_UNLIKELY((prev & SEA_RC_COUNT_MASK) <= 1u)) sea_obj_free(obj);
        } else {                                        /* arena */
            uint32_t prev = obj->refcount--;
            if ((prev & SEA_RC_COUNT_MASK) <= 1u) sea_obj_arena_teardown(obj);
        }
    } else {
        uint32_t prev = obj->refcount--;
        if (SEA_UNLIKELY((prev & SEA_RC_COUNT_MASK) <= 1u)) sea_obj_free(obj);
    }
}

/* Pin an object: from now on retain/release are no-ops and it is never freed
 * by refcounting. Used for chunk string constants, which every goroutine
 * reads: code may load them into owning locals without retaining them. */
static inline void sea_obj_pin(SeaObject *obj) {
    __atomic_fetch_or(&obj->refcount, SEA_RC_PINNED_BIT, __ATOMIC_RELAXED);
}

/* Return a borrowed value as an owned one (+1). Native functions return owned
 * values (docs/memory.md): `return sea_val_ref(args[0]);` when handing back an
 * argument or an element of a container. */
static inline SeaValue sea_val_ref(SeaValue v) {
    if (sea_is_obj(v)) sea_obj_retain(sea_as_obj(v));
    return v;
}

// Cast helpers
#define SEA_AS_STRING(v)    ((SeaString*)sea_as_obj(v))
#define SEA_AS_ARRAY(v)     ((SeaArray*)sea_as_obj(v))
#define SEA_AS_TABLE(v)     ((SeaTable*)sea_as_obj(v))
#define SEA_AS_MAP(v)       ((SeaMap*)sea_as_obj(v))
#define SEA_AS_CLOSURE(v)   ((SeaClosure*)sea_as_obj(v))
#define SEA_AS_FUNCTION(v)  ((SeaFunction*)sea_as_obj(v))
#define SEA_AS_ERROR(v)     ((SeaError*)sea_as_obj(v))
#define SEA_AS_CHANNEL(v)   ((SeaChannel*)sea_as_obj(v))
#define SEA_AS_GOROUTINE(v) ((SeaGoroutine*)sea_as_obj(v))

// Obj type checks
SEA_INLINE bool sea_obj_is_type(SeaValue v, SeaObjType t) {
    return sea_is_obj(v) && sea_as_obj(v)->type == t;
}

#define sea_is_string(v)    sea_obj_is_type(v, OBJ_STRING)
#define sea_is_array(v)     sea_obj_is_type(v, OBJ_ARRAY)
#define sea_is_table(v)     sea_obj_is_type(v, OBJ_TABLE)
#define sea_is_map(v)       sea_obj_is_type(v, OBJ_MAP)
#define sea_is_closure(v)   sea_obj_is_type(v, OBJ_CLOSURE)
#define sea_is_function(v)  sea_obj_is_type(v, OBJ_FUNCTION)
#define sea_is_error(v)     sea_obj_is_type(v, OBJ_ERROR)
#define sea_is_channel(v)   sea_obj_is_type(v, OBJ_CHANNEL)
#define sea_is_goroutine(v) sea_obj_is_type(v, OBJ_GOROUTINE)
#define sea_is_struct(v)    sea_obj_is_type(v, OBJ_STRUCT)
#define sea_is_int_array(v) sea_obj_is_type(v, OBJ_INT_ARRAY)
#define sea_is_float_array(v) sea_obj_is_type(v, OBJ_FLOAT_ARRAY)
