/*
 * host_threads.c -- pthread/Win32 worker threads for futures + promises.
 *
 * Cycle G4.3 (v0.89.0).
 *
 * Threading model:
 *   - Per-state state_lock (G4.3.b) is held by the worker for the
 *     duration of the worker's eval call. Workers + embedder thread
 *     are mutually exclusive within one state. Cross-state is fully
 *     concurrent.
 *   - Worker threads install their own mino_thread_ctx_t into TLS at
 *     entry (mino_tls_ctx) and clear it at exit. The fallback path
 *     in mino_current_ctx returns &S->main_ctx for the embedder, so
 *     the embedder doesn't need TLS plumbing.
 *   - Each future's mu/cv synchronizes the worker -> waiter handoff
 *     of result/exception. Readers (deref, future-done?, etc.) take
 *     the future's mu briefly.
 */

#include "runtime/host_threads.h"
#include "runtime/internal.h"
#include "diag.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && defined(_MSC_VER)
#  include <windows.h>
#  include <process.h>
#else
#  include <pthread.h>
#  include <time.h>
#endif

/* prim_throw_classified is the runtime's catchable-throw entry point;
 * declared in src/prim/internal.h. We declare it here to avoid pulling
 * the prim layer into the runtime header. */
extern mino_val_t *prim_throw_classified(mino_state_t *S, const char *kind,
                                         const char *code, const char *msg);

/* ------------------------------------------------------------------------- */
/* mu/cv portability                                                         */
/* ------------------------------------------------------------------------- */

#if defined(_WIN32) && defined(_MSC_VER)
static void mu_init(CRITICAL_SECTION *m)   { InitializeCriticalSection(m); }
static void mu_destroy(CRITICAL_SECTION *m){ DeleteCriticalSection(m); }
static void mu_lock(CRITICAL_SECTION *m)   { EnterCriticalSection(m); }
static void mu_unlock(CRITICAL_SECTION *m) { LeaveCriticalSection(m); }
static void cv_init(CONDITION_VARIABLE *c) { InitializeConditionVariable(c); }
static void cv_destroy(CONDITION_VARIABLE *c) { (void)c; }
static void cv_wait(CONDITION_VARIABLE *c, CRITICAL_SECTION *m)
{ SleepConditionVariableCS(c, m, INFINITE); }
static void cv_broadcast(CONDITION_VARIABLE *c) { WakeAllConditionVariable(c); }
#else
static void mu_init(pthread_mutex_t *m)    { pthread_mutex_init(m, NULL); }
static void mu_destroy(pthread_mutex_t *m) { pthread_mutex_destroy(m); }
static void mu_lock(pthread_mutex_t *m)    { pthread_mutex_lock(m); }
static void mu_unlock(pthread_mutex_t *m)  { pthread_mutex_unlock(m); }
static void cv_init(pthread_cond_t *c)     { pthread_cond_init(c, NULL); }
static void cv_destroy(pthread_cond_t *c)  { pthread_cond_destroy(c); }
static void cv_wait(pthread_cond_t *c, pthread_mutex_t *m)
{ pthread_cond_wait(c, m); }
static void cv_broadcast(pthread_cond_t *c) { pthread_cond_broadcast(c); }
#endif

/* ------------------------------------------------------------------------- */
/* state init/destroy                                                        */
/* ------------------------------------------------------------------------- */

void mino_host_threads_state_init(mino_state_t *S)
{
    S->future_list_head = NULL;
}

void mino_host_threads_state_destroy(mino_state_t *S)
{
    /* Quiesce drains worker threads first so the impl free below is
     * safe. Note: state_free already calls quiesce explicitly via
     * mino_quiesce_threads if the embedder requested it; this is the
     * fallback for embedders that didn't. */
    mino_host_threads_quiesce(S);
}

/* ------------------------------------------------------------------------- */
/* Future allocation                                                          */
/* ------------------------------------------------------------------------- */

static mino_val_t *future_alloc(mino_state_t *S)
{
    mino_val_t      *v;
    mino_future_t   *impl;

    impl = (mino_future_t *)calloc(1, sizeof(*impl));
    if (impl == NULL) { return NULL; }
    impl->state = S;
    mu_init(&impl->mu);
    cv_init(&impl->cv);
    impl->state_tag = MINO_FUTURE_PENDING;

    v = alloc_val(S, MINO_FUTURE);
    if (v == NULL) {
        cv_destroy(&impl->cv);
        mu_destroy(&impl->mu);
        free(impl);
        return NULL;
    }
    v->as.future.impl = impl;

    /* Link onto the state's outstanding-futures list. We need the
     * state lock to mutate the list head safely against other workers,
     * but we're called from the embedder/driver thread for promise_new
     * and from the spawn path that already holds state_lock. To keep
     * it simple, take the state_lock here. */
    mino_lock(S);
    impl->next_in_state = S->future_list_head;
    S->future_list_head = impl;
    mino_unlock(S);

    return v;
}

/* ------------------------------------------------------------------------- */
/* Public construction                                                        */
/* ------------------------------------------------------------------------- */

