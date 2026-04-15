/*
 * actor_stress_test.c -- stress test the actor model at scale.
 * Tests thousands of actors, supervision trees, fan-out/fan-in,
 * ring topologies, and actor lifecycle churn.
 */

#include "mino.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int tests_run    = 0;
static int tests_passed = 0;

#define TEST(name) \
    do { printf("  %-55s ", name); tests_run++; } while (0)
#define PASS() \
    do { printf("OK\n"); tests_passed++; return; } while (0)
#define FAIL(msg) \
    do { printf("FAIL: %s\n", msg); return; } while (0)
#define ASSERT(cond, msg) \
    do { if (!(cond)) FAIL(msg); } while (0)

/* --- Mass actor spawn/free --- */

static void test_spawn_1000_actors(void)
{
    TEST("Spawn and free 1000 actors");
    mino_actor_t *actors[1000];
    int i;

    for (i = 0; i < 1000; i++) {
        actors[i] = mino_actor_new();
        ASSERT(actors[i] != NULL, "actor_new failed");
    }

    /* Send a message to each */
    mino_state_t *host = mino_state_new();
    for (i = 0; i < 1000; i++) {
        mino_actor_send(actors[i], host, mino_int(host, (long long)i));
    }

    /* Receive from each */
    for (i = 0; i < 1000; i++) {
        mino_val_t *msg = mino_actor_recv(actors[i]);
        ASSERT(msg != NULL, "recv null");
        long long val;
        ASSERT(mino_to_int(msg, &val), "not int");
        ASSERT(val == (long long)i, "wrong value");
    }

    /* Free all */
    for (i = 0; i < 1000; i++) {
        mino_actor_free(actors[i]);
    }
    mino_state_free(host);
    PASS();
}

/* --- Actor ring: token passing --- */

#define RING_SIZE 50

static void test_actor_ring(void)
{
    TEST("Actor ring: pass token around 50-node ring");
    mino_actor_t *ring[RING_SIZE];
    mino_state_t *host = mino_state_new();
    int i, lap;

    for (i = 0; i < RING_SIZE; i++) {
        ring[i] = mino_actor_new();
    }

    /* Pass a token (integer) around the ring 10 times.
     * Each node increments it by 1. */
    long long token = 0;
    for (lap = 0; lap < 10; lap++) {
        for (i = 0; i < RING_SIZE; i++) {
            /* Send token to ring[i] */
            mino_actor_send(ring[i], host, mino_int(host, token));

            /* Ring[i] receives it */
            mino_val_t *msg = mino_actor_recv(ring[i]);
            mino_state_t *as = mino_actor_state(ring[i]);
            mino_env_t *ae = mino_actor_env(ring[i]);

            /* Process: increment by 1 */
            mino_env_set(as, ae, "__t", msg);
            mino_val_t *result = mino_eval_string(as, "(+ __t 1)", ae);
            ASSERT(result != NULL, "eval failed in ring node");

            /* Clone result back to host for next hop */
            mino_val_t *cloned = mino_clone(host, as, result);
            ASSERT(cloned != NULL, "clone failed");
            mino_to_int(cloned, &token);
        }
    }

    ASSERT(token == RING_SIZE * 10, "wrong final token");

    for (i = 0; i < RING_SIZE; i++) {
        mino_actor_free(ring[i]);
    }
    mino_state_free(host);
    PASS();
}

/* --- Fan-out / Fan-in: map-reduce with actors --- */

#define NUM_WORKERS 20

static void test_actor_map_reduce(void)
{
    TEST("Actor map-reduce: scatter-gather across 20 workers");
    mino_actor_t *workers[NUM_WORKERS];
    mino_state_t *host = mino_state_new();
    mino_env_t *host_env = mino_new(host);
    int i;
    long long total = 0;

    /* Create workers that compute square of input */
    for (i = 0; i < NUM_WORKERS; i++) {
        workers[i] = mino_actor_new();
        mino_eval_string(mino_actor_state(workers[i]),
            "(defn work (x) (* x x))", mino_actor_env(workers[i]));
    }

    /* Scatter: send work items */
    for (i = 0; i < NUM_WORKERS; i++) {
        mino_actor_send(workers[i], host, mino_int(host, (long long)(i + 1)));
    }

    /* Gather: collect results */
    for (i = 0; i < NUM_WORKERS; i++) {
        mino_val_t *msg = mino_actor_recv(workers[i]);
        mino_state_t *ws = mino_actor_state(workers[i]);
        mino_env_t *we = mino_actor_env(workers[i]);

        mino_env_set(ws, we, "__input", msg);
        mino_val_t *result = mino_eval_string(ws, "(work __input)", we);
        ASSERT(result != NULL, "worker eval failed");

        mino_val_t *cloned = mino_clone(host, ws, result);
        long long val;
        mino_to_int(cloned, &val);
        total += val;
    }

    /* Sum of squares 1..20 = 2870 */
    ASSERT(total == 2870, "wrong total");

    for (i = 0; i < NUM_WORKERS; i++) {
        mino_actor_free(workers[i]);
    }
    mino_env_free(host, host_env);
    mino_state_free(host);
    PASS();
}

