/*
 * Copyright (c) 2026 The Sea Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sea_gc.h"
#include "sea_array.h"
#include "sea_table.h"
#include "sea_map.h"
#include "sea_struct.h"
#include "sea_string.h"
#include "sea_memory.h"
#include "sea_stdlib.h"
#include <stdlib.h>
#include <string.h>

/* Cycle collector — see sea_gc.h and docs/memory.md. The algorithm is the
 * synchronous collector of Bacon & Rajan, "Concurrent Cycle Collection in
 * Reference Counted Systems" (ECOOP 2001), with the recursion made iterative
 * (long linked structures would overflow the C stack). */

__thread SeaVM *sea_gc_vm;

#define GC_MIN_THRESHOLD 10000u

static uint32_t g_base_threshold = GC_MIN_THRESHOLD;
static bool     g_auto = true;

/* Read once at startup, before any goroutine thread exists. */
__attribute__((constructor)) static void gc_config_init(void) {
    const char *e = getenv("SEA_GC");
    g_auto = !(e && strcmp(e, "0") == 0);   /* SEA_GC=0: no cycle collection at all */
    const char *t = getenv("SEA_GC_THRESHOLD");
    long v = t ? strtol(t, NULL, 10) : 0;
    g_base_threshold = v > 0 ? (uint32_t)v : GC_MIN_THRESHOLD;
}
static inline void gc_config(void) {}

/* ---- Possible roots ------------------------------------------------------ */

static inline bool val_is_container(SeaValue v) {
    return sea_is_obj(v) && sea_gc_tracked(sea_as_obj(v));
}

/* Can o be on a cycle right now? Only through a reference to another
 * container. One that references none (a leaf node, an array of numbers, a
 * closure without upvalues) cannot — and if it gains such a reference later,
 * the decrement that would orphan the cycle happens then and is recorded.
 * Small objects are checked exactly; large ones are assumed cyclic. */
static bool gc_may_cycle(SeaObject *o) {
    switch (o->type) {
    case OBJ_STRUCT: {
        SeaStruct *st = (SeaStruct*)o;
        if (st->field_count > 16) return true;
        SeaValue *vals = sea_struct_values(st);
        for (uint32_t i = 0; i < st->field_count; i++)
            if (val_is_container(vals[i])) return true;
        return false;
    }
    case OBJ_ARRAY: {
        SeaArray *a = (SeaArray*)o;
        if (a->len > 16) return true;
        for (uint32_t i = 0; i < a->len; i++)
            if (val_is_container(a->data[i])) return true;
        return false;
    }
    case OBJ_CLOSURE:
        return ((SeaClosure*)o)->upval_count > 0;
    case OBJ_UPVALUE: {
        SeaUpvalue *up = (SeaUpvalue*)o;
        return up->location == &up->closed && val_is_container(up->closed);   /* open: the frame owns the value */
    }
    case OBJ_ERROR:
        return val_is_container(((SeaError*)o)->message);
    default:   /* tables and maps: assumed (iterating them could be costly) */
        return true;
    }
}

/* Called by sea_obj_release when a container's count went down to a value
 * above zero (non-shared, non-arena objects only). */
void sea_gc_possible_root(SeaObject *o) {
    SeaVM *vm = sea_gc_vm;
    if (!vm || !g_auto) return; /* no goroutine context (startup, compiler), or
                                   the collector is off (SEA_GC=0) */
    if (!gc_may_cycle(o)) return;
    if (vm->gc_nroots == vm->gc_cap) {
        /* Full: first drop the slots of candidates freed since (a native
         * looping over sea_vm_call, like csv.each, reaches no safe point, so
         * no collection empties the buffer). Without this the buffer grows
         * past the 16-bit gc_idx, and candidates that die there cannot be
         * unbuffered: their headers wait for the next collection. A
         * collection takes the buffer, so this never runs under one. */
        uint32_t k = 0;
        for (uint32_t i = 0; i < vm->gc_nroots; i++) {
            SeaObject *r = vm->gc_roots[i];
            if (!r) continue;
            r->gc_idx = k < SEA_GC_NO_IDX ? (uint16_t)k : (uint16_t)SEA_GC_NO_IDX;
            vm->gc_roots[k++] = r;
        }
        vm->gc_nroots = k;
        if (k * 2 > vm->gc_cap || vm->gc_cap == 0) {   /* still more than half live: grow */
            uint32_t nc = vm->gc_cap ? vm->gc_cap * 2 : 1024;
            vm->gc_roots = (SeaObject**)sea_realloc(vm->gc_roots,
                sizeof(SeaObject*) * vm->gc_cap, sizeof(SeaObject*) * nc);
            vm->gc_cap = nc;
        }
    }
    if (!vm->gc_threshold) {
        gc_config();
        vm->gc_threshold = g_auto ? g_base_threshold : UINT32_MAX;
    }
    vm->gc_buffered++;
    o->gc |= SEA_GC_BUFFERED;
    o->gc_idx = vm->gc_nroots < SEA_GC_NO_IDX ? (uint16_t)vm->gc_nroots : (uint16_t)SEA_GC_NO_IDX;
    vm->gc_roots[vm->gc_nroots++] = o;
}

