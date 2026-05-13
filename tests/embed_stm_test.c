/*
 * embed_stm_test.c -- C-level smoke test for the STM Layer 2a API:
 *   mino_tx_ref / mino_is_tx_ref
 *   mino_tx_ref_deref / mino_tx_ref_set
 *   mino_tx_alter_c / mino_tx_commute_c
 *   mino_tx_ensure
 *   mino_tx_run (host-level dosync)
 *   outside-tx error contracts (MST002)
 *
 * Build via `./mino task test-embed`. Standalone build line:
 *   cc -std=c99 -Wall -Wextra -Wpedantic -O2 -Isrc -o embed_stm_test \
 *       tests/embed_stm_test.c <lib srcs>.c -lm -lpthread
 * Run: ./embed_stm_test
 */

#include "mino_internal.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#if defined(_WIN32) && defined(_MSC_VER)
#  include <windows.h>
#  define test_sleep_ms(ms) Sleep((DWORD)(ms))
#else
#  include <time.h>
static void test_sleep_ms(long long ms)
{
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000L);
    nanosleep(&ts, NULL);
}
#endif

static int failures = 0;

#define REQUIRE(cond, msg)                                         \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "FAIL (%s:%d): %s\n",                  \
                    __FILE__, __LINE__, (msg));                    \
            failures++;                                            \
        }                                                          \
    } while (0)

/* --- transformers used by the alter_c / commute_c paths -------------- */

static mino_val_t *xform_inc(mino_state_t *S, mino_val_t *cur,
                              void *user, mino_env_t *env)
{
    long long n;
    long long step = (long long)(intptr_t)user;
    (void)env;
    if (!mino_to_int(cur, &n)) return NULL;
    return mino_int(S, n + step);
}

static mino_val_t *xform_double(mino_state_t *S, mino_val_t *cur,
                                 void *user, mino_env_t *env)
{
    long long n;
    (void)user;
    (void)env;
    if (!mino_to_int(cur, &n)) return NULL;
    return mino_int(S, n * 2);
}

/* --- transaction bodies ---------------------------------------------- */

struct body_basic_ctx {
    mino_val_t *r;
    long long   final;
};

static mino_val_t *body_basic(mino_state_t *S, void *user, mino_env_t *env)
{
    struct body_basic_ctx *c = (struct body_basic_ctx *)user;
    mino_val_t *v;
    long long   inside;
    /* deref records a read */
    v = mino_tx_ref_deref(S, c->r);
    if (v == NULL) return NULL;
    if (!mino_to_int(v, &inside)) return NULL;
    REQUIRE(inside == 0, "body_basic: expected initial 0 inside tx");

    /* alter_c: +5 */
    v = mino_tx_alter_c(S, c->r, xform_inc, (void *)(intptr_t)5, env);
    if (v == NULL) return NULL;

    /* commute_c: *2  -> folds into alter tentative since alter pinned */
    v = mino_tx_commute_c(S, c->r, xform_double, NULL, env);
    if (v == NULL) return NULL;

    /* ensure: pins the same ref again, returns in-tx tentative */
    v = mino_tx_ensure(S, c->r, env);
    if (v == NULL) return NULL;
    if (!mino_to_int(v, &c->final)) return NULL;
    return v;
}

struct body_refset_ctx { mino_val_t *r; };

static mino_val_t *body_refset(mino_state_t *S, void *user, mino_env_t *env)
{
    struct body_refset_ctx *c = (struct body_refset_ctx *)user;
    (void)env;
    return mino_tx_ref_set(S, c->r, mino_int(S, 99));
}

struct body_commute_only_ctx { mino_val_t *r; };

static mino_val_t *body_commute_only(mino_state_t *S, void *user,
                                      mino_env_t *env)
{
    struct body_commute_only_ctx *c = (struct body_commute_only_ctx *)user;
    return mino_tx_commute_c(S, c->r, xform_inc, (void *)(intptr_t)1, env);
}

/* --- tests ----------------------------------------------------------- */

