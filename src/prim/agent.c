/*
 * agent.c -- agents (asynchronous mutable cells with serialized actions).
 *
 * Execution model: send / send-off enqueue (fn args) onto a per-state
 * FIFO and return the agent immediately. Two pools live alongside each
 * other -- POOLED (target of `send`) and SOLO (target of `send-off`)
 * -- each with its own queue and worker thread. Each worker drains
 * its pool's FIFO, acquires state_lock, runs the action under the
 * agent's validator + watches via agent_apply_action, and signals
 * await waiters when the agent's in-flight count reaches zero.
 *
 * Threading contract:
 *
 *  - Each pool's worker counts against S->threading.thread_limit (the embedder
 *    thread does NOT). Default thread_limit is 1, so a host that
 *    hasn't called mino_set_thread_limit can have one agent worker
 *    OR one future OR one host thread alive at a time. send /
 *    send-off throw MTH001 if the host hasn't granted enough thread
 *    budget to spawn the requested pool's worker. Embedders that
 *    want both POOLED and SOLO alive concurrently must raise the
 *    limit to >= 2; mixing with futures / host threads requires
 *    correspondingly more. Standalone `./mino` raises thread_limit
 *    to cpu_count after install_all so the REPL works out of the box.
 *
 *  - Each worker is lazy-spawned on the first send/send-off into its
 *    pool. It exits when its queue is drained so a long-idle worker
 *    doesn't keep S->threading.thread_count > 0 and suppress GC for the rest
 *    of the state's lifetime; the next send into that pool re-spawns.
 *    shutdown-agents and mino_state_free quiesce both pools.
 *
 *  - Lock order: state_lock outer, S->agent.mu inner. The send caller
 *    holds state_lock while it enqueues under agent_mu. The worker
 *    takes agent_mu to pop, releases agent_mu, then acquires
 *    state_lock to run the action; no nested hold across the action
 *    body. await callers yield state_lock before blocking on
 *    agent_cv so the workers can run.
 *
 *  - mino's per-state eval lock means actions on multiple agents in
 *    one state still run one at a time, even across the two pools.
 *    The split is architectural: it preserves enqueue-order within
 *    each shape (a long send-off action queue does not delay sends),
 *    and gives a clean seam for a future SOLO-yields-eval-lock-during-
 *    blocking-IO design without further user-facing churn.
 *
 *  - Per-action *agent* and conveyed dynamic bindings are pushed onto
 *    the worker's dyn_stack before the action body runs, mirroring
 *    JVM Clojure's binding conveyance for futures.
 *
 *  - If an agent fails (action or validator throw) and its error mode
 *    is :fail, send throws on the caller. Actions queued before the
 *    failure that haven't run yet are dropped silently when the
 *    worker pops them and observes err != NULL: mino's single-worker
 *    model can't park them per agent without starving the others.
 *    With error mode :continue, the worker runs every queued action
 *    regardless of intermediate failures, matching JVM canon.
 */

#include "prim/internal.h"
#include "mino.h"
#include "eval/internal.h"
#include "runtime/host_threads.h"

#include <limits.h>
#include <string.h>

#if defined(_WIN32) && defined(_MSC_VER)
#  include <windows.h>
#  include <process.h>
#else
#  include <pthread.h>
#  include <time.h>
#endif

/* --- agent_mu / agent_cv portability ------------------------------------- */

#if defined(_WIN32) && defined(_MSC_VER)
static void agent_mu_init(CRITICAL_SECTION *m)    { InitializeCriticalSection(m); }
static void agent_mu_lock(CRITICAL_SECTION *m)    { EnterCriticalSection(m); }
static void agent_mu_unlock(CRITICAL_SECTION *m)  { LeaveCriticalSection(m); }
static void agent_cv_init(CONDITION_VARIABLE *c)  { InitializeConditionVariable(c); }
static void agent_cv_wait(CONDITION_VARIABLE *c, CRITICAL_SECTION *m)
{ SleepConditionVariableCS(c, m, INFINITE); }
static void agent_cv_broadcast(CONDITION_VARIABLE *c) { WakeAllConditionVariable(c); }
#else
static void agent_mu_init(pthread_mutex_t *m)     { pthread_mutex_init(m, NULL); }
static void agent_mu_lock(pthread_mutex_t *m)     { pthread_mutex_lock(m); }
static void agent_mu_unlock(pthread_mutex_t *m)   { pthread_mutex_unlock(m); }
static void agent_cv_init(pthread_cond_t *c)      { pthread_cond_init(c, NULL); }
static void agent_cv_wait(pthread_cond_t *c, pthread_mutex_t *m)
{ pthread_cond_wait(c, m); }
static void agent_cv_broadcast(pthread_cond_t *c) { pthread_cond_broadcast(c); }
#endif

/* Lazy-init agent_mu/cv. Called under state_lock (so the inited flag
 * read is race-free against concurrent sends from other threads). */
static void agent_mu_ensure_inited(mino_state *S)
{
    if (!S->agent.mu_inited) {
        agent_mu_init(&S->agent.mu);
        agent_cv_init(&S->agent.cv);
        S->agent.mu_inited = 1;
    }
}

/* Forward declarations for the C-API perimeter (defined further down
 * with the prim handlers and the shared core helpers). */
static int          agent_check_state(mino_state *S, mino_val *agent);
static mino_val  *agent_send_core(mino_state *S, agent_pool_kind_t kind,
                                     mino_val *agent, mino_val *fn,
                                     mino_val *extra, mino_env *env,
                                     const char *prim_name);
static mino_val  *agent_array_to_list(mino_state *S, mino_val **agents);
mino_val         *prim_await(mino_state *S, mino_val *args,
                                mino_env *env);
mino_val         *prim_await_for(mino_state *S, mino_val *args,
                                    mino_env *env);
mino_val         *prim_restart_agent(mino_state *S, mino_val *args,
                                        mino_env *env);

/* --- public-API constructor + predicate ----------------------------------- */

mino_val *mino_agent(mino_state *S, mino_val *initial)
{
    mino_val *v = alloc_val(S, MINO_AGENT);
    v->as.agent.val          = initial;
    v->as.agent.watches      = NULL;
    v->as.agent.validator    = NULL;
    v->as.agent.err          = NULL;
    v->as.agent.err_handler  = NULL;
    v->as.agent.err_mode     = 0;  /* :fail */
    v->as.agent.in_flight    = 0;
    v->as.agent.agent_id     = ++S->agent.next_id;
    v->as.agent.owning_state = S;
    return v;
}

int mino_is_agent(const mino_val *v)
{
    return v != NULL && mino_type_of(v) == MINO_AGENT;
}

/* Convert a NULL-terminated array of agents to a cons list, matching
 * the shape that prim_await / prim_await_for already consume. Returns
 * an empty list when agents is NULL or its first slot is NULL.
 * Caller must already hold state_lock so cons allocations are safe. */
static mino_val *agent_array_to_list(mino_state *S, mino_val **agents)
{
    mino_val *head;
    size_t      i, n;
    if (agents == NULL) return mino_empty_list(S);
    n = 0;
    while (agents[n] != NULL) n++;
    head = mino_empty_list(S);
    for (i = n; i > 0; i--) {
        head = mino_cons(S, agents[i - 1], head);
    }
    return head;
}

/* Cross-state defense, mirroring tx_check_ref_owned. Throws MST007
 * if the agent was allocated in a different state than S, so a host
 * that injects a foreign agent through mino_env_set / mino_call
 * cannot tunnel mutations into another state's heap. Returns 0 on
 * match, 1 on mismatch (caller should propagate NULL). */
static int agent_check_state(mino_state *S, mino_val *agent)
{
    if (agent->as.agent.owning_state != S) {
        prim_throw_classified(S, "eval/state", "MST007",
            "agent from foreign state");
        return 1;
    }
    return 0;
}

/* --- action dispatch ------------------------------------------------------ */

/* Build (cur arg1 arg2 ...) for invoking an action fn. */
static mino_val *agent_build_call(mino_state *S, mino_val *cur,
                                     mino_val *extra)
{
    mino_val *head = mino_cons(S, cur, mino_nil(S));
    mino_val *cell = head;
    while (mino_is_cons(extra)) {
        mino_val *next = mino_cons(S, extra->as.cons.car, mino_nil(S));
        cell->as.cons.cdr = next;
        cell = next;
        extra = extra->as.cons.cdr;
    }
    return head;
}

