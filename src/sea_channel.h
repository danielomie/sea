#pragma once
#include "sea_object.h"
#include <pthread.h>

/*
 * Sea channel — CSP-style communication between goroutines.
 * Supports buffered and unbuffered (synchronous) channels.
 *
 * send_waitq / recv_waitq are intrusive linked lists of goroutines
 * blocked on this channel.  The goroutine's sched_next pointer doubles
 * as the link (a goroutine is in exactly one of: run queue, send waitq,
 * or recv waitq at any time).
 */

struct SeaChannel {
    SeaObject       obj;
    uint32_t        cap;          /* 0 = unbuffered                        */
    SeaValue       *buf;          /* ring buffer (NULL if unbuffered)       */
    uint32_t        head, tail, count;
    bool            closed;
    pthread_mutex_t mu;
    pthread_cond_t  not_empty;    /* used by blocking ops (main thread)    */
    pthread_cond_t  not_full;

    /* M:N scheduler wait queues (goroutines parked on this channel) */
    SeaGoroutine   *send_waitq;
    SeaGoroutine   *recv_waitq;
};

/* Status returned by non-blocking channel ops */
typedef enum {
    CHAN_OK,        /* operation completed                                   */
    CHAN_CLOSED,    /* channel is closed (recv: empty+closed; send: panic)   */
    CHAN_PARKED,    /* goroutine was added to wait queue; vm_run must return  */
} SeaChanStatus;

/* Create a new channel with given buffer capacity (0 = unbuffered) */
SeaChannel* sea_chan_new(uint32_t cap);

/* Blocking ops — used by the main thread (no goroutine context) */
bool sea_chan_send(SeaChannel *ch, SeaValue val);
bool sea_chan_recv(SeaChannel *ch, SeaValue *out);

/* Non-blocking ops — used inside goroutines (M:N scheduler context).
 * On CHAN_PARKED the goroutine has been enqueued on ch's wait list and
 * vm_run must return SEA_SUSPENDED so the worker can pick up other work. */
SeaChanStatus sea_chan_send_nb(SeaChannel *ch, SeaValue val, SeaGoroutine *g);
SeaChanStatus sea_chan_recv_nb(SeaChannel *ch, SeaValue *out, SeaGoroutine *g);

/* try_* variants (used by select — always non-blocking, no parking) */
bool sea_chan_try_send(SeaChannel *ch, SeaValue val);
bool sea_chan_try_recv(SeaChannel *ch, SeaValue *out);

/* Close the channel; wakes all waiting goroutines */
void sea_chan_close(SeaChannel *ch);

/* Free channel resources */
void sea_chan_destroy(SeaChannel *ch);
