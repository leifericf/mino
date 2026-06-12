/*
 * async/chan.c -- clojure.core.async channel primitive implementation.
 *
 * See async/chan.h for the design overview. Each operation mutates the
 * channel's impl struct directly; pending callbacks are routed through
 * the async scheduler (the same FIFO drain that go-blocks ride on).
 */

#include "async/chan.h"
#include "async/scheduler.h"
#include "values/internal.h"
#include "values/layout.h"
#include "runtime/value_assert.h"
#include "gc/internal.h"
#include "diag/diag_contract.h"
#include "prim/internal.h"

#include <stdlib.h>
#include <string.h>

void gc_mark_child_push_exported(mino_state *S, const void *p);

/* Check whether an alts flag atom is already committed. A NULL flag
 * (non-alts op) is treated as not-committed (returns 0). */
static inline int flag_is_committed(const mino_val *flag)
{
    mino_val *cur;
    if (flag == NULL) return 0;
    /* Flag is an atom holding :pending or :committed. We check the
     * atom's val pointer-equal to a sentinel keyword. */
    if (mino_type_of(flag) != MINO_ATOM) return 0;
    cur = flag->as.atom.val;
    if (cur == NULL || mino_type_of(cur) != MINO_KEYWORD) return 0;
    return cur->as.s.len == 9
        && memcmp(cur->as.s.data, "committed", 9) == 0;
}

/* The empty op tombstone: all-NULL marks a slot whose alts flag was
 * committed elsewhere; drop-on-next-access. */
static inline int op_is_committed(const mino_chan_op *op)
{
    return flag_is_committed(op->flag);
}

/* Try to commit an alts flag atom atomically. Succeeds (returns 1) iff
 * the flag was not already :committed (then transitions it to :committed).
 * A NULL flag or a non-atom flag is treated as already-committable (no-op,
 * returns 1). */
static int flag_try_commit(mino_state *S, mino_val *flag)
{
    mino_val *cur;
    if (flag == NULL) return 1;
    if (mino_type_of(flag) != MINO_ATOM) return 1;
    cur = flag->as.atom.val;
    if (cur != NULL && mino_type_of(cur) == MINO_KEYWORD
        && cur->as.s.len == 9
        && memcmp(cur->as.s.data, "committed", 9) == 0) {
        return 0;
    }
    /* Commit. The atom holds the value via gc_write_barrier-friendly
     * direct write (this is the same pattern reset! uses on atoms). */
    {
        mino_val *committed = mino_keyword(S, "committed");
        gc_write_barrier(S, flag, flag->as.atom.val, committed);
        flag->as.atom.val = committed;
    }
    return 1;
}

/* Try to commit an alts op atomically. For non-alts ops (flag NULL),
 * always succeeds. For alts ops, delegates to flag_try_commit. */
static int op_try_commit(mino_state *S, mino_chan_op *op)
{
    return flag_try_commit(S, op->flag);
}

/* Drop committed putters/takers from the head of the queue. The queue
 * is a circular array; this rewinds putters_len / takers_len in place,
 * preserving slot order for the remaining ops. */
static void drop_committed_putters(mino_chan_impl *impl)
{
    size_t r = 0, w = 0;
    for (r = 0; r < impl->putters_len; r++) {
        if (!op_is_committed(&impl->putters[r])) {
            if (w != r) impl->putters[w] = impl->putters[r];
            w++;
        }
    }
    impl->putters_len = w;
}

static void drop_committed_takers(mino_chan_impl *impl)
{
    size_t r = 0, w = 0;
    for (r = 0; r < impl->takers_len; r++) {
        if (!op_is_committed(&impl->takers[r])) {
            if (w != r) impl->takers[w] = impl->takers[r];
            w++;
        }
    }
    impl->takers_len = w;
}

/* Grow the putters / takers queue. The dyn array doubles on demand,
 * bounded by ASYNC_CHAN_MAX_PENDING. Returns 0 on success, -1 on
 * capacity overflow (caller throws). */