static void test_predicate_and_construction(mino_state_t *S)
{
    mino_val_t *r = mino_tx_ref(S, mino_int(S, 0));
    REQUIRE(r != NULL, "mino_tx_ref returned NULL");
    REQUIRE(mino_is_tx_ref(r), "mino_is_tx_ref(ref) == 0");
    REQUIRE(!mino_is_tx_ref(NULL), "mino_is_tx_ref(NULL) != 0");
    REQUIRE(!mino_is_tx_ref(mino_int(S, 0)), "mino_is_tx_ref(int) != 0");
    REQUIRE(!mino_is_tx_ref(mino_atom(S, mino_int(S, 0))),
            "mino_is_tx_ref(atom) != 0");

    /* Outside-tx deref: returns committed value, no bookkeeping. */
    {
        mino_val_t *v = mino_tx_ref_deref(S, r);
        long long   n;
        REQUIRE(v != NULL && mino_to_int(v, &n) && n == 0,
                "outside-tx deref didn't return committed 0");
    }
    /* Non-ref deref: returns NULL without throwing. */
    REQUIRE(mino_tx_ref_deref(S, mino_int(S, 0)) == NULL,
            "deref(int) should return NULL");
}

static void test_run_basic(mino_state_t *S, mino_env_t *env)
{
    mino_val_t *r = mino_tx_ref(S, mino_int(S, 0));
    struct body_basic_ctx ctx = { r, 0 };
    mino_val_t *result = mino_tx_run(S, body_basic, &ctx, env);
    long long n;

    REQUIRE(result != NULL, "mino_tx_run(body_basic) returned NULL");
    REQUIRE(ctx.final == 10, "body_basic final tentative != 10 ((0+5)*2)");

    /* After commit, deref outside the tx must show the committed value. */
    {
        mino_val_t *v = mino_tx_ref_deref(S, r);
        REQUIRE(v != NULL && mino_to_int(v, &n) && n == 10,
                "post-commit committed value != 10");
    }
}

static void test_run_refset(mino_state_t *S, mino_env_t *env)
{
    mino_val_t *r = mino_tx_ref(S, mino_int(S, 0));
    struct body_refset_ctx ctx = { r };
    long long n;
    mino_val_t *result = mino_tx_run(S, body_refset, &ctx, env);
    REQUIRE(result != NULL, "mino_tx_run(body_refset) returned NULL");
    REQUIRE(mino_to_int(mino_tx_ref_deref(S, r), &n) && n == 99,
            "post-commit committed value != 99");
}

static void test_run_commute_only(mino_state_t *S, mino_env_t *env)
{
    mino_val_t *r = mino_tx_ref(S, mino_int(S, 7));
    struct body_commute_only_ctx ctx = { r };
    long long n;
    mino_val_t *result = mino_tx_run(S, body_commute_only, &ctx, env);
    REQUIRE(result != NULL, "mino_tx_run(body_commute_only) returned NULL");
    /* Commute-only goes through the log. Replayed at commit against
     * the latest committed value (7) -> 8. */
    REQUIRE(mino_to_int(mino_tx_ref_deref(S, r), &n) && n == 8,
            "commute-only post-commit value != 8");
}

static void test_outside_tx_throws(mino_state_t *S, mino_env_t *env)
{
    mino_val_t *r = mino_tx_ref(S, mino_int(S, 0));
    /* mino_tx_ref_set outside any tx must throw MST002. The throw
     * pops out via the eval-loop's diagnostic; mino_last_error
     * carries the message. */
    {
        mino_val_t *v = mino_tx_ref_set(S, r, mino_int(S, 1));
        REQUIRE(v == NULL, "outside-tx ref-set should return NULL");
    }
    {
        const char *err = mino_last_error(S);
        REQUIRE(err != NULL && strstr(err, "No transaction running") != NULL,
                "outside-tx ref-set error did not mention 'No transaction'");
    }
    {
        mino_val_t *v = mino_tx_alter_c(S, r, xform_inc,
                                         (void *)(intptr_t)1, env);
        REQUIRE(v == NULL, "outside-tx alter_c should return NULL");
    }
    {
        mino_val_t *v = mino_tx_ensure(S, r, env);
        REQUIRE(v == NULL, "outside-tx ensure should return NULL");
    }
}

static void test_type_check_throws(mino_state_t *S, mino_env_t *env)
{
    mino_val_t *not_a_ref = mino_int(S, 42);
    {
        mino_val_t *v = mino_tx_ref_set(S, not_a_ref, mino_int(S, 1));
        REQUIRE(v == NULL, "ref_set(int) should return NULL");
    }
    {
        const char *err = mino_last_error(S);
        REQUIRE(err != NULL && strstr(err, "ref") != NULL,
                "ref_set(int) error should mention 'ref'");
    }
    {
        mino_val_t *v = mino_tx_alter_c(S, not_a_ref, xform_inc,
                                         (void *)(intptr_t)1, env);
        REQUIRE(v == NULL, "alter_c(int) should return NULL");
    }
}

