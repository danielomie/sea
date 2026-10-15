#include "sea_rope.h"
#include <string.h>

SeaRope *sea_rope_new(void) {
    SeaRope *r = (SeaRope*)sea_malloc(sizeof(SeaRope));
    r->obj.type = OBJ_ROPE;
    r->obj.refcount = 1;
    r->total_len = 0;
    r->count     = 0;
    r->cap       = SEA_ROPE_INIT_CAP;
    r->_pad      = 0;
    r->parts     = SEA_ALLOC_N(SeaString*, SEA_ROPE_INIT_CAP);
    r->flat      = NULL;
    return r;
}

void sea_rope_append(SeaRope *r, SeaString *s) {
    /* Content changes: drop the cached flat copy. (In-place append only
     * happens on an exclusively owned rope, so no other thread holds it.) */
    if (r->flat) {
        sea_obj_release(&r->flat->obj);
        r->flat = NULL;
    }
    if (r->count == r->cap) {
        uint32_t new_cap = r->cap * 2;
        r->parts = SEA_REALLOC_N(r->parts, SeaString*, r->cap, new_cap);
        r->cap   = new_cap;
    }
    sea_obj_retain(&s->obj);
    r->parts[r->count++] = s;
    r->total_len += s->len;
}

SeaString *sea_rope_flatten(SeaRope *r) {
    SeaString *result = sea_string_alloc(r->total_len);
    char *p = result->data;
    for (uint32_t i = 0; i < r->count; i++) {
        memcpy(p, r->parts[i]->data, r->parts[i]->len);
        p += r->parts[i]->len;
    }
    *p = '\0';
    return result;
}

SeaString *sea_rope_flat(SeaRope *r) {
    SeaString *f = __atomic_load_n(&r->flat, __ATOMIC_ACQUIRE);
    if (f) return f;
    f = sea_rope_flatten(r);
    if (__atomic_load_n(&r->obj.refcount, __ATOMIC_RELAXED) & SEA_RC_SHARED_BIT)
        sea_obj_mark_shared(&f->obj);
    SeaString *expected = NULL;
    if (!__atomic_compare_exchange_n(&r->flat, &expected, f, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        sea_obj_release(&f->obj);   /* another thread cached one first */
        f = expected;
    }
    return f;
}

void sea_rope_free(SeaRope *r) {
    if (r->flat) sea_obj_release(&r->flat->obj);
    for (uint32_t i = 0; i < r->count; i++) {
        sea_obj_release(&r->parts[i]->obj);
    }
    sea_free(r->parts, sizeof(SeaString*) * r->cap);
    sea_free(r, sizeof(SeaRope));
}
