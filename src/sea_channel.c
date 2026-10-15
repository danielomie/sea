#include "sea_channel.h"
#include "sea_goroutine.h"
#include "sea_sched.h"
#include "sea_memory.h"

/* Ownership (docs/memory.md): a value in the buffer, or handed to a parked
 * receiver (block_val), holds a reference — taken when it is deposited — and
 * the receiver gets that reference. A parked sender's block_val is still the
 * sender's; a receiver taking it directly retains it. */
static inline void chan_retain(SeaValue v) {
    if (sea_is_obj(v)) sea_obj_retain(sea_as_obj(v));
}

SeaChannel* sea_chan_new(uint32_t cap) {
    SeaChannel *ch = SEA_ALLOC(SeaChannel);
    ch->obj.type = OBJ_CHANNEL;
    ch->obj.refcount = 1;
    ch->cap = cap;
    ch->head = 0;
    ch->tail = 0;
    ch->count = 0;
    ch->closed = false;
    ch->send_waitq = NULL;
    ch->recv_waitq = NULL;

    if (cap > 0) {
        ch->buf = SEA_ALLOC_N(SeaValue, cap);
    } else {
        ch->buf = NULL;
    }

    pthread_mutex_init(&ch->mu, NULL);
    pthread_cond_init(&ch->not_empty, NULL);
    pthread_cond_init(&ch->not_full, NULL);
    return ch;
}

/* ---- Blocking ops (main thread, no goroutine context) -------------------- */

bool sea_chan_send(SeaChannel *ch, SeaValue val) {
    pthread_mutex_lock(&ch->mu);

    if (ch->closed) {
        pthread_mutex_unlock(&ch->mu);
        fprintf(stderr, "sea: send on closed channel\n");
        exit(1);
    }

    if (ch->cap == 0) {
        while (ch->count >= 1 && !ch->closed) {
            pthread_cond_wait(&ch->not_full, &ch->mu);
        }
        if (ch->closed) {
            pthread_mutex_unlock(&ch->mu);
            return false;
        }
        if (!ch->buf) ch->buf = SEA_ALLOC_N(SeaValue, 1);
        chan_retain(val);
        ch->buf[0] = val;
        ch->count = 1;
        pthread_cond_signal(&ch->not_empty);
        /* Wake a goroutine parked waiting to recv: it retries and takes the
         * value from the slot (sea_chan_recv_nb, "value already in slot").
         * Without this a main-thread send to a parked goroutine receiver
         * deadlocked: the receiver never ran, the slot never emptied. */
        SeaGoroutine *rcv = ch->recv_waitq;
        if (rcv) {
            ch->recv_waitq = rcv->sched_next;
            rcv->sched_next = NULL;
            rcv->status = GORO_RUNNABLE;
            if (sea_global_sched) sea_sched_requeue(sea_global_sched, rcv);
        }
        while (ch->count > 0 && !ch->closed) {
            pthread_cond_wait(&ch->not_full, &ch->mu);
        }
    } else {
        while (ch->count >= ch->cap && !ch->closed) {
            pthread_cond_wait(&ch->not_full, &ch->mu);
        }
        if (ch->closed) {
            pthread_mutex_unlock(&ch->mu);
            return false;
        }
        chan_retain(val);
        ch->buf[ch->tail] = val;
        ch->tail = (ch->tail + 1) % ch->cap;
        ch->count++;
        pthread_cond_signal(&ch->not_empty);
        /* Wake any goroutines parked waiting to recv */
        SeaGoroutine *rcv = ch->recv_waitq;
        if (rcv) {
            ch->recv_waitq = rcv->sched_next;
            rcv->sched_next = NULL;
            rcv->status = GORO_RUNNABLE;
        }
        pthread_mutex_unlock(&ch->mu);
        if (rcv && sea_global_sched) sea_sched_requeue(sea_global_sched, rcv);
        return true;
    }

    pthread_mutex_unlock(&ch->mu);
    return true;
}

bool sea_chan_recv(SeaChannel *ch, SeaValue *out) {
    pthread_mutex_lock(&ch->mu);

    while (ch->count == 0 && !ch->closed) {
        pthread_cond_wait(&ch->not_empty, &ch->mu);
    }

    if (ch->count == 0) {
        *out = SEA_NIL_VAL;
        pthread_mutex_unlock(&ch->mu);
        return false;
    }

    if (ch->cap == 0) {
        *out = ch->buf[0];
        ch->count = 0;
        pthread_cond_signal(&ch->not_full);
        SeaGoroutine *snd = ch->send_waitq;
        if (snd) {
            ch->send_waitq = snd->sched_next;
            snd->sched_next = NULL;
            snd->status = GORO_RUNNABLE;
        }
        pthread_mutex_unlock(&ch->mu);
        if (snd && sea_global_sched) sea_sched_requeue(sea_global_sched, snd);
    } else {
        *out = ch->buf[ch->head];
        ch->head = (ch->head + 1) % ch->cap;
        ch->count--;
        pthread_cond_signal(&ch->not_full);
        SeaGoroutine *snd = ch->send_waitq;
        if (snd) {
            ch->send_waitq = snd->sched_next;
            snd->sched_next = NULL;
            snd->status = GORO_RUNNABLE;
        }
        pthread_mutex_unlock(&ch->mu);
        if (snd && sea_global_sched) sea_sched_requeue(sea_global_sched, snd);
    }

    return true;
}

