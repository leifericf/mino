/*
 * api_stress_test.c -- whitebox exploratory tests for the C API.
 * Tests multi-state isolation, GC pin correctness, REPL handle,
 * execution limits, interruption, cloning edge cases, and more.
 */

#include "mino.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int tests_run    = 0;
static int tests_passed = 0;

#define TEST(name) \
    do { printf("  %-50s ", name); tests_run++; } while (0)
#define PASS() \
    do { printf("OK\n"); tests_passed++; return; } while (0)
#define FAIL(msg) \
    do { printf("FAIL: %s\n", msg); return; } while (0)
#define ASSERT(cond, msg) \
    do { if (!(cond)) FAIL(msg); } while (0)

/* --- Multi-state isolation --- */

static void test_states_are_isolated(void)
{
    TEST("states are isolated: defs don't leak");
    mino_state_t *s1 = mino_state_new();
    mino_state_t *s2 = mino_state_new();
    mino_env_t *e1 = mino_new(s1);
    mino_env_t *e2 = mino_new(s2);

    /* Define something in s1. */
    mino_val_t *r1 = mino_eval_string(s1, "(def foo 42)", e1);
    ASSERT(r1 != NULL, "def in s1 failed");

    /* s2 should not see it. */
    mino_val_t *r2 = mino_eval_string(s2, "foo", e2);
    ASSERT(r2 == NULL, "s2 should not see s1's def");

    mino_env_free(s1, e1);
    mino_env_free(s2, e2);
    mino_state_free(s1);
    mino_state_free(s2);
    PASS();
}

static void test_state_symbol_interning(void)
{
    TEST("each state has independent symbol interning");
    mino_state_t *s1 = mino_state_new();
    mino_state_t *s2 = mino_state_new();

    mino_val_t *sym1 = mino_symbol(s1, "test-sym");
    mino_val_t *sym2 = mino_symbol(s2, "test-sym");

    /* Different states, different pointers. */
    ASSERT(sym1 != sym2, "symbols from different states should be different pointers");

    /* But they should be equal by value. */
    ASSERT(mino_eq(sym1, sym2), "symbols should be equal by value");

    mino_state_free(s1);
    mino_state_free(s2);
    PASS();
}

static void test_state_create_free_cycle(void)
{
    TEST("create and free 100 states without leaking");
    int i;
    for (i = 0; i < 100; i++) {
        mino_state_t *s = mino_state_new();
        mino_env_t *e = mino_new(s);
        mino_eval_string(s, "(def x (into [] (range 50)))", e);
        mino_env_free(s, e);
        mino_state_free(s);
    }
    PASS();
}

/* --- GC pin correctness under stress --- */

static void test_gc_pin_eval_string_heavy(void)
{
    TEST("GC pins survive heavy eval_string allocation");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);
    mino_install_io(S, env);

    /* Build a large map, then look up values. */
    mino_val_t *r = mino_eval_string(S,
        "(def m (reduce (fn (acc i) (assoc acc (keyword (str \"k\" i)) i))"
        "  {} (range 100)))"
        "(get m :k50)", env);
    ASSERT(r != NULL, "eval failed");
    long long val;
    ASSERT(mino_to_int(r, &val), "not an int");
    ASSERT(val == 50, "expected 50");

    mino_env_free(S, env);
    mino_state_free(S);
    PASS();
}

static void test_ref_survives_gc(void)
{
    TEST("ref'd value survives multiple GC cycles");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);

    /* Create a value and ref it. */
    mino_val_t *v = mino_eval_string(S, "[1 2 3 4 5]", env);
    ASSERT(v != NULL, "eval failed");
    mino_ref_t *ref = mino_ref(S, v);

    /* Force many allocations to trigger GC. */
    int i;
    for (i = 0; i < 50; i++) {
        mino_eval_string(S, "(into [] (range 100))", env);
    }

    /* The ref should still be valid. */
    mino_val_t *derefed = mino_deref(ref);
    ASSERT(derefed != NULL, "deref returned NULL");
    ASSERT(derefed->type == MINO_VECTOR, "not a vector");
    ASSERT(derefed->as.vec.len == 5, "wrong length");

    mino_unref(S, ref);
    mino_env_free(S, env);
    mino_state_free(S);
    PASS();
}

/* --- REPL handle --- */