mino_val_t *mino_promise_new(mino_state_t *S)
{
    return future_alloc(S);
    /* No worker spawned; thread_started stays 0. */
}

int mino_promise_deliver(mino_state_t *S, mino_val_t *promise,
                         mino_val_t *value)
{
    mino_future_t *impl;
    int delivered = 0;

    (void)S;
    if (promise == NULL || promise->type != MINO_FUTURE) { return 0; }
    impl = promise->as.future.impl;
    if (impl == NULL) { return 0; }

    mu_lock(&impl->mu);
    if (impl->state_tag == MINO_FUTURE_PENDING) {
        impl->result    = value;
        impl->state_tag = MINO_FUTURE_RESOLVED;
        cv_broadcast(&impl->cv);
        delivered = 1;
    }
    mu_unlock(&impl->mu);
    return delivered;
}

/* ------------------------------------------------------------------------- */
/* Predicates                                                                 */
/* ------------------------------------------------------------------------- */

int mino_future_realized_p(mino_val_t *fut)
{
    mino_future_t *impl;
    int realized;
    if (fut == NULL || fut->type != MINO_FUTURE) { return 0; }
    impl = fut->as.future.impl;
    if (impl == NULL) { return 0; }
    mu_lock(&impl->mu);
    realized = (impl->state_tag != MINO_FUTURE_PENDING) ? 1 : 0;
    mu_unlock(&impl->mu);
    return realized;
}

int mino_future_done_p(mino_val_t *fut)
{
    return mino_future_realized_p(fut);
}

int mino_future_cancelled_p(mino_val_t *fut)
{
    mino_future_t *impl;
    int cancelled;
    if (fut == NULL || fut->type != MINO_FUTURE) { return 0; }
    impl = fut->as.future.impl;
    if (impl == NULL) { return 0; }
    mu_lock(&impl->mu);
    cancelled = (impl->state_tag == MINO_FUTURE_CANCELLED) ? 1 : 0;
    mu_unlock(&impl->mu);
    return cancelled;
}

int mino_future_cancel(mino_state_t *S, mino_val_t *fut)
{
    mino_future_t *impl;
    int newly_set = 0;

    (void)S;
    if (fut == NULL || fut->type != MINO_FUTURE) { return 0; }
    impl = fut->as.future.impl;
    if (impl == NULL) { return 0; }

    mu_lock(&impl->mu);
    if (impl->state_tag == MINO_FUTURE_PENDING) {
        impl->cancel_flag = 1;
        impl->state_tag   = MINO_FUTURE_CANCELLED;
        cv_broadcast(&impl->cv);
        newly_set = 1;
    }
    mu_unlock(&impl->mu);
    return newly_set;
}

/* ------------------------------------------------------------------------- */
/* Worker entry                                                              */
/* ------------------------------------------------------------------------- */

#if defined(_WIN32) && defined(_MSC_VER)
static unsigned __stdcall worker_entry(void *arg)
#else
static void *worker_entry(void *arg)
#endif
{
    mino_future_t      *impl = (mino_future_t *)arg;
    mino_state_t       *S    = impl->state;
    mino_thread_ctx_t  *ctx;
    mino_val_t         *result;
    char                stack_anchor;

    /* Each worker has its own ctx. Allocate it on the heap (not on
     * the worker's stack) so it survives any sub-call boundary; the
     * ctx's gc_stack_bottom is anchored to the worker's stack. */
    ctx = (mino_thread_ctx_t *)calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        mu_lock(&impl->mu);
        impl->state_tag = MINO_FUTURE_FAILED;
        impl->exception = NULL;
        cv_broadcast(&impl->cv);
        mu_unlock(&impl->mu);
        return 0;
    }
    ctx->gc_stack_bottom = (void *)&stack_anchor;
    mino_tls_ctx = ctx;

    /* Acquire the state lock for the entire eval. Per Cycle G4.3,
     * single-state futures execute serialized w.r.t. each other and
     * the embedder thread. */
    mino_lock(S);

    result = mino_call(S, impl->thunk, mino_nil(S), NULL);

    mino_unlock(S);

    /* Publish result. Note: mino_call returns NULL on uncaught throw;
     * we capture the diagnostic into the exception field so deref can
     * rethrow on the waiter side. */
    mu_lock(&impl->mu);
    if (impl->state_tag == MINO_FUTURE_PENDING) {
        if (result == NULL) {
            impl->state_tag = MINO_FUTURE_FAILED;
            impl->exception = NULL; /* TODO: capture diag */
        } else {
            impl->state_tag = MINO_FUTURE_RESOLVED;
            impl->result    = result;
        }
        cv_broadcast(&impl->cv);
    }
    /* If state_tag was already CANCELLED, leave it as CANCELLED and
     * drop the result. */
    mu_unlock(&impl->mu);

    mino_tls_ctx = NULL;
    free(ctx);

    return 0;
}

/* ------------------------------------------------------------------------- */
/* Spawn                                                                     */
/* ------------------------------------------------------------------------- */