/* Report an action failure. JVM canon: if the agent has an
 * error-handler installed, invoke (handler agent ex) and leave the
 * agent's err slot clean -- the handler is responsible for any
 * latching it wants. If the handler itself throws, capture its
 * thrown payload into err so the failure isn't silently lost. With
 * no handler, latch ex into err directly. */
static void agent_report_failure(mino_state *S, mino_val *agent,
                                  mino_val *ex, mino_env *env)
{
    mino_val *handler = agent->as.agent.err_handler;
    mino_val *result  = NULL;
    mino_val *hthrown = NULL;
    int         pc;
    if (handler == NULL) {
        gc_write_barrier(S, agent, agent->as.agent.err, ex);
        agent->as.agent.err = ex;
        return;
    }
    {
        mino_val *args = mino_cons(S, agent,
                              mino_cons(S, ex, mino_nil(S)));
        pc = mino_pcall(S, handler, args, env, &result, &hthrown);
    }
    if (pc != 0 && hthrown != NULL) {
        gc_write_barrier(S, agent, agent->as.agent.err, hthrown);
        agent->as.agent.err = hthrown;
    }
}

/* Apply one action: run validator, update state, fire watches.
 * Each user-callback invocation goes through mino_pcall so a throw
 * is captured into the agent's err slot rather than propagated to
 * the worker loop.
 *
 * The agent is bound to the `*agent*` dynamic var across the action
 * invocation, matching JVM canon (`(send a (fn [v] (= *agent* a)))`
 * is true). The dyn_frame is stack-allocated; mino_pcall catches
 * throws within itself so the push/pop bracket cannot be skipped by
 * a longjmp escaping past us. */
static void agent_apply_action(mino_state *S, mino_val *agent,
                                mino_val *fn, mino_val *extra,
                                mino_env *env)
{
    mino_val        *call_args = agent_build_call(S, agent->as.agent.val,
                                                     extra);
    mino_val        *new_state = NULL;
    mino_val        *old_state = agent->as.agent.val;
    mino_val        *thrown_ex = NULL;
    mino_thread_ctx_t *ctx       = mino_current_ctx(S);
    dyn_binding_t      ag_bind;
    dyn_frame_t        ag_frame;
    int                pc;

    ag_bind.name  = "*agent*";
    ag_bind.val   = agent;
    ag_bind.next  = NULL;
    ag_frame.bindings = &ag_bind;
    ag_frame.prev     = ctx->dyn_stack;
    ctx->dyn_stack    = &ag_frame;

    pc = mino_pcall(S, fn, call_args, env, &new_state, &thrown_ex);
    if (pc != 0 || new_state == NULL) {
        agent_report_failure(S, agent, thrown_ex, env);
        goto out;
    }

    /* Validator: rejects bypass the publish + watch dispatch. */
    if (agent->as.agent.validator != NULL) {
        mino_val *vargs = mino_cons(S, new_state, mino_nil(S));
        mino_val *vresult = NULL;
        pc = mino_pcall(S, agent->as.agent.validator, vargs, env,
                          &vresult, &thrown_ex);
        if (pc != 0 || vresult == NULL || !mino_is_truthy(vresult)) {
            mino_val *ex = thrown_ex;
            if (ex == NULL) {
                /* Validator returned falsy without throwing: synthesize. */
                ex = mino_string(S, "Invalid reference state");
            }
            agent_report_failure(S, agent, ex, env);
            goto out;
        }
    }

    gc_write_barrier(S, agent, agent->as.agent.val, new_state);
    agent->as.agent.val = new_state;

    /* Watches: each invocation is wrapped in mino_pcall so a thrown
     * watch sets agent.err but does not abort dispatch of later
     * watches. JVM's behavior on watch throws is implementation-
     * defined; this matches the test-add-watch agent arm which
     * expects the watch's thrown payload to surface via
     * agent-error. */
    if (agent->as.agent.watches != NULL
        && mino_type_of(agent->as.agent.watches) == MINO_MAP
        && agent->as.agent.watches->as.map.len > 0) {
        mino_val *watches = agent->as.agent.watches;
        size_t      n = watches->as.map.len;
        size_t      i;
        for (i = 0; i < n; i++) {
            mino_val *key = vec_nth(watches->as.map.key_order, i);
            mino_val *wfn = map_get_val(watches, key);
            mino_val *wargs;
            mino_val *wresult = NULL;
            mino_val *wthrown = NULL;
            if (wfn == NULL) continue;
            wargs = mino_cons(S, key,
                      mino_cons(S, agent,
                        mino_cons(S, old_state,
                          mino_cons(S, new_state, mino_nil(S)))));
            pc = mino_pcall(S, wfn, wargs, env, &wresult, &wthrown);
            if (pc != 0 && wthrown != NULL) {
                /* Capture the watch's thrown payload into agent.err.
                 * Continue dispatching remaining watches: a thrown
                 * watch shouldn't silence later watches on the same
                 * publish. */
                gc_write_barrier(S, agent, agent->as.agent.err, wthrown);
                agent->as.agent.err = wthrown;
            }
        }
    }

out:
    /* Pop *agent* binding. mino_pcall caught any throws so this
     * runs regardless of which branch above we took. */
    ctx->dyn_stack = ag_frame.prev;
}

/* --- worker thread + queue dispatch --------------------------------------- */

/* Pop the head node of S->agent.pool[kind].run_head. Caller must hold agent_mu. */
static agent_action_node_t *agent_runq_pop(mino_state *S,
                                            agent_pool_kind_t kind)
{
    agent_action_node_t *n = S->agent.pool[kind].run_head;
    if (n == NULL) return NULL;
    S->agent.pool[kind].run_head = n->next;
    if (S->agent.pool[kind].run_head == NULL) {
        S->agent.pool[kind].run_tail = NULL;
    }
    n->next = NULL;
    return n;
}

/* Append node to the tail of S->agent.pool[kind].run_head. Caller must
 * hold agent_mu. */
static void agent_runq_push_tail(mino_state *S, agent_pool_kind_t kind,
                                  agent_action_node_t *n)
{
    n->next = NULL;
    if (S->agent.pool[kind].run_tail != NULL) {
        S->agent.pool[kind].run_tail->next = n;
    } else {
        S->agent.pool[kind].run_head = n;
    }
    S->agent.pool[kind].run_tail = n;
}

/* Run one queued action on the worker thread. Holds state_lock for
 * the duration of the action body. Conveys the dyn-binding snapshot
 * captured at send time as a single dyn_frame so the action sees the
 * caller's binding context (matches future / promise dyn conveyance).
 *
 * Skip-on-failed: if the agent has err != NULL && err_mode == :fail,
 * drop the action silently. mino's single-worker model can't park
 * per-agent without starving other agents; users get a loud send-
 * time throw before the queue gets here, so the only way to land in
 * this branch is for an earlier action to have failed the agent
 * mid-queue. Document this as a deviation. */
static void agent_worker_run_one(mino_state *S, agent_action_node_t *n)
{
    mino_thread_ctx_t *ctx = mino_current_ctx(S);
    dyn_frame_t       *conveyed = NULL;
    dyn_binding_t     *bhead = NULL;

    /* Build conveyed dyn-binding chain from the caller's snapshot map. */
    if (n->dyn_snap != NULL && mino_type_of(n->dyn_snap) == MINO_MAP
        && n->dyn_snap->as.map.len > 0) {
        size_t i;
        int    oom = 0;
        for (i = 0; i < n->dyn_snap->as.map.len; i++) {
            mino_val    *key = vec_nth(n->dyn_snap->as.map.key_order, i);
            mino_val    *val = map_get_val(n->dyn_snap, key);
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
            if (conveyed == NULL) { dyn_binding_list_free(bhead); bhead = NULL; }
            else {
                conveyed->bindings = bhead;
                conveyed->prev     = ctx->dyn_stack;
                ctx->dyn_stack     = conveyed;
            }
        } else {
            dyn_binding_list_free(bhead);
            bhead = NULL;
        }
    }

    /* Failed-:fail short-circuit: skip silently. */
    if (n->agent->as.agent.err != NULL
        && n->agent->as.agent.err_mode == 0) {
        goto cleanup;
    }
    agent_apply_action(S, n->agent, n->fn, n->extra, n->env);

cleanup:
    if (conveyed != NULL) {
        ctx->dyn_stack = conveyed->prev;
        dyn_binding_list_free(conveyed->bindings);
        free(conveyed);
    }
}

