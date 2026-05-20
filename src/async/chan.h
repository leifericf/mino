/*
 * async/chan.h -- clojure.core.async channel primitive.
 *
 * A MINO_CHAN value owns its buffer (for buffered kinds), its pending-
 * putters and pending-takers queues, the closed flag, and optional
 * transducer + ex-handler hooks directly in C-side mutable slots. The
 * previous implementation stored the same shape inside an atom-wrapping
 * map and mutated via swap! on every offer!/poll!/put!/take!; the
 * per-cycle map allocation and the inner-atom dance for return values
 * dominated bench runtime. Moving the state into a C cell makes each
 * operation a single C call with no script-side state-map allocation.
 *
 * Buffer kinds:
 *
 *   CHAN_BUF_NONE      unbuffered; offer! never accepts without an
 *                      immediate taker handoff
 *   CHAN_BUF_FIXED     bounded FIFO; full <=> offer! returns false
 *   CHAN_BUF_DROPPING  bounded FIFO; full <=> new value silently dropped
 *   CHAN_BUF_SLIDING   bounded FIFO; full <=> oldest value evicted
 *   CHAN_BUF_PROMISE   single-slot latch; first deliver latches, subsequent
 *                      deliveries are no-ops and all takers see the latched
 *                      value
 *
 * Pending op queues hold parked put / take callbacks. Each op carries
 * its value (for puts), its user callback, an optional alts flag (a
 * shared atom; nil for non-alts ops) used for single-commit arbitration,
 * and the channel handle itself (for alts-shape callbacks that receive
 * [val ch]).
 *
 * Lifecycle: allocation prepends to S->gc.all_young; the GC finalizer
 * (registered with GC_T_VAL via the values-side dispatch) frees the
 * malloc-owned ring buffers and queues. Internal pointers held inside
 * impl (val, callback, flag, ch, putters[i].val, ...) are traced via
 * the MINO_CHAN arm of values/gc_handlers.c::trace_val.
 *
 * Internal to the runtime; embedders should only use mino.h.
 */

#ifndef ASYNC_CHAN_H
#define ASYNC_CHAN_H

#include "mino_internal.h"

#include <stddef.h>

enum {
    CHAN_BUF_NONE     = 0,
    CHAN_BUF_FIXED    = 1,
    CHAN_BUF_DROPPING = 2,
    CHAN_BUF_SLIDING  = 3,
    CHAN_BUF_PROMISE  = 4
};

/* A single pending put or take operation. value is non-NULL only for
 * puts. callback may be NULL (offer!/poll! never park, but the shape
 * is kept uniform across ops). flag is the alts-shared atom or NULL
 * for non-alts ops; ch is the originating channel (for alts callback
 * arity 2). */
typedef struct mino_chan_op {
    mino_val *val;
    mino_val *callback;
    mino_val *flag;
    mino_val *ch;
} mino_chan_op;

typedef struct mino_chan_impl {
    /* Buffer. Ring layout: items live at indices
     * [buf_head, buf_head + buf_len) modulo buf_capacity.
     * Empty <=> buf_len == 0. Full <=> buf_len == buf_capacity.
     * For CHAN_BUF_NONE / CHAN_BUF_PROMISE, buf is NULL and
     * buf_capacity is 0; the promise latch lives in
     * promise_set / promise_val. */
    mino_val     **buf;
    size_t         buf_capacity;
    size_t         buf_len;
    size_t         buf_head;
    unsigned char  buf_kind;
    unsigned char  promise_set;
    unsigned char  closed;
    /* Promise latch (CHAN_BUF_PROMISE only) */
    mino_val      *promise_val;

    /* Pending putters and takers. Dynamic growable arrays; entries
     * may have been committed-then-skipped by alts arbitration, which
     * leaves a tombstone (flag is non-NULL and was reset to :committed
     * elsewhere). Helper drop_committed_* compacts on next access.
     * Capacity is bounded by ASYNC_CHAN_MAX_PENDING (=1024); attempts
     * to exceed throw. */
    mino_chan_op  *putters;
    size_t         putters_len;
    size_t         putters_cap;
    mino_chan_op  *takers;
    size_t         takers_len;
    size_t         takers_cap;

    /* Optional transducer reducing function (chan-level xform) and
     * exception handler. NULL for a plain channel. The xform reduces
     * into a side-cell wrapper that adds successful outputs into this
     * channel's buffer; the wrapper itself is set up at install time. */
    mino_val      *xform;
    mino_val      *ex_handler;
} mino_chan_impl;

#define ASYNC_CHAN_MAX_PENDING 1024u

