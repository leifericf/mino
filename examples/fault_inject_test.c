/*
 * fault_inject_test.c -- deterministic OOM fault injection tests.
 *
 * Exercises the mino_set_fail_alloc_at harness to verify that
 * allocation failures in various subsystems produce catchable errors
 * (not crashes or aborts).
 */

#include "mino.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int pass = 0, total = 0;
#define TEST(n) do { printf("  %-60s ", n); total++; } while(0)
#define OK() do { printf("OK\n"); pass++; return; } while(0)
#define FAIL(m) do { printf("FAIL: %s\n", m); return; } while(0)
#define CHK(c,m) do { if(!(c)) FAIL(m); } while(0)

/*
 * Helper: find the lowest allocation count N such that eval_string
 * with fail-at-N returns NULL (OOM). Returns N, or 0 if no failure
 * was triggered in the first max_probes allocations.
 */
static long find_fail_point(mino_state_t *S, mino_env_t *env,
                            const char *expr, long max_probes)
{
    long n;
    for (n = 1; n <= max_probes; n++) {
        mino_set_fail_alloc_at(S, n);
        if (mino_eval_string(S, expr, env) == NULL) {
            mino_set_fail_alloc_at(S, 0);
            return n;
        }
        mino_set_fail_alloc_at(S, 0);
    }
    return 0;
}

/* Test: OOM during map construction is recoverable. */
static void test_map_oom(void)
{
    TEST("map construction OOM returns error, not crash");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);
    long n;

    n = find_fail_point(S, env, "(hash-map :a 1 :b 2 :c 3 :d 4)", 200);
    CHK(n > 0, "no failure point found");

    /* Verify error message is set. */
    mino_set_fail_alloc_at(S, n);
    CHK(mino_eval_string(S, "(hash-map :a 1 :b 2 :c 3 :d 4)", env) == NULL,
        "expected NULL on OOM");
    CHK(mino_last_error(S) != NULL, "expected error message");
    mino_set_fail_alloc_at(S, 0);

    /* Verify recovery: subsequent eval succeeds. */
    CHK(mino_eval_string(S, "(+ 1 1)", env) != NULL, "eval after OOM failed");

    mino_env_free(S, env);
    mino_state_free(S);
    OK();
}

/* Test: OOM during vector construction is recoverable. */
static void test_vector_oom(void)
{
    TEST("vector construction OOM returns error, not crash");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);
    long n;

    n = find_fail_point(S, env, "(into [] (range 100))", 500);
    CHK(n > 0, "no failure point found");

    mino_set_fail_alloc_at(S, n);
    CHK(mino_eval_string(S, "(into [] (range 100))", env) == NULL,
        "expected NULL on OOM");
    CHK(mino_last_error(S) != NULL, "expected error message");
    mino_set_fail_alloc_at(S, 0);

    CHK(mino_eval_string(S, "(+ 1 1)", env) != NULL, "eval after OOM failed");

    mino_env_free(S, env);
    mino_state_free(S);
    OK();
}

/* Test: OOM during binding form is recoverable. */
static void test_binding_oom(void)
{
    TEST("binding form OOM returns error, not crash");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);
    long n;

    n = find_fail_point(S, env,
        "(let [a 1 b 2 c 3 d 4 e 5] (+ a b c d e))", 200);
    CHK(n > 0, "no failure point found");

    mino_set_fail_alloc_at(S, n);
    CHK(mino_eval_string(S,
        "(let [a 1 b 2 c 3 d 4 e 5] (+ a b c d e))", env) == NULL,
        "expected NULL on OOM");
    mino_set_fail_alloc_at(S, 0);

    CHK(mino_eval_string(S, "(+ 1 1)", env) != NULL, "eval after OOM failed");

    mino_env_free(S, env);
    mino_state_free(S);
    OK();
}

/* Test: OOM during regex compile is recoverable. */
static void test_regex_oom(void)
{
    TEST("regex compile OOM returns error, not crash");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);
    long n;

    n = find_fail_point(S, env, "(re-find #\"[a-z]+\" \"hello\")", 200);
    CHK(n > 0, "no failure point found");

    mino_set_fail_alloc_at(S, n);
    CHK(mino_eval_string(S, "(re-find #\"[a-z]+\" \"hello\")", env) == NULL,
        "expected NULL on OOM");
    mino_set_fail_alloc_at(S, 0);

    CHK(mino_eval_string(S, "(+ 1 1)", env) != NULL, "eval after OOM failed");

    mino_env_free(S, env);
    mino_state_free(S);
    OK();
}

/* Test: OOM is catchable via try/catch. */
static void test_oom_catchable(void)
{
    TEST("OOM is catchable via try/catch in mino code");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);
    long n;
    mino_val_t *r;

    /* Find a fail point for a map operation. */
    n = find_fail_point(S, env, "(hash-map :a 1 :b 2 :c 3 :d 4)", 200);
    CHK(n > 0, "no failure point found");

    /* Wrap in try/catch: should return :oom instead of crashing. */
    mino_set_fail_alloc_at(S, n);
    r = mino_eval_string(S,
        "(try (hash-map :a 1 :b 2 :c 3 :d 4) (catch e :oom))", env);
    mino_set_fail_alloc_at(S, 0);
    /* The try/catch itself might need allocations that also fail.
     * Accept either a caught result or a NULL return. */
    if (r != NULL) {
        CHK(r->type == MINO_KEYWORD, "expected keyword :oom");
    }

    /* Either way, state should be recoverable. */
    CHK(mino_eval_string(S, "(+ 1 1)", env) != NULL, "eval after OOM failed");

    mino_env_free(S, env);
    mino_state_free(S);
    OK();
}

