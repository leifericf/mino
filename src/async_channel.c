/*
 * async_channel.c -- channel kernel for async operations.
 */

#include "async_channel.h"
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
        set_error(S, "out of memory creating channel");
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
    op->next     = NULL;
    return op;
}

void async_op_free(mino_state_t *S, pending_op_t *op)
{
    if (op->val_ref) mino_unref(S, op->val_ref);
    if (op->cb_ref)  mino_unref(S, op->cb_ref);
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
/* Core transfer: put                                                 */
/* ------------------------------------------------------------------ */

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

    /* If a taker is waiting, transfer directly. */
    taker = async_dequeue_take(ch);
    if (taker != NULL) {
        /* Pin values before enqueue calls which allocate and may trigger GC. */
        gc_pin(val);
        gc_pin(put_cb);
        if (taker->callback)
            async_sched_enqueue(S, taker->callback, val);
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

    /* Otherwise enqueue as pending put. */
    {
        pending_op_t *op = op_new(S, val, put_cb);
        if (op == NULL) {
            set_error(S, "out of memory in channel put");
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

        /* Pin values: async_buf_add and async_sched_enqueue allocate,
         * which may trigger GC while val/take_cb are only in registers. */
        gc_pin(val);
        gc_pin(take_cb);

        /* If a putter is waiting, move its value into the buffer. */
        {
            pending_op_t *putter = async_dequeue_put(ch);
            if (putter != NULL) {
                async_buf_add(S, ch->buf, putter->val);
                if (putter->callback)
                    async_sched_enqueue(S, putter->callback, mino_true(S));
                async_op_free(S, putter);
            }
        }

        if (take_cb)
            async_sched_enqueue(S, take_cb, val);
        gc_unpin(2);
        return 1;
    }

    /* If a putter is waiting, transfer directly. */
    {
        pending_op_t *putter = async_dequeue_put(ch);
        if (putter != NULL) {
            mino_val_t *val = putter->val;
            gc_pin(val);
            gc_pin(take_cb);
            if (putter->callback)
                async_sched_enqueue(S, putter->callback, mino_true(S));
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

    /* Otherwise enqueue as pending take. */
    {
        pending_op_t *op = op_new(S, NULL, take_cb);
        if (op == NULL) {
            set_error(S, "out of memory in channel take");
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

    /* Deliver nil to all pending takers. */
    while ((op = async_dequeue_take(ch)) != NULL) {
        if (op->callback)
            async_sched_enqueue(S, op->callback, mino_nil(S));
        async_op_free(S, op);
    }

    /* Discard all pending puts. */
    while ((op = async_dequeue_put(ch)) != NULL) {
        if (op->callback)
            async_sched_enqueue(S, op->callback, mino_false(S));
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

    taker = async_dequeue_take(ch);
    if (taker != NULL) {
        gc_pin(val);
        if (taker->callback)
            async_sched_enqueue(S, taker->callback, val);
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
        pending_op_t *putter = async_dequeue_put(ch);
        gc_pin(val);
        if (putter != NULL) {
            async_buf_add(S, ch->buf, putter->val);
            if (putter->callback)
                async_sched_enqueue(S, putter->callback, mino_true(S));
            async_op_free(S, putter);
        }
        gc_unpin(1);
        return val;
    }

    {
        pending_op_t *putter = async_dequeue_put(ch);
        if (putter != NULL) {
            mino_val_t *val = putter->val;
            gc_pin(val);
            if (putter->callback)
                async_sched_enqueue(S, putter->callback, mino_true(S));
            async_op_free(S, putter);
            gc_unpin(1);
            return val;
        }
    }

    return mino_nil(S);
}