/* Verify watches registered from Clojure fire after a C-side tx
 * commits. Uses the standalone binary's eval path to install the
 * watch and read its observed value back. */
static void test_watch_fires_from_c_tx(mino_state_t *S, mino_env_t *env)
{
    mino_val_t *r = mino_tx_ref(S, mino_int(S, 0));
    struct body_basic_ctx ctx = { r, 0 };
    long long n;

    /* Bind the ref under a known name and install a watch from
     * Clojure that records the new value into a side atom. */
    mino_env_set(S, env, "*c-stm-ref*", r);
    if (mino_eval_string(S,
            "(do (def *c-stm-seen* (atom 0))"
            "    (add-watch *c-stm-ref* :w "
            "      (fn [_ _ _ nv] (reset! *c-stm-seen* nv))))",
            env) == NULL) {
        REQUIRE(0, "watch setup eval failed");
        return;
    }

    /* Drive a commit through the C API. body_basic ends with
     * tentative 10. */
    REQUIRE(mino_tx_run(S, body_basic, &ctx, env) != NULL,
            "tx_run for watch test returned NULL");

    /* Read the side atom from Clojure to confirm watch saw 10. */
    {
        mino_val_t *v = mino_eval_string(S, "@*c-stm-seen*", env);
        REQUIRE(v != NULL && mino_to_int(v, &n) && n == 10,
                "watch did not observe committed value 10");
    }
}

/* C-side prim that runs a transaction body using mino_tx_alter_c.
 * Used by the retry test: registered as `c-incr-ref!` so a Clojure
 * future can drive it from a worker thread. The body increments the
 * ref's int value by 1 plus the user-supplied step. Across retries
 * the user pointer is preserved (the same step value is observed on
 * each iteration), so the eventual committed value reflects exactly
 * one increment per call regardless of how many retry attempts the
 * runner went through. */
struct retry_body_ctx {
    mino_val_t *ref;
    long long   step;
    int         attempts;  /* observed by the body per re-invocation */
};

static mino_val_t *retry_body_xform(mino_state_t *S, mino_val_t *cur,
                                     void *user, mino_env_t *env)
{
    long long n;
    long long step = (long long)(intptr_t)user;
    (void)env;
    if (!mino_to_int(cur, &n)) return NULL;
    return mino_int(S, n + step);
}

static mino_val_t *retry_body(mino_state_t *S, void *user, mino_env_t *env)
{
    struct retry_body_ctx *ctx = (struct retry_body_ctx *)user;
    /* Each invocation (including retries) sees the same step from
     * the user pointer; assert that to lock down the contract. */
    REQUIRE(ctx->step == 1, "retry_body: step lost across retry");
    ctx->attempts++;
    return mino_tx_alter_c(S, ctx->ref, retry_body_xform,
                            (void *)(intptr_t)ctx->step, env);
}

static struct retry_body_ctx *g_retry_ctx_for_prim;

static mino_val_t *prim_c_incr_ref(mino_state_t *S, mino_val_t *args,
                                    mino_env_t *env)
{
    (void)args;
    return mino_tx_run(S, retry_body, g_retry_ctx_for_prim, env);
}