static int grow_q(mino_chan_op **arr, size_t *cap, size_t cur_len)
{
    size_t new_cap;
    mino_chan_op *na;
    if (cur_len < *cap) return 0;
    new_cap = (*cap == 0) ? 8u : (*cap * 2u);
    if (new_cap > ASYNC_CHAN_MAX_PENDING) new_cap = ASYNC_CHAN_MAX_PENDING;
    if (new_cap <= *cap) return -1; /* at cap; nowhere to grow */
    na = (mino_chan_op *)realloc(*arr, new_cap * sizeof(*na));
    if (na == NULL) return -1;
    *arr = na;
    *cap = new_cap;
    return 0;
}

/* Buffer add: append to the tail of the ring. Caller has already
 * confirmed the buffer kind permits the add and capacity allows it
 * (or for dropping/sliding, has handled the policy). Returns 0 on
 * success, -1 on internal error. */
static void chan_buf_push(mino_chan_impl *impl, mino_val *val)
{
    size_t idx = (impl->buf_head + impl->buf_len) % impl->buf_capacity;
    impl->buf[idx] = val;
    impl->buf_len++;
}

static mino_val *chan_buf_pop(mino_chan_impl *impl)
{
    mino_val *v;
    if (impl->buf_len == 0) return NULL;
    v = impl->buf[impl->buf_head];
    impl->buf[impl->buf_head] = NULL;
    impl->buf_head = (impl->buf_head + 1) % impl->buf_capacity;
    impl->buf_len--;
    return v;
}

/* ------------------------------------------------------------------ */
/* Construction / destruction                                          */
/* ------------------------------------------------------------------ */

mino_val *mino_chan_new(mino_state *S, int buf_kind, size_t buf_capacity,
                        mino_val *xform, mino_val *ex_handler)
{
    mino_val       *v;
    mino_chan_impl *impl;
    if (buf_kind < CHAN_BUF_NONE || buf_kind > CHAN_BUF_PROMISE) {
        return NULL;
    }
    impl = (mino_chan_impl *)calloc(1, sizeof(*impl));
    if (impl == NULL) return NULL;
    impl->buf_kind = (unsigned char)buf_kind;
    if (buf_kind == CHAN_BUF_FIXED
        || buf_kind == CHAN_BUF_DROPPING
        || buf_kind == CHAN_BUF_SLIDING) {
        if (buf_capacity == 0) {
            /* Treat 0-capacity buffered as unbuffered. */
            impl->buf_kind = CHAN_BUF_NONE;
        } else {
            impl->buf = (mino_val **)calloc(buf_capacity, sizeof(*impl->buf));
            if (impl->buf == NULL) {
                free(impl);
                return NULL;
            }
            impl->buf_capacity = buf_capacity;
        }
    }
    v = alloc_val(S, MINO_CHAN);
    if (v == NULL) {
        free(impl->buf);
        free(impl);
        return NULL;
    }
    /* Attach impl before storing GC pointers so the chan tracer can
     * reach xform/ex_handler through the live val if a subsequent
     * allocation triggers a collection. */
    v->as.chan.impl  = impl;
    impl->xform      = xform;
    impl->ex_handler = ex_handler;
    return v;
}

