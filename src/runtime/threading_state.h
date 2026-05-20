/*
 * runtime/threading_state.h -- per-state host-thread block.
 *
 * Holds the thread-limit grant, live thread count, the embed-thread
 * pool surface, the state-lock, the outstanding futures list, the
 * worker-ctx list with its inner lock, and the stop-the-world flag.
 *
 * Internal to the runtime; embedders should only use mino.h.
 */

#ifndef RUNTIME_THREADING_STATE_H
#define RUNTIME_THREADING_STATE_H

#include "mino_internal.h"
#include "runtime/thread_ctx.h"    /* mino_thread_ctx_t */
#include "runtime/host_future.h"   /* mino_future */

#include <stddef.h>

#if defined(__linux__) && !defined(_XOPEN_SOURCE)
#  define _XOPEN_SOURCE 600
#endif

#if defined(_WIN32) && defined(_MSC_VER)
#  include <windows.h>
#else
#  include <pthread.h>
#endif

struct mino_thread_pool;

typedef struct threading_state {
    int             thread_limit;
    int             thread_count;
    int             multi_threaded;

    struct mino_thread_pool *thread_pool;
    void          (*thread_start_fn)(mino_state *S, void *ctx);
    void          (*thread_end_fn)(mino_state *S, void *ctx);
    void           *thread_factory_ctx;
    size_t          thread_stack_size;

#if defined(_WIN32) && defined(_MSC_VER)
    void           *state_lock;        /* CRITICAL_SECTION; see state.c */
#else
    pthread_mutex_t state_lock;
#endif

    mino_future  *future_list_head;
    mino_thread_ctx_t *worker_ctxs_head;

#if defined(_WIN32) && defined(_MSC_VER)
    void           *worker_list_lock;  /* CRITICAL_SECTION; see state.c */
#else
    pthread_mutex_t worker_list_lock;
#endif

    volatile int    stw_request;
} threading_state_t;

#endif /* RUNTIME_THREADING_STATE_H */