/* Constructor: returns a fresh MINO_CHAN val. buf_kind in
 * CHAN_BUF_NONE..CHAN_BUF_PROMISE. buf_capacity is 0 for NONE/PROMISE
 * and a positive integer otherwise. xform / ex_handler may be NULL. */
mino_val *mino_chan_new(mino_state *S, int buf_kind, size_t buf_capacity,
                        mino_val *xform, mino_val *ex_handler);

/* Trace the chan's owned children for the GC. */
void mino_chan_trace(mino_state *S, mino_val *v);

/* Finalizer: free the malloc-owned buffer + queues. Called by the
 * GC value finalizer when the cell is collected. */
void mino_chan_finalize(mino_state *S, mino_val *v);

/* Operation primitives. Each returns NULL only on a thrown error;
 * normal success / failure flows through the returned value (true /
 * false / nil per the Clojure surface). */

/* Synchronous put: deliver if a parked taker exists or buffer has
 * room (or it's a promise that hasn't latched). Returns true / false
 * per the offer! surface. ex_handler dispatch and xform reduction
 * happen inline. */
int mino_chan_offer(mino_state *S, mino_val *ch, mino_val *val,
                    int *out_accepted);

/* Synchronous take: return a buffered value or NULL (Clojure nil) if
 * the channel is empty. Wakes the next parked putter if any. */
mino_val *mino_chan_poll(mino_state *S, mino_val *ch);

/* Asynchronous put: deliver inline if possible, otherwise enqueue
 * a parked put op with callback. Returns 1 if delivered immediately
 * (callback was scheduled with the delivery outcome), 0 if parked.
 * Throws MAR001 if pending putters would exceed ASYNC_CHAN_MAX_PENDING. */
int mino_chan_put(mino_state *S, mino_val *ch, mino_val *val,
                  mino_val *callback);

/* Asynchronous take: hand off inline if possible, otherwise enqueue
 * a parked take op with callback. Returns 1 if completed immediately
 * (callback was scheduled), 0 if parked. */
int mino_chan_take(mino_state *S, mino_val *ch, mino_val *callback);

/* Close the channel. Buffered values still drain to waiting takers;
 * subsequent puts return false; subsequent takes drain the buffer
 * then receive nil. Idempotent (close on an already-closed channel
 * is a no-op). */
void mino_chan_close(mino_state *S, mino_val *ch);

/* Predicates: bare type checks. */
int mino_chan_closed_p(mino_val *ch);

/* Inspection primitives used by script-side alts! arbitration.
 * has_pending_taker_p / has_pending_putter_p drop committed
 * tombstones in place before reading the result. */
int mino_chan_buf_count(mino_val *ch);
int mino_chan_buf_full_p(mino_val *ch);
int mino_chan_has_pending_taker_p(mino_state *S, mino_val *ch);
int mino_chan_has_pending_putter_p(mino_state *S, mino_val *ch);

/* alts-flavoured ops: same as put/take but the parked op carries the
 * shared alts flag. The arbiter sets one op's flag to :committed when
 * one of the involved channels resolves; the other channels' tombstones
 * are then ignored. */
int mino_chan_put_alts(mino_state *S, mino_val *ch, mino_val *val,
                       mino_val *callback, mino_val *flag);
int mino_chan_take_alts(mino_state *S, mino_val *ch,
                        mino_val *callback, mino_val *flag);

/* Direct buffer push, used by the script-side transducer reducing step
 * after the xform has produced one or more outputs. Bypasses the offer!
 * fast-path's xform check (since we're already inside it) and the
 * taker handoff (the caller chooses whether to hand off). Returns 0
 * normally; -1 if ch is invalid. */
int mino_chan_buf_add(mino_state *S, mino_val *ch, mino_val *val);

/* Install or replace the channel's transducer reducing function and
 * (optional) exception handler. Called by the script-side xform setup
 * after wrapping the user-supplied xform into a reducing step that
 * uses mino_chan_buf_add to write into the buffer. */
void mino_chan_set_xform(mino_state *S, mino_val *ch,
                        mino_val *rf, mino_val *ex_handler);

/* Read the channel's installed xform reducing function, or NULL if
 * none is installed. Used by script-side dispatch to decide whether
 * an offer!/put! should route through the xform reduction. */
mino_val *mino_chan_get_xform(mino_val *ch);

/* Read the channel's exception handler, or NULL if none is installed. */
mino_val *mino_chan_get_ex_handler(mino_val *ch);

/* Wake every parked taker with a buffered value handoff. Used by the
 * script-side close! and xform-completion paths. Each successful wake
 * commits any alts flag the taker carries. */
void mino_chan_flush_buf_to_takers(mino_state *S, mino_val *ch);

#endif /* ASYNC_CHAN_H */