static void test_run_retry_under_contention(mino_state_t *S, mino_env_t *env)
{
    mino_val_t *r = mino_tx_ref(S, mino_int(S, 0));
    struct retry_body_ctx body_ctx = { r, 1, 0 };
    long long  n = 0;
    long long  per_thread = 200;
    long long  workers    = 4;
    char       script[512];

    /* Grant threads so (future ...) works. The test process runs as
     * a host that opts into multi-threading; the standalone ./mino
     * binary does this automatically, but a fresh embedder default
     * is thread_limit=1. */
    mino_set_thread_limit(S, 4);

    g_retry_ctx_for_prim = &body_ctx;
    mino_register_fn(S, env, "c-incr-ref!", prim_c_incr_ref);
    mino_env_set(S, env, "*c-stm-retry-ref*", r);

    /* Drive (workers) Clojure futures that each call the C-side
     * mino_tx_run loop (per-thread) times. Under contention the
     * commit phase will see version conflicts and retry the body;
     * the eventual sum must equal workers * per-thread regardless
     * of how often retry fired. */
    snprintf(script, sizeof(script),
        "(let [futs (doall (for [_ (range %lld)]"
        "                    (future (dotimes [_ %lld] (c-incr-ref!)))))]"
        "  (doseq [f futs] @f)"
        "  @*c-stm-retry-ref*)",
        workers, per_thread);

    {
        mino_val_t *result = mino_eval_string(S, script, env);
        REQUIRE(result != NULL, "retry: futures-driven eval returned NULL");
        if (result == NULL) {
            fprintf(stderr, "retry: error: %s\n", mino_last_error(S));
            return;
        }
        REQUIRE(mino_to_int(result, &n), "retry: result not int");
    }
    REQUIRE(n == workers * per_thread,
            "retry: final ref value != workers * per-thread");
    /* attempts >= successful commits (workers * per_thread).
     * Strictly greater would mean retry fired; equal means the
     * threads serialized fully via mino's state_lock and no commit
     * conflict was observed. Both outcomes preserve the contract:
     * the user pointer survived every body invocation and every
     * commit produced exactly +step. */
    REQUIRE(body_ctx.attempts >= workers * per_thread,
            "retry: attempts < successful commits");
}

/* Pass a ref allocated in another mino_state_t to S's mino_tx_*
 * entries. Each must throw eval/state MST007 ("ref from foreign
 * state") rather than silently mutate the foreign heap. */
static void test_cross_state_ref_throws(mino_state_t *S, mino_env_t *env)
{
    mino_state_t *S2     = mino_state_new();
    mino_env_t   *env2   = mino_new(S2);
    mino_val_t   *foreign_r = mino_tx_ref(S2, mino_int(S2, 0));
    const char   *err;

    /* deref returns NULL but does not throw (the public deref tolerates
     * non-ref input as a NULL-result; cross-state goes through the
     * same NULL path). */
    REQUIRE(mino_tx_ref_deref(S, foreign_r) == NULL,
            "cross-state deref should return NULL");

    /* ref_set must throw MST007. */
    {
        mino_val_t *v = mino_tx_ref_set(S, foreign_r, mino_int(S, 1));
        REQUIRE(v == NULL, "cross-state ref_set should return NULL");
        err = mino_last_error(S);
        REQUIRE(err != NULL && strstr(err, "foreign state") != NULL,
                "cross-state ref_set should mention 'foreign state'");
    }

    /* alter_c must throw MST007. */
    {
        mino_val_t *v = mino_tx_alter_c(S, foreign_r, xform_inc,
                                         (void *)(intptr_t)1, env);
        REQUIRE(v == NULL, "cross-state alter_c should return NULL");
        err = mino_last_error(S);
        REQUIRE(err != NULL && strstr(err, "foreign state") != NULL,
                "cross-state alter_c should mention 'foreign state'");
    }

    /* commute_c must throw MST007. */
    {
        mino_val_t *v = mino_tx_commute_c(S, foreign_r, xform_inc,
                                           (void *)(intptr_t)1, env);
        REQUIRE(v == NULL, "cross-state commute_c should return NULL");
        err = mino_last_error(S);
        REQUIRE(err != NULL && strstr(err, "foreign state") != NULL,
                "cross-state commute_c should mention 'foreign state'");
    }

    /* ensure must throw MST007. */
    {
        mino_val_t *v = mino_tx_ensure(S, foreign_r, env);
        REQUIRE(v == NULL, "cross-state ensure should return NULL");
        err = mino_last_error(S);
        REQUIRE(err != NULL && strstr(err, "foreign state") != NULL,
                "cross-state ensure should mention 'foreign state'");
    }

    /* The foreign state's own ref operations still work normally. */
    {
        long long n;
        mino_val_t *v = mino_tx_ref_deref(S2, foreign_r);
        REQUIRE(v != NULL && mino_to_int(v, &n) && n == 0,
                "foreign state's own ref should still read");
    }

    mino_env_free(S2, env2);
    mino_state_free(S2);
}

/* Inject an agent allocated in S2 into S's env and drive every public
 * agent prim from Clojure code. Each must throw eval/state MST007
 * instead of silently mutating across states. Mirrors the cross-
 * state ref test above; agents lacked the defense before.
 *
 * Agents are an opt-in install so the test installs them on S
 * itself; the foreign agent comes from a fresh S2. */