/* Worker entry. Pops actions from the pool's runq, running each under
 * state_lock. Exits as soon as the queue drains (or when shutdown
 * is set) so a long-idle agent worker doesn't keep thread_count > 0
 * and suppress GC for the rest of the state's lifetime. The next
 * send re-spawns; the spawn cost is paid only by burst transitions.
 *
 * Lock acquisition order: state_lock outer, agent_mu inner. The
 * worker takes agent_mu to pop, releases it before taking
 * state_lock to run, retakes agent_mu to decrement in_flight and
 * broadcast. Awaiters yield state_lock before blocking on agent_cv,
 * so they don't block the worker. */
static void agent_worker_run(mino_state *S, agent_pool_kind_t kind,
                              char *stack_anchor)
{
    mino_thread_ctx_t *ctx;

    ctx = (mino_thread_ctx_t *)calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        /* Best-effort: without a ctx we can't run any action. Mark
         * the worker as gone so the next send re-spawns; pending
         * nodes leak (memory is reclaimed by mino_state_free's
         * drain). */
        agent_mu_lock(&S->agent.mu);
        S->agent.pool[kind].worker_alive = 0;
        agent_cv_broadcast(&S->agent.cv);
        agent_mu_unlock(&S->agent.mu);
        return;
    }
    ctx->gc_stack_bottom = (void *)stack_anchor;
    mino_tls_ctx = ctx;

    /* Link onto S->threading.worker_ctxs_head so gc_mark_roots reaches the
     * worker's pinned values while it's blocked. The brief
     * worker_list_lock keeps this off the heavy state_lock so a
     * tight embedder loop can't stall agent worker entry. */
    mino_worker_list_lock_acquire(S);
    ctx->next_worker = S->threading.worker_ctxs_head;
    S->threading.worker_ctxs_head = ctx;
    mino_worker_list_lock_release(S);

    for (;;) {
        agent_action_node_t *n;

        agent_mu_lock(&S->agent.mu);
        if (S->agent.pool[kind].run_head == NULL) {
            /* Queue drained or shutdown: exit the worker so it
             * stops counting against thread_count. The next send
             * into this pool re-spawns. */
            S->agent.pool[kind].worker_alive = 0;
            agent_cv_broadcast(&S->agent.cv);
            agent_mu_unlock(&S->agent.mu);
            break;
        }
        n = agent_runq_pop(S, kind);
        agent_mu_unlock(&S->agent.mu);

        /* Run the action under state_lock so eval invariants hold.
         * Use mino_lock here rather than the raw state_lock_acquire so
         * lock_depth tracks the recursion: the convey-binding build
         * inside agent_worker_run_one interns symbols, and the intern
         * table's caller-must-hold-lock assert checks lock_depth, not
         * the underlying mutex. */
        mino_lock(S);
        agent_worker_run_one(S, n);
        mino_unlock(S);

        /* Decrement the agent's in_flight and signal await waiters. */
        agent_mu_lock(&S->agent.mu);
        if (n->agent->as.agent.in_flight > 0) {
            n->agent->as.agent.in_flight--;
        }
        agent_cv_broadcast(&S->agent.cv);
        agent_mu_unlock(&S->agent.mu);

        free(n);
    }

    /* Detach worker ctx from S->threading.worker_ctxs_head and decrement the
     * thread count under the brief worker_list_lock. The pthread_t
     * is left intact for a follow-up pthread_join from a later
     * agent_enqueue or mino_agent_quiesce_workers (signaled by
     * worker_pending_join, which the spawn path set when it created
     * us). Joining an already-exited joinable pthread returns
     * immediately. */
    mino_worker_list_lock_acquire(S);
    {
        mino_thread_ctx_t **pp = &S->threading.worker_ctxs_head;
        while (*pp != NULL && *pp != ctx) { pp = &(*pp)->next_worker; }
        if (*pp == ctx) { *pp = ctx->next_worker; }
    }
    if (S->threading.thread_count > 0) { S->threading.thread_count--; }
    mino_worker_list_lock_release(S);

    mino_tls_ctx = NULL;
    free(ctx);
}

/* Per-pool entry stubs. pthread_create takes one void*; encoding the
 * pool kind in two static stubs avoids a tiny malloc on every spawn
 * and keeps the spawn path branch-free. */
#if defined(_WIN32) && defined(_MSC_VER)
static unsigned __stdcall agent_worker_entry_pooled(void *arg)
{
    char anchor;
    agent_worker_run((mino_state *)arg, AGENT_POOL_POOLED, &anchor);
    return 0;
}
static unsigned __stdcall agent_worker_entry_solo(void *arg)
{
    char anchor;
    agent_worker_run((mino_state *)arg, AGENT_POOL_SOLO, &anchor);
    return 0;
}
#else
static void *agent_worker_entry_pooled(void *arg)
{
    char anchor;
    agent_worker_run((mino_state *)arg, AGENT_POOL_POOLED, &anchor);
    return NULL;
}
static void *agent_worker_entry_solo(void *arg)
{
    char anchor;
    agent_worker_run((mino_state *)arg, AGENT_POOL_SOLO, &anchor);
    return NULL;
}
#endif

/* Reap a previously-exited (drain-exit) worker for the given pool so
 * its pthread handle can be released before the next spawn. Caller
 * must hold state_lock; the join itself yields the lock so the
 * exiting worker can finish its ctx detach + thread_count decrement. */
static void agent_worker_reap_pending(mino_state *S, agent_pool_kind_t kind)
{
    int saved_depth;
    if (!S->agent.pool[kind].worker_pending_join) return;
    S->agent.pool[kind].worker_pending_join = 0;
    saved_depth = mino_yield_lock(S);
#if defined(_WIN32) && defined(_MSC_VER)
    WaitForSingleObject(S->agent.pool[kind].worker, INFINITE);
    CloseHandle(S->agent.pool[kind].worker);
#else
    pthread_join(S->agent.pool[kind].worker, NULL);
#endif
    mino_resume_lock(S, saved_depth);
}

/* Enqueue (agent fn extra env dyn_snap) onto the named pool's queue.
 * Caller holds state_lock. Spawns the pool's worker if needed and
 * enqueues the action under a single agent_mu critical section so a
 * newly-spawned worker cannot observe an empty runq before this
 * producer publishes its node.
 *
 * Race that this closes: if ensure released agent_mu before
 * agent_enqueue re-acquired it, a freshly-spawned worker could
 * sneak in between the two acquisitions, see an empty runq, set
 * worker_alive=0, and exit. The subsequent enqueue would then push
 * the node + bump in_flight into a runq with no consumer, and
 * `(await a)` blocks forever waiting on in_flight=0. Reproduces on
 * GHA ubuntu-24.04 (both arches), masked on macos-14 + Apple Silicon
 * Docker by stricter effective memory ordering.
 *
 * Returns 0 on success, 1 on OOM or thread-budget refusal (caller
 * propagates NULL). On 1, the diag has been published via
 * prim_throw_classified and no observable state change happened
 * (no enqueue, no in_flight bump, no pthread spawned). */
