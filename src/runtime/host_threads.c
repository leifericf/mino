/*
 * host_threads.c -- pthread/Win32 worker threads for futures + promises.
 *
 * Threading model:
 *   - The per-state state_lock is held by the worker for the
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

/* Lock invariant: takes only impl->mu (the per-future mutex). State_lock
 * is not required and may be held or not by the caller; deliver never
 * touches mino_state_t fields. Idempotent — second deliver on a non-pending
 * promise is a no-op. */
int mino_promise_deliver(mino_state_t *S, mino_val_t *promise,
                         mino_val_t *value)
{
    mino_future_t *impl;
    int delivered = 0;

    (void)S;
    if (promise == NULL || mino_type_of(promise) != MINO_FUTURE) { return 0; }
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
    if (fut == NULL || mino_type_of(fut) != MINO_FUTURE) { return 0; }
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
    if (fut == NULL || mino_type_of(fut) != MINO_FUTURE) { return 0; }
    impl = fut->as.future.impl;
    if (impl == NULL) { return 0; }
    mu_lock(&impl->mu);
    cancelled = (impl->state_tag == MINO_FUTURE_CANCELLED) ? 1 : 0;
    mu_unlock(&impl->mu);
    return cancelled;
}

/* Lock invariant: takes only impl->mu. State_lock is not required.
 * Cancel sets state_tag = CANCELLED and broadcasts the cv; in-flight
 * worker_run may publish a result that races with the cancel — that's
 * fine, worker_run notices state_tag != PENDING under impl->mu and drops
 * its result. */