/* sea_obj_free of a buffered object. Usually the buffer is this thread's and
 * the slot is known: empty it, and the object is freed normally (true).
 * Otherwise (slot unknown) the caller keeps the header for the collector. */
bool sea_gc_unbuffer_dying(SeaObject *o) {
    SeaVM *vm = sea_gc_vm;
    uint32_t i = o->gc_idx;
    if (vm && i != SEA_GC_NO_IDX && i < vm->gc_nroots && vm->gc_roots[i] == o) {
        vm->gc_roots[i] = NULL;
        o->gc &= (uint8_t)~SEA_GC_BUFFERED;
        return true;
    }
    return false;
}

/* ---- Object structure ---------------------------------------------------- */

/* Takes part in the collection: a container no other goroutine can see.
 * Shared, arena and pinned objects count as external references. (A child
 * may be shared, its count changed by other threads as we look: read the
 * flag bits atomically. They never change once set.) */
static inline bool gc_part(SeaObject *o) {
    return sea_gc_tracked(o) &&
           !(__atomic_load_n(&o->refcount, __ATOMIC_RELAXED) & SEA_RC_FLAG_BITS) &&
           !(o->gc & SEA_GC_DEAD);
}
static inline int  gc_color(SeaObject *o)           { return o->gc & SEA_GC_COLOR_MASK; }
static inline void gc_set_color(SeaObject *o, int c) { o->gc = (uint8_t)((o->gc & ~SEA_GC_COLOR_MASK) | c); }

typedef void (*GcVisit)(SeaObject *child, void *ctx);

/* Visit every object o references (all of them; callers filter with gc_part). */
static void gc_children(SeaObject *o, GcVisit visit, void *ctx) {
#define V(val) do { SeaValue _v = (val); if (sea_is_obj(_v)) visit(sea_as_obj(_v), ctx); } while (0)
    switch (o->type) {
    case OBJ_ARRAY: {
        SeaArray *a = (SeaArray*)o;
        for (uint32_t i = 0; i < a->len && a->data; i++) V(a->data[i]);
        break;
    }
    case OBJ_TABLE: {
        uint32_t it = 0; SeaValue k, v;
        while (sea_table_next((SeaTable*)o, &it, &k, &v)) { V(k); V(v); }
        break;
    }
    case OBJ_MAP: {
        uint32_t it = 0; SeaValue k, v;
        while (sea_map_next((SeaMap*)o, &it, &k, &v)) { V(k); V(v); }
        break;
    }
    case OBJ_STRUCT: {
        SeaStruct *s = (SeaStruct*)o;
        SeaValue *vals = sea_struct_values(s);
        for (uint32_t i = 0; i < s->field_count; i++) V(vals[i]);
        break;   /* field names are pinned constants */
    }
    case OBJ_CLOSURE: {
        SeaClosure *cl = (SeaClosure*)o;
        for (int j = 0; j < cl->upval_count; j++)
            if (cl->upvalues && cl->upvalues[j]) visit(&cl->upvalues[j]->obj, ctx);
        break;
    }
    case OBJ_UPVALUE: {
        SeaUpvalue *up = (SeaUpvalue*)o;
        /* an open upvalue points at a register: the frame owns that value */
        if (up->location == &up->closed) V(up->closed);
        break;
    }
    case OBJ_ERROR:
        V(((SeaError*)o)->message);
        break;
    default:
        break;
    }
#undef V
}

/* ---- Freeing ------------------------------------------------------------- */