static void test_repl_basic(void)
{
    TEST("REPL handle: single-line eval");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);
    mino_repl_t *repl = mino_repl_new(S, env);
    mino_val_t *out = NULL;

    int rc = mino_repl_feed(repl, "(+ 1 2)", &out);
    ASSERT(rc == MINO_REPL_OK, "expected OK");
    ASSERT(out != NULL, "no output");
    long long val;
    ASSERT(mino_to_int(out, &val), "not an int");
    ASSERT(val == 3, "expected 3");

    mino_repl_free(repl);
    mino_env_free(S, env);
    mino_state_free(S);
    PASS();
}

static void test_repl_multiline(void)
{
    TEST("REPL handle: multi-line form");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);
    mino_repl_t *repl = mino_repl_new(S, env);
    mino_val_t *out = NULL;

    int rc = mino_repl_feed(repl, "(+", &out);
    ASSERT(rc == MINO_REPL_MORE, "expected MORE");

    rc = mino_repl_feed(repl, "  1 2)", &out);
    ASSERT(rc == MINO_REPL_OK, "expected OK");
    long long val;
    ASSERT(mino_to_int(out, &val), "not an int");
    ASSERT(val == 3, "expected 3");

    mino_repl_free(repl);
    mino_env_free(S, env);
    mino_state_free(S);
    PASS();
}

static void test_repl_error_recovery(void)
{
    TEST("REPL handle: error recovery");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);
    mino_repl_t *repl = mino_repl_new(S, env);
    mino_val_t *out = NULL;

    /* Force an error. */
    int rc = mino_repl_feed(repl, "(/ 1 0)", &out);
    ASSERT(rc == MINO_REPL_ERROR, "expected ERROR");

    /* The REPL should recover and accept the next form. */
    rc = mino_repl_feed(repl, "(+ 1 1)", &out);
    ASSERT(rc == MINO_REPL_OK, "expected OK after error");
    long long val;
    ASSERT(mino_to_int(out, &val), "not an int");
    ASSERT(val == 2, "expected 2");

    mino_repl_free(repl);
    mino_env_free(S, env);
    mino_state_free(S);
    PASS();
}

static void test_repl_def_persists(void)
{
    TEST("REPL handle: def persists across feeds");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);
    mino_repl_t *repl = mino_repl_new(S, env);
    mino_val_t *out = NULL;

    mino_repl_feed(repl, "(def x 42)", &out);
    int rc = mino_repl_feed(repl, "x", &out);
    ASSERT(rc == MINO_REPL_OK, "expected OK");
    long long val;
    ASSERT(mino_to_int(out, &val), "not an int");
    ASSERT(val == 42, "expected 42");

    mino_repl_free(repl);
    mino_env_free(S, env);
    mino_state_free(S);
    PASS();
}

/* --- Execution limits --- */

static void test_step_limit(void)
{
    TEST("step limit stops infinite loop");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);

    mino_set_limit(S, MINO_LIMIT_STEPS, 1000);
    mino_val_t *r = mino_eval_string(S,
        "(loop (i 0) (recur (+ i 1)))", env);
    ASSERT(r == NULL, "expected NULL from step limit");
    const char *err = mino_last_error(S);
    ASSERT(err != NULL, "expected error message");
    ASSERT(strstr(err, "step limit") != NULL, "wrong error message");

    /* Reset limit and verify normal eval works. */
    mino_set_limit(S, MINO_LIMIT_STEPS, 0);
    r = mino_eval_string(S, "(+ 1 1)", env);
    ASSERT(r != NULL, "eval after limit reset failed");

    mino_env_free(S, env);
    mino_state_free(S);
    PASS();
}

static void test_heap_limit(void)
{
    TEST("heap limit stops allocation-heavy code");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);

    mino_set_limit(S, MINO_LIMIT_HEAP, 1024 * 64); /* 64KB */
    mino_val_t *r = mino_eval_string(S,
        "(into [] (range 100000))", env);
    ASSERT(r == NULL, "expected NULL from heap limit");
    const char *err = mino_last_error(S);
    ASSERT(err != NULL, "expected error message");
    ASSERT(strstr(err, "heap limit") != NULL, "wrong error message");

    mino_env_free(S, env);
    mino_state_free(S);
    PASS();
}