static int agent_enqueue(mino_state *S, agent_pool_kind_t kind,
                          mino_val *agent, mino_val *fn,
                          mino_val *extra, mino_env *env)
{
    agent_action_node_t *n;
    int                  need_spawn = 0;
    int                  budget_ok  = 1;

    if (!S->agent.mu_inited) {
        agent_mu_ensure_inited(S);
    }

    n = (agent_action_node_t *)calloc(1, sizeof(*n));
    if (n == NULL) return 1;
    n->agent    = agent;
    n->fn       = fn;
    n->extra    = extra;
    n->env      = env;
    n->dyn_snap = mino_snapshot_thread_bindings(S);
    n->next     = NULL;

    /* Decide whether a new worker is needed, take the thread-budget
     * slot, and publish the node + in_flight bump as one atomic
     * critical section. The worker we are about to spawn (if any)
     * cannot acquire agent_mu until we release, so its first runq
     * check is guaranteed to see this node. */
    agent_mu_lock(&S->agent.mu);
    if (!S->agent.pool[kind].worker_alive) {
        need_spawn = 1;
        /* Reap a prior worker's pthread handle synchronously here
         * rather than at the spawn step below, so the budget check
         * sees the up-to-date thread_count. The previous worker
         * exited via the for-loop break and decremented thread_count
         * during its detach; its pthread_t still lives in the slot
         * until we join it. */
        if (S->agent.pool[kind].worker_pending_join) {
            S->agent.pool[kind].worker_pending_join = 0;
            agent_mu_unlock(&S->agent.mu);
            {
                int saved_depth = mino_yield_lock(S);
#if defined(_WIN32) && defined(_MSC_VER)
                WaitForSingleObject(S->agent.pool[kind].worker, INFINITE);
                CloseHandle(S->agent.pool[kind].worker);
#else
                pthread_join(S->agent.pool[kind].worker, NULL);
#endif
                mino_resume_lock(S, saved_depth);
            }
            agent_mu_lock(&S->agent.mu);
            /* While the lock was yielded a concurrent producer could
             * have re-spawned the worker. Re-check; if alive again,
             * we no longer need to spawn. */
            if (S->agent.pool[kind].worker_alive) {
                need_spawn = 0;
            }
        }
        if (need_spawn) {
            mino_worker_list_lock_acquire(S);
            if (S->threading.thread_count >= S->threading.thread_limit) {
                mino_worker_list_lock_release(S);
                budget_ok = 0;
            } else {
                S->threading.thread_count++;
                mino_worker_list_lock_release(S);
                S->threading.multi_threaded = 1;
                S->agent.pool[kind].worker_alive = 1;
            }
        }
    }
    if (!budget_ok) {
        agent_mu_unlock(&S->agent.mu);
        free(n);
        prim_throw_classified(S, "mino/thread-limit-exceeded", "MTH001",
            "agent dispatch requires a host-granted worker thread; "
            "raise via mino_set_thread_limit (>= 1 for one agent "
            "worker; >= 2 if both send and send-off are used "
            "concurrently). The embedder thread does not count "
            "against the limit -- only spawned workers do.");
        return 1;
    }
    agent_runq_push_tail(S, kind, n);
    if (agent->as.agent.in_flight < INT_MAX) {
        agent->as.agent.in_flight++;
    }
    agent_cv_broadcast(&S->agent.cv);
    agent_mu_unlock(&S->agent.mu);

    /* Spawn the worker AFTER the node is published. The new pthread
     * will block on agent_mu_lock at its first iteration if we are
     * still holding (we aren't anymore) and immediately observe the
     * node we just pushed. */
    if (need_spawn) {
#if defined(_WIN32) && defined(_MSC_VER)
        unsigned stack = (unsigned)S->threading.thread_stack_size;
        uintptr_t h = _beginthreadex(NULL, stack,
            kind == AGENT_POOL_POOLED ? agent_worker_entry_pooled
                                       : agent_worker_entry_solo,
            S, 0, NULL);
        if (h == 0) {
            /* Spawn refused. Pop our own node + decrement in_flight
             * for this agent so the failed send doesn't strand its
             * own await. A concurrent producer may have enqueued
             * their own node into this pool during the spawn window
             * (they saw worker_alive=1 and skipped spawning); if
             * the runq is non-empty after we pop ours, retry the
             * spawn so their action still runs. Only reset
             * worker_alive=0 + decrement thread_count when the
             * runq has nothing left to drain OR the retry also
             * fails. */
            int retry_ok = 0;
            agent_mu_lock(&S->agent.mu);
            if (S->agent.pool[kind].run_tail == n) {
                S->agent.pool[kind].run_tail = NULL;
            }
            S->agent.pool[kind].run_head = n->next;
            if (agent->as.agent.in_flight > 0) {
                agent->as.agent.in_flight--;
            }
            if (S->agent.pool[kind].run_head != NULL) {
                /* Concurrent producer's node is still queued.
                 * Drop the agent mu around the syscall so we don't
                 * stall pop attempts; reacquire to commit the
                 * outcome. */
                agent_mu_unlock(&S->agent.mu);
                {
                    uintptr_t h2 = _beginthreadex(NULL, stack,
                        kind == AGENT_POOL_POOLED ? agent_worker_entry_pooled
                                                   : agent_worker_entry_solo,
                        S, 0, NULL);
                    if (h2 != 0) {
                        S->agent.pool[kind].worker = (HANDLE)h2;
                        retry_ok = 1;
                    }
                }
                agent_mu_lock(&S->agent.mu);
            }
            if (retry_ok) {
                S->agent.pool[kind].worker_pending_join = 1;
                agent_cv_broadcast(&S->agent.cv);
                agent_mu_unlock(&S->agent.mu);
                free(n);
                prim_throw_classified(S, "mino/thread-limit-exceeded", "MTH001",
                    "host refused agent worker thread for this send; "
                    "a concurrent producer's queued action is being "
                    "drained by a retried worker");
                return 1;
            }
            S->agent.pool[kind].worker_alive = 0;
            agent_cv_broadcast(&S->agent.cv);
            agent_mu_unlock(&S->agent.mu);
            mino_worker_list_lock_acquire(S);
            if (S->threading.thread_count > 0) { S->threading.thread_count--; }
            mino_worker_list_lock_release(S);
            free(n);
            prim_throw_classified(S, "mino/thread-limit-exceeded", "MTH001",
                "host refused agent worker thread");
            return 1;
        }
        S->agent.pool[kind].worker = (HANDLE)h;
#else
        pthread_attr_t attr;
        pthread_attr_t *attrp = NULL;
        int rc;
        if (S->threading.thread_stack_size > 0) {
            if (pthread_attr_init(&attr) == 0) {
                pthread_attr_setstacksize(&attr, S->threading.thread_stack_size);
                attrp = &attr;
            }
        }
        rc = pthread_create(&S->agent.pool[kind].worker, attrp,
            kind == AGENT_POOL_POOLED ? agent_worker_entry_pooled
                                       : agent_worker_entry_solo,
            S);
        if (attrp != NULL) { pthread_attr_destroy(attrp); }
        if (rc != 0) {
            /* Mirror the Windows-arm rollback: pop our own node,
             * decrement in_flight, and retry the spawn once if a
             * concurrent producer has already queued an action that
             * would otherwise be stranded behind a worker that never
             * starts. */
            int retry_ok = 0;
            agent_mu_lock(&S->agent.mu);
            if (S->agent.pool[kind].run_tail == n) {
                S->agent.pool[kind].run_tail = NULL;
            }
            S->agent.pool[kind].run_head = n->next;
            if (agent->as.agent.in_flight > 0) {
                agent->as.agent.in_flight--;
            }
            if (S->agent.pool[kind].run_head != NULL) {
                agent_mu_unlock(&S->agent.mu);
                {
                    pthread_attr_t attr2;
                    pthread_attr_t *attrp2 = NULL;
                    int rc2;
                    if (S->threading.thread_stack_size > 0
                        && pthread_attr_init(&attr2) == 0) {
                        pthread_attr_setstacksize(&attr2,
                            S->threading.thread_stack_size);
                        attrp2 = &attr2;
                    }
                    rc2 = pthread_create(&S->agent.pool[kind].worker,
                        attrp2,
                        kind == AGENT_POOL_POOLED ? agent_worker_entry_pooled
                                                   : agent_worker_entry_solo,
                        S);
                    if (attrp2 != NULL) { pthread_attr_destroy(attrp2); }
                    if (rc2 == 0) retry_ok = 1;
                }
                agent_mu_lock(&S->agent.mu);
            }
            if (retry_ok) {
                S->agent.pool[kind].worker_pending_join = 1;
                agent_cv_broadcast(&S->agent.cv);
                agent_mu_unlock(&S->agent.mu);
                free(n);
                prim_throw_classified(S, "mino/thread-limit-exceeded", "MTH001",
                    "host refused agent worker thread for this send; "
                    "a concurrent producer's queued action is being "
                    "drained by a retried worker");
                return 1;
            }
            S->agent.pool[kind].worker_alive = 0;
            agent_cv_broadcast(&S->agent.cv);
            agent_mu_unlock(&S->agent.mu);
            mino_worker_list_lock_acquire(S);
            if (S->threading.thread_count > 0) { S->threading.thread_count--; }
            mino_worker_list_lock_release(S);
            free(n);
            prim_throw_classified(S, "mino/thread-limit-exceeded", "MTH001",
                "host refused agent worker thread");
            return 1;
        }
#endif
        S->agent.pool[kind].worker_pending_join = 1;
    }
    return 0;
}

/* Public quiesce hook: flip agents_shutdown so live workers exit
 * after draining their queues, then reap any pending pthread
 * handles. Idempotent. Called by prim_shutdown_agents and by
 * mino_state_free during teardown.
 *
 * Self-join detection: if the calling thread IS one of the workers,
 * we cannot join ourselves. prim_shutdown_agents catches that case
 * and throws; this helper is a no-op when invoked in that
 * pathological state since each pool's worker_pending_join is set
 * only by the spawn path on a different thread. */
