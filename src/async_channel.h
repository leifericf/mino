/*
 * async_channel.h -- channel kernel for async operations.
 *
 * A channel coordinates pending puts and takes through an optional buffer.
 * Channels are represented as MINO_HANDLE values with tag "async/chan".
 */

#ifndef ASYNC_CHANNEL_H
#define ASYNC_CHANNEL_H

#include "mino.h"
#include "async_buffer.h"

/* Pending put or take operation. */
typedef struct pending_op {
    mino_val_t      *val;      /* put value (NULL for takes) */
    mino_val_t      *callback; /* fn(result) to call on completion */
    mino_ref_t      *val_ref;
    mino_ref_t      *cb_ref;
    struct pending_op *next;
} pending_op_t;

/* Channel state. */
typedef struct mino_async_chan {
    mino_async_buf_t *buf;         /* NULL = unbuffered */
    pending_op_t     *takes_head;
    pending_op_t     *takes_tail;
    pending_op_t     *puts_head;
    pending_op_t     *puts_tail;
    int               closed;
    size_t            pending_puts_count;
    size_t            pending_takes_count;
} mino_async_chan_t;

/* Create a channel. buf may be NULL for unbuffered.
 * Returns a MINO_HANDLE value wrapping the channel. */
mino_val_t *async_chan_create(mino_state_t *S, mino_async_buf_t *buf);

/* Extract the channel struct from a MINO_HANDLE value.
 * Returns NULL if v is not a channel handle. */
mino_async_chan_t *async_chan_get(const mino_val_t *v);

/* Put a value onto the channel.
 * If a taker is waiting, transfers directly and schedules the taker callback.
 * If buffer has room, buffers the value and calls put_cb immediately.
 * Otherwise enqueues as a pending put.
 * val must not be nil. put_cb is called with true on success, false on closed.
 * Returns 1 if the put completed immediately, 0 if pending. */
int async_chan_put(mino_state_t *S, mino_async_chan_t *ch,
                  mino_val_t *val, mino_val_t *put_cb);

/* Take a value from the channel.
 * If buffer has items, removes one (and maybe completes a pending put).
 * If a putter is waiting, transfers directly.
 * Otherwise enqueues as a pending take.
 * take_cb is called with the value (or nil if closed and empty).
 * Returns 1 if the take completed immediately, 0 if pending. */
int async_chan_take(mino_state_t *S, mino_async_chan_t *ch,
                   mino_val_t *take_cb);

/* Close the channel.
 * Delivers nil to all pending takers. Pending puts are discarded. */
void async_chan_close(mino_state_t *S, mino_async_chan_t *ch);

/* Returns 1 if the channel is closed. */
int async_chan_closed(const mino_async_chan_t *ch);

/* Non-blocking put. Returns true if value was delivered, false otherwise.
 * Does not enqueue as pending. */
int async_chan_offer(mino_state_t *S, mino_async_chan_t *ch, mino_val_t *val);

/* Non-blocking take. Returns value if available, nil otherwise.
 * Does not enqueue as pending. */
mino_val_t *async_chan_poll(mino_state_t *S, mino_async_chan_t *ch);

/* Pending-op helpers (shared with async_select.c). */
pending_op_t *async_dequeue_put(mino_async_chan_t *ch);
pending_op_t *async_dequeue_take(mino_async_chan_t *ch);
void async_op_free(mino_state_t *S, pending_op_t *op);

#endif /* ASYNC_CHANNEL_H */
