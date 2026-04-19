/*
 * async_channel.c -- channel kernel for async operations.
 */

#include "async_channel.h"
#include "async_handler.h"
#include "async_scheduler.h"
#include "mino_internal.h"

/* ------------------------------------------------------------------ */
/* Handle tag and finalizer                                           */
/* ------------------------------------------------------------------ */

static const char *CHAN_TAG = "async/chan";

static void chan_finalizer(void *ptr, const char *tag)
{
    /* Channels may outlive the state in edge cases.  The finalizer
     * cannot unref because the state may be gone.  We just free the
     * raw C memory; GC refs are cleaned up by async_chan_close or
     * explicit teardown before state destruction. */
    mino_async_chan_t *ch = (mino_async_chan_t *)ptr;
    (void)tag;
    /* Free pending op nodes (without unref -- no state available). */
    {
        pending_op_t *op, *next;
        for (op = ch->puts_head; op; op = next) { next = op->next; free(op); }
        for (op = ch->takes_head; op; op = next) { next = op->next; free(op); }
    }
    /* Buffer freed without unref -- values are GC-managed. */
    if (ch->buf) {
        free(ch->buf->ring);
        free(ch->buf->refs);
        free(ch->buf);
    }
    free(ch);
}

/* ------------------------------------------------------------------ */
/* Create / extract                                                   */
/* ------------------------------------------------------------------ */

mino_val_t *async_chan_create(mino_state_t *S, mino_async_buf_t *buf)
{
    mino_async_chan_t *ch = calloc(1, sizeof(*ch));
    if (ch == NULL) {
        set_eval_diag(S, S->eval_current_form, "internal", "MIN001", "out of memory creating channel");
        return NULL;
    }
    ch->buf    = buf;
    ch->closed = 0;
    return mino_handle_ex(S, ch, CHAN_TAG, chan_finalizer);
}

mino_async_chan_t *async_chan_get(const mino_val_t *v)
{
    if (v == NULL || v->type != MINO_HANDLE) return NULL;
    if (v->as.handle.tag != CHAN_TAG) return NULL;
    return (mino_async_chan_t *)v->as.handle.ptr;
}

/* ------------------------------------------------------------------ */
/* Pending-op helpers                                                 */
/* ------------------------------------------------------------------ */

static pending_op_t *op_new(mino_state_t *S, mino_val_t *val,
                            mino_val_t *callback)
{
    pending_op_t *op = calloc(1, sizeof(*op));
    if (op == NULL) return NULL;
    op->val      = val;
    op->callback = callback;
    op->val_ref  = val ? mino_ref(S, val) : NULL;
    op->cb_ref   = callback ? mino_ref(S, callback) : NULL;
    op->flag     = NULL;
    op->ch_val   = NULL;
    op->ch_ref   = NULL;
    op->next     = NULL;
    return op;
}

void async_op_free(mino_state_t *S, pending_op_t *op)
{
    if (op->val_ref) mino_unref(S, op->val_ref);
    if (op->cb_ref)  mino_unref(S, op->cb_ref);
    if (op->ch_ref)  mino_unref(S, op->ch_ref);
    if (op->flag)    async_flag_unref(op->flag);
    free(op);
}

static void enqueue_put(mino_async_chan_t *ch, pending_op_t *op)
{
    if (ch->puts_tail) ch->puts_tail->next = op;
    else               ch->puts_head = op;
    ch->puts_tail = op;
    ch->pending_puts_count++;
}

pending_op_t *async_dequeue_put(mino_async_chan_t *ch)
{
    pending_op_t *op = ch->puts_head;
    if (op == NULL) return NULL;
    ch->puts_head = op->next;
    if (ch->puts_head == NULL) ch->puts_tail = NULL;
    op->next = NULL;
    ch->pending_puts_count--;
    return op;
}

static void enqueue_take(mino_async_chan_t *ch, pending_op_t *op)
{
    if (ch->takes_tail) ch->takes_tail->next = op;
    else                ch->takes_head = op;
    ch->takes_tail = op;
    ch->pending_takes_count++;
}

pending_op_t *async_dequeue_take(mino_async_chan_t *ch)
{
    pending_op_t *op = ch->takes_head;
    if (op == NULL) return NULL;
    ch->takes_head = op->next;
    if (ch->takes_head == NULL) ch->takes_tail = NULL;
    op->next = NULL;
    ch->pending_takes_count--;
    return op;
}

/* ------------------------------------------------------------------ */
/* Active-dequeue helpers (skip committed alts ops)                   */
/* ------------------------------------------------------------------ */