void mino_agent_quiesce_workers(mino_state *S)
{
    int pi;
    /* Flag shutdown + wake workers so they can drain + exit. */
    if (S->agent.mu_inited) {
        agent_mu_lock(&S->agent.mu);
        S->agent.shutdown = 1;
        agent_cv_broadcast(&S->agent.cv);
        agent_mu_unlock(&S->agent.mu);
    } else {
        S->agent.shutdown = 1;
    }

    for (pi = 0; pi < AGENT_POOL_COUNT; pi++) {
        agent_worker_reap_pending(S, (agent_pool_kind_t)pi);
    }
}

/* --- public C-API for embedders ------------------------------------------ *
 *
 * Each entry is a thin perimeter around the same core helpers the
 * Clojure-level primitives use, with the embedder mino_lock /
 * mino_unlock perimeter that mino_call / mino_eval_string already
 * use. The cross-state guard catches a host that mistakenly hands an
 * agent from another mino_state into S.
 *
 * The action env passed at queue time is NULL; closures bring their
 * own captured env via the closure value, and unbound symbols would
 * resolve through the agent's owning state's namespace fallbacks
 * regardless of what env we pass here. Embedders that need a custom
 * env for symbol resolution within an action should hand in a
 * closure built from mino_eval_string. */

mino_val *mino_send(mino_state *S, mino_val *agent,
                      mino_val *fn, mino_val *extra_args)
{
    mino_val   *result;
    volatile char probe = 0;
    mino_lock(S);
    gc_note_host_frame(S, (void *)&probe);
    (void)probe;
    if (extra_args == NULL) extra_args = mino_nil(S);
    result = agent_send_core(S, AGENT_POOL_POOLED, agent, fn, extra_args,
                             NULL,
                             "mino_send: first argument must be an agent");
    mino_unlock(S);
    return result;
}

mino_val *mino_send_off(mino_state *S, mino_val *agent,
                          mino_val *fn, mino_val *extra_args)
{
    mino_val   *result;
    volatile char probe = 0;
    mino_lock(S);
    gc_note_host_frame(S, (void *)&probe);
    (void)probe;
    if (extra_args == NULL) extra_args = mino_nil(S);
    result = agent_send_core(S, AGENT_POOL_SOLO, agent, fn, extra_args,
                             NULL,
                             "mino_send_off: first argument must be an agent");
    mino_unlock(S);
    return result;
}

mino_val *mino_await(mino_state *S, mino_val **agents)
{
    mino_val   *list;
    mino_val   *result;
    volatile char probe = 0;
    mino_lock(S);
    gc_note_host_frame(S, (void *)&probe);
    (void)probe;
    list   = agent_array_to_list(S, agents);
    result = prim_await(S, list, NULL);
    mino_unlock(S);
    return result;
}

int mino_await_for(mino_state *S, long long timeout_ms,
                   mino_val **agents)
{
    mino_val   *list, *args;
    mino_val   *result;
    volatile char probe = 0;
    int           ok;
    mino_lock(S);
    gc_note_host_frame(S, (void *)&probe);
    (void)probe;
    list   = agent_array_to_list(S, agents);
    args   = mino_cons(S, mino_int(S, timeout_ms), list);
    result = prim_await_for(S, args, NULL);
    /* prim_await_for returns true on success, false on timeout. A
     * throw publishes the exception via S; result is NULL in that
     * case and we propagate timeout=0 so the caller treats it as a
     * failure. */
    ok = (result != NULL && mino_is_truthy(result)) ? 1 : 0;
    mino_unlock(S);
    return ok;
}

mino_val *mino_agent_error(mino_state *S, mino_val *agent)
{
    mino_val   *err;
    volatile char probe = 0;
    mino_lock(S);
    gc_note_host_frame(S, (void *)&probe);
    (void)probe;
    if (!mino_is_agent(agent)) {
        prim_throw_classified(S, "eval/type", "MTY001",
            "mino_agent_error: argument must be an agent");
        mino_unlock(S);
        return NULL;
    }
    if (agent_check_state(S, agent)) { mino_unlock(S); return NULL; }
    err = agent->as.agent.err;
    mino_unlock(S);
    return err;
}

mino_val *mino_restart_agent(mino_state *S, mino_val *agent,
                                mino_val *new_state, int clear_actions)
{
    mino_val   *args, *result;
    volatile char probe = 0;
    mino_lock(S);
    gc_note_host_frame(S, (void *)&probe);
    (void)probe;
    /* Build (agent new-state :clear-actions <bool>) for prim_restart_agent. */
    args = mino_cons(S, agent,
             mino_cons(S, new_state,
               mino_cons(S, mino_keyword(S, "clear-actions"),
                 mino_cons(S, clear_actions ? mino_true(S) : mino_false(S),
                           mino_nil(S)))));
    result = prim_restart_agent(S, args, NULL);
    mino_unlock(S);
    return result;
}

/* Shared core for send / send-off (and the public C-API mino_send /
 * mino_send_off). Validates the agent, applies the cross-state and
 * shutdown / failed-:fail / dosync-defer guards, then either parks
 * the action onto the current tx's pending-send list or enqueues it
 * onto the named pool's run-queue (lazy-spawning the pool's worker).
 *
 * Returns the agent on success, NULL on throw (caller propagates).
 * The caller must already hold state_lock (the prims do via their
 * mino_lock perimeter; the C-API entries acquire it themselves). */
static mino_val *agent_send_core(mino_state *S, agent_pool_kind_t kind,
                                    mino_val *agent, mino_val *fn,
                                    mino_val *extra, mino_env *env,
                                    const char *prim_name)
{
    mino_thread_ctx_t *ctx;
    if (!mino_is_agent(agent)) {
        prim_throw_classified(S, "eval/type", "MTY001",
            prim_name);
        return NULL;
    }
    if (agent_check_state(S, agent)) return NULL;
    if (S->agent.shutdown) {
        prim_throw_classified(S, "eval/state", "MST008",
            "Agents have been shut down; new sends are not accepted");
        return NULL;
    }
    if (agent->as.agent.err != NULL && agent->as.agent.err_mode == 0) {
        prim_throw_classified(S, "eval/state", "MST002",
            "Agent is failed, needs restart");
        return NULL;
    }
    ctx = mino_current_ctx(S);
    if (ctx->current_tx != NULL) {
        mino_val *triple = mino_cons(S, agent,
                                mino_cons(S, fn, extra));
        ctx->current_tx->pending_sends =
            mino_cons(S, triple,
                ctx->current_tx->pending_sends != NULL
                    ? ctx->current_tx->pending_sends
                    : mino_empty_list(S));
        return agent;
    }
    /* agent_enqueue handles lazy worker spawn under the same
     * agent_mu critical section as the push + in_flight bump, so a
     * freshly-spawned worker is guaranteed to observe the node on
     * its first runq check (see the race note in the function
     * comment). */
    if (agent_enqueue(S, kind, agent, fn, extra, env)) {
        /* Diag already published by agent_enqueue. */
        return NULL;
    }
    return agent;
}

/* --- pending-sends drain ------------------------------------------------- *
 *
 * Used by stm.c after a successful commit. Pending entries were
 * pushed cons-prepended (LIFO) by prim_send when called inside a
 * dosync; reverse onto a fresh stack so the dispatch order matches
 * the order the user called send. Each entry is a (agent fn . extra)
 * triple. Drain enqueues onto the worker's runq so post-commit
 * sends run on the worker thread (same path as a top-level send),
 * not on the embedder's mutator thread.
 *
 * Failed-state at drain time:
 *   :fail mode + err set -- skip silently. JVM's dispatcher would
 *     throw into the agent's executor thread, never reaching the
 *     dosync caller; mino's drain models that by dropping the
 *     action with no caller-visible signal (the agent.err latch
 *     remains the failure record).
 *   :continue mode -- enqueue regardless. Matches prim_send: failed
 *     agents in :continue keep accepting actions.
 *
 * If the worker can't be spawned (e.g. host hasn't granted threads)
 * the drain silently drops; the dosync commit has already happened,
 * so throwing here would not undo the ref writes and would surprise
 * a caller that wasn't holding state_lock. Document this contract:
 * STM dosync sends require the same thread budget as direct sends. */
