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
extern mino_val *prim_throw_classified(mino_state *S, const char *kind,
                                         const char *code, const char *msg);
/* Wraps any thrown value (kind-tagged map, ex-info, raw value, NULL)
 * into the standard diagnostic map the future stores + deref rethrows.
 * Defined in eval/control.c; declared in eval/special_internal.h,
 * which is eval-layer-private, so forward-declared here. */
extern mino_val *normalize_exception(mino_state *S, mino_val *ex_val);

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
/* Returns 1 on signal, 0 on timeout. */
static int cv_timedwait_ms(CONDITION_VARIABLE *c, CRITICAL_SECTION *m,
                           long ms)
{
    DWORD wait = (ms < 0) ? 0 : (DWORD)ms;
    return SleepConditionVariableCS(c, m, wait) ? 1 : 0;
}
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
/* Returns 1 on signal, 0 on timeout. Negative ms means "already
 * elapsed"; treated as a 0-wait poll. */
static int cv_timedwait_ms(pthread_cond_t *c, pthread_mutex_t *m, long ms)
{
    struct timespec deadline;
    long sec, nsec;
    int rc;
    clock_gettime(CLOCK_REALTIME, &deadline);
    if (ms < 0) ms = 0;
    sec  = ms / 1000;
    nsec = (ms % 1000) * 1000000L;
    deadline.tv_sec += sec;
    deadline.tv_nsec += nsec;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec  += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    rc = pthread_cond_timedwait(c, m, &deadline);
    return (rc == 0) ? 1 : 0;
}
#endif

/* ------------------------------------------------------------------------- */
/* state init/destroy                                                        */
/* ------------------------------------------------------------------------- */

void mino_host_threads_state_init(mino_state *S)
{
    S->threading.future_list_head = NULL;
}

void mino_host_threads_state_destroy(mino_state *S)
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

static mino_val *future_alloc(mino_state *S)
{
    mino_val      *v;
    mino_future   *impl;

    impl = (mino_future *)calloc(1, sizeof(*impl));
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
    impl->next_in_state = S->threading.future_list_head;
    S->threading.future_list_head = impl;
    mino_unlock(S);

    return v;
}

/* ------------------------------------------------------------------------- */
/* Public construction                                                        */
/* ------------------------------------------------------------------------- */

mino_val *mino_promise_new(mino_state *S)
{
    return future_alloc(S);
    /* No worker spawned; thread_started stays 0. */
}

/* Lock invariant: takes only impl->mu (the per-future mutex). State_lock
 * is not required and may be held or not by the caller; deliver never
 * touches mino_state fields. Idempotent — second deliver on a non-pending
 * promise is a no-op. */