void sea_gc_release_contents(SeaObject *o) {
    switch (o->type) {
    case OBJ_ARRAY:  sea_array_destroy((SeaArray*)o); break;
    case OBJ_TABLE:  sea_table_destroy((SeaTable*)o); break;
    case OBJ_MAP:    sea_map_destroy((SeaMap*)o); break;
    case OBJ_STRUCT: {
        SeaStruct *s = (SeaStruct*)o;
        SeaValue *vals = sea_struct_values(s);
        uint32_t n = s->field_count;
        s->field_count = 0;
        for (uint32_t i = 0; i < n; i++)
            if (sea_is_obj(vals[i])) sea_obj_release(sea_as_obj(vals[i]));
        break;
    }
    case OBJ_CLOSURE: {
        SeaClosure *cl = (SeaClosure*)o;
        int n = cl->upval_count;
        SeaUpvalue **ups = cl->upvalues;
        cl->upval_count = 0;
        cl->upvalues = NULL;
        for (int j = 0; j < n; j++) if (ups[j]) sea_obj_release(&ups[j]->obj);
        if (ups) sea_free(ups, sizeof(SeaUpvalue*) * (size_t)n);
        break;
    }
    case OBJ_UPVALUE: {
        SeaUpvalue *up = (SeaUpvalue*)o;
        SeaValue v = up->closed;
        up->closed = SEA_NIL_VAL;
        if (up->location == &up->closed && sea_is_obj(v)) sea_obj_release(sea_as_obj(v));
        break;
    }
    case OBJ_ERROR: {
        SeaError *e = (SeaError*)o;
        SeaValue m = e->message;
        e->message = SEA_NIL_VAL;
        if (sea_is_obj(m)) sea_obj_release(sea_as_obj(m));
        break;
    }
    default: break;
    }
}

void sea_gc_free_header(SeaObject *o) {
    switch (o->type) {
    case OBJ_ARRAY:   SEA_FREE((SeaArray*)o, SeaArray); break;
    case OBJ_TABLE:   SEA_FREE((SeaTable*)o, SeaTable); break;
    case OBJ_MAP:     SEA_FREE((SeaMap*)o, SeaMap); break;
    case OBJ_STRUCT:  sea_struct_free_block((SeaStruct*)o); break;
    case OBJ_CLOSURE: sea_free(o, sizeof(SeaClosure)); break;
    case OBJ_UPVALUE: sea_free(o, sizeof(SeaUpvalue)); break;
    case OBJ_ERROR:   sea_free(o, sizeof(SeaError)); break;
    default: break;
    }
}

/* Free a garbage object's storage and header WITHOUT touching what it
 * references: the references to other garbage are gone with it, and those
 * to live participants were already subtracted (gc_release_outside dropped
 * the rest). */
static void gc_free_white(SeaObject *o) {
    switch (o->type) {
    case OBJ_ARRAY: {
        SeaArray *a = (SeaArray*)o;
        if (a->data && !SEA_ARRAY_IS_INLINE(a)) sea_free(a->data, sizeof(SeaValue) * a->cap);
        a->data = NULL;
        break;
    }
    case OBJ_TABLE: sea_table_free_storage((SeaTable*)o); break;
    case OBJ_MAP:   sea_map_free_storage((SeaMap*)o); break;
    case OBJ_CLOSURE: {
        SeaClosure *cl = (SeaClosure*)o;
        if (cl->upvalues) sea_free(cl->upvalues, sizeof(SeaUpvalue*) * (size_t)cl->upval_count);
        cl->upvalues = NULL;
        break;
    }
    default: break;   /* struct, upvalue, error: no separate storage */
    }
    sea_gc_free_header(o);
}

/* ---- The collection ------------------------------------------------------ */

typedef struct {
    SeaObject **items;
    uint32_t    n, cap;
} GcStack;

static void gs_push(GcStack *s, SeaObject *o) {
    if (s->n == s->cap) {
        uint32_t nc = s->cap ? s->cap * 2 : 256;
        s->items = (SeaObject**)sea_realloc(s->items, sizeof(SeaObject*) * s->cap,
                                            sizeof(SeaObject*) * nc);
        s->cap = nc;
    }
    s->items[s->n++] = o;
}
static void gs_free(GcStack *s) {
    if (s->items) sea_free(s->items, sizeof(SeaObject*) * s->cap);
    s->items = NULL;
    s->n = s->cap = 0;
}

typedef struct {
    GcStack  stack;
    GcStack  whites;
    uint64_t traversed;
} GcCtx;

/* MarkGray: subtract the references internal to the subgraph. */
static void visit_gray(SeaObject *t, void *vctx) {
    GcCtx *c = (GcCtx*)vctx;
    if (!gc_part(t)) return;
    t->refcount--;                       /* plain count: no flag bits here */
    if (gc_color(t) != SEA_GC_GRAY) gs_push(&c->stack, t);
}
static void mark_gray(GcCtx *c, SeaObject *root) {
    gs_push(&c->stack, root);
    while (c->stack.n) {
        SeaObject *s = c->stack.items[--c->stack.n];
        if (gc_color(s) == SEA_GC_GRAY) continue;
        gc_set_color(s, SEA_GC_GRAY);
        c->traversed++;
        gc_children(s, visit_gray, c);
    }
}