void mino_agent_drain_pending(mino_state *S, mino_val *pending,
                               mino_env *env)
{
    mino_val *reversed = mino_empty_list(S);
    mino_val *p;
    if (pending == NULL) return;
    for (p = pending; mino_is_cons(p); p = p->as.cons.cdr) {
        reversed = mino_cons(S, p->as.cons.car, reversed);
    }
    for (p = reversed; mino_is_cons(p); p = p->as.cons.cdr) {
        mino_val *triple = p->as.cons.car;
        mino_val *agent_v, *fn, *extra;
        if (!mino_is_cons(triple)) continue;
        agent_v = triple->as.cons.car;
        if (!mino_is_cons(triple->as.cons.cdr)) continue;
        fn      = triple->as.cons.cdr->as.cons.car;
        extra   = triple->as.cons.cdr->as.cons.cdr;
        if (!mino_is_agent(agent_v)) continue;
        if (agent_v->as.agent.err != NULL
            && agent_v->as.agent.err_mode == 0) {
            continue;
        }
        if (S->agent.shutdown) continue;
        /* In-tx send is the JVM-canon shape -- post-commit pending
         * drains route through the POOLED pool. agent_enqueue lazy-
         * spawns the worker atomically with the push so the worker
         * is guaranteed to drain. If the spawn refuses (no thread
         * budget) the action is silently dropped: the commit has
         * already gone through and throwing here would surprise a
         * caller that wasn't holding state_lock. Clear the diag
         * that agent_enqueue published. */
        if (agent_enqueue(S, AGENT_POOL_POOLED, agent_v, fn, extra, env)) {
            clear_error(S);
            return;
        }
    }
}

/* --- primitives ----------------------------------------------------------- */

mino_val *prim_agent(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *initial;
    mino_val *opts;
    mino_val *agent;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "agent requires at least one argument");
    }
    initial = args->as.cons.car;
    agent   = mino_agent(S, initial);
    /* Parse trailing keyword options. JVM accepts :validator,
     * :error-handler, :error-mode, :meta. mino supports the first
     * three; :meta is rejected (cell meta on agents is not yet
     * surfaced through supports_meta, so silently storing it would
     * be invisible to (meta a)). Unknown keys throw -- silent
     * acceptance would mask user typos. */
    for (opts = args->as.cons.cdr; mino_is_cons(opts); ) {
        mino_val *key = opts->as.cons.car;
        mino_val *val;
        if (!mino_is_cons(opts->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                "agent: option key without value");
        }
        val  = opts->as.cons.cdr->as.cons.car;
        opts = opts->as.cons.cdr->as.cons.cdr;
        if (key == NULL || mino_type_of(key) != MINO_KEYWORD) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "agent: option key must be a keyword");
        }
        if (strcmp(key->as.s.data, "validator") == 0) {
            if (val == NULL || (mino_type_of(val) != MINO_FN
                                 && mino_type_of(val) != MINO_PRIM
                                 && mino_type_of(val) != MINO_MACRO
                                 && mino_type_of(val) != MINO_NIL)) {
                return prim_throw_classified(S, "eval/type", "MTY001",
                    "agent: :validator must be a fn or nil");
            }
            if (mino_type_of(val) == MINO_NIL) val = NULL;
            gc_write_barrier(S, agent, agent->as.agent.validator, val);
            agent->as.agent.validator = val;
        } else if (strcmp(key->as.s.data, "error-handler") == 0) {
            if (val == NULL || (mino_type_of(val) != MINO_FN
                                 && mino_type_of(val) != MINO_PRIM
                                 && mino_type_of(val) != MINO_MACRO
                                 && mino_type_of(val) != MINO_NIL)) {
                return prim_throw_classified(S, "eval/type", "MTY001",
                    "agent: :error-handler must be a fn or nil");
            }
            if (mino_type_of(val) == MINO_NIL) val = NULL;
            gc_write_barrier(S, agent, agent->as.agent.err_handler, val);
            agent->as.agent.err_handler = val;
        } else if (strcmp(key->as.s.data, "error-mode") == 0) {
            if (val == NULL || mino_type_of(val) != MINO_KEYWORD
                || (strcmp(val->as.s.data, "fail") != 0
                    && strcmp(val->as.s.data, "continue") != 0)) {
                return prim_throw_classified(S, "eval/type", "MTY001",
                    "agent: :error-mode must be :fail or :continue");
            }
            agent->as.agent.err_mode =
                (strcmp(val->as.s.data, "continue") == 0) ? 1 : 0;
        } else if (strcmp(key->as.s.data, "meta") == 0) {
            if (val != NULL && mino_type_of(val) != MINO_NIL
                && mino_type_of(val) != MINO_MAP) {
                return prim_throw_classified(S, "eval/type", "MTY001",
                    "agent: :meta must be a map or nil");
            }
            if (val != NULL && mino_type_of(val) == MINO_NIL) val = NULL;
            /* Cell-level meta. with-meta is intentionally NOT
             * routed through supports_meta for agents (shallow
             * copy would diverge on first send); (meta a) reads
             * this slot directly. */
            gc_write_barrier(S, agent, agent->meta, val);
            agent->meta = val;
        } else {
            return prim_throw_classified(S, "eval/state", "MST002",
                "agent: unknown option key");
        }
    }
    return agent;
}

mino_val *prim_agent_p(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "agent? requires one argument");
    }
    return mino_is_agent(args->as.cons.car) ? mino_true(S) : mino_false(S);
}

mino_val *prim_send(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *agent, *fn, *extra;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "send requires at least two arguments: agent and fn");
    }
    agent = args->as.cons.car;
    fn    = args->as.cons.cdr->as.cons.car;
    extra = args->as.cons.cdr->as.cons.cdr;
    return agent_send_core(S, AGENT_POOL_POOLED, agent, fn, extra, env,
                           "send: first argument must be an agent");
}

mino_val *prim_send_off(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *agent, *fn, *extra;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "send-off requires at least two arguments: agent and fn");
    }
    agent = args->as.cons.car;
    fn    = args->as.cons.cdr->as.cons.car;
    extra = args->as.cons.cdr->as.cons.cdr;
    return agent_send_core(S, AGENT_POOL_SOLO, agent, fn, extra, env,
                           "send-off: first argument must be an agent");
}

/* Block until in_flight == 0 for every named agent. Drops state_lock
 * across the wait so the worker thread can run the queued actions
 * (the worker takes state_lock before each action body). The wait
 * loop is per-agent: agent_cv is broadcast after every action
 * completes, so a sleeping waiter wakes once for any change and
 * rechecks every agent's counter.
 *
 * Self-await detection: if the calling thread IS the worker, await
 * would deadlock (the worker is the one that decrements in_flight).
 * mino_tls_ctx is non-NULL on the worker; check before yielding the
 * lock and throw MST002. The embedder thread leaves tls_ctx NULL so
 * the normal path is unaffected. */
mino_val *prim_await(mino_state *S, mino_val *args, mino_env *env)
{
    int saved_depth;
    mino_val *iter;
    (void)env;
    /* Validate args + cross-state guard before any blocking. */
    for (iter = args; mino_is_cons(iter); iter = iter->as.cons.cdr) {
        mino_val *a = iter->as.cons.car;
        if (!mino_is_agent(a)) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "await: argument must be an agent");
        }
        if (agent_check_state(S, a)) return NULL;
    }
    if (!S->agent.mu_inited) {
        /* No worker has spawned: every agent's in_flight is 0,
         * await is trivially complete. */
        return mino_nil(S);
    }
    if (mino_tls_ctx != NULL) {
        /* Calling thread is a host worker (the agent worker is the
         * only writer of mino_tls_ctx in this state); await would
         * self-deadlock. */
        return prim_throw_classified(S, "eval/state", "MST002",
            "await called from agent action body would deadlock");
    }
    saved_depth = mino_yield_lock(S);
    agent_mu_lock(&S->agent.mu);
    for (;;) {
        int still_busy = 0;
        for (iter = args; mino_is_cons(iter); iter = iter->as.cons.cdr) {
            mino_val *a = iter->as.cons.car;
            if (a->as.agent.in_flight > 0) { still_busy = 1; break; }
        }
        if (!still_busy) break;
        agent_cv_wait(&S->agent.cv, &S->agent.mu);
    }
    agent_mu_unlock(&S->agent.mu);
    mino_resume_lock(S, saved_depth);
    return mino_nil(S);
}

#if !(defined(_WIN32) && defined(_MSC_VER))
/* Add ms milliseconds to abs_ts (a struct timespec). */
static void timespec_add_ms(struct timespec *ts, long long ms)
{
    long long secs = ms / 1000;
    long long nsec = (ms % 1000) * 1000000LL;
    ts->tv_sec += (time_t)secs;
    ts->tv_nsec += (long)nsec;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += ts->tv_nsec / 1000000000L;
        ts->tv_nsec %= 1000000000L;
    }
}
#endif