/* Dequeue the next active put, skipping any whose alts flag is
 * already committed.  Commits the flag of the returned op. */
pending_op_t *async_dequeue_active_put(mino_state_t *S,
                                       mino_async_chan_t *ch)
{
    for (;;) {
        pending_op_t *op = async_dequeue_put(ch);
        if (op == NULL) return NULL;
        if (op->flag && op->flag->committed) {
            async_op_free(S, op);
            continue;
        }
        if (op->flag) op->flag->committed = 1;
        return op;
    }
}

/* Dequeue the next active take, skipping committed alts ops.
 * Commits the flag of the returned op. */
pending_op_t *async_dequeue_active_take(mino_state_t *S,
                                        mino_async_chan_t *ch)
{
    for (;;) {
        pending_op_t *op = async_dequeue_take(ch);
        if (op == NULL) return NULL;
        if (op->flag && op->flag->committed) {
            async_op_free(S, op);
            continue;
        }
        if (op->flag) op->flag->committed = 1;
        return op;
    }
}

/* Schedule a completion callback, wrapping the result as [val, ch_val]
 * for alts ops or delivering val directly for regular ops. */
static void schedule_op_result(mino_state_t *S, pending_op_t *op,
                               mino_val_t *val)
{
    if (op->callback == NULL) return;
    if (op->ch_val) {
        /* Alts op: deliver [val, ch_val]. */
        mino_val_t *items[2];
        mino_val_t *result;
        gc_pin(val);
        items[0] = val;
        items[1] = op->ch_val;
        result = mino_vector(S, items, 2);
        async_sched_enqueue(S, op->callback, result);
        gc_unpin(1);
    } else {
        async_sched_enqueue(S, op->callback, val);
    }
}

/* ------------------------------------------------------------------ */
/* Core transfer: put                                                 */
/* ------------------------------------------------------------------ */

/* After an xform step may have added items to the buffer, transfer
 * buffered values to any waiting takers. */
static void flush_buf_to_takers(mino_state_t *S, mino_async_chan_t *ch)
{
    while (ch->buf && async_buf_count(ch->buf) > 0 && ch->takes_head) {
        pending_op_t *taker = async_dequeue_active_take(S, ch);
        if (taker) {
            mino_val_t *v = async_buf_remove(S, ch->buf);
            gc_pin(v);
            schedule_op_result(S, taker, v);
            async_op_free(S, taker);
            gc_unpin(1);
        }
    }
}

