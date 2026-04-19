/*
 * async_buffer.h -- buffer types for async channels.
 *
 * Four buffer kinds: fixed, dropping, sliding, promise.
 * All buffers use a ring-buffer array for value storage.
 */

#ifndef ASYNC_BUFFER_H
#define ASYNC_BUFFER_H

#include "mino.h"

/* Buffer kind discriminant. */
enum {
    ASYNC_BUF_FIXED    = 0,
    ASYNC_BUF_DROPPING = 1,
    ASYNC_BUF_SLIDING  = 2,
    ASYNC_BUF_PROMISE  = 3
};

typedef struct mino_async_buf {
    int           kind;
    size_t        capacity;
    size_t        count;
    mino_val_t  **ring;           /* malloc'd ring buffer */
    mino_ref_t  **refs;           /* GC refs for ring entries */
    size_t        head;
    size_t        tail;
    /* Promise buffer fields. */
    int           promise_delivered;
    mino_val_t   *promise_val;
    mino_ref_t   *promise_ref;
} mino_async_buf_t;

/* Create a buffer of the given kind and capacity.
 * For promise buffers, capacity is ignored (always 1).
 * Returns NULL on allocation failure. */
mino_async_buf_t *async_buf_create(mino_state_t *S, int kind, size_t capacity);

/* Free a buffer and unref all held values. */
void async_buf_free(mino_state_t *S, mino_async_buf_t *buf);

/* Returns 1 if the buffer is full according to its kind's semantics. */
int async_buf_full(const mino_async_buf_t *buf);

/* Returns the number of items currently in the buffer. */
size_t async_buf_count(const mino_async_buf_t *buf);

/* Add a value to the buffer.
 * For fixed buffers: caller must check !full first.
 * For dropping buffers: drops val silently when full.
 * For sliding buffers: drops oldest when full.
 * For promise buffers: only first add is kept.
 * Returns 1 if the value was actually stored, 0 if dropped. */
int async_buf_add(mino_state_t *S, mino_async_buf_t *buf, mino_val_t *val);

/* Remove and return the oldest value from the buffer.
 * Returns NULL if buffer is empty.
 * Unrefs the removed value's GC root. */
mino_val_t *async_buf_remove(mino_state_t *S, mino_async_buf_t *buf);

#endif /* ASYNC_BUFFER_H */