static void test_cross_state_agent_throws(mino_state_t *S, mino_env_t *env)
{
    mino_state_t *S2     = mino_state_new();
    mino_env_t   *env2   = mino_new(S2);
    mino_val_t   *foreign_a;
    const char   *err;
    static const char *const probes[] = {
        "(send *foreign-agent* inc)",
        "(send-off *foreign-agent* inc)",
        "(await *foreign-agent*)",
        "(await-for 100 *foreign-agent*)",
        "(agent-error *foreign-agent*)",
        "(restart-agent *foreign-agent* 1)",
        "(set-error-handler! *foreign-agent* (fn [_ _] nil))",
        "(error-handler *foreign-agent*)",
        "(set-error-mode! *foreign-agent* :continue)",
        "(error-mode *foreign-agent*)",
        "(add-watch *foreign-agent* :w (fn [_ _ _ _] nil))",
        "(remove-watch *foreign-agent* :w)",
        "(set-validator! *foreign-agent* number?)",
        "(get-validator *foreign-agent*)",
    };
    size_t i;

    /* Install the agent prims on S so (send / await / ...) resolve.
     * S2 needs the same install so its own agent ops still work below.
     * Both states need a thread budget for async send to spawn its
     * worker (default thread_limit == 1 makes send throw MTH001). */
    mino_set_thread_limit(S2, 2);
    mino_install_agent(S, env);
    mino_install_agent(S2, env2);
    foreign_a = mino_agent(S2, mino_int(S2, 0));

    REQUIRE(mino_is_agent(foreign_a), "foreign agent constructor failed");
    mino_env_set(S, env, "*foreign-agent*", foreign_a);

    for (i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        mino_val_t *r = mino_eval_string(S, probes[i], env);
        REQUIRE(r == NULL, "cross-state agent op should error (returned non-NULL)");
        err = mino_last_error(S);
        REQUIRE(err != NULL && strstr(err, "foreign state") != NULL,
                "cross-state agent op should mention 'foreign state'");
    }

    /* Foreign state's own agent ops still work normally. */
    {
        long long n;
        mino_env_set(S2, env2, "*own-agent*", foreign_a);
        mino_eval_string(S2, "(send *own-agent* inc)", env2);
        mino_eval_string(S2, "(await *own-agent*)", env2);
        REQUIRE(foreign_a->as.agent.val != NULL
                && mino_to_int(foreign_a->as.agent.val, &n)
                && n == 1,
                "foreign state's own agent should still accept its own send");
    }

    mino_env_free(S2, env2);
    mino_state_free(S2);
}

/* shutdown-agents flips a state-level flag that makes every
 * subsequent send / send-off throw MST008. The flag is permanent
 * (no reverse), so this test runs in a private state to avoid
 * poisoning the rest of the suite. */
static void test_shutdown_agents_seals_state(void)
{
    mino_state_t *S   = mino_state_new();
    mino_env_t   *env = mino_new(S);
    mino_set_thread_limit(S, 2);
    mino_install_agent(S, env);
    {
        /* Pre-shutdown: send works. */
        mino_val_t *r = mino_eval_string(S,
            "(let [a (agent 0)] (send a inc) (await a) @a)", env);
        long long n = 0;
        REQUIRE(r != NULL && mino_to_int(r, &n) && n == 1,
                "pre-shutdown send should publish");
    }
    {
        /* Trigger shutdown. */
        mino_val_t *r = mino_eval_string(S, "(shutdown-agents)", env);
        REQUIRE(r != NULL, "shutdown-agents returned NULL");
    }
    {
        /* Post-shutdown: send must throw MST008. */
        const char *err;
        mino_val_t *r = mino_eval_string(S,
            "(send (agent 0) inc)", env);
        REQUIRE(r == NULL, "post-shutdown send should error");
        err = mino_last_error(S);
        REQUIRE(err != NULL && strstr(err, "shut down") != NULL,
                "post-shutdown error should mention 'shut down'");
    }
    {
        /* Idempotent: calling shutdown again is fine. */
        mino_val_t *r = mino_eval_string(S, "(shutdown-agents)", env);
        REQUIRE(r != NULL, "second shutdown-agents should not error");
    }
    mino_env_free(S, env);
    mino_state_free(S);
}

/* shutdown-agents called from inside an agent action body would
 * try to pthread_join the worker thread that's running the action.
 * Detect and throw rather than self-deadlock. */
