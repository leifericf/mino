/*
 * async_handler.c -- handler protocol for async channel operations.
 */

#include "async_handler.h"
#include "mino_internal.h"

static const char *HANDLER_TAG = "async/handler";

static void handler_finalizer(void *ptr, const char *tag)
{
    mino_async_handler_t *h = (mino_async_handler_t *)ptr;
    (void)tag;
    /* Flag is shared; do not free here (flag owner frees it). */
    free(h);
}

mino_async_flag_t *async_flag_create(void)
{
    mino_async_flag_t *f = calloc(1, sizeof(*f));
    return f;
}

mino_val_t *async_handler_create(mino_state_t *S, mino_val_t *callback,
                                 mino_async_flag_t *flag, uint32_t lock_id)
{
    mino_async_handler_t *h = calloc(1, sizeof(*h));
    if (h == NULL) {
        set_error(S, "out of memory creating handler");
        return NULL;
    }

    /* If no shared flag provided, create a standalone one. */
    if (flag == NULL) {
        flag = async_flag_create();
        if (flag == NULL) {
            free(h);
            set_error(S, "out of memory creating handler flag");
            return NULL;
        }
    }

    h->flag     = flag;
    h->callback = callback;
    h->cb_ref   = callback ? mino_ref(S, callback) : NULL;
    h->lock_id  = lock_id;

    return mino_handle_ex(S, h, HANDLER_TAG, handler_finalizer);
}

mino_async_handler_t *async_handler_get(const mino_val_t *v)
{
    if (v == NULL || v->type != MINO_HANDLE) return NULL;
    if (v->as.handle.tag != HANDLER_TAG) return NULL;
    return (mino_async_handler_t *)v->as.handle.ptr;
}

int async_handler_active(const mino_async_handler_t *h)
{
    return !h->flag->committed;
}

mino_val_t *async_handler_commit(mino_async_handler_t *h)
{
    if (h->flag->committed) return NULL;
    h->flag->committed = 1;
    return h->callback;
}