void mino_chan_trace(mino_state *S, mino_val *v)
{
    mino_chan_impl *impl;
    size_t i;
    if (v == NULL || mino_type_of(v) != MINO_CHAN) return;
    impl = v->as.chan.impl;
    if (impl == NULL) return;
    if (impl->buf != NULL) {
        for (i = 0; i < impl->buf_capacity; i++) {
            if (impl->buf[i] != NULL) {
                gc_mark_child_push_exported(S, impl->buf[i]);
            }
        }
    }
    if (impl->promise_val != NULL) {
        gc_mark_child_push_exported(S, impl->promise_val);
    }
    for (i = 0; i < impl->putters_len; i++) {
        gc_mark_child_push_exported(S, impl->putters[i].val);
        gc_mark_child_push_exported(S, impl->putters[i].callback);
        gc_mark_child_push_exported(S, impl->putters[i].flag);
        gc_mark_child_push_exported(S, impl->putters[i].ch);
    }
    for (i = 0; i < impl->takers_len; i++) {
        gc_mark_child_push_exported(S, impl->takers[i].val);
        gc_mark_child_push_exported(S, impl->takers[i].callback);
        gc_mark_child_push_exported(S, impl->takers[i].flag);
        gc_mark_child_push_exported(S, impl->takers[i].ch);
    }
    if (impl->xform != NULL) {
        gc_mark_child_push_exported(S, impl->xform);
    }
    if (impl->ex_handler != NULL) {
        gc_mark_child_push_exported(S, impl->ex_handler);
    }
}

void mino_chan_finalize(mino_state *S, mino_val *v)
{
    mino_chan_impl *impl;
    (void)S;
    if (v == NULL || mino_type_of(v) != MINO_CHAN) return;
    impl = v->as.chan.impl;
    if (impl == NULL) return;
    free(impl->buf);
    free(impl->putters);
    free(impl->takers);
    free(impl);
    v->as.chan.impl = NULL;
}

/* ------------------------------------------------------------------ */
/* Operations                                                          */
/* ------------------------------------------------------------------ */

/* Schedule a callback on the async run queue. Both arities (cb val)
 * and (cb [val ch]) are handled at the call site -- the alts callback
 * receives a 2-vector. We just enqueue cb + the materialized arg. */
static void schedule_cb(mino_state *S, mino_val *cb, mino_val *arg)
{
    if (cb == NULL) return;
    async_sched_enqueue(S, cb, arg);
}

static void schedule_alts_pair(mino_state *S, mino_val *cb,
                               mino_val *val, mino_val *ch)
{
    mino_val *items[2];
    mino_val *pair;
    if (cb == NULL) return;
    items[0] = (val == NULL) ? mino_nil(S) : val;
    items[1] = ch;
    pair = mino_vector(S, items, 2);
    async_sched_enqueue(S, cb, pair);
}

static void schedule_op_result(mino_state *S, const mino_chan_op *op,
                               mino_val *val)
{
    if (op->callback == NULL) return;
    if (op->flag != NULL) {
        schedule_alts_pair(S, op->callback, val, op->ch);
    } else {
        schedule_cb(S, op->callback, val == NULL ? mino_nil(S) : val);
    }
}