mino_val_t *mino_future_spawn(mino_state_t *S, mino_val_t *thunk,
                              mino_env_t *env)
{
    mino_val_t *fut;
    mino_future_t *impl;

    /* Enforce the host-thread grant. */
    if (S->thread_count >= S->thread_limit) {
        return prim_throw_classified(S,
            "mino/thread-limit-exceeded", "MTH001",
            "thread limit exceeded; raise via mino_set_thread_limit");
    }
    (void)env; /* env borrowing reserved for richer body forms */

    fut = future_alloc(S);
    if (fut == NULL) { return NULL; }
    impl = fut->as.future.impl;
    impl->thunk    = thunk;
    impl->body_env = (mino_val_t *)env; /* env is gc-rooted via thunk closure */

    /* First spawn flips multi_threaded; from this point gc/atom paths
     * take their multi-threaded branches. */
    S->multi_threaded = 1;
    S->thread_count++;

#if defined(_WIN32) && defined(_MSC_VER)
    {
        uintptr_t h = _beginthreadex(NULL, 0, worker_entry, impl, 0, NULL);
        if (h == 0) {
            S->thread_count--;
            return NULL;
        }
        impl->thread = (HANDLE)h;
        impl->thread_started = 1;
    }
#else
    {
        int rc = pthread_create(&impl->thread, NULL, worker_entry, impl);
        if (rc != 0) {
            S->thread_count--;
            return NULL;
        }
        impl->thread_started = 1;
    }
#endif

    return fut;
}

/* ------------------------------------------------------------------------- */
/* Deref                                                                     */
/* ------------------------------------------------------------------------- */

mino_val_t *mino_future_deref(mino_state_t *S, mino_val_t *fut)
{
    mino_future_t *impl;
    mino_val_t    *out;

    if (fut == NULL || fut->type != MINO_FUTURE) {
        return mino_nil(S);
    }
    impl = fut->as.future.impl;
    if (impl == NULL) { return mino_nil(S); }

    /* Release the state_lock during the wait so other workers can
     * make progress. The waiter is itself running under state_lock
     * (it's an in-eval call); cross-state futures depend on the
     * holder dropping it. */
    if (S->multi_threaded) {
        mino_unlock(S);
    }

    mu_lock(&impl->mu);
    while (impl->state_tag == MINO_FUTURE_PENDING) {
        cv_wait(&impl->cv, &impl->mu);
    }
    out = (impl->state_tag == MINO_FUTURE_RESOLVED) ? impl->result : NULL;
    mu_unlock(&impl->mu);

    /* Re-acquire so the caller's eval continues holding it. */
    if (S->multi_threaded) {
        mino_lock(S);
    }

    if (out != NULL) {
        return out;
    }

    /* FAILED or CANCELLED: deref re-raises. */
    if (impl->state_tag == MINO_FUTURE_CANCELLED) {
        return prim_throw_classified(S, "mino/cancelled", "MTH002",
                                     "future was cancelled");
    }
    /* FAILED with no captured exception (worker hit OOM): synthesize. */
    return prim_throw_classified(S, "mino/future-failed", "MTH003",
                                 "future failed");
}

/* ------------------------------------------------------------------------- */
/* Quiesce                                                                   */
/* ------------------------------------------------------------------------- */

void mino_host_threads_quiesce(mino_state_t *S)
{
    mino_future_t *impl;

    /* Walk outstanding futures and join each worker. We don't hold
     * state_lock here — workers need to take it to resolve. We
     * rely on the embedder having stopped feeding new evals before
     * calling quiesce. */
    impl = S->future_list_head;
    while (impl != NULL) {
        if (impl->thread_started && !impl->thread_joined) {
#if defined(_WIN32) && defined(_MSC_VER)
            WaitForSingleObject(impl->thread, INFINITE);
            CloseHandle(impl->thread);
#else
            pthread_join(impl->thread, NULL);
#endif
            impl->thread_joined = 1;
        }
        impl = impl->next_in_state;
    }
}

/* ------------------------------------------------------------------------- */
/* GC                                                                        */
/* ------------------------------------------------------------------------- */

void mino_future_gc_trace(mino_val_t *fut)
{
    /* Trace-only: mark the impl's referenced values. The caller in
     * gc/trace.c handles the actual mark-and-push. We expose the
     * pointers here for the trace driver to walk. */
    (void)fut;
    /* Implemented inline in gc/trace.c; this fn is kept for
     * symmetry with the sweep hook below. */
}

void mino_future_gc_sweep(mino_val_t *fut)
{
    mino_future_t *impl;
    if (fut == NULL || fut->type != MINO_FUTURE) { return; }
    impl = fut->as.future.impl;
    if (impl == NULL) { return; }

    /* If the worker is still running, joining here would deadlock
     * (worker needs state_lock; sweep holds the mutator). We rely
     * on quiesce having joined every worker before state_free
     * tears down the heap. */
    cv_destroy(&impl->cv);
    mu_destroy(&impl->mu);
    free(impl);
    fut->as.future.impl = NULL;
}
