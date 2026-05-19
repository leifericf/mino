/*
 * host_future.h -- host-thread future state enum and struct mino_future.
 *
 * Defined separately from runtime/host_threads.h so GC trace + sweep,
 * print, and other dispatch sites can see field offsets without
 * including host_threads.h transitively. Pthread/Win32 mu+cv wrappers
 * are the only platform-conditional fields.
 *
 * Internal to the runtime; embedders should only use mino.h.
 */

#ifndef RUNTIME_HOST_FUTURE_H
#define RUNTIME_HOST_FUTURE_H

#include "mino_internal.h"

/* glibc gates PTHREAD_MUTEX_RECURSIVE and pthread_mutexattr_settype on
 * _XOPEN_SOURCE >= 500. macOS exposes them unconditionally; Windows
 * uses CRITICAL_SECTION. Define before any system header pulls in
 * features.h so glibc sees the request. */
#if defined(__linux__) && !defined(_XOPEN_SOURCE)
#  define _XOPEN_SOURCE 600
#endif

#if defined(_WIN32) && defined(_MSC_VER)
#  include <windows.h>
#else
#  include <pthread.h>
#endif

typedef enum {
    MINO_FUTURE_PENDING   = 0,
    MINO_FUTURE_RESOLVED  = 1,
    MINO_FUTURE_FAILED    = 2,
    MINO_FUTURE_CANCELLED = 3
} mino_future_state_t;

struct mino_future {
    mino_state_t       *state;

#if defined(_WIN32) && defined(_MSC_VER)
    CRITICAL_SECTION    mu;
    CONDITION_VARIABLE  cv;
#else
    pthread_mutex_t     mu;
    pthread_cond_t      cv;
#endif
    int                 state_tag;       /* mino_future_state_t */
    mino_val_t         *result;          /* RESOLVED: worker's return */
    mino_val_t         *exception;       /* FAILED: thrown value */

    volatile int        cancel_flag;     /* set by future-cancel */

#if defined(_WIN32) && defined(_MSC_VER)
    HANDLE              thread;
#else
    pthread_t           thread;
#endif
    int                 thread_started;  /* 1 once spawn succeeded */
    int                 thread_joined;   /* 1 once join completed */

    mino_val_t         *thunk;           /* zero-arg fn for the body */
    mino_val_t         *body_env;        /* env captured at spawn */
    /* Map of dyn-var bindings at spawn time; the worker unpacks this
     * into a dyn_frame before invoking the thunk so the body sees the
     * same binding context as the caller. */
    mino_val_t         *dyn_snapshot;

    mino_future_t      *next_in_state;   /* S->threading.future_list_head chain */
};

#endif /* RUNTIME_HOST_FUTURE_H */