int mino_chan_offer(mino_state *S, mino_val *ch, mino_val *val,
                    int *out_accepted)
{
    mino_chan_impl *impl;
    if (ch == NULL || mino_type_of(ch) != MINO_CHAN) {
        *out_accepted = 0;
        return -1;
    }
    impl = ch->as.chan.impl;
    if (impl->closed) {
        *out_accepted = 0;
        return 0;
    }
    /* Promise: latch on first deliver. */
    if (impl->buf_kind == CHAN_BUF_PROMISE) {
        if (impl->promise_set) {
            *out_accepted = 0;
            return 0;
        }
        /* GC-barriered write to the impl's slot. We're storing a heap
         * value into a slot owned by ch (an OLD-or-YOUNG container).
         * Even though the slot lives in malloc'd memory, the container
         * is the chan val; pass that to the barrier. */
        gc_write_barrier(S, ch, impl->promise_val, val);
        impl->promise_val = val;
        impl->promise_set = 1;
        /* Wake every parked taker with the latched value. */
        drop_committed_takers(impl);
        {
            size_t i;
            for (i = 0; i < impl->takers_len; i++) {
                if (op_try_commit(S, &impl->takers[i])) {
                    schedule_op_result(S, &impl->takers[i], val);
                }
            }
            impl->takers_len = 0;
        }
        *out_accepted = 1;
        return 0;
    }
    /* Drop committed takers; if any remain, handoff to first. */
    drop_committed_takers(impl);
    if (impl->takers_len > 0) {
        mino_chan_op taker = impl->takers[0];
        size_t i;
        if (op_try_commit(S, &taker)) {
            for (i = 1; i < impl->takers_len; i++) {
                impl->takers[i - 1] = impl->takers[i];
            }
            impl->takers_len--;
            schedule_op_result(S, &taker, val);
            *out_accepted = 1;
            return 0;
        }
        /* The first taker was committed-by-other (shouldn't happen
         * after drop, but safe-guard). Fall through to buffer add. */
        for (i = 1; i < impl->takers_len; i++) {
            impl->takers[i - 1] = impl->takers[i];
        }
        impl->takers_len--;
    }
    /* Buffer add. */
    if (impl->buf_kind == CHAN_BUF_FIXED) {
        if (impl->buf_len < impl->buf_capacity) {
            gc_write_barrier(S, ch, NULL, val);
            chan_buf_push(impl, val);
            *out_accepted = 1;
            return 0;
        }
        *out_accepted = 0;
        return 0;
    }
    if (impl->buf_kind == CHAN_BUF_DROPPING) {
        if (impl->buf_len < impl->buf_capacity) {
            gc_write_barrier(S, ch, NULL, val);
            chan_buf_push(impl, val);
        }
        /* full -> drop silently; still report accepted=true per Clojure */
        *out_accepted = 1;
        return 0;
    }
    if (impl->buf_kind == CHAN_BUF_SLIDING) {
        if (impl->buf_len >= impl->buf_capacity) {
            (void)chan_buf_pop(impl); /* evict oldest */
        }
        gc_write_barrier(S, ch, NULL, val);
        chan_buf_push(impl, val);
        *out_accepted = 1;
        return 0;
    }
    /* CHAN_BUF_NONE with no taker: cannot offer immediately. */
    *out_accepted = 0;
    return 0;
}

mino_val *mino_chan_poll(mino_state *S, mino_val *ch)
{
    mino_chan_impl *impl;
    mino_val       *v;
    if (ch == NULL || mino_type_of(ch) != MINO_CHAN) return NULL;
    impl = ch->as.chan.impl;
    /* Promise: any take returns the latched value (or nil if not set + closed). */
    if (impl->buf_kind == CHAN_BUF_PROMISE) {
        if (impl->promise_set) {
            return impl->promise_val;
        }
        return mino_nil(S);
    }
    /* Buffer first. */
    if (impl->buf_len > 0) {
        v = chan_buf_pop(impl);
        /* Wake a parked putter to refill the slot. */
        drop_committed_putters(impl);
        if (impl->putters_len > 0) {
            mino_chan_op putter = impl->putters[0];
            size_t i;
            if (op_try_commit(S, &putter)) {
                for (i = 1; i < impl->putters_len; i++) {
                    impl->putters[i - 1] = impl->putters[i];
                }
                impl->putters_len--;
                gc_write_barrier(S, ch, NULL, putter.val);
                chan_buf_push(impl, putter.val);
                schedule_op_result(S, &putter, mino_true(S));
            }
        }
        return v == NULL ? mino_nil(S) : v;
    }
    /* Unbuffered: try parked putter direct handoff. */
    drop_committed_putters(impl);
    if (impl->putters_len > 0) {
        mino_chan_op putter = impl->putters[0];
        size_t i;
        if (op_try_commit(S, &putter)) {
            for (i = 1; i < impl->putters_len; i++) {
                impl->putters[i - 1] = impl->putters[i];
            }
            impl->putters_len--;
            schedule_op_result(S, &putter, mino_true(S));
            return putter.val == NULL ? mino_nil(S) : putter.val;
        }
    }
    return mino_nil(S);
}

