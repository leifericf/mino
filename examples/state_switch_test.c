/*
 * state_switch_test.c -- test S_ state pointer integrity across
 * interleaved operations on multiple states.
 */

#include "mino.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static int pass = 0, total = 0;
#define TEST(name) do { printf("  %-55s ", name); total++; } while(0)
#define OK() do { printf("OK\n"); pass++; return; } while(0)
#define FAIL(m) do { printf("FAIL: %s\n", m); return; } while(0)
#define CHK(c,m) do { if(!(c)) FAIL(m); } while(0)

/* Test: interleave eval_string calls across two states */
static void test_interleaved_eval(void)
{
    TEST("Interleaved eval_string across two states");
    mino_state_t *s1 = mino_state_new();
    mino_state_t *s2 = mino_state_new();
    mino_env_t *e1 = mino_new(s1);
    mino_env_t *e2 = mino_new(s2);
    int i;
    long long v1, v2;

    mino_eval_string(s1, "(def counter (atom 0))", e1);
    mino_eval_string(s2, "(def counter (atom 100))", e2);

    for (i = 0; i < 50; i++) {
        mino_eval_string(s1, "(swap! counter + 1)", e1);
        mino_eval_string(s2, "(swap! counter + 1)", e2);
    }

    mino_val_t *r1 = mino_eval_string(s1, "@counter", e1);
    mino_val_t *r2 = mino_eval_string(s2, "@counter", e2);
    CHK(r1 && r2, "eval failed");
    CHK(mino_to_int(r1, &v1), "not int");
    CHK(mino_to_int(r2, &v2), "not int");
    CHK(v1 == 50, "s1 counter wrong");
    CHK(v2 == 150, "s2 counter wrong");

    mino_env_free(s1, e1); mino_env_free(s2, e2);
    mino_state_free(s1); mino_state_free(s2);
    OK();
}

/* Test: create values in one state, try to use in another */
static void test_cross_state_value_safety(void)
{
    TEST("Values from one state used via eval in another (via clone)");
    mino_state_t *s1 = mino_state_new();
    mino_state_t *s2 = mino_state_new();
    mino_env_t *e1 = mino_new(s1);
    mino_env_t *e2 = mino_new(s2);

    mino_val_t *v = mino_eval_string(s1, "{:data [1 2 3] :name \"test\"}", e1);
    CHK(v != NULL, "eval failed");

    /* Clone into s2 */
    mino_val_t *cloned = mino_clone(s2, s1, v);
    CHK(cloned != NULL, "clone failed");

    /* Use cloned value in s2's eval */
    mino_env_set(s2, e2, "imported", cloned);
    mino_val_t *r = mino_eval_string(s2, "(reduce + 0 (get imported :data))", e2);
    CHK(r != NULL, "eval in s2 failed");
    long long sum;
    CHK(mino_to_int(r, &sum), "not int");
    CHK(sum == 6, "wrong sum");

    mino_env_free(s1, e1); mino_env_free(s2, e2);
    mino_state_free(s1); mino_state_free(s2);
    OK();
}

/* Test: explicit state parameter inside a primitive callback */
static mino_val_t *prim_check_state(mino_state_t *S, mino_val_t *args,
                                     mino_env_t *env)
{
    (void)args; (void)env;
    /* S is the state we're evaluating in, passed explicitly */
    return mino_int(S, 42);
}

static void test_current_state_in_primitive(void)
{
    TEST("explicit state correct inside primitive callback");
    mino_state_t *s1 = mino_state_new();
    mino_state_t *s2 = mino_state_new();
    mino_env_t *e1 = mino_new(s1);
    mino_env_t *e2 = mino_new(s2);

    mino_register_fn(s1, e1, "check-state", prim_check_state);
    mino_register_fn(s2, e2, "check-state", prim_check_state);

    mino_val_t *r1 = mino_eval_string(s1, "(check-state)", e1);
    mino_val_t *r2 = mino_eval_string(s2, "(check-state)", e2);
    CHK(r1 && r2, "eval failed");
    long long v1, v2;
    CHK(mino_to_int(r1, &v1) && v1 == 42, "wrong from s1");
    CHK(mino_to_int(r2, &v2) && v2 == 42, "wrong from s2");

    mino_env_free(s1, e1); mino_env_free(s2, e2);
    mino_state_free(s1); mino_state_free(s2);
    OK();
}

/* Test: env_clone shares values -- modifying a mutable value (atom)
 * through one env should be visible in the other */
static void test_env_clone_shared_atoms(void)
{
    TEST("env_clone shares mutable atom references");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);

    mino_eval_string(S, "(def shared-atom (atom 0))", env);
    mino_env_t *clone = mino_env_clone(S, env);

    /* Mutate via clone */
    mino_eval_string(S, "(reset! shared-atom 42)", clone);

    /* Read via original -- atoms are shared, so this should see 42 */
    mino_val_t *r = mino_eval_string(S, "@shared-atom", env);
    CHK(r != NULL, "eval failed");
    long long val;
    CHK(mino_to_int(r, &val), "not int");
    CHK(val == 42, "atom mutation not shared");

    mino_env_free(S, env); mino_env_free(S, clone);
    mino_state_free(S);
    OK();
}