mino_val *prim_await_for(mino_state *S, mino_val *args,
                            mino_env *env)
{
    long long timeout_ms;
    mino_val *first;
    mino_val *agents;
    mino_val *iter;
    int saved_depth;
    int timed_out = 0;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "await-for requires a timeout and at least one agent");
    }
    first = args->as.cons.car;
    if (first == NULL || !mino_val_int_p(first)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "await-for: timeout must be an integer (milliseconds)");
    }
    timeout_ms = mino_val_int_get(first);
    if (timeout_ms < 0) timeout_ms = 0;
    agents = args->as.cons.cdr;
    for (iter = agents; mino_is_cons(iter); iter = iter->as.cons.cdr) {
        mino_val *a = iter->as.cons.car;
        if (!mino_is_agent(a)) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "await-for: argument must be an agent");
        }
        if (agent_check_state(S, a)) return NULL;
    }
    if (!S->agent.mu_inited) return mino_true(S);
    if (mino_tls_ctx != NULL) {
        return prim_throw_classified(S, "eval/state", "MST002",
            "await-for called from agent action body would deadlock");
    }
    saved_depth = mino_yield_lock(S);
    agent_mu_lock(&S->agent.mu);
#if defined(_WIN32) && defined(_MSC_VER)
    {
        DWORD deadline_ms = GetTickCount() + (DWORD)timeout_ms;
        for (;;) {
            int still_busy = 0;
            DWORD now;
            for (iter = agents; mino_is_cons(iter); iter = iter->as.cons.cdr) {
                mino_val *a = iter->as.cons.car;
                if (a->as.agent.in_flight > 0) { still_busy = 1; break; }
            }
            if (!still_busy) break;
            now = GetTickCount();
            if (now >= deadline_ms) { timed_out = 1; break; }
            if (!SleepConditionVariableCS(&S->agent.cv, &S->agent.mu,
                                          deadline_ms - now)) {
                timed_out = 1;
                break;
            }
        }
    }
#else
    {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        timespec_add_ms(&deadline, timeout_ms);
        for (;;) {
            int still_busy = 0;
            int rc;
            for (iter = agents; mino_is_cons(iter); iter = iter->as.cons.cdr) {
                mino_val *a = iter->as.cons.car;
                if (a->as.agent.in_flight > 0) { still_busy = 1; break; }
            }
            if (!still_busy) break;
            rc = pthread_cond_timedwait(&S->agent.cv, &S->agent.mu,
                                        &deadline);
            if (rc != 0) { timed_out = 1; break; }
        }
    }
#endif
    /* On timeout, recheck once more under the lock -- a wakeup could
     * have raced with the timeout. */
    if (timed_out) {
        int still_busy = 0;
        for (iter = agents; mino_is_cons(iter); iter = iter->as.cons.cdr) {
            mino_val *a = iter->as.cons.car;
            if (a->as.agent.in_flight > 0) { still_busy = 1; break; }
        }
        if (!still_busy) timed_out = 0;
    }
    agent_mu_unlock(&S->agent.mu);
    mino_resume_lock(S, saved_depth);
    return timed_out ? mino_false(S) : mino_true(S);
}

mino_val *prim_agent_error(mino_state *S, mino_val *args,
                              mino_env *env)
{
    mino_val *agent;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "agent-error requires one argument");
    }
    agent = args->as.cons.car;
    if (!mino_is_agent(agent)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "agent-error: argument must be an agent");
    }
    if (agent_check_state(S, agent)) return NULL;
    return agent->as.agent.err != NULL ? agent->as.agent.err : mino_nil(S);
}

mino_val *prim_restart_agent(mino_state *S, mino_val *args,
                                mino_env *env)
{
    mino_val *agent, *new_state;
    mino_val *opts;
    int         clear_actions = 0;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "restart-agent requires at least two arguments: agent and value");
    }
    agent     = args->as.cons.car;
    new_state = args->as.cons.cdr->as.cons.car;
    /* Parse trailing keyword options. JVM accepts :clear-actions. */
    for (opts = args->as.cons.cdr->as.cons.cdr; mino_is_cons(opts); ) {
        mino_val *key = opts->as.cons.car;
        mino_val *val;
        if (!mino_is_cons(opts->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                "restart-agent: option key without value");
        }
        val  = opts->as.cons.cdr->as.cons.car;
        opts = opts->as.cons.cdr->as.cons.cdr;
        if (key == NULL || mino_type_of(key) != MINO_KEYWORD) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "restart-agent: option key must be a keyword");
        }
        if (strcmp(key->as.s.data, "clear-actions") == 0) {
            clear_actions = mino_is_truthy(val) ? 1 : 0;
        } else {
            return prim_throw_classified(S, "eval/state", "MST002",
                "restart-agent: unknown option key");
        }
    }
    if (!mino_is_agent(agent)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "restart-agent: first argument must be an agent");
    }
    if (agent_check_state(S, agent)) return NULL;
    if (agent->as.agent.err == NULL) {
        return prim_throw_classified(S, "eval/state", "MST002",
            "Agent does not need a restart");
    }
    /* JVM canon: restart-agent runs the validator on the new state
     * before clearing the error. A failed agent must not be lifted
     * back into circulation in a state the validator forbids --
     * the next send would just refail. Throws here propagate to
     * the caller; the agent stays in its failed state. */
    if (agent->as.agent.validator != NULL) {
        mino_val *vargs   = mino_cons(S, new_state, mino_nil(S));
        mino_val *vresult = NULL;
        mino_val *thrown  = NULL;
        int         pc      = mino_pcall(S, agent->as.agent.validator,
                                          vargs, env, &vresult, &thrown);
        if (pc != 0) {
            return mino_throw(S, thrown);
        }
        if (vresult == NULL || !mino_is_truthy(vresult)) {
            return prim_throw_classified(S, "eval/contract", "MCT001",
                "Invalid reference state");
        }
    }
    /* JVM canon: with :clear-actions true, drop any actions the
     * agent has queued (they would have been blocked behind the
     * failure latch anyway). Walk both pools' runqs under agent_mu,
     * splice out entries targeting this agent, decrement in_flight
     * per removal, broadcast in case an await waiter is sleeping. */
    if (clear_actions && S->agent.mu_inited) {
        int pi;
        agent_mu_lock(&S->agent.mu);
        for (pi = 0; pi < AGENT_POOL_COUNT; pi++) {
            agent_action_node_t **pp = &S->agent.pool[pi].run_head;
            while (*pp != NULL) {
                agent_action_node_t *cur = *pp;
                if (cur->agent == agent) {
                    *pp = cur->next;
                    if (agent->as.agent.in_flight > 0) {
                        agent->as.agent.in_flight--;
                    }
                    free(cur);
                } else {
                    pp = &cur->next;
                }
            }
            /* Re-establish run_tail by walking once. */
            if (S->agent.pool[pi].run_head == NULL) {
                S->agent.pool[pi].run_tail = NULL;
            } else {
                agent_action_node_t *t = S->agent.pool[pi].run_head;
                while (t->next != NULL) t = t->next;
                S->agent.pool[pi].run_tail = t;
            }
        }
        agent_cv_broadcast(&S->agent.cv);
        agent_mu_unlock(&S->agent.mu);
    }
    gc_write_barrier(S, agent, agent->as.agent.err, NULL);
    agent->as.agent.err = NULL;
    gc_write_barrier(S, agent, agent->as.agent.val, new_state);
    agent->as.agent.val = new_state;
    return new_state;
}

mino_val *prim_set_error_handler_bang(mino_state *S, mino_val *args,
                                         mino_env *env)
{
    mino_val *agent, *fn;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "set-error-handler! requires two arguments");
    }
    agent = args->as.cons.car;
    fn    = args->as.cons.cdr->as.cons.car;
    if (!mino_is_agent(agent)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "set-error-handler!: first argument must be an agent");
    }
    if (agent_check_state(S, agent)) return NULL;
    /* nil clears. Anything else must be callable so a future action
     * failure can dispatch through it. Earlier mino stored any value
     * (e.g. an int) which then failed at dispatch time, far from the
     * install site. Reject at install. */
    if (fn != NULL && mino_type_of(fn) == MINO_NIL) fn = NULL;
    if (fn != NULL && mino_type_of(fn) != MINO_FN
        && mino_type_of(fn) != MINO_PRIM
        && mino_type_of(fn) != MINO_MACRO) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "set-error-handler!: handler must be a fn or nil");
    }
    gc_write_barrier(S, agent, agent->as.agent.err_handler, fn);
    agent->as.agent.err_handler = fn;
    return mino_nil(S);
}