int mino_chan_put(mino_state *S, mino_val *ch, mino_val *val,
                  mino_val *callback)
{
    int accepted = 0;
    int rc;
    mino_chan_impl *impl;
    if (ch == NULL || mino_type_of(ch) != MINO_CHAN) return -1;
    impl = ch->as.chan.impl;
    rc = mino_chan_offer(S, ch, val, &accepted);
    if (rc != 0) return rc;
    if (impl->closed) {
        if (callback != NULL) schedule_cb(S, callback, mino_false(S));
        return 1;
    }
    if (accepted) {
        if (callback != NULL) schedule_cb(S, callback, mino_true(S));
        return 1;
    }
    /* Park. */
    drop_committed_putters(impl);
    if (impl->putters_len >= ASYNC_CHAN_MAX_PENDING) {
        prim_throw_classified(S, "eval/arity", "MAR001",
            "channel has too many pending puts (> 1024)");
        return -1;
    }
    if (grow_q(&impl->putters, &impl->putters_cap, impl->putters_len) < 0) {
        prim_throw_classified(S, "internal", "MIN001",
            "channel putters queue out of memory");
        return -1;
    }
    {
        mino_chan_op op;
        op.val      = val;
        op.callback = callback;
        op.flag     = NULL;
        op.ch       = ch;
        gc_write_barrier(S, ch, NULL, val);
        gc_write_barrier(S, ch, NULL, callback);
        impl->putters[impl->putters_len++] = op;
    }
    return 0;
}

int mino_chan_take(mino_state *S, mino_val *ch, mino_val *callback)
{
    mino_chan_impl *impl;
    if (ch == NULL || mino_type_of(ch) != MINO_CHAN) return -1;
    impl = ch->as.chan.impl;
    /* Try immediate take. */
    if (impl->buf_kind == CHAN_BUF_PROMISE) {
        if (impl->promise_set) {
            if (callback != NULL) schedule_cb(S, callback, impl->promise_val);
            return 1;
        }
        if (impl->closed) {
            if (callback != NULL) schedule_cb(S, callback, mino_nil(S));
            return 1;
        }
        /* park */
    } else if (impl->buf_len > 0) {
        mino_val *v = chan_buf_pop(impl);
        drop_committed_putters(impl);
        if (impl->putters_len > 0) {
            mino_chan_op putter = impl->putters[0];
            size_t i;
            if (op_try_commit(S, &putter)) {
                for (i = 1; i < impl->putters_len; i++) {
                    impl->putters[i - 1] = impl->putters[i];
                }
                impl->putters_len--;
                gc_write_barrier(S, ch, NULL, putter.val);
                chan_buf_push(impl, putter.val);
                schedule_op_result(S, &putter, mino_true(S));
            }
        }
        if (callback != NULL) schedule_cb(S, callback, v == NULL ? mino_nil(S) : v);
        return 1;
    } else {
        drop_committed_putters(impl);
        if (impl->putters_len > 0) {
            mino_chan_op putter = impl->putters[0];
            size_t i;
            if (op_try_commit(S, &putter)) {
                for (i = 1; i < impl->putters_len; i++) {
                    impl->putters[i - 1] = impl->putters[i];
                }
                impl->putters_len--;
                schedule_op_result(S, &putter, mino_true(S));
                if (callback != NULL)
                    schedule_cb(S, callback, putter.val == NULL
                                ? mino_nil(S) : putter.val);
                return 1;
            }
        }
        if (impl->closed) {
            if (callback != NULL) schedule_cb(S, callback, mino_nil(S));
            return 1;
        }
    }
    /* Park. */
    drop_committed_takers(impl);
    if (impl->takers_len >= ASYNC_CHAN_MAX_PENDING) {
        prim_throw_classified(S, "eval/arity", "MAR001",
            "channel has too many pending takes (> 1024)");
        return -1;
    }
    if (grow_q(&impl->takers, &impl->takers_cap, impl->takers_len) < 0) {
        prim_throw_classified(S, "internal", "MIN001",
            "channel takers queue out of memory");
        return -1;
    }
    {
        mino_chan_op op;
        op.val      = NULL;
        op.callback = callback;
        op.flag     = NULL;
        op.ch       = ch;
        gc_write_barrier(S, ch, NULL, callback);
        impl->takers[impl->takers_len++] = op;
    }
    return 0;
}