int mino_future_cancel(mino_state_t *S, mino_val_t *fut)
{
    mino_future_t *impl;
    int newly_set = 0;

    (void)S;
    if (fut == NULL || mino_type_of(fut) != MINO_FUTURE) { return 0; }
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

/* Lock invariant: enters with no locks held — this thread did not pass
 * through mino_call. Acquires worker_list_lock explicitly twice: once
 * briefly to attach ctx to S->worker_ctxs_head before running the
 * body, and once at exit to detach ctx and decrement S->thread_count
 * atomically. The mino_call inside the body acquires state_lock
 * recursively for the duration of the user thunk. impl->mu is taken
 * only to publish the result and broadcast the cv.
 *
 * Why worker_list_lock and not state_lock for the entry/exit step:
 * a tight embedder loop holding state_lock would otherwise stall
 * the worker at link/unlink, which (a) leaves the worker invisible
 * to the GC root walker for the duration of the loop and (b) keeps
 * thread_count inflated even after the worker's body has finished. */
static void worker_run(mino_future_t *impl, char *stack_anchor)
{
    mino_state_t       *S    = impl->state;
    mino_thread_ctx_t  *ctx;
    mino_val_t         *result;

    /* Each worker has its own ctx. Allocate it on the heap (not on
     * the worker's stack) so it survives any sub-call boundary; the
     * ctx's gc_stack_bottom is anchored to the worker's stack via
     * the stack_anchor pointer the entry shell forwards. */
    ctx = (mino_thread_ctx_t *)calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        mu_lock(&impl->mu);
        impl->state_tag = MINO_FUTURE_FAILED;
        impl->exception = NULL;
        cv_broadcast(&impl->cv);
        mu_unlock(&impl->mu);
        return;
    }
    ctx->gc_stack_bottom = (void *)stack_anchor;
    mino_tls_ctx = ctx;

    /* Link onto S->worker_ctxs_head so gc_mark_roots can walk the
     * worker's gc_save and dyn_stack while it's blocked. Take the
     * brief worker_list_lock for the list mutation. */
    mino_worker_list_lock_acquire(S);
    ctx->next_worker = S->worker_ctxs_head;
    S->worker_ctxs_head = ctx;
    mino_worker_list_lock_release(S);

    /* Embed-distinctive lifecycle hook. Spawn-per-future path only.
     * Pool-managed workers run under the pool's own lifecycle hooks.
     * Hook fires on the worker thread so it can do pthread_setname_np,
     * CPU pinning, or priority class. */
    if (S->thread_pool == NULL && S->thread_start_fn != NULL) {
        S->thread_start_fn(S, S->thread_factory_ctx);
    }

    /* Install conveyed dynamic bindings before running the thunk. The
     * snapshot is a map keyed by symbol; build a malloc-owned binding
     * chain and push as a single dyn_frame so the thunk sees the same
     * dynamic-binding context active at spawn time. */
    {
        mino_val_t  *snap = impl->dyn_snapshot;
        dyn_frame_t *conveyed = NULL;
        if (snap != NULL && mino_type_of(snap) == MINO_MAP && snap->as.map.len > 0) {
            dyn_binding_t *bhead = NULL;
            size_t i;
            int    oom = 0;
            for (i = 0; i < snap->as.map.len; i++) {
                mino_val_t    *key = vec_nth(snap->as.map.key_order, i);
                mino_val_t    *val = map_get_val(snap, key);
                dyn_binding_t *b;
                if (key == NULL
                    || (mino_type_of(key) != MINO_SYMBOL && mino_type_of(key) != MINO_STRING)) {
                    continue;
                }
                b = (dyn_binding_t *)malloc(sizeof(*b));
                if (b == NULL) { oom = 1; break; }
                b->name = mino_symbol(S, key->as.s.data)->as.s.data;
                b->val  = val;
                b->next = bhead;
                bhead   = b;
            }
            if (!oom) {
                conveyed = (dyn_frame_t *)malloc(sizeof(*conveyed));
                if (conveyed == NULL) { dyn_binding_list_free(bhead); }
                else {
                    conveyed->bindings = bhead;
                    conveyed->prev     = NULL;
                    ctx->dyn_stack     = conveyed;
                }
            } else {
                dyn_binding_list_free(bhead);
            }
        }
        result = mino_call(S, impl->thunk, mino_nil(S), NULL);
        if (conveyed != NULL) {
            ctx->dyn_stack = conveyed->prev;
            dyn_binding_list_free(conveyed->bindings);
            free(conveyed);
        }
    }

    /* Publish result. mino_call returns NULL on uncaught throw; we
     * capture the worker's diagnostic as a value-map BEFORE taking
     * impl->mu since the conversion allocates through the GC (needs
     * state_lock). The captured value is reachable from the future
     * (the GC traces impl->exception), so consumer-side deref can
     * rethrow with the original kind/code/message rather than the
     * generic "future failed". If state_tag was already CANCELLED,
     * leave it as CANCELLED and drop the result. */
    {
        mino_val_t *captured = NULL;
        if (result == NULL) {
            mino_lock(S);
            if (mino_last_error(S) != NULL) {
                captured = mino_last_error_map(S);
            }
            mino_unlock(S);
        }
        mu_lock(&impl->mu);
        if (impl->state_tag == MINO_FUTURE_PENDING) {
            if (result == NULL) {
                impl->state_tag = MINO_FUTURE_FAILED;
                impl->exception = captured;
            } else {
                impl->state_tag = MINO_FUTURE_RESOLVED;
                impl->result    = result;
            }
            cv_broadcast(&impl->cv);
        }
        mu_unlock(&impl->mu);
    }

    if (S->thread_pool == NULL && S->thread_end_fn != NULL) {
        S->thread_end_fn(S, S->thread_factory_ctx);
    }

    /* Detach this worker's ctx from S->worker_ctxs_head before
     * freeing so GC root scanning never visits a freed ctx, and
     * release the slot so spawn() bookkeeping reflects only
     * concurrently-live workers. For the spawn-per-future path the
     * pthread itself remains joinable until mino_host_threads_quiesce;
     * pthread_join on an already exited joinable thread is a no-op
     * that returns immediately.
     *
     * Worker exit takes the brief worker_list_lock: this thread did
     * not enter through mino_call, so no caller is holding any lock
     * for us. The detach of worker_ctxs_head and the thread_count
     * decrement happen as one atomic update under the worker-list
     * lock so a concurrent mino_thread_count or spawn-time gate never
     * sees a ctx removed from the GC-root list while the counter
     * still claims a live worker. */
    mino_worker_list_lock_acquire(S);
    {
        mino_thread_ctx_t **pp = &S->worker_ctxs_head;
        while (*pp != NULL && *pp != ctx) { pp = &(*pp)->next_worker; }
        if (*pp == ctx) { *pp = ctx->next_worker; }
    }
    if (S->thread_count > 0) { S->thread_count--; }
    mino_worker_list_lock_release(S);

    mino_tls_ctx = NULL;
    free(ctx);
}

