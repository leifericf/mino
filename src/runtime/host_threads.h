/*
 * host_threads.h -- internal interface for host worker threads.
 *
 * Declares the entry points used by the future/promise primitives in
 * src/prim/. The struct mino_future definition lives in
 * runtime/internal.h so all GC trace/sweep + print + collections sites
 * can see its fields.
 *
 * Internal to the runtime; embedders use mino.h.
 */
#ifndef RUNTIME_HOST_THREADS_H
#define RUNTIME_HOST_THREADS_H

#include "runtime/internal.h"


/* ------------------------------------------------------------------------- */
/* thread_count atomic shim                                                  */
/* ------------------------------------------------------------------------- */

/* thread_count is a plain int; all writes happen under worker_list_lock
 * so sequential-consistency is not required. We only need atomicity to
 * let mino_thread_count() read without taking the lock. On MSVC the
 * Interlocked* family is the correct portability surface; on GCC/Clang
 * we keep the __atomic_* builtins already in use here. */
#if defined(_MSC_VER)
static inline int tc_load(int *p)
{
    return (int)InterlockedCompareExchange((LONG volatile *)p, 0, 0);
}
static inline void tc_add(int *p, int delta)
{
    InterlockedExchangeAdd((LONG volatile *)p, (LONG)delta);
}
#elif defined(__GNUC__) || defined(__clang__)
static inline int tc_load(int *p)
{
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}
static inline void tc_add(int *p, int delta)
{
    __atomic_fetch_add(p, delta, __ATOMIC_RELAXED);
}
#else
/* C11 stdatomic fallback for compilers that are neither MSVC nor GCC/Clang. */
#  include <stdatomic.h>
static inline int tc_load(int *p)
{
    return atomic_load_explicit((_Atomic int *)p, memory_order_relaxed);
}
static inline void tc_add(int *p, int delta)
{
    (void)atomic_fetch_add_explicit((_Atomic int *)p, delta, memory_order_relaxed);
}
#endif

/* Decrement p guarded against going below zero. */
static inline void tc_dec_if_positive(int *p)
{
    if (tc_load(p) > 0) { tc_add(p, -1); }
}

/* Construct a fresh promise (no worker spawned). Returns a MINO_FUTURE
 * value in the PENDING state with thread_started == 0. */
mino_val *mino_promise_new(mino_state *S);

/* Spawn a worker thread that evaluates (thunk) under env. Returns a
 * MINO_FUTURE in PENDING. Throws :mino/thread-limit-exceeded if
 * S->threading.thread_count >= S->threading.thread_limit. */
mino_val *mino_future_spawn(mino_state *S, mino_val *thunk,
                              mino_env *env);

/* Block until the future is RESOLVED, FAILED, or CANCELLED. Returns
 * the result, throws the captured exception, or throws
 * :mino/cancelled. */
mino_val *mino_future_deref(mino_state *S, mino_val *fut);

/* Block until the future is realized or ms milliseconds elapse. On
 * timeout, return timeout_val without throwing. On realized/failed/
 * cancelled, behaves like mino_future_deref. */
mino_val *mino_future_deref_timed(mino_state *S, mino_val *fut,
                                    long ms, mino_val *timeout_val);

/* Promise delivery. Returns 1 if the value was delivered, 0 if the
 * promise was already realized. */
int mino_promise_deliver(mino_state *S, mino_val *promise,
                         mino_val *value);

/* Predicates. */
int mino_future_realized_p(mino_val *fut);
int mino_future_done_p(mino_val *fut);
int mino_future_cancelled_p(mino_val *fut);

/* Best-effort cancel. Sets cancel_flag and signals the cv so a deref
 * waiter can observe the new state. Returns 1 if the cancel flag was
 * set newly, 0 if the future was already realized. */
int mino_future_cancel(mino_state *S, mino_val *fut);

/* Init/destroy hooks called from state_init / state_free. */
void mino_host_threads_state_init(mino_state *S);
void mino_host_threads_state_destroy(mino_state *S);

/* Join all outstanding worker threads. Called from mino_state_free
 * before the heap is torn down so workers don't run after free. Also
 * exposed publicly as mino_quiesce_threads. */
void mino_host_threads_quiesce(mino_state *S);

/* GC sweep hook: free the impl struct and destroy mu/cv. Called from
 * minor and major sweep when a MINO_FUTURE val is collected. */
void mino_future_gc_sweep(mino_val *fut);

/* State-teardown variant: frees the impl shell of a still-live future
 * after the quiesce pass has joined its worker, without touching the
 * cv/mu. Called from state_free_heap only. */
void mino_future_teardown_free(mino_val *fut);

/* Walk a MINO_FUTURE.impl's GC-owned slots (result, exception, thunk,
 * body_env, dyn_snapshot). The values-side trace_val tracer delegates
 * here so it doesn't have to know struct mino_future's layout. */
void mino_future_trace_impl(mino_state *S, const mino_val *fut);

#endif