int async_chan_put(mino_state_t *S, mino_async_chan_t *ch,
                  mino_val_t *val, mino_val_t *put_cb)
{
    pending_op_t *taker;

    /* Closed channel: reject put. */
    if (ch->closed) {
        if (put_cb)
            async_sched_enqueue(S, put_cb, mino_false(S));
        return 1;
    }

    /* Transducer path: call step fn which adds to buffer via side effect.
     * The step fn internally calls chan-buf-add* for each output value. */
    if (ch->xform) {
        mino_val_t *args, *result;
        gc_pin(val);
        gc_pin(put_cb);
        args = mino_cons(S, mino_nil(S), mino_cons(S, val, NULL));

        if (mino_pcall(S, ch->xform, args, NULL, &result) != 0) {
            /* xform threw. Try ex_handler if available. */
            if (ch->ex_handler) {
                const char *err = mino_last_error(S);
                mino_val_t *err_str = mino_string(S, err ? err : "xform error");
                mino_val_t *ex_args = mino_cons(S, err_str, NULL);
                mino_val_t *ex_result;
                if (mino_pcall(S, ch->ex_handler, ex_args, NULL,
                               &ex_result) == 0
                    && ex_result != NULL
                    && ex_result->type != MINO_NIL) {
                    async_chan_buf_add(S, ch, ex_result);
                }
            }
            /* If no ex_handler, the value is silently dropped. */
        } else if (result != NULL && result->type == MINO_REDUCED) {
            /* Early termination: close the channel after this put. */
            async_chan_close(S, ch);
        }

        /* Transfer any buffered values to waiting takers. */
        flush_buf_to_takers(S, ch);

        if (put_cb && !ch->closed)
            async_sched_enqueue(S, put_cb, mino_true(S));
        else if (put_cb)
            async_sched_enqueue(S, put_cb, mino_false(S));
        gc_unpin(2);
        return 1;
    }

    /* If an active taker is waiting, transfer directly. */
    taker = async_dequeue_active_take(S, ch);
    if (taker != NULL) {
        gc_pin(val);
        gc_pin(put_cb);
        schedule_op_result(S, taker, val);
        async_op_free(S, taker);
        if (put_cb)
            async_sched_enqueue(S, put_cb, mino_true(S));
        gc_unpin(2);
        return 1;
    }

    /* If buffer has room, add to buffer. */
    if (ch->buf && !async_buf_full(ch->buf)) {
        gc_pin(val);
        gc_pin(put_cb);
        async_buf_add(S, ch->buf, val);
        if (put_cb)
            async_sched_enqueue(S, put_cb, mino_true(S));
        gc_unpin(2);
        return 1;
    }

    /* Enforce pending puts limit. */
    if (ch->pending_puts_count >= ASYNC_MAX_PENDING) {
        set_eval_diag(S, S->eval_current_form, "eval/contract", "MCT001", "channel has too many pending puts (> 1024)");
        return 0;
    }

    /* Otherwise enqueue as pending put. */
    {
        pending_op_t *op = op_new(S, val, put_cb);
        if (op == NULL) {
            set_eval_diag(S, S->eval_current_form, "internal", "MIN001", "out of memory in channel put");
            return 0;
        }
        enqueue_put(ch, op);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Core transfer: take                                                */
/* ------------------------------------------------------------------ */

int async_chan_take(mino_state_t *S, mino_async_chan_t *ch,
                   mino_val_t *take_cb)
{
    /* If buffer has items, take from buffer. */
    if (ch->buf && async_buf_count(ch->buf) > 0) {
        mino_val_t *val = async_buf_remove(S, ch->buf);

        gc_pin(val);
        gc_pin(take_cb);

        /* If an active putter is waiting, move its value into the buffer. */
        {
            pending_op_t *putter = async_dequeue_active_put(S, ch);
            if (putter != NULL) {
                async_buf_add(S, ch->buf, putter->val);
                schedule_op_result(S, putter, mino_true(S));
                async_op_free(S, putter);
            }
        }

        if (take_cb)
            async_sched_enqueue(S, take_cb, val);
        gc_unpin(2);
        return 1;
    }

    /* If an active putter is waiting, transfer directly. */
    {
        pending_op_t *putter = async_dequeue_active_put(S, ch);
        if (putter != NULL) {
            mino_val_t *val = putter->val;
            gc_pin(val);
            gc_pin(take_cb);
            schedule_op_result(S, putter, mino_true(S));
            if (take_cb)
                async_sched_enqueue(S, take_cb, val);
            async_op_free(S, putter);
            gc_unpin(2);
            return 1;
        }
    }

    /* Closed channel with nothing buffered: deliver nil. */
    if (ch->closed) {
        if (take_cb)
            async_sched_enqueue(S, take_cb, mino_nil(S));
        return 1;
    }

    /* Enforce pending takes limit. */
    if (ch->pending_takes_count >= ASYNC_MAX_PENDING) {
        set_eval_diag(S, S->eval_current_form, "eval/contract", "MCT001", "channel has too many pending takes (> 1024)");
        return 0;
    }

    /* Otherwise enqueue as pending take. */
    {
        pending_op_t *op = op_new(S, NULL, take_cb);
        if (op == NULL) {
            set_eval_diag(S, S->eval_current_form, "internal", "MIN001", "out of memory in channel take");
            return 0;
        }
        enqueue_take(ch, op);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Close                                                              */
/* ------------------------------------------------------------------ */

void async_chan_close(mino_state_t *S, mino_async_chan_t *ch)
{
    pending_op_t *op;

    if (ch->closed) return;
    ch->closed = 1;

    /* Call xform completion arity if present (lets stateful xforms flush). */
    if (ch->xform) {
        mino_val_t *result;
        mino_val_t *args = mino_cons(S, mino_nil(S), NULL);
        (void)mino_pcall(S, ch->xform, args, NULL, &result);
        /* Transfer any flushed values to waiting takers. */
        flush_buf_to_takers(S, ch);
        /* Release xform refs. */
        if (ch->xform_ref) { mino_unref(S, ch->xform_ref); ch->xform_ref = NULL; }
        if (ch->ex_ref)    { mino_unref(S, ch->ex_ref);     ch->ex_ref    = NULL; }
        ch->xform      = NULL;
        ch->ex_handler = NULL;
    }

    /* Deliver nil to all active pending takers. */
    while ((op = async_dequeue_active_take(S, ch)) != NULL) {
        schedule_op_result(S, op, mino_nil(S));
        async_op_free(S, op);
    }

    /* Notify all active pending putters that the channel closed. */
    while ((op = async_dequeue_active_put(S, ch)) != NULL) {
        schedule_op_result(S, op, mino_false(S));
        async_op_free(S, op);
    }
}

int async_chan_closed(const mino_async_chan_t *ch)
{
    return ch->closed;
}

/* ------------------------------------------------------------------ */
/* Non-blocking offer / poll                                          */
/* ------------------------------------------------------------------ */

int async_chan_offer(mino_state_t *S, mino_async_chan_t *ch, mino_val_t *val)
{
    pending_op_t *taker;

    if (ch->closed) return 0;

    taker = async_dequeue_active_take(S, ch);
    if (taker != NULL) {
        gc_pin(val);
        schedule_op_result(S, taker, val);
        async_op_free(S, taker);
        gc_unpin(1);
        return 1;
    }

    if (ch->buf && !async_buf_full(ch->buf)) {
        gc_pin(val);
        async_buf_add(S, ch->buf, val);
        gc_unpin(1);
        return 1;
    }

    return 0;
}

mino_val_t *async_chan_poll(mino_state_t *S, mino_async_chan_t *ch)
{
    if (ch->buf && async_buf_count(ch->buf) > 0) {
        mino_val_t *val = async_buf_remove(S, ch->buf);
        pending_op_t *putter = async_dequeue_active_put(S, ch);
        gc_pin(val);
        if (putter != NULL) {
            async_buf_add(S, ch->buf, putter->val);
            schedule_op_result(S, putter, mino_true(S));
            async_op_free(S, putter);
        }
        gc_unpin(1);
        return val;
    }

    {
        pending_op_t *putter = async_dequeue_active_put(S, ch);
        if (putter != NULL) {
            mino_val_t *val = putter->val;
            gc_pin(val);
            schedule_op_result(S, putter, mino_true(S));
            async_op_free(S, putter);
            gc_unpin(1);
            return val;
        }
    }

    return mino_nil(S);
}

/* ------------------------------------------------------------------ */
/* Alts pending-op registration                                       */
/* ------------------------------------------------------------------ */

/* Attach alts arbitration fields to a pending op. */
static void op_set_alts(mino_state_t *S, pending_op_t *op,
                        mino_async_flag_t *flag, mino_val_t *ch_handle)
{
    op->flag   = flag;
    op->ch_val = ch_handle;
    op->ch_ref = ch_handle ? mino_ref(S, ch_handle) : NULL;
    async_flag_ref(flag);
}

void async_chan_enqueue_put_alts(mino_state_t *S, mino_async_chan_t *ch,
                                mino_val_t *val, mino_val_t *callback,
                                mino_async_flag_t *flag,
                                mino_val_t *ch_handle)
{
    pending_op_t *op = op_new(S, val, callback);
    if (op == NULL) {
        set_eval_diag(S, S->eval_current_form, "internal", "MIN001", "out of memory in alts put registration");
        return;
    }
    op_set_alts(S, op, flag, ch_handle);
    enqueue_put(ch, op);
}

void async_chan_enqueue_take_alts(mino_state_t *S, mino_async_chan_t *ch,
                                 mino_val_t *callback,
                                 mino_async_flag_t *flag,
                                 mino_val_t *ch_handle)
{
    pending_op_t *op = op_new(S, NULL, callback);
    if (op == NULL) {
        set_eval_diag(S, S->eval_current_form, "internal", "MIN001", "out of memory in alts take registration");
        return;
    }
    op_set_alts(S, op, flag, ch_handle);
    enqueue_take(ch, op);
}

/* ------------------------------------------------------------------ */
/* Transducer support                                                 */
/* ------------------------------------------------------------------ */

void async_chan_set_xform(mino_state_t *S, mino_async_chan_t *ch,
                          mino_val_t *xform, mino_val_t *ex_handler)
{
    /* Release previous refs if re-setting. */
    if (ch->xform_ref) mino_unref(S, ch->xform_ref);
    if (ch->ex_ref)    mino_unref(S, ch->ex_ref);

    ch->xform      = xform;
    ch->xform_ref  = xform ? mino_ref(S, xform) : NULL;
    ch->ex_handler = ex_handler;
    ch->ex_ref     = ex_handler ? mino_ref(S, ex_handler) : NULL;
}

void async_chan_buf_add(mino_state_t *S, mino_async_chan_t *ch,
                        mino_val_t *val)
{
    if (ch->buf == NULL) return;
    gc_pin(val);
    async_buf_add(S, ch->buf, val);
    gc_unpin(1);
}
