/*
 * async_channel.h -- channel kernel for async operations.
 *
 * A channel coordinates pending puts and takes through an optional buffer.
 * Channels are represented as MINO_HANDLE values with tag "async/chan".
 *
 * Ownership model:
 *   - async_chan_create takes ownership of the buffer (buf); the channel
 *     frees the buffer in its finalizer.
 *   - Pending ops are malloc'd and owned by the channel's queues.
 *     Each pending op holds mino_ref GC roots for its val, callback,
 *     and ch_val (if alts).  async_op_free unrefs all roots and
 *     decrements the alts flag refcount.
 *   - async_chan_close properly drains all pending ops (unrefs + frees).
 *     The finalizer is a last-resort path that frees malloc'd memory
 *     but cannot unref (the state may be gone); channels should be
 *     closed before becoming unreachable.
 *   - Alts flags are refcounted (async_flag_ref/unref).  Each pending
 *     op that references a flag holds one ref.  The flag is freed
 *     when the last ref is released.
 */

#ifndef ASYNC_CHANNEL_H
#define ASYNC_CHANNEL_H

#include "mino.h"
#include "async_buffer.h"
#include "async_handler.h"

/* Maximum pending puts or takes per channel (matches core.async). */
#define ASYNC_MAX_PENDING 1024

/* Pending put or take operation.
 * For alts ops, flag and ch_val are set; for regular ops they are NULL. */
typedef struct pending_op {
    mino_val_t         *val;      /* put value (NULL for takes) */
    mino_val_t         *callback; /* fn(result) to call on completion */
    mino_ref_t         *val_ref;
    mino_ref_t         *cb_ref;
    mino_async_flag_t  *flag;     /* shared alts flag (NULL = non-alts) */
    mino_val_t         *ch_val;   /* channel handle for alts result (NULL = non-alts) */
    mino_ref_t         *ch_ref;   /* GC ref for ch_val */
    struct pending_op  *next;
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

/* Register a pending put for alts arbitration.
 * Does not attempt immediate completion -- always enqueues.
 * flag is the shared arbitration flag (refcount is incremented).
 * ch_handle is the channel handle for alts result formatting. */
void async_chan_enqueue_put_alts(mino_state_t *S, mino_async_chan_t *ch,
                                mino_val_t *val, mino_val_t *callback,
                                mino_async_flag_t *flag,
                                mino_val_t *ch_handle);

/* Register a pending take for alts arbitration. */
void async_chan_enqueue_take_alts(mino_state_t *S, mino_async_chan_t *ch,
                                 mino_val_t *callback,
                                 mino_async_flag_t *flag,
                                 mino_val_t *ch_handle);

#endif /* ASYNC_CHANNEL_H */