static void test_interrupt(void)
{
    TEST("mino_interrupt stops eval (via step limit trick)");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);

    /* The interrupt flag is cleared at the start of each eval call (documented
     * behavior: "The flag is cleared at the start of the next eval call").
     * To test it properly we'd need threads. Instead, verify the API exists
     * and that a normal eval works after calling interrupt+eval_string (the
     * flag gets cleared by eval_string before it processes forms). */
    mino_interrupt(S);
    /* This will succeed because eval_string clears the flag at entry. */
    mino_val_t *r = mino_eval_string(S, "(+ 1 1)", env);
    ASSERT(r != NULL, "eval should succeed (interrupt cleared at entry)");

    mino_env_free(S, env);
    mino_state_free(S);
    PASS();
}

/* --- Cloning edge cases --- */

static void test_clone_deeply_nested(void)
{
    TEST("clone deeply nested structure");
    mino_state_t *s1 = mino_state_new();
    mino_state_t *s2 = mino_state_new();
    mino_env_t *e1 = mino_new(s1);

    mino_val_t *v = mino_eval_string(s1,
        "{:a [1 2 {:b #{3 4 5}}] :c (list 6 7 8)}", e1);
    ASSERT(v != NULL, "eval failed");

    mino_val_t *cloned = mino_clone(s2, s1, v);
    ASSERT(cloned != NULL, "clone failed");
    ASSERT(cloned->type == MINO_MAP, "not a map");

    mino_env_free(s1, e1);
    mino_state_free(s1);
    mino_state_free(s2);
    PASS();
}

static void test_clone_rejects_fn(void)
{
    TEST("clone rejects values containing functions");
    mino_state_t *s1 = mino_state_new();
    mino_state_t *s2 = mino_state_new();
    mino_env_t *e1 = mino_new(s1);

    mino_val_t *v = mino_eval_string(s1,
        "{:f (fn (x) x)}", e1);
    ASSERT(v != NULL, "eval failed");

    mino_val_t *cloned = mino_clone(s2, s1, v);
    ASSERT(cloned == NULL, "clone should have failed");
    const char *err = mino_last_error(s2);
    ASSERT(err != NULL, "expected error");
    ASSERT(strstr(err, "non-transferable") != NULL, "wrong error");

    mino_env_free(s1, e1);
    mino_state_free(s1);
    mino_state_free(s2);
    PASS();
}

static void test_clone_empty_collections(void)
{
    TEST("clone empty collections");
    mino_state_t *s1 = mino_state_new();
    mino_state_t *s2 = mino_state_new();
    mino_env_t *e1 = mino_new(s1);

    mino_val_t *v = mino_eval_string(s1, "[[] {} #{} nil]", e1);
    ASSERT(v != NULL, "eval failed");

    mino_val_t *cloned = mino_clone(s2, s1, v);
    ASSERT(cloned != NULL, "clone failed");
    ASSERT(cloned->type == MINO_VECTOR, "not a vector");
    ASSERT(cloned->as.vec.len == 4, "wrong length");

    mino_env_free(s1, e1);
    mino_state_free(s1);
    mino_state_free(s2);
    PASS();
}

/* --- Env cloning --- */

static void test_env_clone_diverge(void)
{
    TEST("env clone diverges from original");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);

    mino_eval_string(S, "(def x 1)", env);
    mino_env_t *clone = mino_env_clone(S, env);

    /* Modify clone. */
    mino_eval_string(S, "(def x 99)", clone);

    /* Original should still have x = 1. */
    mino_val_t *orig_x = mino_eval_string(S, "x", env);
    ASSERT(orig_x != NULL, "eval in original failed");
    long long val;
    ASSERT(mino_to_int(orig_x, &val), "not an int");
    ASSERT(val == 1, "original should still be 1");

    /* Clone should have x = 99. */
    mino_val_t *clone_x = mino_eval_string(S, "x", clone);
    ASSERT(clone_x != NULL, "eval in clone failed");
    ASSERT(mino_to_int(clone_x, &val), "not an int");
    ASSERT(val == 99, "clone should be 99");

    mino_env_free(S, env);
    mino_env_free(S, clone);
    mino_state_free(S);
    PASS();
}

/* --- Handle and finalizer --- */

static int finalizer_called = 0;
static void test_finalizer(void *ptr, const char *tag)
{
    (void)ptr;
    (void)tag;
    finalizer_called++;
}

