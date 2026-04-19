/*
 * async_select.c -- alts arbitration for async channels.
 *
 * The C level handles immediate completions and :default.
 * Returns -1 when no immediate completion is possible, signaling
 * the mino-level alts! to set up pending ops with flag callbacks.
 */

#include "async_select.h"
#include "async_channel.h"
#include "async_scheduler.h"
#include "prim_internal.h"

/* Build a 2-element vector [a, b]. */
static mino_val_t *vec2(mino_state_t *S, mino_val_t *a, mino_val_t *b)
{
    mino_val_t *items[2];
    items[0] = a;
    items[1] = b;
    return mino_vector(S, items, 2);
}

/* Fisher-Yates shuffle for index array. */
static void shuffle_indices(size_t *arr, size_t n)
{
    size_t i, j, tmp;
    if (n <= 1) return;
    for (i = n - 1; i > 0; i--) {
        j = (size_t)(rand() % (int)(i + 1));
        tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
}

int async_do_alts(mino_state_t *S, mino_env_t *env,
                  mino_val_t *ops, mino_val_t *opts,
                  mino_val_t *callback)
{
    size_t n, i;
    int priority = 0;
    mino_val_t *default_val = NULL;
    int has_default = 0;
    size_t *indices = NULL;

    (void)env;

    if (ops == NULL || ops->type != MINO_VECTOR) {
        set_error(S, "alts: operations must be a vector");
        return 0;
    }
    n = ops->as.vec.len;
    if (n == 0) {
        set_error(S, "alts: must have at least one operation");
        return 0;
    }

    /* Parse options. */
    if (opts != NULL && opts->type == MINO_MAP) {
        mino_val_t *pk = mino_keyword(S, "priority");
        mino_val_t *dk = mino_keyword(S, "default");
        mino_val_t *pv = map_get_val(opts, pk);
        mino_val_t *dv = map_get_val(opts, dk);
        if (pv != NULL && pv->type == MINO_BOOL && pv->as.b)
            priority = 1;
        if (dv != NULL) {
            has_default = 1;
            default_val = dv;
        }
    }

    /* Build shuffled index array. */
    indices = malloc(n * sizeof(size_t));
    if (indices == NULL) {
        set_error(S, "alts: out of memory");
        return 0;
    }
    for (i = 0; i < n; i++) indices[i] = i;
    if (!priority) shuffle_indices(indices, n);

    /* Try each operation for immediate completion. */
    for (i = 0; i < n; i++) {
        size_t idx = indices[i];
        mino_val_t *op = vec_nth(ops, idx);
        mino_async_chan_t *ch;
        mino_val_t *ch_val;

        if (op->type == MINO_VECTOR && op->as.vec.len == 2) {
            /* Put operation: [ch val] */
            mino_val_t *put_val;
            ch_val  = vec_nth(op, 0);
            put_val = vec_nth(op, 1);
            ch = async_chan_get(ch_val);
            if (ch == NULL) {
                free(indices);
                set_error(S, "alts: invalid channel in put operation");
                return 0;
            }
            if (put_val->type == MINO_NIL) {
                free(indices);
                prim_throw_error(S, "alts: cannot put nil on a channel");
                return 0;
            }

            if (ch->closed) {
                mino_val_t *result;
                gc_pin(callback); gc_pin(ch_val);
                result = vec2(S, mino_false(S), ch_val);
                async_sched_enqueue(S, callback, result);
                gc_unpin(2);
                free(indices);
                return 1;
            }

            if (ch->takes_head != NULL) {
                pending_op_t *taker = ch->takes_head;
                mino_val_t *result;
                ch->takes_head = taker->next;
                if (ch->takes_head == NULL) ch->takes_tail = NULL;
                taker->next = NULL;
                ch->pending_takes_count--;
                if (taker->callback)
                    async_sched_enqueue(S, taker->callback, put_val);
                if (taker->val_ref) mino_unref(S, taker->val_ref);
                if (taker->cb_ref)  mino_unref(S, taker->cb_ref);
                free(taker);
                gc_pin(callback); gc_pin(ch_val);
                result = vec2(S, mino_true(S), ch_val);
                async_sched_enqueue(S, callback, result);
                gc_unpin(2);
                free(indices);
                return 1;
            }

            if (ch->buf != NULL && !async_buf_full(ch->buf)) {
                mino_val_t *result;
                async_buf_add(S, ch->buf, put_val);
                gc_pin(callback); gc_pin(ch_val);
                result = vec2(S, mino_true(S), ch_val);
                async_sched_enqueue(S, callback, result);
                gc_unpin(2);
                free(indices);
                return 1;
            }
        } else {
            /* Take operation: just a channel value. */
            ch_val = op;
            ch = async_chan_get(ch_val);
            if (ch == NULL) {
                free(indices);
                set_error(S, "alts: invalid channel in take operation");
                return 0;
            }

            if (ch->buf != NULL && async_buf_count(ch->buf) > 0) {
                mino_val_t *val = async_buf_remove(S, ch->buf);
                mino_val_t *result;
                if (ch->puts_head != NULL) {
                    pending_op_t *putter = ch->puts_head;
                    ch->puts_head = putter->next;
                    if (ch->puts_head == NULL) ch->puts_tail = NULL;
                    putter->next = NULL;
                    ch->pending_puts_count--;
                    async_buf_add(S, ch->buf, putter->val);
                    if (putter->callback)
                        async_sched_enqueue(S, putter->callback,
                                            mino_true(S));
                    if (putter->val_ref) mino_unref(S, putter->val_ref);
                    if (putter->cb_ref)  mino_unref(S, putter->cb_ref);
                    free(putter);
                }
                gc_pin(callback); gc_pin(ch_val); gc_pin(val);
                result = vec2(S, val, ch_val);
                async_sched_enqueue(S, callback, result);
                gc_unpin(3);
                free(indices);
                return 1;
            }

            if (ch->puts_head != NULL) {
                pending_op_t *putter = ch->puts_head;
                mino_val_t *val = putter->val;
                mino_val_t *result;
                ch->puts_head = putter->next;
                if (ch->puts_head == NULL) ch->puts_tail = NULL;
                putter->next = NULL;
                ch->pending_puts_count--;
                if (putter->callback)
                    async_sched_enqueue(S, putter->callback, mino_true(S));
                gc_pin(callback); gc_pin(ch_val); gc_pin(val);
                result = vec2(S, val, ch_val);
                async_sched_enqueue(S, callback, result);
                gc_unpin(3);
                if (putter->val_ref) mino_unref(S, putter->val_ref);
                if (putter->cb_ref)  mino_unref(S, putter->cb_ref);
                free(putter);
                free(indices);
                return 1;
            }

            if (ch->closed) {
                mino_val_t *result;
                gc_pin(callback); gc_pin(ch_val);
                result = vec2(S, mino_nil(S), ch_val);
                async_sched_enqueue(S, callback, result);
                gc_unpin(2);
                free(indices);
                return 1;
            }
        }
    }

    /* No immediate completion. Try :default. */
    if (has_default) {
        mino_val_t *result;
        gc_pin(callback);
        {
            mino_val_t *dk = mino_keyword(S, "default");
            result = vec2(S, default_val, dk);
        }
        async_sched_enqueue(S, callback, result);
        gc_unpin(1);
        free(indices);
        return 1;
    }

    free(indices);
    /* Return -1: no immediate completion possible.
     * The mino-level alts! sets up pending ops with flag callbacks. */
    return -1;
}