void mino_chan_close(mino_state *S, mino_val *ch)
{
    mino_chan_impl *impl;
    if (ch == NULL || mino_type_of(ch) != MINO_CHAN) return;
    impl = ch->as.chan.impl;
    if (impl->closed) return;
    impl->closed = 1;
    /* Drain buffer to waiting takers. */
    drop_committed_takers(impl);
    while (impl->buf_len > 0 && impl->takers_len > 0) {
        mino_chan_op taker = impl->takers[0];
        size_t i;
        if (!op_try_commit(S, &taker)) {
            for (i = 1; i < impl->takers_len; i++) {
                impl->takers[i - 1] = impl->takers[i];
            }
            impl->takers_len--;
            continue;
        }
        for (i = 1; i < impl->takers_len; i++) {
            impl->takers[i - 1] = impl->takers[i];
        }
        impl->takers_len--;
        {
            mino_val *v = chan_buf_pop(impl);
            schedule_op_result(S, &taker, v);
        }
    }
    /* Notify remaining takers with nil. */
    {
        size_t i;
        for (i = 0; i < impl->takers_len; i++) {
            if (op_try_commit(S, &impl->takers[i])) {
                schedule_op_result(S, &impl->takers[i], mino_nil(S));
            }
        }
        impl->takers_len = 0;
    }
    /* Notify pending putters with false. */
    {
        size_t i;
        for (i = 0; i < impl->putters_len; i++) {
            if (op_try_commit(S, &impl->putters[i])) {
                schedule_op_result(S, &impl->putters[i], mino_false(S));
            }
        }
        impl->putters_len = 0;
    }
}

int mino_chan_closed_p(mino_val *ch)
{
    if (ch == NULL || mino_type_of(ch) != MINO_CHAN) return 0;
    return ch->as.chan.impl->closed ? 1 : 0;
}

int mino_chan_buf_count(mino_val *ch)
{
    mino_chan_impl *impl;
    if (ch == NULL || mino_type_of(ch) != MINO_CHAN) return 0;
    impl = ch->as.chan.impl;
    if (impl->buf_kind == CHAN_BUF_PROMISE) {
        return impl->promise_set ? 1 : 0;
    }
    return (int)impl->buf_len;
}

int mino_chan_buf_full_p(mino_val *ch)
{
    mino_chan_impl *impl;
    if (ch == NULL || mino_type_of(ch) != MINO_CHAN) return 1;
    impl = ch->as.chan.impl;
    switch (impl->buf_kind) {
    case CHAN_BUF_NONE:     return 1;
    case CHAN_BUF_FIXED:    return impl->buf_len >= impl->buf_capacity;
    case CHAN_BUF_DROPPING: return 0;
    case CHAN_BUF_SLIDING:  return 0;
    case CHAN_BUF_PROMISE:  return impl->promise_set ? 1 : 0;
    }
    return 1;
}

int mino_chan_has_pending_taker_p(mino_state *S, mino_val *ch)
{
    mino_chan_impl *impl;
    (void)S;
    if (ch == NULL || mino_type_of(ch) != MINO_CHAN) return 0;
    impl = ch->as.chan.impl;
    drop_committed_takers(impl);
    return impl->takers_len > 0;
}

int mino_chan_has_pending_putter_p(mino_state *S, mino_val *ch)
{
    mino_chan_impl *impl;
    (void)S;
    if (ch == NULL || mino_type_of(ch) != MINO_CHAN) return 0;
    impl = ch->as.chan.impl;
    drop_committed_putters(impl);
    return impl->putters_len > 0;
}

/* alts! flavoured ops. The parked op carries the shared alts flag;
 * the arbiter (script-side) commits one of them. The semantics mirror
 * regular put/take except the op carries flag != NULL. */