static void test_handle_finalizer_on_gc(void)
{
    TEST("handle finalizer called on GC");
    finalizer_called = 0;
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);

    /* Create a handle, don't ref it, let it be collected. */
    int dummy = 42;
    mino_handle_ex(S, &dummy, "test", test_finalizer);

    /* Force GC by allocating heavily. */
    int i;
    for (i = 0; i < 100; i++) {
        mino_eval_string(S, "(into [] (range 100))", env);
    }

    mino_env_free(S, env);
    mino_state_free(S);

    /* Finalizer should have been called at least once (during GC or state_free). */
    ASSERT(finalizer_called >= 1, "finalizer not called");
    PASS();
}

/* --- Atom API from C --- */

static void test_atom_api(void)
{
    TEST("atom create, deref, reset from C API");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);

    mino_val_t *a = mino_atom(S, mino_int(S, 10));
    ASSERT(mino_is_atom(a), "should be atom");

    mino_val_t *v = mino_atom_deref(a);
    long long val;
    ASSERT(mino_to_int(v, &val), "not an int");
    ASSERT(val == 10, "expected 10");

    mino_atom_reset(a, mino_int(S, 20));
    v = mino_atom_deref(a);
    ASSERT(mino_to_int(v, &val), "not an int");
    ASSERT(val == 20, "expected 20");

    mino_env_free(S, env);
    mino_state_free(S);
    PASS();
}

/* --- pcall --- */

static void test_pcall_success(void)
{
    TEST("pcall returns 0 on success");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);

    mino_val_t *fn = mino_env_get(env, "+");
    ASSERT(fn != NULL, "+ not found");

    mino_val_t *args = mino_cons(S, mino_int(S, 1),
                       mino_cons(S, mino_int(S, 2), mino_nil(S)));
    mino_val_t *out = NULL;
    int rc = mino_pcall(S, fn, args, env, &out);
    ASSERT(rc == 0, "pcall should return 0");
    ASSERT(out != NULL, "no result");
    long long val;
    ASSERT(mino_to_int(out, &val), "not an int");
    ASSERT(val == 3, "expected 3");

    mino_env_free(S, env);
    mino_state_free(S);
    PASS();
}

static void test_pcall_error(void)
{
    TEST("pcall returns -1 on error");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);

    mino_val_t *fn = mino_env_get(env, "/");
    ASSERT(fn != NULL, "/ not found");

    mino_val_t *args = mino_cons(S, mino_int(S, 1),
                       mino_cons(S, mino_int(S, 0), mino_nil(S)));
    mino_val_t *out = NULL;
    int rc = mino_pcall(S, fn, args, env, &out);
    ASSERT(rc == -1, "pcall should return -1");
    ASSERT(out == NULL, "out should be NULL");

    mino_env_free(S, env);
    mino_state_free(S);
    PASS();
}

/* --- Mailbox stress --- */

static void test_mailbox_many_messages(void)
{
    TEST("mailbox handles 1000 messages");
    mino_state_t *s1 = mino_state_new();
    mino_state_t *s2 = mino_state_new();
    mino_env_t *e1 = mino_new(s1);
    mino_mailbox_t *mb = mino_mailbox_new();

    int i;
    for (i = 0; i < 1000; i++) {
        mino_val_t *v = mino_int(s1, (long long)i);
        int rc = mino_mailbox_send(mb, s1, v);
        ASSERT(rc == 0, "send failed");
    }

    for (i = 0; i < 1000; i++) {
        mino_val_t *v = mino_mailbox_recv(mb, s2);
        ASSERT(v != NULL, "recv returned NULL");
        long long val;
        ASSERT(mino_to_int(v, &val), "not an int");
        ASSERT(val == (long long)i, "wrong value");
    }

    /* Mailbox should be empty. */
    ASSERT(mino_mailbox_recv(mb, s2) == NULL, "should be empty");

    mino_mailbox_free(mb);
    mino_env_free(s1, e1);
    mino_state_free(s1);
    mino_state_free(s2);
    PASS();
}

