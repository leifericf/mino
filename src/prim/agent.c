/*
 * agent.c -- agents (asynchronous mutable cells with serialized actions).
 *
 * mino's MVP implementation runs sends synchronously on the calling
 * thread. JVM Clojure dispatches the action on a worker thread pool
 * and returns the agent immediately; mino runs the action eagerly,
 * validates against the agent's validator, captures any throw into
 * the agent's error slot via mino_pcall's out_ex parameter, dispatches
 * watches, and returns the agent.
 *
 * Why sync: mino's eval loop is serialized under a per-state mutex
 * (mino_state_lock_acquire). A future-driven worker would acquire
 * that lock to run its action and otherwise idle, so the parallelism
 * advantage of JVM's worker pool does not exist here. Running
 * synchronously is observably equivalent for any program that does
 * not race against the agent itself, with the bonus that await is
 * trivially a no-op (the queue is always drained on return from
 * send).
 *
 * Documented deviations from JVM Clojure:
 *
 *  1. send / send-off run synchronously on the calling thread. await
 *     and await-for are no-ops -- the queue is always drained.
 *
 *  2. send-via is not implemented (no public Executor type exposed).
 *
 *  3. shutdown-agents and release-pending-sends are stubs (mino's
 *     workload doesn't have an agent-shutdown contract to honor).
 *
 *  4. Action throws and watch throws both set the agent's err slot
 *     via mino_pcall so the caller sees the thrown payload via
 *     agent-error. JVM routes action throws through error-handler /
 *     error-mode similarly; mino's MVP captures the bare exception.
 */

#include "prim/internal.h"
#include "eval/internal.h"

#include <string.h>

/* --- public-API constructor + predicate ----------------------------------- */

mino_val_t *mino_agent(mino_state_t *S, mino_val_t *initial)
{
    mino_val_t *v = alloc_val(S, MINO_AGENT);
    v->as.agent.val         = initial;
    v->as.agent.watches     = NULL;
    v->as.agent.validator   = NULL;
    v->as.agent.err         = NULL;
    v->as.agent.err_handler = NULL;
    v->as.agent.err_mode    = 0;  /* :fail */
    v->as.agent.queue       = NULL;  /* unused in sync MVP */
    return v;
}

int mino_is_agent(const mino_val_t *v)
{
    return v != NULL && v->type == MINO_AGENT;
}

/* --- action dispatch ------------------------------------------------------ */