/* --- Supervision tree: 2-level hierarchy --- */

typedef struct {
    mino_actor_t *actor;
    int           restart_count;
    const char   *init_code;
} supervised_t;

static void restart_child(supervised_t *child, mino_state_t *host)
{
    if (child->actor) mino_actor_free(child->actor);
    child->actor = mino_actor_new();
    mino_eval_string(mino_actor_state(child->actor),
                     child->init_code, mino_actor_env(child->actor));
    child->restart_count++;
    (void)host;
}

static void test_supervision_tree(void)
{
    TEST("Supervision tree: supervisor with 5 children, crash recovery");
    mino_state_t *host = mino_state_new();
    supervised_t children[5];
    int i;

    const char *init = "(defn handle (x) (if (= x \"crash\") (throw \"boom\") (str \"ok:\" x)))";

    for (i = 0; i < 5; i++) {
        children[i].actor = NULL;
        children[i].restart_count = 0;
        children[i].init_code = init;
        restart_child(&children[i], host);
    }

    /* Send work to children: normal, normal, crash, normal, crash */
    const char *inputs[] = {"hello", "world", "crash", "mino", "crash"};
    int expected_restarts[] = {0, 0, 1, 0, 1};

    for (i = 0; i < 5; i++) {
        mino_actor_send(children[i].actor, host,
                        mino_string(host, inputs[i]));

        mino_val_t *msg = mino_actor_recv(children[i].actor);
        mino_state_t *ws = mino_actor_state(children[i].actor);
        mino_env_t *we = mino_actor_env(children[i].actor);

        mino_env_set(ws, we, "__m", msg);
        mino_val_t *fn = mino_env_get(we, "handle");
        mino_val_t *out = NULL;
        int rc = mino_pcall(ws, fn,
                            mino_cons(ws, msg, mino_nil(ws)), we, &out);

        if (rc != 0) {
            /* Supervisor restarts the child */
            restart_child(&children[i], host);
        }
    }

    /* Verify restart counts */
    for (i = 0; i < 5; i++) {
        /* restart_count includes the initial start, so crashed ones have 2 */
        int extra = expected_restarts[i];
        ASSERT(children[i].restart_count == 1 + extra, "wrong restart count");
    }

    for (i = 0; i < 5; i++) {
        mino_actor_free(children[i].actor);
    }
    mino_state_free(host);
    PASS();
}

/* --- Actor with stateful accumulator --- */

static void test_actor_stateful(void)
{
    TEST("Actor with mutable state via atoms");
    mino_state_t *host = mino_state_new();
    mino_actor_t *worker = mino_actor_new();
    mino_state_t *ws = mino_actor_state(worker);
    mino_env_t *we = mino_actor_env(worker);
    int i;

    /* Actor maintains a running sum in an atom */
    mino_eval_string(ws,
        "(def *total* (atom 0))"
        "(defn add-to-total (n) (swap! *total* + n) @*total*)", we);

    /* Send 100 values */
    for (i = 1; i <= 100; i++) {
        mino_actor_send(worker, host, mino_int(host, (long long)i));
        mino_val_t *msg = mino_actor_recv(worker);
        mino_env_set(ws, we, "__n", msg);
        mino_eval_string(ws, "(add-to-total __n)", we);
    }

    /* Check total: 1+2+...+100 = 5050 */
    mino_val_t *total = mino_eval_string(ws, "@*total*", we);
    ASSERT(total != NULL, "deref failed");
    long long val;
    ASSERT(mino_to_int(total, &val), "not int");
    ASSERT(val == 5050, "wrong total");

    mino_actor_free(worker);
    mino_state_free(host);
    PASS();
}

/* --- Actor with complex data exchange --- */