/* ScanBlack: s is externally referenced — restore everything it reaches. */
static void visit_black(SeaObject *t, void *vctx) {
    GcCtx *c = (GcCtx*)vctx;
    if (!gc_part(t)) return;
    t->refcount++;
    if (gc_color(t) != SEA_GC_BLACK) {
        gc_set_color(t, SEA_GC_BLACK);
        gs_push(&c->stack, t);
    }
}
static void scan_black(GcCtx *c, SeaObject *s) {
    GcStack saved = c->stack;            /* scan() is mid-walk on c->stack */
    c->stack = (GcStack){0};
    gc_set_color(s, SEA_GC_BLACK);
    gs_push(&c->stack, s);
    while (c->stack.n) {
        SeaObject *x = c->stack.items[--c->stack.n];
        gc_children(x, visit_black, c);
    }
    gs_free(&c->stack);
    c->stack = saved;
}

/* Scan: gray objects still referenced from outside become black again; the
 * rest are white (garbage). */
static void visit_scan(SeaObject *t, void *vctx) {
    GcCtx *c = (GcCtx*)vctx;
    if (gc_part(t) && gc_color(t) == SEA_GC_GRAY) gs_push(&c->stack, t);
}
static void scan(GcCtx *c, SeaObject *root) {
    gs_push(&c->stack, root);
    while (c->stack.n) {
        SeaObject *s = c->stack.items[--c->stack.n];
        if (gc_color(s) != SEA_GC_GRAY) continue;
        if ((s->refcount & SEA_RC_COUNT_MASK) > 0) {
            scan_black(c, s);
        } else {
            gc_set_color(s, SEA_GC_WHITE);
            gc_children(s, visit_scan, c);
        }
    }
}

/* CollectWhite: gather the garbage reachable from a root. */
static void visit_white(SeaObject *t, void *vctx) {
    GcCtx *c = (GcCtx*)vctx;
    if (gc_part(t) && gc_color(t) == SEA_GC_WHITE && !(t->gc & SEA_GC_BUFFERED))
        gs_push(&c->stack, t);
}
static void collect_white(GcCtx *c, SeaObject *root) {
    gs_push(&c->stack, root);
    while (c->stack.n) {
        SeaObject *s = c->stack.items[--c->stack.n];
        if (gc_color(s) != SEA_GC_WHITE || (s->gc & SEA_GC_BUFFERED)) continue;
        gc_set_color(s, SEA_GC_BLACK);
        gs_push(&c->whites, s);
        gc_children(s, visit_white, c);
    }
}

/* A garbage object's references to non-participants (strings, shared
 * objects...) were never subtracted: drop them normally. */
static void visit_release_outside(SeaObject *t, void *vctx) {
    (void)vctx;
    if (!gc_part(t)) sea_obj_release(t);
}

/* The functions running in this VM are referenced only by their frames,
 * which hold no count: make them external for the collection. */
static void protect_frames(SeaVM *vm, int delta) {
    for (int i = 0; i < vm->frame_count; i++) {
        SeaClosure *cl = vm->frames[i].closure;
        if (cl && gc_part(&cl->obj)) cl->obj.refcount += (uint32_t)delta;
    }
}