int mino_promise_deliver(mino_state *S, mino_val *promise,
                         mino_val *value)
{
    mino_future *impl;
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

int mino_future_realized_p(mino_val *fut)
{
    mino_future *impl;
    int realized;
    if (fut == NULL || mino_type_of(fut) != MINO_FUTURE) { return 0; }
    impl = fut->as.future.impl;
    if (impl == NULL) { return 0; }
    mu_lock(&impl->mu);
    realized = (impl->state_tag != MINO_FUTURE_PENDING) ? 1 : 0;
    mu_unlock(&impl->mu);
    return realized;
}

int mino_future_done_p(mino_val *fut)
{
    return mino_future_realized_p(fut);
}

int mino_future_cancelled_p(mino_val *fut)
{
    mino_future *impl;
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
int mino_future_cancel(mino_state *S, mino_val *fut)
{
    mino_future *impl;
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
 * briefly to attach ctx to S->threading.worker_ctxs_head before running the
 * body, and once at exit to detach ctx and decrement S->threading.thread_count
 * atomically. The mino_call inside the body acquires state_lock
 * recursively for the duration of the user thunk. impl->mu is taken
 * only to publish the result and broadcast the cv.
 *
 * Why worker_list_lock and not state_lock for the entry/exit step:
 * a tight embedder loop holding state_lock would otherwise stall
 * the worker at link/unlink, which (a) leaves the worker invisible
 * to the GC root walker for the duration of the loop and (b) keeps
 * thread_count inflated even after the worker's body has finished. */
static void worker_run(mino_future *impl, char *stack_anchor)
{
    mino_state       *S    = impl->state;
    mino_thread_ctx_t  *ctx;
    mino_val         *result;
    mino_val         *thrown = NULL;

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
    /* Wire cooperative cancel: BC safepoint reads through this TLS
     * pointer. future-cancel writes impl->cancel_flag; the worker
     * observes on its next safepoint poll and throws :mino/cancelled.
     * Reset the safepoint counter so the auto-yield slot is aligned
     * to this worker's start, not whatever value was left over from
     * a recycled pool thread. */
    mino_tls_cancel_ptr      = &impl->cancel_flag;
    mino_tls_safepoint_count = 0;

    /* Link onto S->threading.worker_ctxs_head so gc_mark_roots can walk the
     * worker's gc_save and dyn_stack while it's blocked. Take the
     * brief worker_list_lock for the list mutation. */
    mino_worker_list_lock_acquire(S);
    ctx->next_worker = S->threading.worker_ctxs_head;
    S->threading.worker_ctxs_head = ctx;
    mino_worker_list_lock_release(S);

    /* Embed-distinctive lifecycle hook. Spawn-per-future path only.
     * Pool-managed workers run under the pool's own lifecycle hooks.
     * Hook fires on the worker thread so it can do pthread_setname_np,
     * CPU pinning, or priority class. */
    if (S->threading.thread_pool == NULL && S->threading.thread_start_fn != NULL) {
        S->threading.thread_start_fn(S, S->threading.thread_factory_ctx);
    }

    /* Install conveyed dynamic bindings before running the thunk. The
     * snapshot is a map keyed by symbol; build a malloc-owned binding
     * chain and push as a single dyn_frame so the thunk sees the same
     * dynamic-binding context active at spawn time. */
    {
        mino_val  *snap = impl->dyn_snapshot;
        dyn_frame_t *conveyed = NULL;
        if (snap != NULL && mino_type_of(snap) == MINO_MAP && snap->as.map.len > 0) {
            dyn_binding_t *bhead = NULL;
            size_t i;
            int    oom = 0;
            /* Symbol interning mutates the shared intern table; on a
             * multi-threaded state every writer must hold state_lock.
             * The thunk's mino_call acquires it just below, but the
             * convey-binding build runs before that, so take the lock
             * explicitly for the symbol/string installs. */
            mino_lock(S);
            for (i = 0; i < snap->as.map.len; i++) {
                mino_val    *key = vec_nth(snap->as.map.key_order, i);
                mino_val    *val = map_get_val(snap, key);
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
            mino_unlock(S);
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
        /* Run the thunk under a protected call so an uncaught throw
         * longjmps to this worker boundary instead of returning NULL.
         * The worker enters mino with no enclosing try frame; without
         * one, a prim raise on the worker (try_depth == 0) returns NULL
         * through the call chain. The interpreter propagates that NULL
         * safely, but a JIT'd region's stencils chain the NULL register
         * window into the next instance and dereference it -- a crash.
         * mino_pcall installs the frame (and recovers lock depth on the
         * longjmp), matching how the agent worker invokes its action. */
        result = NULL;
        (void)mino_pcall(S, impl->thunk, mino_nil(S), NULL,
                         &result, &thrown);
        if (conveyed != NULL) {
            ctx->dyn_stack = conveyed->prev;
            dyn_binding_list_free(conveyed->bindings);
            free(conveyed);
        }
    }

    /* Publish result. mino_pcall returns NULL with the thrown payload
     * in `thrown` on an uncaught throw; we normalize it into the
     * standard diagnostic map BEFORE taking impl->mu since the
     * conversion allocates through the GC (needs state_lock).
     * normalize_exception handles every payload shape: a kind-tagged
     * diag map (prim raises) passes through; an ex-info map keeps its
     * :mino/data; a raw value (e.g. (throw 42)) is wrapped with its
     * printed form as the message. The captured value is reachable
     * from the future (the GC traces impl->exception), so consumer-side
     * deref can rethrow with full fidelity rather than the generic
     * "future failed". If state_tag was already CANCELLED, leave it as
     * CANCELLED and drop the result. */
    {
        mino_val *captured = NULL;
        if (result == NULL) {
            mino_lock(S);
            captured = normalize_exception(S, thrown);
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

    if (S->threading.thread_pool == NULL && S->threading.thread_end_fn != NULL) {
        S->threading.thread_end_fn(S, S->threading.thread_factory_ctx);
    }

    /* Detach this worker's ctx from S->threading.worker_ctxs_head before
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
        mino_thread_ctx_t **pp = &S->threading.worker_ctxs_head;
        while (*pp != NULL && *pp != ctx) { pp = &(*pp)->next_worker; }
        if (*pp == ctx) { *pp = ctx->next_worker; }
    }
    if (__atomic_load_n(&S->threading.thread_count, __ATOMIC_RELAXED) > 0) {
        __atomic_fetch_sub(&S->threading.thread_count, 1, __ATOMIC_RELAXED);
    }
    mino_worker_list_lock_release(S);

    /* Worker may have set ctx->last_diag if its body threw and the
     * throw landed at try_depth == 0 (no enclosing try). The diag's
     * cached_map (if any) was already captured into impl->exception
     * and is reachable from the future on the GC heap; only the
     * unmanaged diag struct itself remains. Reclaim it before free(ctx)
     * so a future that throws does not leak a ~160-byte diag per call.
     * LSan surfaced this on the gcc-built linux runners where libasan
     * runs the leak detector at exit. */
    if (ctx->last_diag != NULL) {
        diag_free(ctx->last_diag);
        ctx->last_diag = NULL;
    }
    free(ctx->lazy_inflight);
    mino_tls_ctx        = NULL;
    mino_tls_cancel_ptr = NULL;
    free(ctx);
}

#if defined(_WIN32) && defined(_MSC_VER)
static unsigned __stdcall worker_entry(void *arg)
{
    char anchor;
    worker_run((mino_future *)arg, &anchor);
    return 0;
}
#else
static void *worker_entry(void *arg)
{
    char anchor;
    worker_run((mino_future *)arg, &anchor);
    return NULL;
}
#endif

/* Pool work entry — the void(*)(void*) signature host pools expect.
 * Same body as worker_entry; caller's responsibility to actually run
 * this on a thread other than the submitter. */
static void worker_pool_entry(void *arg)
{
    char anchor;
    worker_run((mino_future *)arg, &anchor);
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
mino_val *mino_future_spawn(mino_state *S, mino_val *thunk,
                              mino_env *env)
{
    mino_val *fut;
    mino_future *impl;

    /* Enforce the host-thread grant. The gate-and-increment is one
     * critical section under worker_list_lock so a parallel spawn
     * can't both read thread_count < limit and then both increment. */
    mino_worker_list_lock_acquire(S);
    if (__atomic_load_n(&S->threading.thread_count, __ATOMIC_RELAXED) >= S->threading.thread_limit) {
        mino_worker_list_lock_release(S);
        return prim_throw_classified(S,
            "mino/thread-limit-exceeded", "MTH001",
            "thread limit exceeded; raise via mino_set_thread_limit");
    }
    __atomic_fetch_add(&S->threading.thread_count, 1, __ATOMIC_RELAXED);
    mino_worker_list_lock_release(S);
    (void)env; /* env borrowing reserved for richer body forms */

    /* First spawn flips multi_threaded; from this point gc/atom paths
     * take their multi-threaded branches. Reached only via the apply
     * path from mino_call, which holds state_lock for the entire call. */
    S->threading.multi_threaded = 1;

    fut = future_alloc(S);
    if (fut == NULL) {
        mino_worker_list_lock_acquire(S);
        if (__atomic_load_n(&S->threading.thread_count, __ATOMIC_RELAXED) > 0) {
        __atomic_fetch_sub(&S->threading.thread_count, 1, __ATOMIC_RELAXED);
    }
        mino_worker_list_lock_release(S);
        return NULL;
    }
    impl = fut->as.future.impl;
    impl->thunk    = thunk;
    impl->body_env = (mino_val *)env; /* env is gc-rooted via thunk closure */
    /* Convey the caller's dynamic bindings to the worker. The worker
     * unpacks this map into a dyn_frame before invoking the thunk so
     * `(future ...)` sees the binding context active at spawn time --
     * matching Clojure JVM's binding-conveyance contract that
     * `bound-fn`, `binding`, and `*ns*` rely on across threads. */
    impl->dyn_snapshot = mino_snapshot_thread_bindings(S);

    /* Pool path: the embedder hands us a host pool and we just submit
     * the work item. impl->thread_started stays 0 so the sweep + quiesce
     * paths skip pthread_join (mino doesn't own the pthread). */
    if (S->threading.thread_pool != NULL && S->threading.thread_pool->submit_fn != NULL) {
        int rc = S->threading.thread_pool->submit_fn(S->threading.thread_pool,
                                           worker_pool_entry, impl);
        if (rc != 0) {
            mino_worker_list_lock_acquire(S);
            if (__atomic_load_n(&S->threading.thread_count, __ATOMIC_RELAXED) > 0) {
        __atomic_fetch_sub(&S->threading.thread_count, 1, __ATOMIC_RELAXED);
    }
            mino_worker_list_lock_release(S);
            return prim_throw_classified(S,
                "mino/thread-limit-exceeded", "MTH001",
                "host thread pool refused submission");
        }
        return fut;
    }

#if defined(_WIN32) && defined(_MSC_VER)
    {
        unsigned stack = (unsigned)S->threading.thread_stack_size;  /* 0 = default */
        uintptr_t h = _beginthreadex(NULL, stack, worker_entry, impl,
                                     0, NULL);
        if (h == 0) {
            mino_worker_list_lock_acquire(S);
            if (__atomic_load_n(&S->threading.thread_count, __ATOMIC_RELAXED) > 0) {
        __atomic_fetch_sub(&S->threading.thread_count, 1, __ATOMIC_RELAXED);
    }
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
        if (S->threading.thread_stack_size > 0) {
            if (pthread_attr_init(&attr) == 0) {
                pthread_attr_setstacksize(&attr, S->threading.thread_stack_size);
                attrp = &attr;
            }
        }
        rc = pthread_create(&impl->thread, attrp, worker_entry, impl);
        if (attrp != NULL) { pthread_attr_destroy(attrp); }
        if (rc != 0) {
            mino_worker_list_lock_acquire(S);
            if (__atomic_load_n(&S->threading.thread_count, __ATOMIC_RELAXED) > 0) {
        __atomic_fetch_sub(&S->threading.thread_count, 1, __ATOMIC_RELAXED);
    }
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

mino_val *mino_future_deref(mino_state *S, mino_val *fut)
{
    mino_future *impl;
    mino_val    *out;

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
    /* FAILED with a captured worker diag: rethrow with full fidelity.
     * The captured value-map is the diag the worker latched right
     * before it failed; longjmp it into the enclosing try-frame
     * unmodified so the consumer sees the same shape the worker
     * saw, including :mino/data (ex-info payload) and any other
     * fields. The previous variant copied only kind/code/message via
     * prim_throw_classified, which dropped :mino/data and re-emitted
     * a fresh :mino/location pointing at deref's call site. With
     * try_depth == 0 we still go through prim_throw_classified so
     * the host-style diag path runs, but accept the message-only
     * narrowing in that path because there's no catch frame to
     * receive the full map anyway. */
    if (impl->exception != NULL
        && mino_type_of(impl->exception) == MINO_MAP) {
        mino_val *ex = impl->exception;
        if (mino_current_ctx(S)->try_depth > 0) {
            int tdepth = mino_current_ctx(S)->try_depth;
            mino_current_ctx(S)->try_stack[tdepth - 1].exception = ex;
            longjmp(mino_current_ctx(S)->try_stack[tdepth - 1].buf, 1);
        }
        /* try_depth == 0: extract the narrow narrative for the
         * top-level diag path so the user still sees the original
         * kind/code/message in stderr. */
        {
            mino_val *k_kind  = mino_keyword(S, "mino/kind");
            mino_val *k_code  = mino_keyword(S, "mino/code");
            mino_val *k_msg   = mino_keyword(S, "mino/message");
            mino_val *v_kind  = map_get_val(ex, k_kind);
            mino_val *v_code  = map_get_val(ex, k_code);
            mino_val *v_msg   = map_get_val(ex, k_msg);
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
    }
    /* FAILED with no captured exception (worker hit OOM): synthesize. */
    return prim_throw_classified(S, "mino/future-failed", "MTH003",
                                 "future failed");
}

mino_val *mino_future_deref_timed(mino_state *S, mino_val *fut,
                                    long ms, mino_val *timeout_val)
{
    mino_future *impl;
    mino_val    *out;
    int            timed_out = 0;

    if (fut == NULL || mino_type_of(fut) != MINO_FUTURE) {
        return mino_nil(S);
    }
    impl = fut->as.future.impl;
    if (impl == NULL) { return mino_nil(S); }

    /* Fast path: if already done, skip the cv dance entirely. */
    {
        int depth = mino_yield_lock(S);
        mu_lock(&impl->mu);
        if (impl->state_tag != MINO_FUTURE_PENDING) {
            mu_unlock(&impl->mu);
            mino_resume_lock(S, depth);
            return mino_future_deref(S, fut);
        }

        /* Wait up to ms milliseconds. Spurious wake re-checks the
         * deadline by computing the remaining budget. */
        {
            long long start_ns = mino_monotonic_ns();
            long      remain   = (ms < 0) ? 0 : ms;
            for (;;) {
                int signalled;
                if (impl->state_tag != MINO_FUTURE_PENDING) { break; }
                if (remain <= 0) {
                    timed_out = 1;
                    break;
                }
                signalled = cv_timedwait_ms(&impl->cv, &impl->mu, remain);
                if (impl->state_tag != MINO_FUTURE_PENDING) { break; }
                if (!signalled) {
                    timed_out = 1;
                    break;
                }
                /* Spurious wake: recompute remaining budget. */
                {
                    long long now_ns   = mino_monotonic_ns();
                    long long used_ms  = (now_ns - start_ns) / 1000000LL;
                    remain = (long)((long long)ms - used_ms);
                }
            }
        }
        out = (impl->state_tag == MINO_FUTURE_RESOLVED) ? impl->result : NULL;
        mu_unlock(&impl->mu);
        mino_resume_lock(S, depth);
    }

    if (timed_out) {
        return timeout_val;
    }
    if (out != NULL) {
        return out;
    }
    /* Finalised state without a result: delegate to the standard
     * deref so the FAILED / CANCELLED rethrow paths stay in one
     * place. The future has already been observed as non-PENDING, so
     * mino_future_deref's loop will fall through immediately. */
    return mino_future_deref(S, fut);
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
void mino_host_threads_quiesce(mino_state *S)
{
    mino_future *impl;
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
    impl = S->threading.future_list_head;
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
    impl = S->threading.future_list_head;
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

/* Lock invariant: called from the GC sweep phase, which itself runs
 * under the GC's own serialization (no concurrent mutator). State_lock
 * is not taken here — instead we rely on the GC suppression while
 * thread_count > 0 invariant: any worker for this future has already
 * decremented thread_count and is past its last lock acquire, so a
 * pthread_join here is safe and never blocks indefinitely. impl->mu is
 * destroyed only after the join. */

void mino_future_trace_impl(mino_state *S, const mino_val *fut)
{
    mino_future *impl;
    if (fut == NULL || mino_type_of(fut) != MINO_FUTURE) return;
    impl = fut->as.future.impl;
    if (impl == NULL) return;
    gc_mark_child_push_exported(S, impl->result);
    gc_mark_child_push_exported(S, impl->exception);
    gc_mark_child_push_exported(S, impl->thunk);
    gc_mark_child_push_exported(S, impl->body_env);
    gc_mark_child_push_exported(S, impl->dyn_snapshot);
}

void mino_future_gc_sweep(mino_val *fut)
{
    mino_future *impl;
    mino_state *S;
    if (fut == NULL || mino_type_of(fut) != MINO_FUTURE) { return; }
    impl = fut->as.future.impl;
    if (impl == NULL) { return; }

    /* Join the worker thread before destroying mu/cv -- otherwise the
     * worker's final mu_unlock would touch destroyed mutex memory.
     *
     * Lock-yield discipline: the worker thread needs state_lock to
     * make progress through its body / through worker_entry's exit
     * sequence (the thread_count decrement and ctx detach run under
     * worker_list_lock, but the body itself ran inside mino_call
     * which holds state_lock). If our caller -- a sweep nested inside
     * mino_gc_collect via (gc!) -- still holds state_lock recursively
     * we deadlock: worker is blocked acquiring state_lock; we are
     * blocked in pthread_join waiting for the worker to finish.
     *
     * Drop state_lock for the join, then resume the same depth after
     * the worker is reaped. mino_future_deref uses the same pattern
     * for cv_wait; this brings sweep into alignment.
     *
     * Background: gc_tick_should_suppress used to gate the collector
     * out while thread_count > 0, so this hang couldn't surface from
     * auto-tick. mino_gc_collect (the public (gc!) API) skips that
     * suppression and runs sweep even with live workers -- which is
     * how transient-survives-gc-yield exercises the path. */
    S = impl->state;
    if (impl->thread_started && !impl->thread_joined) {
        int depth = 0;
        if (S != NULL) depth = mino_yield_lock(S);
#if defined(_WIN32) && defined(_MSC_VER)
        WaitForSingleObject(impl->thread, INFINITE);
        CloseHandle(impl->thread);
#else
        pthread_join(impl->thread, NULL);
#endif
        impl->thread_joined = 1;
        if (S != NULL) mino_resume_lock(S, depth);
    }

    /* Detach from S->threading.future_list_head before freeing so quiesce
     * never walks into a dangling impl. */
    if (S != NULL) {
        mino_future **pp = &S->threading.future_list_head;
        while (*pp != NULL && *pp != impl) { pp = &(*pp)->next_in_state; }
        if (*pp == impl) { *pp = impl->next_in_state; }
    }

    cv_destroy(&impl->cv);
    mu_destroy(&impl->mu);
    free(impl);
    fut->as.future.impl = NULL;
}
