/*
 * async_handler.h -- handler protocol for async channel operations.
 *
 * A handler wraps a callback with single-commit semantics.
 * Handlers share a flag for alts arbitration: when one handler
 * commits, all siblings become inactive.
 *
 * Ownership model:
 *   - Flags are malloc'd (async_flag_create) and refcounted.
 *     Each pending op or handler that references a flag holds one
 *     ref via async_flag_ref.  async_flag_unref frees when the last
 *     ref is released.
 *   - Handler MINO_HANDLE values are GC-managed.  The handler
 *     finalizer frees the handler struct but does NOT free the flag
 *     (flag lifetime is managed by refcount, not handle lifetime).
 */

#ifndef ASYNC_HANDLER_H
#define ASYNC_HANDLER_H

#include "mino.h"
#include <stdint.h>

/* Shared arbitration flag for alts pending ops.
 * Refcounted: each pending op that references the flag holds a ref.
 * Freed when the last ref is released via async_flag_unref. */
typedef struct mino_async_flag {
    int committed;   /* 0 = open, 1 = one handler has committed */
    int refcount;    /* number of pending ops holding a reference */
} mino_async_flag_t;

/* Handler state. */
typedef struct mino_async_handler {
    mino_async_flag_t *flag;      /* shared across alts candidates */
    mino_val_t        *callback;  /* fn to call with the result */
    mino_ref_t        *cb_ref;    /* GC root for callback */
    uint32_t           lock_id;   /* for deadlock-free lock ordering */
} mino_async_handler_t;

/* Create a shared flag with refcount 0.
 * Caller must call async_flag_ref for each pending op that uses it. */
mino_async_flag_t *async_flag_create(void);

/* Increment the flag's reference count. */
void async_flag_ref(mino_async_flag_t *f);

/* Decrement the flag's reference count.  Frees the flag when it
 * reaches zero. */
void async_flag_unref(mino_async_flag_t *f);

/* Create a handler with the given callback and shared flag.
 * If flag is NULL, creates a standalone (non-alts) handler.
 * Returns a MINO_HANDLE wrapping the handler. */
mino_val_t *async_handler_create(mino_state_t *S, mino_val_t *callback,
                                 mino_async_flag_t *flag, uint32_t lock_id);

/* Extract handler struct from a MINO_HANDLE.
 * Returns NULL if v is not a handler handle. */
mino_async_handler_t *async_handler_get(const mino_val_t *v);

/* Returns 1 if the handler can still be committed. */
int async_handler_active(const mino_async_handler_t *h);

/* Commit the handler: marks the flag as committed and returns
 * the callback. Returns NULL if already committed. */
mino_val_t *async_handler_commit(mino_async_handler_t *h);

#endif /* ASYNC_HANDLER_H */