/* Build (cur arg1 arg2 ...) for invoking an action fn. */
static mino_val_t *agent_build_call(mino_state_t *S, mino_val_t *cur,
                                     mino_val_t *extra)
{
    mino_val_t *head = mino_cons(S, cur, mino_nil(S));
    mino_val_t *cell = head;
    while (mino_is_cons(extra)) {
        mino_val_t *next = mino_cons(S, extra->as.cons.car, mino_nil(S));
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
static void agent_report_failure(mino_state_t *S, mino_val_t *agent,
                                  mino_val_t *ex, mino_env_t *env)
{
    mino_val_t *handler = agent->as.agent.err_handler;
    mino_val_t *result  = NULL;
    mino_val_t *hthrown = NULL;
    int         pc;
    if (handler == NULL) {
        gc_write_barrier(S, agent, agent->as.agent.err, ex);
        agent->as.agent.err = ex;
        return;
    }
    {
        mino_val_t *args = mino_cons(S, agent,
                              mino_cons(S, ex, mino_nil(S)));
        pc = mino_pcall(S, handler, args, env, &result, &hthrown);
    }
    if (pc != 0 && hthrown != NULL) {
        gc_write_barrier(S, agent, agent->as.agent.err, hthrown);
        agent->as.agent.err = hthrown;
    }
}

/* Apply one action synchronously: run validator, update state, fire
 * watches. Each user-callback invocation goes through mino_pcall so a
 * throw is captured into the agent's err slot rather than propagated
 * to the sender. */
static void agent_apply_action(mino_state_t *S, mino_val_t *agent,
                                mino_val_t *fn, mino_val_t *extra,
                                mino_env_t *env)
{
    mino_val_t *call_args = agent_build_call(S, agent->as.agent.val, extra);
    mino_val_t *new_state = NULL;
    mino_val_t *old_state = agent->as.agent.val;
    mino_val_t *thrown_ex = NULL;
    int         pc;

    pc = mino_pcall(S, fn, call_args, env, &new_state, &thrown_ex);
    if (pc != 0 || new_state == NULL) {
        agent_report_failure(S, agent, thrown_ex, env);
        return;
    }

    /* Validator: rejects bypass the publish + watch dispatch. */
    if (agent->as.agent.validator != NULL) {
        mino_val_t *vargs = mino_cons(S, new_state, mino_nil(S));
        mino_val_t *vresult = NULL;
        pc = mino_pcall(S, agent->as.agent.validator, vargs, env,
                          &vresult, &thrown_ex);
        if (pc != 0 || vresult == NULL || !mino_is_truthy(vresult)) {
            mino_val_t *ex = thrown_ex;
            if (ex == NULL) {
                /* Validator returned falsy without throwing: synthesize. */
                ex = mino_string(S, "Invalid reference state");
            }
            agent_report_failure(S, agent, ex, env);
            return;
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
        && agent->as.agent.watches->type == MINO_MAP
        && agent->as.agent.watches->as.map.len > 0) {
        mino_val_t *watches = agent->as.agent.watches;
        size_t      n = watches->as.map.len;
        size_t      i;
        for (i = 0; i < n; i++) {
            mino_val_t *key = vec_nth(watches->as.map.key_order, i);
            mino_val_t *wfn = map_get_val(watches, key);
            mino_val_t *wargs;
            mino_val_t *wresult = NULL;
            mino_val_t *wthrown = NULL;
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
}

/* --- primitives ----------------------------------------------------------- */

mino_val_t *prim_agent(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *initial;
    mino_val_t *opts;
    mino_val_t *agent;
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
        mino_val_t *key = opts->as.cons.car;
        mino_val_t *val;
        if (!mino_is_cons(opts->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                "agent: option key without value");
        }
        val  = opts->as.cons.cdr->as.cons.car;
        opts = opts->as.cons.cdr->as.cons.cdr;
        if (key == NULL || key->type != MINO_KEYWORD) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "agent: option key must be a keyword");
        }
        if (strcmp(key->as.s.data, "validator") == 0) {
            gc_write_barrier(S, agent, agent->as.agent.validator, val);
            agent->as.agent.validator = val;
        } else if (strcmp(key->as.s.data, "error-handler") == 0) {
            gc_write_barrier(S, agent, agent->as.agent.err_handler, val);
            agent->as.agent.err_handler = val;
        } else if (strcmp(key->as.s.data, "error-mode") == 0) {
            if (val == NULL || val->type != MINO_KEYWORD
                || (strcmp(val->as.s.data, "fail") != 0
                    && strcmp(val->as.s.data, "continue") != 0)) {
                return prim_throw_classified(S, "eval/type", "MTY001",
                    "agent: :error-mode must be :fail or :continue");
            }
            agent->as.agent.err_mode =
                (strcmp(val->as.s.data, "continue") == 0) ? 1 : 0;
        } else if (strcmp(key->as.s.data, "meta") == 0) {
            return prim_throw_classified(S, "eval/state", "MST002",
                "agent: :meta option not yet supported");
        } else {
            return prim_throw_classified(S, "eval/state", "MST002",
                "agent: unknown option key");
        }
    }
    return agent;
}

mino_val_t *prim_agent_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "agent? requires one argument");
    }
    return mino_is_agent(args->as.cons.car) ? mino_true(S) : mino_false(S);
}

mino_val_t *prim_send(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *agent, *fn, *extra;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "send requires at least two arguments: agent and fn");
    }
    agent = args->as.cons.car;
    fn    = args->as.cons.cdr->as.cons.car;
    extra = args->as.cons.cdr->as.cons.cdr;
    if (!mino_is_agent(agent)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "send: first argument must be an agent");
    }
    /* JVM-canon: dispatching to an agent in the failed state throws
     * unless error-mode is :continue. mino's MVP captures the failure
     * but allows further dispatch (matches :continue). */
    if (agent->as.agent.err != NULL && agent->as.agent.err_mode == 0) {
        return prim_throw_classified(S, "eval/state", "MST002",
            "Agent is failed, needs restart");
    }
    agent_apply_action(S, agent, fn, extra, env);
    return agent;
}

mino_val_t *prim_send_off(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    return prim_send(S, args, env);
}

mino_val_t *prim_await(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    /* No-op: the sync MVP drains the queue on every send return. We
     * still type-check so a misuse is loud. */
    while (mino_is_cons(args)) {
        if (!mino_is_agent(args->as.cons.car)) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "await: argument must be an agent");
        }
        args = args->as.cons.cdr;
    }
    return mino_nil(S);
}

mino_val_t *prim_await_for(mino_state_t *S, mino_val_t *args,
                            mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "await-for requires a timeout and at least one agent");
    }
    /* MVP: same as await; timeout is irrelevant when send is sync. */
    args = args->as.cons.cdr;
    while (mino_is_cons(args)) {
        if (!mino_is_agent(args->as.cons.car)) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "await-for: argument must be an agent");
        }
        args = args->as.cons.cdr;
    }
    return mino_true(S);
}

