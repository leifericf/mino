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

/* Construct a fresh promise (no worker spawned). Returns a MINO_FUTURE
 * value in the PENDING state with thread_started == 0. */
mino_val_t *mino_promise_new(mino_state_t *S);

/* Spawn a worker thread that evaluates (thunk) under env. Returns a
 * MINO_FUTURE in PENDING. Throws :mino/thread-limit-exceeded if
 * S->thread_count >= S->thread_limit. */
mino_val_t *mino_future_spawn(mino_state_t *S, mino_val_t *thunk,
                              mino_env_t *env);

/* Block until the future is RESOLVED, FAILED, or CANCELLED. Returns
 * the result, throws the captured exception, or throws
 * :mino/cancelled. */
mino_val_t *mino_future_deref(mino_state_t *S, mino_val_t *fut);

/* Promise delivery. Returns 1 if the value was delivered, 0 if the
 * promise was already realized. */
int mino_promise_deliver(mino_state_t *S, mino_val_t *promise,
                         mino_val_t *value);

/* Predicates. */
int mino_future_realized_p(mino_val_t *fut);
int mino_future_done_p(mino_val_t *fut);
int mino_future_cancelled_p(mino_val_t *fut);

/* Best-effort cancel. Sets cancel_flag and signals the cv so a deref
 * waiter can observe the new state. Returns 1 if the cancel flag was
 * set newly, 0 if the future was already realized. */
int mino_future_cancel(mino_state_t *S, mino_val_t *fut);

/* Init/destroy hooks called from state_init / state_free. */
void mino_host_threads_state_init(mino_state_t *S);
void mino_host_threads_state_destroy(mino_state_t *S);

/* Join all outstanding worker threads. Called from mino_state_free
 * before the heap is torn down so workers don't run after free. Also
 * exposed publicly as mino_quiesce_threads. */
void mino_host_threads_quiesce(mino_state_t *S);

/* GC sweep hook: free the impl struct and destroy mu/cv. Called from
 * minor and major sweep when a MINO_FUTURE val is collected. */
void mino_future_gc_sweep(mino_val_t *fut);

#endif