uint32_t sea_gc_collect(SeaVM *vm) {
    if (vm->gc_running) return 0;
    vm->gc_running = true;
    gc_config();

    /* Take the buffer: objects released while we free go to a fresh one. */
    SeaObject **roots = vm->gc_roots;
    uint32_t nroots = vm->gc_nroots, cap = vm->gc_cap;
    vm->gc_roots = NULL;
    vm->gc_nroots = vm->gc_cap = 0;

    protect_frames(vm, +1);
    GcCtx c = {0};
    uint32_t freed = 0;

    /* MarkRoots: drop the roots that died or became shared; mark the rest. */
    uint32_t k = 0;
    for (uint32_t i = 0; i < nroots; i++) {
        SeaObject *s = roots[i];
        if (!s) continue;                        /* freed while buffered */
        if (s->gc & SEA_GC_DEAD) {               /* contents already released */
            sea_gc_free_header(s);
            freed++;
            continue;
        }
        if (__atomic_load_n(&s->refcount, __ATOMIC_RELAXED) & SEA_RC_SHARED_BIT) {   /* another goroutine may see it */
            bool held = s->gc & SEA_GC_BUFREF;
            s->gc &= (uint8_t)~(SEA_GC_BUFFERED | SEA_GC_BUFREF);
            if (held) sea_obj_release(s);
            continue;
        }
        roots[k++] = s;
    }
    nroots = k;
    for (uint32_t i = 0; i < nroots; i++) mark_gray(&c, roots[i]);
    for (uint32_t i = 0; i < nroots; i++) scan(&c, roots[i]);
    for (uint32_t i = 0; i < nroots; i++) {
        roots[i]->gc &= (uint8_t)~SEA_GC_BUFFERED;
        collect_white(&c, roots[i]);
    }

    /* Free the garbage: first the references leaving it, then the objects. */
    for (uint32_t i = 0; i < c.whites.n; i++)
        gc_children(c.whites.items[i], visit_release_outside, NULL);
    for (uint32_t i = 0; i < c.whites.n; i++) gc_free_white(c.whites.items[i]);
    freed += c.whites.n;

    protect_frames(vm, -1);
    gs_free(&c.stack);
    gs_free(&c.whites);
    if (roots) sea_free(roots, sizeof(SeaObject*) * cap);

    vm->gc_collections++;
    vm->gc_freed += freed;
    vm->gc_traversed += c.traversed;
    /* Next collection after at least as many new candidates as objects this
     * one had to look at (up to ~64K): the traversal cost stays proportional
     * to the work that produced the candidates, even with large live
     * structures. */
    if (g_auto) {
        /* capped so buffer slots fit in gc_idx (dying candidates are then
         * unbuffered and freed at once instead of lingering as DEAD) */
        uint64_t next = c.traversed > g_base_threshold ? c.traversed : g_base_threshold;
        uint32_t cap_th = SEA_GC_NO_IDX - 1024;
        if (g_base_threshold > cap_th) cap_th = g_base_threshold;
        if (next > cap_th && next < 4ull * cap_th) next = cap_th;   /* very large live
            graphs: re-examining them every 64K candidates would cost more than
            the lingering DEAD headers past the indexable slots */
        vm->gc_threshold = next > UINT32_MAX - 1 ? UINT32_MAX - 1 : (uint32_t)next;
    } else {
        vm->gc_threshold = UINT32_MAX;
    }
    vm->gc_running = false;
    return freed;
}

void sea_gc_vm_finish(SeaVM *vm) {
    if (vm->gc_nroots) sea_gc_collect(vm);
    if (vm->gc_roots) sea_free(vm->gc_roots, sizeof(SeaObject*) * vm->gc_cap);
    vm->gc_roots = NULL;
    vm->gc_nroots = vm->gc_cap = 0;
}

/* ---- The gc module ------------------------------------------------------- */

// gc.collect() → number of objects freed
static SeaValue gc_collect_fn(SeaVM *vm, int argc, SeaValue *args) {
    (void)argc; (void)args;
    return sea_int_val((int64_t)sea_gc_collect(vm));
}

// gc.stats() → {collections, freed, traversed, candidates, threshold}
static SeaValue gc_stats_fn(SeaVM *vm, int argc, SeaValue *args) {
    (void)argc; (void)args;
    gc_config();
    SeaTable *t = sea_table_new(TABLE_MAP);
    sea_table_set_str(t, "collections", sea_int_val((int64_t)vm->gc_collections));
    sea_table_set_str(t, "freed",       sea_int_val((int64_t)vm->gc_freed));
    sea_table_set_str(t, "traversed",   sea_int_val((int64_t)vm->gc_traversed));
    sea_table_set_str(t, "candidates",  sea_int_val((int64_t)vm->gc_nroots));
    sea_table_set_str(t, "buffered",    sea_int_val((int64_t)vm->gc_buffered));
    uint32_t th = vm->gc_threshold ? vm->gc_threshold : (g_auto ? g_base_threshold : UINT32_MAX);
    sea_table_set_str(t, "threshold",   th == UINT32_MAX ? sea_int_val(-1) : sea_int_val((int64_t)th));
    return sea_obj_val(&t->obj);
}

void sea_lib_gc_register(SeaVM *vm) {
    SeaTable *mod = sea_table_new(TABLE_MAP);
    sea_mod_set(mod, "collect", gc_collect_fn);
    sea_mod_set(mod, "stats",   gc_stats_fn);
    sea_vm_register_module(vm, "gc", mod);
    sea_obj_release(&mod->obj);
}