static void test_shutdown_agents_self_call_throws(void)
{
    mino_state_t *S   = mino_state_new();
    mino_env_t   *env = mino_new(S);
    mino_set_thread_limit(S, 2);
    mino_install_agent(S, env);
    {
        /* The action body calls shutdown-agents. The worker
         * catches the throw and the agent's value is set to a
         * sentinel; the agent itself stays clean. */
        mino_val_t *r = mino_eval_string(S,
            "(let [a (agent 0)]"
            "  (send a (fn [_] (try (shutdown-agents) :unreachable"
            "                       (catch e :saw-throw))))"
            "  (await a)"
            "  (deref a))",
            env);
        REQUIRE(r != NULL, "self-shutdown action should not abort the runtime");
        REQUIRE(r->type == MINO_KEYWORD,
                "self-shutdown action's catch should publish :saw-throw");
    }
    mino_env_free(S, env);
    mino_state_free(S);
}

/* Sleep for N milliseconds inside an action, so an embed-side test
 * can block an agent's in-flight long enough to make await_for
 * actually time out. Registered in env as `test-sleep`. */
static mino_val_t *prim_test_sleep(mino_state_t *S, mino_val_t *args,
                                    mino_env_t *env)
{
    long long ms = 0;
    (void)env;
    if (mino_is_cons(args)) {
        (void)mino_to_int(args->as.cons.car, &ms);
    }
    test_sleep_ms(ms);
    return mino_nil(S);
}

/* Drive every public C-API agent entry from C. Validates that the
 * mino_send / mino_send_off / mino_await / mino_await_for /
 * mino_agent_error / mino_restart_agent perimeter is wired up the
 * same way the Clojure-level prims are. */
