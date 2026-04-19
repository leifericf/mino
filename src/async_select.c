/*
 * async_select.c -- alts arbitration for async channels.
 *
 * Handles both immediate completions and pending registration.
 * For pending ops, creates a shared arbitration flag so that
 * exactly one operation commits.
 */

#include "async_select.h"
#include "async_channel.h"
#include "async_handler.h"
#include "async_scheduler.h"
#include "mino_internal.h"
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
                pending_op_t *taker = async_dequeue_active_take(S, ch);
                if (taker != NULL) {
                    mino_val_t *result;
                    if (taker->callback)
                        async_sched_enqueue(S, taker->callback, put_val);
                    async_op_free(S, taker);
                    gc_pin(callback); gc_pin(ch_val);
                    result = vec2(S, mino_true(S), ch_val);
                    async_sched_enqueue(S, callback, result);
                    gc_unpin(2);
                    free(indices);
                    return 1;
                }
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
                    pending_op_t *putter = async_dequeue_active_put(S, ch);
                    if (putter != NULL) {
                        async_buf_add(S, ch->buf, putter->val);
                        if (putter->callback)
                            async_sched_enqueue(S, putter->callback,
                                                mino_true(S));
                        async_op_free(S, putter);
                    }
                }
                gc_pin(callback); gc_pin(ch_val); gc_pin(val);
                result = vec2(S, val, ch_val);
                async_sched_enqueue(S, callback, result);
                gc_unpin(3);
                free(indices);
                return 1;
            }

            if (ch->puts_head != NULL) {
                pending_op_t *putter = async_dequeue_active_put(S, ch);
                if (putter != NULL) {
                    mino_val_t *val = putter->val;
                    mino_val_t *result;
                    if (putter->callback)
                        async_sched_enqueue(S, putter->callback, mino_true(S));
                    gc_pin(callback); gc_pin(ch_val); gc_pin(val);
                    result = vec2(S, val, ch_val);
                    async_sched_enqueue(S, callback, result);
                    gc_unpin(3);
                    async_op_free(S, putter);
                    free(indices);
                    return 1;
                }
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

    /* No immediate completion and no :default.
     * Register pending ops on each channel with a shared flag
     * for exactly-one-wins arbitration. */
    {
        mino_async_flag_t *flag = async_flag_create();
        if (flag == NULL) {
            free(indices);
            set_error(S, "alts: out of memory creating flag");
            return 0;
        }

        for (i = 0; i < n; i++) {
            mino_val_t *op = vec_nth(ops, i);
            mino_async_chan_t *ch;
            mino_val_t *ch_val;

            if (op->type == MINO_VECTOR && op->as.vec.len == 2) {
                /* Put operation: [ch val] */
                mino_val_t *put_val;
                ch_val  = vec_nth(op, 0);
                put_val = vec_nth(op, 1);
                ch = async_chan_get(ch_val);
                if (ch == NULL || ch->closed) continue;
                async_chan_enqueue_put_alts(S, ch, put_val, callback,
                                           flag, ch_val);
            } else {
                /* Take operation: channel */
                ch_val = op;
                ch = async_chan_get(ch_val);
                if (ch == NULL || ch->closed) continue;
                async_chan_enqueue_take_alts(S, ch, callback,
                                            flag, ch_val);
            }
        }

        /* If no ops were registered (all channels closed/invalid),
         * the flag has refcount 0 and must be freed manually. */
        if (flag->refcount == 0)
            free(flag);
    }

    free(indices);
    return 1;
}