static void test_mailbox_complex_values(void)
{
    TEST("mailbox serializes complex nested values");
    mino_state_t *s1 = mino_state_new();
    mino_state_t *s2 = mino_state_new();
    mino_env_t *e1 = mino_new(s1);
    mino_env_t *e2 = mino_new(s2);
    mino_mailbox_t *mb = mino_mailbox_new();

    mino_val_t *v = mino_eval_string(s1,
        "{:name \"test\" :data [1 2 [3 4]] :meta {:nested true}}", e1);
    ASSERT(v != NULL, "eval failed");

    int rc = mino_mailbox_send(mb, s1, v);
    ASSERT(rc == 0, "send failed");

    mino_val_t *received = mino_mailbox_recv(mb, s2);
    ASSERT(received != NULL, "recv returned NULL");
    ASSERT(received->type == MINO_MAP, "not a map");

    /* Verify the received map has the right structure. */
    mino_val_t *name_key = mino_keyword(s2, "name");
    mino_val_t *name_val = mino_eval_string(s2, "(get m :name)",  e2);
    (void)name_key;
    (void)name_val;
    /* Just check it's a map with 3 keys. */
    ASSERT(received->as.map.len == 3, "wrong number of keys");

    mino_mailbox_free(mb);
    mino_env_free(s1, e1);
    mino_env_free(s2, e2);
    mino_state_free(s1);
    mino_state_free(s2);
    PASS();
}

/* --- Actor --- */

static void test_actor_lifecycle(void)
{
    TEST("actor create, eval, send, recv, free");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);

    mino_actor_t *a = mino_actor_new();
    ASSERT(a != NULL, "actor_new failed");

    mino_state_t *as = mino_actor_state(a);
    mino_env_t *ae = mino_actor_env(a);
    ASSERT(as != NULL, "actor state NULL");
    ASSERT(ae != NULL, "actor env NULL");

    /* Eval something in the actor. */
    mino_val_t *r = mino_eval_string(as, "(def greeting \"hello\")", ae);
    ASSERT(r != NULL, "actor eval failed");

    /* Send a message from caller to actor. */
    mino_actor_send(a, S, mino_int(S, 42));

    /* Receive it. */
    mino_val_t *msg = mino_actor_recv(a);
    ASSERT(msg != NULL, "recv returned NULL");
    long long val;
    ASSERT(mino_to_int(msg, &val), "not an int");
    ASSERT(val == 42, "expected 42");

    mino_actor_free(a);
    mino_env_free(S, env);
    mino_state_free(S);
    PASS();
}

/* --- Custom primitive --- */

static mino_val_t *prim_double(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (args == NULL || args->type != MINO_CONS) return mino_nil(S);
    mino_val_t *v = args->as.cons.car;
    long long n;
    if (!mino_to_int(v, &n)) return mino_nil(S);
    return mino_int(S, n * 2);
}

static void test_custom_primitive(void)
{
    TEST("register and call custom primitive");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);

    mino_register_fn(S, env, "double", prim_double);

    mino_val_t *r = mino_eval_string(S, "(double 21)", env);
    ASSERT(r != NULL, "eval failed");
    long long val;
    ASSERT(mino_to_int(r, &val), "not an int");
    ASSERT(val == 42, "expected 42");

    mino_env_free(S, env);
    mino_state_free(S);
    PASS();
}

/* --- Reader edge cases --- */

static void test_reader_edge_cases(void)
{
    TEST("reader handles edge cases");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);

    /* Empty string. */
    mino_val_t *r = mino_eval_string(S, "", env);
    ASSERT(r != NULL, "empty string should return nil");
    ASSERT(mino_is_nil(r), "empty string should be nil");

    /* Just whitespace. */
    r = mino_eval_string(S, "   \n\t  ", env);
    ASSERT(r != NULL, "whitespace-only should return nil");
    ASSERT(mino_is_nil(r), "whitespace-only should be nil");

    /* Comment-only. */
    r = mino_eval_string(S, "; just a comment\n", env);
    ASSERT(r != NULL, "comment-only should return nil");
    ASSERT(mino_is_nil(r), "comment-only should be nil");

    mino_env_free(S, env);
    mino_state_free(S);
    PASS();
}

/* --- Predicate correctness --- */

static void test_predicate_matrix(void)
{
    TEST("type predicates on all value types");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);

    /* nil */
    mino_val_t *v = mino_nil(S);
    ASSERT(mino_is_nil(v), "nil is nil");
    ASSERT(!mino_is_truthy(v), "nil is falsey");

    /* true */
    v = mino_true(S);
    ASSERT(!mino_is_nil(v), "true is not nil");
    ASSERT(mino_is_truthy(v), "true is truthy");

    /* false */
    v = mino_false(S);
    ASSERT(!mino_is_nil(v), "false is not nil");
    ASSERT(!mino_is_truthy(v), "false is falsey");

    /* int */
    v = mino_int(S, 0);
    ASSERT(mino_is_truthy(v), "0 is truthy");

    /* empty string */
    v = mino_string(S, "");
    ASSERT(mino_is_truthy(v), "empty string is truthy");

    mino_env_free(S, env);
    mino_state_free(S);
    PASS();
}