/* ---- Non-blocking ops (goroutine context, M:N scheduler) ----------------- */

SeaChanStatus sea_chan_send_nb(SeaChannel *ch, SeaValue val, SeaGoroutine *g) {
    pthread_mutex_lock(&ch->mu);

    if (ch->closed) {
        pthread_mutex_unlock(&ch->mu);
        return CHAN_CLOSED;
    }

    if (ch->cap == 0) {
        /* Unbuffered.
         *
         * Priority 1: goroutine receiver waiting → direct handoff, no buffer.
         * Priority 2: buffer slot free → place value, signal condvar so a
         *   main-thread blocking receiver can wake up; sender continues (no park).
         * Priority 3: slot busy → park sender until slot is freed.
         */
        SeaGoroutine *rcv = ch->recv_waitq;
        if (rcv) {
            ch->recv_waitq        = rcv->sched_next;
            rcv->sched_next       = NULL;
            chan_retain(val);
            rcv->block_val        = val;
            rcv->has_pending_recv = true;
            rcv->status           = GORO_RUNNABLE;
            pthread_mutex_unlock(&ch->mu);
            sea_sched_requeue(sea_global_sched, rcv);
            return CHAN_OK;
        }
        if (ch->count == 0) {
            /* Slot free: deposit value, wake any condvar waiter (main thread) */
            if (!ch->buf) ch->buf = SEA_ALLOC_N(SeaValue, 1);
            chan_retain(val);
            ch->buf[0] = val;
            ch->count  = 1;
            pthread_cond_signal(&ch->not_empty);
            pthread_mutex_unlock(&ch->mu);
            return CHAN_OK;   /* sender does NOT park — value is buffered */
        }
        /* Slot busy: park sender */
        g->block_val   = val;
        g->block_chan  = ch;
        g->status      = GORO_BLOCKED_SEND;
        g->sched_next  = ch->send_waitq;
        ch->send_waitq = g;
        pthread_mutex_unlock(&ch->mu);
        return CHAN_PARKED;
    }

    /* Buffered */
    if (ch->count < ch->cap) {
        chan_retain(val);
        ch->buf[ch->tail] = val;
        ch->tail = (ch->tail + 1) % ch->cap;
        ch->count++;
        /* Wake one waiting receiver to retry its CHAN_RECV */
        SeaGoroutine *rcv = ch->recv_waitq;
        if (rcv) {
            ch->recv_waitq = rcv->sched_next;
            rcv->sched_next = NULL;
            rcv->status = GORO_RUNNABLE;
        }
        pthread_cond_signal(&ch->not_empty);
        pthread_mutex_unlock(&ch->mu);
        if (rcv) sea_sched_requeue(sea_global_sched, rcv);
        return CHAN_OK;
    }

    /* Buffer full: park sender */
    g->block_val   = val;
    g->block_chan  = ch;
    g->status      = GORO_BLOCKED_SEND;
    g->sched_next  = ch->send_waitq;
    ch->send_waitq = g;
    pthread_mutex_unlock(&ch->mu);
    return CHAN_PARKED;
}