/* Test: GC doesn't collect values reachable from multiple envs */
static void test_gc_multi_env_roots(void)
{
    TEST("GC respects multiple env roots");
    mino_state_t *S = mino_state_new();
    mino_env_t *env1 = mino_new(S);
    mino_env_t *env2 = mino_new(S);
    int i;

    mino_eval_string(S, "(def big-vec (into [] (range 200)))", env1);
    /* env2 gets a reference to the same big vector */
    mino_val_t *v = mino_env_get(env1, "big-vec");
    mino_env_set(S, env2, "shared-vec", v);

    /* Free env1 -- the vector should survive because env2 still holds it */
    mino_env_free(S, env1);

    /* Force GC */
    for (i = 0; i < 100; i++) {
        mino_eval_string(S, "(into [] (range 100))", env2);
    }

    mino_val_t *r = mino_eval_string(S, "(count shared-vec)", env2);
    CHK(r != NULL, "eval failed");
    long long val;
    CHK(mino_to_int(r, &val), "not int");
    CHK(val == 200, "vector was collected prematurely");

    mino_env_free(S, env2);
    mino_state_free(S);
    OK();
}

/* Test: what happens when you free an env and then try to use it? */
/* (We can't safely test this -- it would be UB. Skip.) */

/* Test: rapid ref/unref cycles */
static void test_ref_churn(void)
{
    TEST("Rapid ref/unref cycles (1000 iterations)");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);
    int i;

    for (i = 0; i < 1000; i++) {
        mino_val_t *v = mino_int(S, (long long)i);
        mino_ref_t *r = mino_ref(S, v);
        /* Do some allocation to trigger GC */
        mino_eval_string(S, "(into [] (range 10))", env);
        long long val;
        CHK(mino_to_int(mino_deref(r), &val), "deref failed");
        CHK(val == (long long)i, "wrong value");
        mino_unref(S, r);
    }

    mino_env_free(S, env);
    mino_state_free(S);
    OK();
}

/* Test: eval_string with multiple forms where middle one errors */
static void test_eval_string_multi_form_error(void)
{
    TEST("eval_string: error in middle form stops execution");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);

    /* First form succeeds, second errors, third should not run */
    mino_val_t *r = mino_eval_string(S,
        "(def a 1) (def b (/ 1 0)) (def c 3)", env);
    /* div by zero is now throwable -- but without try, it's still fatal */
    CHK(r == NULL, "should have errored");

    /* a should be defined, c should not */
    CHK(mino_env_get(env, "a") != NULL, "a should exist");
    CHK(mino_env_get(env, "c") == NULL, "c should not exist");

    mino_env_free(S, env);
    mino_state_free(S);
    OK();
}

/* Test: mino_read + mino_eval separately */
static void test_read_eval_cycle(void)
{
    TEST("Manual read-eval cycle");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);

    const char *src = "(+ 1 2) (* 3 4)";
    const char *end = NULL;

    mino_val_t *form1 = mino_read(S, src, &end);
    CHK(form1 != NULL, "read 1 failed");
    mino_val_t *r1 = mino_eval(S, form1, env);
    CHK(r1 != NULL, "eval 1 failed");
    long long v;
    CHK(mino_to_int(r1, &v) && v == 3, "wrong r1");

    mino_val_t *form2 = mino_read(S, end, &end);
    CHK(form2 != NULL, "read 2 failed");
    mino_val_t *r2 = mino_eval(S, form2, env);
    CHK(r2 != NULL, "eval 2 failed");
    CHK(mino_to_int(r2, &v) && v == 12, "wrong r2");

    /* No more forms */
    mino_val_t *form3 = mino_read(S, end, &end);
    CHK(form3 == NULL, "should be EOF");
    CHK(mino_last_error(S) == NULL, "should not be an error");

    mino_env_free(S, env);
    mino_state_free(S);
    OK();
}

/* Test: mino_call with user-defined function */
static void test_call_user_fn(void)
{
    TEST("mino_call with user-defined closure");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);

    mino_val_t *fn = mino_eval_string(S,
        "(fn (a b c) (+ (* a b) c))", env);
    CHK(fn != NULL, "fn creation failed");
    CHK(fn->type == MINO_FN, "not a fn");

    mino_val_t *args = mino_cons(S, mino_int(S, 3),
                       mino_cons(S, mino_int(S, 4),
                       mino_cons(S, mino_int(S, 5), mino_nil(S))));

    mino_val_t *r = mino_call(S, fn, args, env);
    CHK(r != NULL, "call failed");
    long long v;
    CHK(mino_to_int(r, &v) && v == 17, "wrong result");

    mino_env_free(S, env);
    mino_state_free(S);
    OK();
}

/* Test: multiple REPL handles on the same env */
static void test_multiple_repls(void)
{
    TEST("Two REPL handles sharing one env");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);
    mino_repl_t *r1 = mino_repl_new(S, env);
    mino_repl_t *r2 = mino_repl_new(S, env);
    mino_val_t *out = NULL;

    /* r1 defines x */
    mino_repl_feed(r1, "(def x 10)", &out);

    /* r2 should see x and can modify it */
    mino_repl_feed(r2, "(def x (+ x 5))", &out);

    /* r1 should see the updated value */
    int rc = mino_repl_feed(r1, "x", &out);
    CHK(rc == MINO_REPL_OK, "feed failed");
    long long v;
    CHK(mino_to_int(out, &v) && v == 15, "wrong value");

    mino_repl_free(r1);
    mino_repl_free(r2);
    mino_env_free(S, env);
    mino_state_free(S);
    OK();
}

int main(void)
{
    printf("State/boundary stress tests\n");
    printf("----------------------------\n");

    test_interleaved_eval();
    test_cross_state_value_safety();
    test_current_state_in_primitive();
    test_env_clone_shared_atoms();
    test_gc_multi_env_roots();
    test_ref_churn();
    test_eval_string_multi_form_error();
    test_read_eval_cycle();
    test_call_user_fn();
    test_multiple_repls();

    printf("----------------------------\n");
    printf("%d/%d tests passed\n", pass, total);
    return pass == total ? 0 : 1;
}