static void test_c_api_agents(void)
{
    mino_state_t *S   = mino_state_new();
    mino_env_t   *env = mino_new(S);
    mino_set_thread_limit(S, 4);
    mino_install_agent(S, env);

    /* Build (fn [v] (inc v)) once, reuse across sends. */
    mino_val_t *inc_fn = mino_eval_string(S, "(fn [v] (inc v))", env);
    REQUIRE(inc_fn != NULL, "inc fn should compile");

    /* mino_send: enqueue + await + read. */
    {
        mino_val_t *a = mino_agent(S, mino_int(S, 0));
        mino_val_t *agents[2];
        REQUIRE(mino_send(S, a, inc_fn, NULL) != NULL,
                "mino_send should return the agent");
        REQUIRE(mino_send(S, a, inc_fn, NULL) != NULL,
                "second mino_send should also return the agent");
        agents[0] = a; agents[1] = NULL;
        REQUIRE(mino_await(S, agents) != NULL,
                "mino_await should return non-NULL on success");
        {
            long long n = 0;
            REQUIRE(mino_to_int(a->as.agent.val, &n),
                    "agent value should be int after sends");
            REQUIRE(n == 2,
                    "agent should have value 2 after two inc sends");
        }
    }

    /* mino_send_off: separate pool, same eval-lock serialization. */
    {
        mino_val_t *a = mino_agent(S, mino_int(S, 100));
        mino_val_t *agents[2];
        REQUIRE(mino_send_off(S, a, inc_fn, NULL) != NULL,
                "mino_send_off should return the agent");
        agents[0] = a; agents[1] = NULL;
        REQUIRE(mino_await(S, agents) != NULL,
                "mino_await across send-off should succeed");
        {
            long long n = 0;
            REQUIRE(mino_to_int(a->as.agent.val, &n) && n == 101,
                    "agent should have value 101 after one inc send-off");
        }
    }

    /* mino_await_for trivial path: zero in-flight means it returns 1
     * immediately without blocking. */
    {
        mino_val_t *a = mino_agent(S, mino_int(S, 0));
        mino_val_t *agents[2];
        agents[0] = a; agents[1] = NULL;
        REQUIRE(mino_await_for(S, 50, agents) == 1,
                "mino_await_for should return 1 when nothing is queued");
    }

    /* mino_await_for fires the timeout: register a C-side test-sleep
     * primitive, queue an action that sleeps 250ms, then call
     * mino_await_for with a 50ms deadline. The first call must time
     * out (return 0). A subsequent mino_await with no deadline must
     * drain. Asserts both sides of the perimeter -- the timed wait
     * actually waits, and the cv broadcast on action completion
     * wakes the next await. */
    {
        mino_val_t *a = mino_agent(S, mino_int(S, 0));
        mino_val_t *agents[2];
        mino_val_t *slow_fn;
        mino_register_fn(S, env, "test-sleep", prim_test_sleep);
        slow_fn = mino_eval_string(S,
            "(fn [v] (test-sleep 250) (inc v))", env);
        REQUIRE(slow_fn != NULL, "slow_fn should compile");
        REQUIRE(mino_send(S, a, slow_fn, NULL) != NULL,
                "mino_send of slow action should enqueue");
        agents[0] = a; agents[1] = NULL;
        REQUIRE(mino_await_for(S, 50, agents) == 0,
                "mino_await_for should return 0 when the deadline "
                "fires before the action completes");
        /* Action is still running on the worker; await with no
         * deadline drains it. */
        REQUIRE(mino_await(S, agents) != NULL,
                "mino_await must drain the action started above");
        {
            long long n = 0;
            REQUIRE(mino_to_int(a->as.agent.val, &n),
                    "agent value should be int after the slow action runs");
            REQUIRE(n == 1,
                    "agent value should be 1 after one slow inc action");
        }
    }

    /* mino_agent_error / mino_restart_agent: install a validator
     * that rejects negatives, drive the agent into the failed
     * state, then restart with a clean value. */
    {
        mino_val_t *a = mino_eval_string(S,
            "(agent 0 :validator (fn [v] (>= v 0)))", env);
        mino_val_t *dec_fn = mino_eval_string(S, "(fn [v] (dec v))", env);
        mino_val_t *agents[2];
        REQUIRE(a != NULL && mino_is_agent(a), "agent should be created");
        REQUIRE(mino_send(S, a, dec_fn, NULL) != NULL,
                "send should accept the action even though it will fail");
        agents[0] = a; agents[1] = NULL;
        mino_await(S, agents);
        REQUIRE(mino_agent_error(S, a) != NULL,
                "mino_agent_error should expose the validator failure");
        /* Subsequent send to a failed :fail agent should throw. */
        REQUIRE(mino_send(S, a, dec_fn, NULL) == NULL,
                "send to failed :fail agent should throw / return NULL");
        REQUIRE(mino_restart_agent(S, a, mino_int(S, 5), 1) != NULL,
                "mino_restart_agent should clear the failure");
        REQUIRE(mino_agent_error(S, a) == NULL,
                "agent_error should be NULL after restart");
        {
            long long n = 0;
            REQUIRE(mino_to_int(a->as.agent.val, &n) && n == 5,
                    "agent value should be reset to 5 after restart");
        }
    }

    /* Cross-state guard: pass an agent from S2 to mino_send running
     * on S; must throw MST007 and return NULL. */
    {
        mino_state_t *S2 = mino_state_new();
        mino_env_t   *env2 = mino_new(S2);
        mino_val_t   *foreign;
        mino_set_thread_limit(S2, 2);
        mino_install_agent(S2, env2);
        foreign = mino_agent(S2, mino_int(S2, 0));
        REQUIRE(mino_send(S, foreign, inc_fn, NULL) == NULL,
                "mino_send must reject a foreign-state agent");
        REQUIRE(mino_send_off(S, foreign, inc_fn, NULL) == NULL,
                "mino_send_off must reject a foreign-state agent");
        REQUIRE(mino_agent_error(S, foreign) == NULL,
                "mino_agent_error must reject a foreign-state agent");
        REQUIRE(mino_restart_agent(S, foreign, mino_int(S, 0), 0) == NULL,
                "mino_restart_agent must reject a foreign-state agent");
        mino_env_free(S2, env2);
        mino_state_free(S2);
    }

    mino_env_free(S, env);
    mino_state_free(S);
}

int main(void)
{
    mino_state_t *S   = mino_state_new();
    mino_env_t   *env = mino_new(S);

    test_predicate_and_construction(S);
    test_run_basic(S, env);
    test_run_refset(S, env);
    test_run_commute_only(S, env);
    test_outside_tx_throws(S, env);
    test_type_check_throws(S, env);
    test_watch_fires_from_c_tx(S, env);
    test_cross_state_ref_throws(S, env);
    test_cross_state_agent_throws(S, env);
    test_run_retry_under_contention(S, env);
    test_shutdown_agents_seals_state();
    test_shutdown_agents_self_call_throws();
    test_c_api_agents();

    mino_env_free(S, env);
    mino_state_free(S);

    if (failures > 0) {
        fprintf(stderr, "embed_stm_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("embed_stm_test: all checks passed\n");
    return 0;
}