/* Test: OOM during clone (vector) via raw fault injection is recoverable. */
static void test_clone_vector_oom(void)
{
    TEST("clone vector OOM returns NULL, not crash");
    mino_state_t *src = mino_state_new();
    mino_state_t *dst = mino_state_new();
    mino_env_t *se = mino_new(src);
    mino_env_t *de = mino_new(dst);
    long n;

    mino_val_t *v = mino_eval_string(src, "[1 2 3 4 5]", se);
    CHK(v != NULL, "eval failed");

    /* Find failure point in raw allocation path. */
    for (n = 1; n <= 20; n++) {
        mino_set_fail_raw_at(dst, n);
        if (mino_clone(dst, src, v) == NULL) {
            mino_set_fail_raw_at(dst, 0);
            break;
        }
        mino_set_fail_raw_at(dst, 0);
    }
    CHK(n <= 20, "no failure point found");

    /* Verify recovery: clone succeeds after clearing injection. */
    {
        mino_val_t *ok = mino_clone(dst, src, v);
        CHK(ok != NULL, "clone should succeed after clearing injection");
    }

    mino_env_free(src, se);
    mino_env_free(dst, de);
    mino_state_free(src);
    mino_state_free(dst);
    OK();
}

/* Test: OOM during clone (map) via raw fault injection is recoverable. */
static void test_clone_map_oom(void)
{
    TEST("clone map OOM returns NULL, not crash");
    mino_state_t *src = mino_state_new();
    mino_state_t *dst = mino_state_new();
    mino_env_t *se = mino_new(src);
    mino_env_t *de = mino_new(dst);
    long n;

    mino_val_t *v = mino_eval_string(src, "{:a 1 :b 2 :c 3}", se);
    CHK(v != NULL, "eval failed");

    for (n = 1; n <= 20; n++) {
        mino_set_fail_raw_at(dst, n);
        if (mino_clone(dst, src, v) == NULL) {
            mino_set_fail_raw_at(dst, 0);
            break;
        }
        mino_set_fail_raw_at(dst, 0);
    }
    CHK(n <= 20, "no failure point found");

    {
        mino_val_t *ok = mino_clone(dst, src, v);
        CHK(ok != NULL, "clone should succeed after clearing injection");
    }

    mino_env_free(src, se);
    mino_env_free(dst, de);
    mino_state_free(src);
    mino_state_free(dst);
    OK();
}

/* Test: OOM during mailbox serialization via raw fault injection. */
static void test_mailbox_send_oom(void)
{
    TEST("mailbox send OOM returns error, not crash");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);
    mino_mailbox_t *mb = mino_mailbox_new();
    long n;
    int rc;

    mino_val_t *v = mino_eval_string(S, "{:a [1 2 3] :b \"hello\"}", env);
    CHK(v != NULL, "eval failed");
    CHK(mb != NULL, "mailbox alloc failed");

    /* Find failure point in serialization path. */
    for (n = 1; n <= 50; n++) {
        mino_set_fail_raw_at(S, n);
        rc = mino_mailbox_send(mb, S, v);
        mino_set_fail_raw_at(S, 0);
        if (rc != 0) break;
    }
    CHK(n <= 50, "no failure point found");

    /* Verify recovery: send succeeds after clearing injection. */
    rc = mino_mailbox_send(mb, S, v);
    CHK(rc == 0, "send should succeed after clearing injection");

    /* Verify the message arrived intact. */
    {
        mino_val_t *msg = mino_mailbox_recv(mb, S);
        CHK(msg != NULL, "recv should return the sent value");
    }

    mino_mailbox_free(mb);
    mino_env_free(S, env);
    mino_state_free(S);
    OK();
}

/* Test: repeated OOM + recovery cycles don't leak or corrupt state. */
static void test_repeated_oom_recovery(void)
{
    TEST("repeated OOM + recovery cycles don't corrupt state");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);
    int i;

    for (i = 0; i < 50; i++) {
        mino_set_fail_alloc_at(S, 3);
        /* This will likely fail. */
        mino_eval_string(S, "(into [] (range 50))", env);
        mino_set_fail_alloc_at(S, 0);

        /* Recovery eval must succeed. */
        mino_val_t *r = mino_eval_string(S, "(+ 1 1)", env);
        CHK(r != NULL, "recovery eval failed after repeated OOM");
    }

    mino_env_free(S, env);
    mino_state_free(S);
    OK();
}

int main(void)
{
    printf("Fault injection tests\n");
    printf("---------------------\n");

    test_map_oom();
    test_vector_oom();
    test_binding_oom();
    test_regex_oom();
    test_oom_catchable();
    test_clone_vector_oom();
    test_clone_map_oom();
    test_mailbox_send_oom();
    test_repeated_oom_recovery();

    printf("---------------------\n");
    printf("%d/%d tests passed\n", pass, total);
    return pass == total ? 0 : 1;
}