int mino_chan_put_alts(mino_state *S, mino_val *ch, mino_val *val,
                       mino_val *callback, mino_val *flag)
{
    int accepted = 0;
    int rc;
    mino_chan_impl *impl;
    if (ch == NULL || mino_type_of(ch) != MINO_CHAN) return -1;
    impl = ch->as.chan.impl;
    /* Try immediate. Need to check alts flag still active before committing
     * the alts side; the actual atomic commit happens in op_try_commit
     * when we hand off. */
    if (impl->closed) {
        if (callback != NULL && flag_try_commit(S, flag)) {
            schedule_alts_pair(S, callback, mino_false(S), ch);
        }
        return 1;
    }
    rc = mino_chan_offer(S, ch, val, &accepted);
    if (rc != 0) return rc;
    if (accepted) {
        if (callback != NULL && flag_try_commit(S, flag)) {
            schedule_alts_pair(S, callback, mino_true(S), ch);
        }
        return 1;
    }
    /* Park with the alts flag attached. */
    drop_committed_putters(impl);
    if (impl->putters_len >= ASYNC_CHAN_MAX_PENDING) {
        prim_throw_classified(S, "eval/arity", "MAR001",
            "channel has too many pending puts (> 1024)");
        return -1;
    }
    if (grow_q(&impl->putters, &impl->putters_cap, impl->putters_len) < 0) {
        prim_throw_classified(S, "internal", "MIN001",
            "channel putters queue out of memory");
        return -1;
    }
    {
        mino_chan_op op;
        op.val      = val;
        op.callback = callback;
        op.flag     = flag;
        op.ch       = ch;
        gc_write_barrier(S, ch, NULL, val);
        gc_write_barrier(S, ch, NULL, callback);
        gc_write_barrier(S, ch, NULL, flag);
        impl->putters[impl->putters_len++] = op;
    }
    return 0;
}

int mino_chan_buf_add(mino_state *S, mino_val *ch, mino_val *val)
{
    mino_chan_impl *impl;
    if (ch == NULL || mino_type_of(ch) != MINO_CHAN) return -1;
    impl = ch->as.chan.impl;
    if (impl->buf_kind == CHAN_BUF_PROMISE) {
        if (!impl->promise_set) {
            gc_write_barrier(S, ch, impl->promise_val, val);
            impl->promise_val = val;
            impl->promise_set = 1;
        }
        return 0;
    }
    if (impl->buf == NULL || impl->buf_capacity == 0) return 0;
    if (impl->buf_kind == CHAN_BUF_SLIDING
        && impl->buf_len >= impl->buf_capacity) {
        (void)chan_buf_pop(impl);
    } else if (impl->buf_len >= impl->buf_capacity) {
        return 0; /* drop on full for FIXED + DROPPING */
    }
    gc_write_barrier(S, ch, NULL, val);
    chan_buf_push(impl, val);
    return 0;
}

void mino_chan_set_xform(mino_state *S, mino_val *ch,
                        mino_val *rf, mino_val *ex_handler)
{
    mino_chan_impl *impl;
    if (ch == NULL || mino_type_of(ch) != MINO_CHAN) return;
    impl = ch->as.chan.impl;
    gc_write_barrier(S, ch, impl->xform, rf);
    impl->xform = rf;
    gc_write_barrier(S, ch, impl->ex_handler, ex_handler);
    impl->ex_handler = ex_handler;
}

mino_val *mino_chan_get_xform(mino_val *ch)
{
    if (ch == NULL || mino_type_of(ch) != MINO_CHAN) return NULL;
    return ch->as.chan.impl->xform;
}

mino_val *mino_chan_get_ex_handler(mino_val *ch)
{
    if (ch == NULL || mino_type_of(ch) != MINO_CHAN) return NULL;
    return ch->as.chan.impl->ex_handler;
}