SeaChanStatus sea_chan_recv_nb(SeaChannel *ch, SeaValue *out, SeaGoroutine *g) {
    pthread_mutex_lock(&ch->mu);

    if (ch->cap == 0) {
        /* Unbuffered.
         *
         * Priority 1: goroutine sender in waitq → direct handoff from block_val.
         * Priority 2: value already in slot (placed by a goroutine sender that
         *   didn't park) → take it, free slot, wake any parked senders to retry.
         * Priority 3: closed and no value → CHAN_CLOSED.
         * Priority 4: park receiver.
         */
        SeaGoroutine *snd = ch->send_waitq;
        if (snd) {
            ch->send_waitq  = snd->sched_next;
            snd->sched_next = NULL;
            *out = snd->block_val;
            chan_retain(*out);            /* the sender's register keeps its own */
            snd->vm->frames[snd->vm->frame_count - 1].ip++;   /* skip CHAN_SEND */
            snd->status = GORO_RUNNABLE;
            pthread_mutex_unlock(&ch->mu);
            sea_sched_requeue(sea_global_sched, snd);
            return CHAN_OK;
        }
        if (ch->count > 0) {
            /* Value deposited by a non-blocking goroutine sender */
            *out = ch->buf[0];
            ch->count = 0;
            pthread_cond_signal(&ch->not_full);
            pthread_mutex_unlock(&ch->mu);
            return CHAN_OK;
        }
        if (ch->closed) {
            *out = SEA_NIL_VAL;
            pthread_mutex_unlock(&ch->mu);
            return CHAN_CLOSED;
        }
        /* Park receiver */
        g->block_chan  = ch;
        g->status      = GORO_BLOCKED_RECV;
        g->sched_next  = ch->recv_waitq;
        ch->recv_waitq = g;
        pthread_mutex_unlock(&ch->mu);
        return CHAN_PARKED;
    }

    /* Buffered */
    if (ch->count > 0) {
        *out = ch->buf[ch->head];
        ch->head = (ch->head + 1) % ch->cap;
        ch->count--;
        /* Wake one waiting sender to retry its CHAN_SEND (slot now free) */
        SeaGoroutine *snd = ch->send_waitq;
        if (snd) {
            ch->send_waitq = snd->sched_next;
            snd->sched_next = NULL;
            snd->status = GORO_RUNNABLE;
        }
        pthread_cond_signal(&ch->not_full);
        pthread_mutex_unlock(&ch->mu);
        if (snd) sea_sched_requeue(sea_global_sched, snd);
        return CHAN_OK;
    }

    if (ch->closed) {
        *out = SEA_NIL_VAL;
        pthread_mutex_unlock(&ch->mu);
        return CHAN_CLOSED;
    }

    /* Park receiver */
    g->block_chan  = ch;
    g->status      = GORO_BLOCKED_RECV;
    g->sched_next  = ch->recv_waitq;
    ch->recv_waitq = g;
    pthread_mutex_unlock(&ch->mu);
    return CHAN_PARKED;
}

/* ---- try_* (non-blocking, no parking — for select) ----------------------- */

bool sea_chan_try_send(SeaChannel *ch, SeaValue val) {
    pthread_mutex_lock(&ch->mu);
    if (ch->closed || ch->count >= (ch->cap == 0 ? 1 : ch->cap)) {
        pthread_mutex_unlock(&ch->mu);
        return false;
    }
    chan_retain(val);
    if (ch->cap == 0) {
        if (!ch->buf) ch->buf = SEA_ALLOC_N(SeaValue, 1);
        ch->buf[0] = val;
        ch->count = 1;
    } else {
        ch->buf[ch->tail] = val;
        ch->tail = (ch->tail + 1) % ch->cap;
        ch->count++;
    }
    pthread_cond_signal(&ch->not_empty);
    pthread_mutex_unlock(&ch->mu);
    return true;
}

bool sea_chan_try_recv(SeaChannel *ch, SeaValue *out) {
    pthread_mutex_lock(&ch->mu);
    if (ch->count == 0) {
        *out = SEA_NIL_VAL;
        pthread_mutex_unlock(&ch->mu);
        return false;
    }
    if (ch->cap == 0) {
        *out = ch->buf[0];
        ch->count = 0;
        pthread_cond_signal(&ch->not_full);
    } else {
        *out = ch->buf[ch->head];
        ch->head = (ch->head + 1) % ch->cap;
        ch->count--;
        pthread_cond_signal(&ch->not_full);
    }
    pthread_mutex_unlock(&ch->mu);
    return true;
}

/* ---- Close --------------------------------------------------------------- */

void sea_chan_close(SeaChannel *ch) {
    pthread_mutex_lock(&ch->mu);
    ch->closed = true;

    /* Wake all goroutines waiting to receive (they'll see CHAN_CLOSED on retry) */
    SeaGoroutine *g;
    while ((g = ch->recv_waitq) != NULL) {
        ch->recv_waitq = g->sched_next;
        g->sched_next = NULL;
        g->status = GORO_RUNNABLE;
        if (sea_global_sched) sea_sched_requeue(sea_global_sched, g);
    }

    /* Wake all goroutines waiting to send (they'll see CHAN_CLOSED → panic) */
    while ((g = ch->send_waitq) != NULL) {
        ch->send_waitq = g->sched_next;
        g->sched_next = NULL;
        g->status = GORO_RUNNABLE;
        if (sea_global_sched) sea_sched_requeue(sea_global_sched, g);
    }

    pthread_cond_broadcast(&ch->not_empty);
    pthread_cond_broadcast(&ch->not_full);
    pthread_mutex_unlock(&ch->mu);
}

void sea_chan_destroy(SeaChannel *ch) {
    pthread_mutex_destroy(&ch->mu);
    pthread_cond_destroy(&ch->not_empty);
    pthread_cond_destroy(&ch->not_full);
    if (ch->buf) {
        uint32_t buf_size = ch->cap == 0 ? 1 : ch->cap;
        for (uint32_t k = 0; k < ch->count; k++) {     /* values never received */
            SeaValue v = ch->buf[(ch->head + k) % buf_size];
            if (sea_is_obj(v)) sea_obj_release(sea_as_obj(v));
        }
        sea_free(ch->buf, sizeof(SeaValue) * buf_size);
        ch->buf = NULL;
    }
}