mino_val *prim_error_handler(mino_state *S, mino_val *args,
                                mino_env *env)
{
    mino_val *agent;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "error-handler requires one argument");
    }
    agent = args->as.cons.car;
    if (!mino_is_agent(agent)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "error-handler: argument must be an agent");
    }
    if (agent_check_state(S, agent)) return NULL;
    return agent->as.agent.err_handler != NULL
         ? agent->as.agent.err_handler : mino_nil(S);
}

mino_val *prim_set_error_mode_bang(mino_state *S, mino_val *args,
                                      mino_env *env)
{
    mino_val *agent, *mode;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "set-error-mode! requires two arguments");
    }
    agent = args->as.cons.car;
    mode  = args->as.cons.cdr->as.cons.car;
    if (!mino_is_agent(agent)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "set-error-mode!: first argument must be an agent");
    }
    if (agent_check_state(S, agent)) return NULL;
    /* Only :fail and :continue are accepted. Earlier mino silently
     * routed any unrecognised keyword to :fail (so :silent on a
     * previously :continue agent quietly flipped it) and silently
     * ignored non-keywords. Both behaviors are loud surprises; the
     * fix throws so the caller sees the typo. */
    if (mode == NULL || mino_type_of(mode) != MINO_KEYWORD
        || (strcmp(mode->as.s.data, "fail") != 0
            && strcmp(mode->as.s.data, "continue") != 0)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "set-error-mode!: mode must be :fail or :continue");
    }
    agent->as.agent.err_mode =
        (strcmp(mode->as.s.data, "continue") == 0) ? 1 : 0;
    return mino_nil(S);
}

mino_val *prim_error_mode(mino_state *S, mino_val *args,
                             mino_env *env)
{
    mino_val *agent;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "error-mode requires one argument");
    }
    agent = args->as.cons.car;
    if (!mino_is_agent(agent)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "error-mode: argument must be an agent");
    }
    if (agent_check_state(S, agent)) return NULL;
    return mino_keyword(S,
        agent->as.agent.err_mode == 1 ? "continue" : "fail");
}

mino_val *prim_shutdown_agents(mino_state *S, mino_val *args,
                                  mino_env *env)
{
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "shutdown-agents takes no arguments");
    }
    /* Self-detect: an action body running on the worker thread
     * cannot join itself. mino_tls_ctx is set only on host worker
     * threads, so a non-NULL tls_ctx during eval implies the agent
     * worker is the caller. */
    if (mino_tls_ctx != NULL) {
        return prim_throw_classified(S, "eval/state", "MST002",
            "shutdown-agents cannot be called from inside an agent action");
    }
    /* Flip the shutdown flag, signal the worker to drain + exit,
     * then reap the pthread handle. Idempotent: a no-op if no
     * worker has been spawned, or if quiesce has already run. */
    mino_agent_quiesce_workers(S);
    return mino_nil(S);
}

mino_val *prim_send_via(mino_state *S, mino_val *args, mino_env *env)
{
    (void)args;
    (void)env;
    /* JVM-canon (send-via executor a fn & args) routes the action
     * through a host-supplied Executor. mino has no public Executor
     * type yet (the embedder API surface for handing in a custom
     * dispatcher is intentionally deferred), so this prim throws
     * with a clear message rather than aliasing to send and
     * silently dropping the executor argument. Use send / send-off
     * to dispatch through the per-state worker. */
    return prim_throw_classified(S, "eval/state", "MST008",
        "send-via is intentionally deferred -- mino has no public "
        "Executor type. Use send or send-off to dispatch through the "
        "per-state worker.");
}

mino_val *prim_release_pending_sends(mino_state *S, mino_val *args,
                                        mino_env *env)
{
    mino_thread_ctx_t *ctx;
    long long          count = 0;
    mino_val        *p;
    (void)args;
    (void)env;
    /* JVM canon: returns the number of sends that were queued by the
     * current transaction and clears them so they will NOT fire on
     * commit. Outside a transaction it's a no-op returning 0. mino's
     * pending_sends lives on tx_state_t; walk + null it out. */
    ctx = mino_current_ctx(S);
    if (ctx->current_tx == NULL) return mino_int(S, 0);
    for (p = ctx->current_tx->pending_sends;
         mino_is_cons(p);
         p = p->as.cons.cdr) {
        count++;
    }
    ctx->current_tx->pending_sends = NULL;
    return mino_int(S, count);
}

/* --- primitive table + install hook -------------------------------------- */

static const mino_prim_def k_prims_agent[] = {
    {"agent",       prim_agent,
     "Creates an asynchronous agent holding the given initial state. "
     "Mutate via send / send-off; read via @agent. The action runs "
     "on a per-state worker thread; await blocks until queued "
     "actions complete."},
    {"agent?",      prim_agent_p,
     "Returns true if x is an agent."},
    {"send",        prim_send,
     "Dispatches an action onto the agent's POOLED run-queue and "
     "returns the agent immediately. The action runs on the POOLED "
     "worker under state_lock. Throws MTH001 if the host has not "
     "granted a thread budget for the worker."},
    {"send-off",    prim_send_off,
     "Dispatches an action onto the agent's SOLO run-queue. mino's "
     "per-state eval lock means actions across the two pools still "
     "serialize, but the queues are independent: a long-running "
     "send-off action does not stall pending sends, and vice versa. "
     "Throws MTH001 if the host has not granted a thread budget."},
    {"send-via",    prim_send_via,
     "JVM-canon dispatches the action through a host-supplied "
     "Executor. mino has no public Executor type yet; this prim "
     "throws MST008 rather than aliasing to send and dropping the "
     "executor argument. Use send / send-off."},
    {"await",       prim_await,
     "Blocks the calling thread until every named agent's queued "
     "actions have finished. Throws MST002 if called from inside "
     "an agent action body (would self-deadlock)."},
    {"await-for",   prim_await_for,
     "Like await with a millisecond timeout. Returns true if every "
     "named agent reached zero in-flight actions before the deadline, "
     "false on timeout."},
    {"agent-error", prim_agent_error,
     "Returns the exception captured by the agent's most recent failed "
     "action or watch, or nil if the agent is in a clean state."},
    {"restart-agent", prim_restart_agent,
     "Clears the agent's error and resets its state to the given "
     "value. Trailing :clear-actions true also drops every queued "
     "action targeting this agent."},
    {"set-error-handler!", prim_set_error_handler_bang,
     "Sets the agent's error-handler fn (called with [agent ex] when an "
     "action throws)."},
    {"error-handler", prim_error_handler,
     "Returns the agent's current error-handler fn or nil."},
    {"set-error-mode!", prim_set_error_mode_bang,
     "Sets the agent's error mode (:fail or :continue)."},
    {"error-mode", prim_error_mode,
     "Returns the agent's current error mode."},
    {"shutdown-agents", prim_shutdown_agents,
     "Quiesces both per-state agent workers: signals each to drain "
     "its remaining queue, joins the pthreads, seals the agent "
     "surface so subsequent send / send-off throw MST008. "
     "Idempotent. Throws MST002 if called from inside an action "
     "body (self-join)."},
    {"release-pending-sends", prim_release_pending_sends,
     "Returns the count of sends queued by the current transaction "
     "and clears them so they will NOT fire on commit. Outside a "
     "transaction returns 0."},
};

static const size_t k_prims_agent_count = sizeof(k_prims_agent)
                                    / sizeof(k_prims_agent[0]);

void mino_install_agent(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_agent, k_prims_agent_count,
                                       "agent");
    S->caps_installed |= MINO_CAP_AGENT;
    /* Intern *agent* as a dynamic var with nil default. The
     * dispatcher (agent_apply_action) pushes a thread binding for
     * this name to the running agent so action/validator/watch
     * bodies can refer to themselves via *agent*. Outside any
     * dispatch the var resolves to nil through dyn_lookup falling
     * back on the var's root value. */
    {
        mino_val *agent_var = var_intern(S, "clojure.core", "*agent*");
        if (agent_var != NULL) {
            agent_var->as.var.dynamic = 1;
            var_set_root(S, agent_var, mino_nil(S));
            mino_env_set(S, core_env, "*agent*", agent_var->as.var.root);
        }
    }
}