mino_val_t *prim_agent_error(mino_state_t *S, mino_val_t *args,
                              mino_env_t *env)
{
    mino_val_t *agent;
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
    return agent->as.agent.err != NULL ? agent->as.agent.err : mino_nil(S);
}

mino_val_t *prim_restart_agent(mino_state_t *S, mino_val_t *args,
                                mino_env_t *env)
{
    mino_val_t *agent, *new_state;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "restart-agent requires at least two arguments: agent and value");
    }
    agent     = args->as.cons.car;
    new_state = args->as.cons.cdr->as.cons.car;
    /* Trailing :clear-actions option ignored in this MVP. */
    if (!mino_is_agent(agent)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "restart-agent: first argument must be an agent");
    }
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
        mino_val_t *vargs   = mino_cons(S, new_state, mino_nil(S));
        mino_val_t *vresult = NULL;
        mino_val_t *thrown  = NULL;
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
    gc_write_barrier(S, agent, agent->as.agent.err, NULL);
    agent->as.agent.err = NULL;
    gc_write_barrier(S, agent, agent->as.agent.val, new_state);
    agent->as.agent.val = new_state;
    return new_state;
}

mino_val_t *prim_set_error_handler_bang(mino_state_t *S, mino_val_t *args,
                                         mino_env_t *env)
{
    mino_val_t *agent, *fn;
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
    if (fn != NULL && fn->type == MINO_NIL) fn = NULL;
    gc_write_barrier(S, agent, agent->as.agent.err_handler, fn);
    agent->as.agent.err_handler = fn;
    return mino_nil(S);
}

mino_val_t *prim_error_handler(mino_state_t *S, mino_val_t *args,
                                mino_env_t *env)
{
    mino_val_t *agent;
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
    return agent->as.agent.err_handler != NULL
         ? agent->as.agent.err_handler : mino_nil(S);
}

mino_val_t *prim_set_error_mode_bang(mino_state_t *S, mino_val_t *args,
                                      mino_env_t *env)
{
    mino_val_t *agent, *mode;
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
    if (mode != NULL && mode->type == MINO_KEYWORD) {
        agent->as.agent.err_mode =
            (strcmp(mode->as.s.data, "continue") == 0) ? 1 : 0;
    }
    return mino_nil(S);
}

mino_val_t *prim_error_mode(mino_state_t *S, mino_val_t *args,
                             mino_env_t *env)
{
    mino_val_t *agent;
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
    return mino_keyword(S,
        agent->as.agent.err_mode == 1 ? "continue" : "fail");
}

mino_val_t *prim_shutdown_agents(mino_state_t *S, mino_val_t *args,
                                  mino_env_t *env)
{
    (void)args;
    (void)env;
    return mino_nil(S);
}

mino_val_t *prim_release_pending_sends(mino_state_t *S, mino_val_t *args,
                                        mino_env_t *env)
{
    (void)args;
    (void)env;
    return mino_int(S, 0);
}

/* --- primitive table + install hook -------------------------------------- */

const mino_prim_def k_prims_agent[] = {
    {"agent",       prim_agent,
     "Creates an asynchronous agent holding the given initial state. "
     "Mutate via send / send-off; read via @agent. mino's MVP runs "
     "actions synchronously on the calling thread."},
    {"agent?",      prim_agent_p,
     "Returns true if x is an agent."},
    {"send",        prim_send,
     "Dispatches an action to an agent. Returns the agent. mino runs "
     "the action synchronously; in JVM Clojure send is async."},
    {"send-off",    prim_send_off,
     "Like send. mino runs both shapes through one synchronous path."},
    {"await",       prim_await,
     "No-op in mino's MVP. The synchronous send leaves no pending "
     "actions to wait for."},
    {"await-for",   prim_await_for,
     "Like await with a timeout (milliseconds). Always returns true "
     "in mino's MVP since the queue is always drained."},
    {"agent-error", prim_agent_error,
     "Returns the exception captured by the agent's most recent failed "
     "action or watch, or nil if the agent is in a clean state."},
    {"restart-agent", prim_restart_agent,
     "Clears the agent's error and resets its state to the given value."},
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
     "Stub. Returns nil."},
    {"release-pending-sends", prim_release_pending_sends,
     "Stub. Returns 0; mino runs actions eagerly."},
};

const size_t k_prims_agent_count = sizeof(k_prims_agent)
                                    / sizeof(k_prims_agent[0]);

void mino_install_agent(mino_state_t *S, mino_env_t *env)
{
    prim_install_table(S, env, "clojure.core",
                       k_prims_agent, k_prims_agent_count);
}