#if defined(_WIN32) && defined(_MSC_VER)
static unsigned __stdcall worker_entry(void *arg)
{
    char anchor;
    worker_run((mino_future_t *)arg, &anchor);
    return 0;
}
#else
static void *worker_entry(void *arg)
{
    char anchor;
    worker_run((mino_future_t *)arg, &anchor);
    return NULL;
}
#endif

/* Pool work entry — the void(*)(void*) signature host pools expect.
 * Same body as worker_entry; caller's responsibility to actually run
 * this on a thread other than the submitter. */
static void worker_pool_entry(void *arg)
{
    char anchor;
    worker_run((mino_future_t *)arg, &anchor);
}

/* ------------------------------------------------------------------------- */
/* Spawn                                                                     */
/* ------------------------------------------------------------------------- */

/* Lock invariant: caller is the apply path from mino_call, which holds
 * state_lock recursively for the entire call. multi_threaded is
 * written under the caller-held state_lock; the thread_count gate
 * and increment use the worker_list_lock (state_lock outer ->
 * worker_list_lock inner). The lock is not released before this
 * function returns; the worker thread takes its own lock acquisitions
 * later. */
mino_val_t *mino_future_spawn(mino_state_t *S, mino_val_t *thunk,
                              mino_env_t *env)
{
    mino_val_t *fut;
    mino_future_t *impl;

    /* Enforce the host-thread grant. The gate-and-increment is one
     * critical section under worker_list_lock so a parallel spawn
     * can't both read thread_count < limit and then both increment. */
    mino_worker_list_lock_acquire(S);
    if (S->thread_count >= S->thread_limit) {
        mino_worker_list_lock_release(S);
        return prim_throw_classified(S,
            "mino/thread-limit-exceeded", "MTH001",
            "thread limit exceeded; raise via mino_set_thread_limit");
    }
    S->thread_count++;
    mino_worker_list_lock_release(S);
    (void)env; /* env borrowing reserved for richer body forms */

    /* First spawn flips multi_threaded; from this point gc/atom paths
     * take their multi-threaded branches. Reached only via the apply
     * path from mino_call, which holds state_lock for the entire call. */
    S->multi_threaded = 1;

    fut = future_alloc(S);
    if (fut == NULL) {
        mino_worker_list_lock_acquire(S);
        if (S->thread_count > 0) { S->thread_count--; }
        mino_worker_list_lock_release(S);
        return NULL;
    }
    impl = fut->as.future.impl;
    impl->thunk    = thunk;
    impl->body_env = (mino_val_t *)env; /* env is gc-rooted via thunk closure */
    /* Convey the caller's dynamic bindings to the worker. The worker
     * unpacks this map into a dyn_frame before invoking the thunk so
     * `(future ...)` sees the binding context active at spawn time --
     * matching Clojure JVM's binding-conveyance contract that
     * `bound-fn`, `binding`, and `*ns*` rely on across threads. */
    impl->dyn_snapshot = mino_snapshot_thread_bindings(S);

    /* Pool path: the embedder hands us a host pool and we just submit
     * the work item. impl->thread_started stays 0 so the sweep + quiesce
     * paths skip pthread_join (mino doesn't own the pthread). */
    if (S->thread_pool != NULL && S->thread_pool->submit_fn != NULL) {
        int rc = S->thread_pool->submit_fn(S->thread_pool,
                                           worker_pool_entry, impl);
        if (rc != 0) {
            mino_worker_list_lock_acquire(S);
            if (S->thread_count > 0) { S->thread_count--; }
            mino_worker_list_lock_release(S);
            return prim_throw_classified(S,
                "mino/thread-limit-exceeded", "MTH001",
                "host thread pool refused submission");
        }
        return fut;
    }

#if defined(_WIN32) && defined(_MSC_VER)
    {
        unsigned stack = (unsigned)S->thread_stack_size;  /* 0 = default */
        uintptr_t h = _beginthreadex(NULL, stack, worker_entry, impl,
                                     0, NULL);
        if (h == 0) {
            mino_worker_list_lock_acquire(S);
            if (S->thread_count > 0) { S->thread_count--; }
            mino_worker_list_lock_release(S);
            return NULL;
        }
        impl->thread = (HANDLE)h;
        impl->thread_started = 1;
    }
#else
    {
        pthread_attr_t attr;
        pthread_attr_t *attrp = NULL;
        int rc;
        if (S->thread_stack_size > 0) {
            if (pthread_attr_init(&attr) == 0) {
                pthread_attr_setstacksize(&attr, S->thread_stack_size);
                attrp = &attr;
            }
        }
        rc = pthread_create(&impl->thread, attrp, worker_entry, impl);
        if (attrp != NULL) { pthread_attr_destroy(attrp); }
        if (rc != 0) {
            mino_worker_list_lock_acquire(S);
            if (S->thread_count > 0) { S->thread_count--; }
            mino_worker_list_lock_release(S);
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

    if (fut == NULL || mino_type_of(fut) != MINO_FUTURE) {
        return mino_nil(S);
    }
    impl = fut->as.future.impl;
    if (impl == NULL) { return mino_nil(S); }

    /* Yield the state_lock fully before blocking on cv so the worker
     * thread (which needs to take state_lock for its eval) can run.
     * Resume the same depth after wake. */
    {
        int depth = mino_yield_lock(S);
        mu_lock(&impl->mu);
        while (impl->state_tag == MINO_FUTURE_PENDING) {
            cv_wait(&impl->cv, &impl->mu);
        }
        out = (impl->state_tag == MINO_FUTURE_RESOLVED) ? impl->result : NULL;
        mu_unlock(&impl->mu);
        mino_resume_lock(S, depth);
    }

    if (out != NULL) {
        return out;
    }

    /* FAILED or CANCELLED: deref re-raises. */
    if (impl->state_tag == MINO_FUTURE_CANCELLED) {
        return prim_throw_classified(S, "mino/cancelled", "MTH002",
                                     "future was cancelled");
    }
    /* FAILED with a captured worker diag: rethrow with the original
     * kind/code/message. The captured value-map is the diag the
     * worker latched right before it failed, so the consumer sees
     * the actual cause rather than a generic "future failed". */
    if (impl->exception != NULL
        && mino_type_of(impl->exception) == MINO_MAP) {
        mino_val_t *ex      = impl->exception;
        mino_val_t *k_kind  = mino_keyword(S, "mino/kind");
        mino_val_t *k_code  = mino_keyword(S, "mino/code");
        mino_val_t *k_msg   = mino_keyword(S, "mino/message");
        mino_val_t *v_kind  = map_get_val(ex, k_kind);
        mino_val_t *v_code  = map_get_val(ex, k_code);
        mino_val_t *v_msg   = map_get_val(ex, k_msg);
        char        kbuf[64];
        char        cbuf[32];
        char        mbuf[512];
        const char *kind = "mino/future-failed";
        const char *code = "MTH003";
        const char *msg  = "future failed";
        if (v_kind != NULL && mino_type_of(v_kind) == MINO_KEYWORD
            && v_kind->as.s.len < sizeof(kbuf)) {
            memcpy(kbuf, v_kind->as.s.data, v_kind->as.s.len);
            kbuf[v_kind->as.s.len] = '\0';
            kind = kbuf;
        }
        if (v_code != NULL && mino_type_of(v_code) == MINO_STRING
            && v_code->as.s.len < sizeof(cbuf)) {
            memcpy(cbuf, v_code->as.s.data, v_code->as.s.len);
            cbuf[v_code->as.s.len] = '\0';
            code = cbuf;
        }
        if (v_msg != NULL && mino_type_of(v_msg) == MINO_STRING
            && v_msg->as.s.len < sizeof(mbuf)) {
            memcpy(mbuf, v_msg->as.s.data, v_msg->as.s.len);
            mbuf[v_msg->as.s.len] = '\0';
            msg = mbuf;
        }
        return prim_throw_classified(S, kind, code, msg);
    }
    /* FAILED with no captured exception (worker hit OOM): synthesize. */
    return prim_throw_classified(S, "mino/future-failed", "MTH003",
                                 "future failed");
}

/* ------------------------------------------------------------------------- */
/* Quiesce                                                                   */
/* ------------------------------------------------------------------------- */

/* Lock invariant: caller may hold state_lock recursively (typical: from
 * prim_exit during eval). We yield_lock to fully drop our holds before
 * blocking on join/cv_wait — workers need to acquire state_lock to run
 * their body, so holding it here would deadlock. resume_lock restores
 * the original depth before returning so the caller's invariants still
 * hold. */
void mino_host_threads_quiesce(mino_state_t *S)
{
    mino_future_t *impl;
    int saved_depth;

    /* If the caller is mid-eval (e.g. prim_exit), we hold state_lock
     * recursively. Workers need state_lock to run mino_call inside
     * the future body, so we must drop our holds before blocking on
     * pthread_join / cv_wait — otherwise an in-flight worker can
     * never finish and we deadlock. */
    saved_depth = mino_yield_lock(S);

    /* Pass 1: cancel every still-pending future and promise cell.
     * Without this, a worker thunk blocked in mino_future_deref on
     * a never-to-be-delivered promise — e.g. `@undelivered-gate`
     * inside a future body — would stay parked in cv_wait forever
     * and pthread_join would hang the embedder. Cancelling flips
     * the state_tag to CANCELLED and broadcasts the cell's cv, so
     * the deref's while-loop exits, throws "future was cancelled"
     * up through the thunk, and worker_run reaches the publish
     * path. Worker_run already no-ops the publish when state_tag
     * is non-PENDING, so the cancelled state is preserved.
     *
     * The cancel scope is "every cell still PENDING when teardown
     * begins" — that's the contract for state_free: tear down the
     * state, no matter what is parked on a promise or in flight.
     * Cells with no thread (bare promises) and no waiter cost
     * nothing here; they simply move to CANCELLED before the GC
     * sweep collects them. */
    impl = S->future_list_head;
    while (impl != NULL) {
        mu_lock(&impl->mu);
        if (impl->state_tag == MINO_FUTURE_PENDING) {
            impl->cancel_flag = 1;
            impl->state_tag   = MINO_FUTURE_CANCELLED;
            cv_broadcast(&impl->cv);
        }
        mu_unlock(&impl->mu);
        impl = impl->next_in_state;
    }

    /* Pass 2: join. Two waiting cases remain:
     *   - Spawn-per-future: pthread_join (Win: WaitForSingleObject)
     *     for the worker thread to return after its cancelled
     *     deref unwinds.
     *   - Pool-managed: thread_started is 0 (the pool spawned the
     *     OS thread, not us), but a thunk was queued. Wait on
     *     impl->cv until state_tag is no longer PENDING — after
     *     Pass 1 it should already be CANCELLED for any cell that
     *     hadn't completed, but the cv_wait is the safe shape and
     *     handles the in-flight-just-finishing race too.
     * Bare promises with no thunk and no thread skip both cases. */
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
        } else if (!impl->thread_started && impl->thunk != NULL) {
            mu_lock(&impl->mu);
            while (impl->state_tag == MINO_FUTURE_PENDING) {
                cv_wait(&impl->cv, &impl->mu);
            }
            mu_unlock(&impl->mu);
        }
        impl = impl->next_in_state;
    }

    mino_resume_lock(S, saved_depth);
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

/* Lock invariant: called from the GC sweep phase, which itself runs
 * under the GC's own serialization (no concurrent mutator). State_lock
 * is not taken here — instead we rely on the GC suppression while
 * thread_count > 0 invariant: any worker for this future has already
 * decremented thread_count and is past its last lock acquire, so a
 * pthread_join here is safe and never blocks indefinitely. impl->mu is
 * destroyed only after the join. */
void mino_future_gc_sweep(mino_val_t *fut)
{
    mino_future_t *impl;
    mino_state_t *S;
    if (fut == NULL || mino_type_of(fut) != MINO_FUTURE) { return; }
    impl = fut->as.future.impl;
    if (impl == NULL) { return; }

    /* GC is suppressed while thread_count > 0, so any worker for this
     * future has decremented S->thread_count and is on its way out of
     * worker_entry. Join here so the pthread is fully reaped before
     * we destroy mu/cv -- otherwise the worker's final mu_unlock can
     * touch destroyed mutex memory. pthread_join on an already-exited
     * joinable thread returns immediately. */
    S = impl->state;
    if (impl->thread_started && !impl->thread_joined) {
#if defined(_WIN32) && defined(_MSC_VER)
        WaitForSingleObject(impl->thread, INFINITE);
        CloseHandle(impl->thread);
#else
        pthread_join(impl->thread, NULL);
#endif
        impl->thread_joined = 1;
    }

    /* Detach from S->future_list_head before freeing so quiesce
     * never walks into a dangling impl. */
    if (S != NULL) {
        mino_future_t **pp = &S->future_list_head;
        while (*pp != NULL && *pp != impl) { pp = &(*pp)->next_in_state; }
        if (*pp == impl) { *pp = impl->next_in_state; }
    }

    cv_destroy(&impl->cv);
    mu_destroy(&impl->mu);
    free(impl);
    fut->as.future.impl = NULL;
}