void mino_chan_flush_buf_to_takers(mino_state *S, mino_val *ch)
{
    mino_chan_impl *impl;
    if (ch == NULL || mino_type_of(ch) != MINO_CHAN) return;
    impl = ch->as.chan.impl;
    drop_committed_takers(impl);
    while (impl->buf_len > 0 && impl->takers_len > 0) {
        mino_chan_op taker = impl->takers[0];
        size_t i;
        if (!op_try_commit(S, &taker)) {
            for (i = 1; i < impl->takers_len; i++) {
                impl->takers[i - 1] = impl->takers[i];
            }
            impl->takers_len--;
            continue;
        }
        for (i = 1; i < impl->takers_len; i++) {
            impl->takers[i - 1] = impl->takers[i];
        }
        impl->takers_len--;
        {
            mino_val *v = chan_buf_pop(impl);
            schedule_op_result(S, &taker, v);
        }
    }
}

int mino_chan_take_alts(mino_state *S, mino_val *ch,
                        mino_val *callback, mino_val *flag)
{
    mino_chan_impl *impl;
    if (ch == NULL || mino_type_of(ch) != MINO_CHAN) return -1;
    impl = ch->as.chan.impl;
    /* Try immediate take. */
    if (impl->buf_kind == CHAN_BUF_PROMISE && impl->promise_set) {
        if (flag_try_commit(S, flag)) {
            if (callback != NULL)
                schedule_alts_pair(S, callback, impl->promise_val, ch);
        }
        return 1;
    }
    if (impl->buf_len > 0) {
        if (flag_try_commit(S, flag)) {
            mino_val *v = chan_buf_pop(impl);
            /* Wake a parked putter if any. */
            drop_committed_putters(impl);
            if (impl->putters_len > 0) {
                mino_chan_op putter = impl->putters[0];
                size_t i;
                if (op_try_commit(S, &putter)) {
                    for (i = 1; i < impl->putters_len; i++) {
                        impl->putters[i - 1] = impl->putters[i];
                    }
                    impl->putters_len--;
                    gc_write_barrier(S, ch, NULL, putter.val);
                    chan_buf_push(impl, putter.val);
                    schedule_op_result(S, &putter, mino_true(S));
                }
            }
            if (callback != NULL)
                schedule_alts_pair(S, callback, v == NULL ? mino_nil(S) : v, ch);
        }
        return 1;
    }
    /* Try parked putter handoff. */
    drop_committed_putters(impl);
    if (impl->putters_len > 0) {
        mino_chan_op putter = impl->putters[0];
        if (!flag_is_committed(flag) && op_try_commit(S, &putter)) {
            size_t i;
            flag_try_commit(S, flag);
            for (i = 1; i < impl->putters_len; i++) {
                impl->putters[i - 1] = impl->putters[i];
            }
            impl->putters_len--;
            schedule_op_result(S, &putter, mino_true(S));
            if (callback != NULL)
                schedule_alts_pair(S, callback,
                                   putter.val == NULL ? mino_nil(S) : putter.val,
                                   ch);
        }
        return 1;
    }
    if (impl->closed) {
        if (flag_try_commit(S, flag)) {
            if (callback != NULL) schedule_alts_pair(S, callback, mino_nil(S), ch);
        }
        return 1;
    }
    /* Park with the alts flag. */
    drop_committed_takers(impl);
    if (impl->takers_len >= ASYNC_CHAN_MAX_PENDING) {
        prim_throw_classified(S, "eval/arity", "MAR001",
            "channel has too many pending takes (> 1024)");
        return -1;
    }
    if (grow_q(&impl->takers, &impl->takers_cap, impl->takers_len) < 0) {
        prim_throw_classified(S, "internal", "MIN001",
            "channel takers queue out of memory");
        return -1;
    }
    {
        mino_chan_op op;
        op.val      = NULL;
        op.callback = callback;
        op.flag     = flag;
        op.ch       = ch;
        gc_write_barrier(S, ch, NULL, callback);
        gc_write_barrier(S, ch, NULL, flag);
        impl->takers[impl->takers_len++] = op;
    }
    return 0;
}