static void test_actor_complex_messages(void)
{
    TEST("Actor exchange: deeply nested maps and vectors");
    mino_state_t *host = mino_state_new();
    mino_env_t *host_env = mino_new(host);
    mino_actor_t *worker = mino_actor_new();
    mino_state_t *ws = mino_actor_state(worker);
    mino_env_t *we = mino_actor_env(worker);

    mino_eval_string(ws,
        "(defn transform (data)"
        "  {:status :ok"
        "   :count (count (get data :items))"
        "   :total (reduce + 0 (get data :items))})", we);

    /* Build a complex message in host */
    mino_val_t *msg = mino_eval_string(host,
        "{:items [10 20 30 40 50] :meta {:source \"host\" :version 1}}", host_env);
    ASSERT(msg != NULL, "build msg failed");

    /* Send via mailbox (serialization) */
    mino_actor_send(worker, host, msg);

    /* Worker receives and processes */
    mino_val_t *received = mino_actor_recv(worker);
    ASSERT(received != NULL, "recv null");

    mino_env_set(ws, we, "__data", received);
    mino_val_t *result = mino_eval_string(ws, "(transform __data)", we);
    ASSERT(result != NULL, "transform failed");
    ASSERT(result->type == MINO_MAP, "not a map");

    /* Clone result back to host and verify */
    mino_val_t *cloned = mino_clone(host, ws, result);
    ASSERT(cloned != NULL, "clone back failed");
    mino_env_set(host, host_env, "__result", cloned);

    mino_val_t *total = mino_eval_string(host, "(get __result :total)", host_env);
    long long val;
    ASSERT(mino_to_int(total, &val), "not int");
    ASSERT(val == 150, "wrong total");

    mino_val_t *count = mino_eval_string(host, "(get __result :count)", host_env);
    ASSERT(mino_to_int(count, &val), "not int");
    ASSERT(val == 5, "wrong count");

    mino_actor_free(worker);
    mino_env_free(host, host_env);
    mino_state_free(host);
    PASS();
}

/* --- Churn: rapidly create/use/destroy actors --- */

static void test_actor_churn(void)
{
    TEST("Actor churn: create, use, destroy 500 actors rapidly");
    mino_state_t *host = mino_state_new();
    long long total = 0;
    int i;

    for (i = 0; i < 500; i++) {
        mino_actor_t *a = mino_actor_new();
        mino_state_t *as = mino_actor_state(a);
        mino_env_t *ae = mino_actor_env(a);

        /* Each actor computes something different */
        mino_actor_send(a, host, mino_int(host, (long long)i));
        mino_val_t *msg = mino_actor_recv(a);
        mino_env_set(as, ae, "__x", msg);
        mino_val_t *r = mino_eval_string(as, "(+ __x __x)", ae);

        if (r != NULL) {
            mino_val_t *cloned = mino_clone(host, as, r);
            long long val;
            if (cloned && mino_to_int(cloned, &val)) {
                total += val;
            }
        }

        mino_actor_free(a);
    }

    /* Sum of 2*i for i=0..499 = 2 * (499*500/2) = 249500 */
    ASSERT(total == 249500, "wrong churn total");
    mino_state_free(host);
    PASS();
}

/* --- spawn from mino code --- */

static void test_mino_spawn_actors(void)
{
    TEST("Spawn actors from mino code");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);

    /* Spawn an actor from mino, send it a message, have it process */
    mino_val_t *r = mino_eval_string(S,
        "(def worker (spawn \"(defn double (x) (* x 2))\"))"
        "(send! worker 21)"
        ":sent", env);
    ASSERT(r != NULL, "spawn/send failed");

    /* The actor should have the message in its mailbox.
     * We can verify the actor handle exists. */
    mino_val_t *worker = mino_env_get(env, "worker");
    ASSERT(worker != NULL, "worker not bound");
    ASSERT(worker->type == MINO_HANDLE, "not a handle");
    ASSERT(strcmp(worker->as.handle.tag, "actor") == 0, "not actor");

    mino_env_free(S, env);
    mino_state_free(S);
    PASS();
}

/* --- Main --- */

int main(void)
{
    printf("Actor stress tests\n");
    printf("------------------\n");

    test_spawn_1000_actors();
    test_actor_ring();
    test_actor_map_reduce();
    test_supervision_tree();
    test_actor_stateful();
    test_actor_complex_messages();
    test_actor_churn();
    test_mino_spawn_actors();

    printf("------------------\n");
    printf("%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