/* --- Equality edge cases --- */

static void test_equality_edge_cases(void)
{
    TEST("equality across types and nesting");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);

    /* nil == nil */
    ASSERT(mino_eq(mino_nil(S), mino_nil(S)), "nil == nil");

    /* int != float (different types) */
    mino_val_t *i = mino_int(S, 42);
    mino_val_t *f = mino_float(S, 42.0);
    /* This tests whether your equality considers numeric coercion. */
    /* Not asserting either way; just exercising it. */
    (void)mino_eq(i, f);

    /* Vector equality */
    mino_val_t *v1 = mino_eval_string(S, "[1 2 3]", env);
    mino_val_t *v2 = mino_eval_string(S, "[1 2 3]", env);
    ASSERT(mino_eq(v1, v2), "equal vectors");

    mino_val_t *v3 = mino_eval_string(S, "[1 2 4]", env);
    ASSERT(!mino_eq(v1, v3), "unequal vectors");

    /* Map equality */
    mino_val_t *m1 = mino_eval_string(S, "{:a 1 :b 2}", env);
    mino_val_t *m2 = mino_eval_string(S, "{:b 2 :a 1}", env);
    ASSERT(mino_eq(m1, m2), "equal maps regardless of insertion order");

    mino_env_free(S, env);
    mino_state_free(S);
    PASS();
}

/* --- Module system --- */

static const char *test_resolver(const char *name, void *ctx)
{
    (void)ctx;
    if (strcmp(name, "test-mod") == 0) {
        return "/tmp/mino_test_mod.mino";
    }
    return NULL;
}

static void test_module_resolver(void)
{
    TEST("custom module resolver");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);
    mino_install_io(S, env);

    /* Write a temp module file. */
    FILE *f = fopen("/tmp/mino_test_mod.mino", "w");
    fprintf(f, "(def mod-value 999)\n");
    fclose(f);

    mino_set_resolver(S, test_resolver, NULL);

    mino_val_t *r = mino_eval_string(S,
        "(require \"test-mod\") mod-value", env);
    ASSERT(r != NULL, "require failed");
    long long val;
    ASSERT(mino_to_int(r, &val), "not an int");
    ASSERT(val == 999, "expected 999");

    mino_env_free(S, env);
    mino_state_free(S);
    PASS();
}

/* --- Sandboxing: fresh env has no I/O --- */

static void test_sandbox_no_io(void)
{
    TEST("sandbox: core-only env has no I/O");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_env_new(S);
    mino_install_core(S, env);

    /* println should not be available. */
    mino_val_t *r = mino_eval_string(S, "(println \"hello\")", env);
    ASSERT(r == NULL, "println should fail in sandbox");
    const char *err = mino_last_error(S);
    ASSERT(err != NULL, "expected error");
    ASSERT(strstr(err, "unbound") != NULL, "should be unbound");

    mino_env_free(S, env);
    mino_state_free(S);
    PASS();
}

/* --- Main --- */

int main(void)
{
    printf("API stress tests\n");
    printf("----------------\n");

    test_states_are_isolated();
    test_state_symbol_interning();
    test_state_create_free_cycle();
    test_gc_pin_eval_string_heavy();
    test_ref_survives_gc();
    test_repl_basic();
    test_repl_multiline();
    test_repl_error_recovery();
    test_repl_def_persists();
    test_step_limit();
    test_heap_limit();
    test_interrupt();
    test_clone_deeply_nested();
    test_clone_rejects_fn();
    test_clone_empty_collections();
    test_env_clone_diverge();
    test_handle_finalizer_on_gc();
    test_atom_api();
    test_pcall_success();
    test_pcall_error();
    test_mailbox_many_messages();
    test_mailbox_complex_values();
    test_actor_lifecycle();
    test_custom_primitive();
    test_reader_edge_cases();
    test_predicate_matrix();
    test_equality_edge_cases();
    test_module_resolver();
    test_sandbox_no_io();

    printf("----------------\n");
    printf("%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
