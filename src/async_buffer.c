/*
 * async_buffer.c -- buffer implementations for async channels.
 */

#include "async_buffer.h"
#include "mino_internal.h"

mino_async_buf_t *async_buf_create(mino_state_t *S, int kind, size_t capacity)
{
    mino_async_buf_t *buf;
    (void)S;

    if (kind == ASYNC_BUF_PROMISE) capacity = 1;
    if (capacity == 0 && kind != ASYNC_BUF_PROMISE) return NULL;

    buf = calloc(1, sizeof(*buf));
    if (buf == NULL) return NULL;

    buf->kind     = kind;
    buf->capacity = capacity;
    buf->count    = 0;
    buf->head     = 0;
    buf->tail     = 0;

    if (kind != ASYNC_BUF_PROMISE) {
        buf->ring = calloc(capacity, sizeof(mino_val_t *));
        buf->refs = calloc(capacity, sizeof(mino_ref_t *));
        if (buf->ring == NULL || buf->refs == NULL) {
            free(buf->ring);
            free(buf->refs);
            free(buf);
            return NULL;
        }
    }

    buf->promise_delivered = 0;
    buf->promise_val       = NULL;
    buf->promise_ref       = NULL;

    return buf;
}

void async_buf_free(mino_state_t *S, mino_async_buf_t *buf)
{
    size_t i;
    if (buf == NULL) return;

    if (buf->kind == ASYNC_BUF_PROMISE) {
        if (buf->promise_ref != NULL)
            mino_unref(S, buf->promise_ref);
    } else {
        /* Unref all buffered values. */
        for (i = 0; i < buf->count; i++) {
            size_t idx = (buf->head + i) % buf->capacity;
            if (buf->refs[idx] != NULL)
                mino_unref(S, buf->refs[idx]);
        }
        free(buf->ring);
        free(buf->refs);
    }

    free(buf);
}

int async_buf_full(const mino_async_buf_t *buf)
{
    if (buf->kind == ASYNC_BUF_PROMISE)
        return buf->promise_delivered;
    /* Dropping and sliding buffers never report "full" to callers --
     * they handle overflow internally in buf_add. */
    if (buf->kind == ASYNC_BUF_DROPPING || buf->kind == ASYNC_BUF_SLIDING)
        return 0;
    return buf->count >= buf->capacity;
}

size_t async_buf_count(const mino_async_buf_t *buf)
{
    if (buf->kind == ASYNC_BUF_PROMISE)
        return buf->promise_delivered ? 1 : 0;
    return buf->count;
}

int async_buf_add(mino_state_t *S, mino_async_buf_t *buf, mino_val_t *val)
{
    /* Promise buffer: only first value is kept. */
    if (buf->kind == ASYNC_BUF_PROMISE) {
        if (buf->promise_delivered) return 0;
        buf->promise_val       = val;
        buf->promise_ref       = mino_ref(S, val);
        buf->promise_delivered = 1;
        return 1;
    }

    /* Dropping buffer: silently drop when full. */
    if (buf->kind == ASYNC_BUF_DROPPING && buf->count >= buf->capacity)
        return 0;

    /* Sliding buffer: drop oldest when full. */
    if (buf->kind == ASYNC_BUF_SLIDING && buf->count >= buf->capacity) {
        if (buf->refs[buf->head] != NULL)
            mino_unref(S, buf->refs[buf->head]);
        buf->head = (buf->head + 1) % buf->capacity;
        buf->count--;
    }

    /* Fixed buffer: caller should have checked !full. */
    if (buf->count >= buf->capacity) return 0;

    buf->ring[buf->tail] = val;
    buf->refs[buf->tail] = mino_ref(S, val);
    buf->tail = (buf->tail + 1) % buf->capacity;
    buf->count++;
    return 1;
}

mino_val_t *async_buf_remove(mino_state_t *S, mino_async_buf_t *buf)
{
    mino_val_t *val;

    /* Promise buffer: value stays after delivery (multiple reads). */
    if (buf->kind == ASYNC_BUF_PROMISE)
        return buf->promise_delivered ? buf->promise_val : NULL;

    if (buf->count == 0) return NULL;

    val = buf->ring[buf->head];
    if (buf->refs[buf->head] != NULL) {
        mino_unref(S, buf->refs[buf->head]);
        buf->refs[buf->head] = NULL;
    }
    buf->ring[buf->head] = NULL;
    buf->head = (buf->head + 1) % buf->capacity;
    buf->count--;

    return val;
}
