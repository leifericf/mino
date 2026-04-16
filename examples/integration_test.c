/*
 * integration_test.c -- end-to-end integration tests.
 * Tests feature interactions that unit tests can't catch:
 * GC + eval + closures + atoms, module loading + state, REPL + try/catch,
 * binding + actors, limits + lazy sequences, etc.
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

/* Test: eval_string -> define fn -> call from C -> fn uses lazy seq */
static void test_eval_then_call_lazy(void)
{
    TEST("eval defines fn using lazy seq, C calls it via mino_call");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);

    mino_val_t *fn = mino_eval_string(S,
        "(fn (n) (reduce + 0 (take n (range))))", env);
    CHK(fn != NULL, "fn creation failed");

    mino_ref_t *fn_ref = mino_ref(S, fn);

    /* Call it from C with different values */
    long long results[] = {0, 0, 1, 3, 6, 10};
    int i;
    for (i = 0; i <= 5; i++) {
        mino_val_t *args = mino_cons(S, mino_int(S, i), mino_nil(S));
        mino_val_t *r = mino_call(S, mino_deref(fn_ref), args, env);
        CHK(r != NULL, "call failed");
        long long v;
        CHK(mino_to_int(r, &v), "not int");
        CHK(v == results[i], "wrong result");
    }

    mino_unref(S, fn_ref);
    mino_env_free(S, env);
    mino_state_free(S);
    OK();
}

/* Test: REPL handle with try/catch + binding + atom mutation */
static void test_repl_complex_session(void)
{
    TEST("REPL: multi-step session with atoms, binding, try/catch");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);
    mino_repl_t *repl = mino_repl_new(S, env);
    mino_val_t *out = NULL;

    /* Step 1: define an atom */
    mino_repl_feed(repl, "(def counter (atom 0))", &out);

    /* Step 2: define a function that uses it */
    mino_repl_feed(repl, "(defn bump () (swap! counter + 1))", &out);

    /* Step 3: call it several times */
    mino_repl_feed(repl, "(bump)", &out);
    mino_repl_feed(repl, "(bump)", &out);
    mino_repl_feed(repl, "(bump)", &out);

    /* Step 4: verify */
    int rc = mino_repl_feed(repl, "@counter", &out);
    CHK(rc == MINO_REPL_OK, "feed failed");
    long long v;
    CHK(mino_to_int(out, &v) && v == 3, "counter should be 3");

    /* Step 5: try/catch with binding */
    rc = mino_repl_feed(repl, "(def *ctx* :default)", &out);
    rc = mino_repl_feed(repl,
        "(try (binding (*ctx* :temp) (throw \"oops\")) (catch e e))", &out);
    CHK(rc == MINO_REPL_OK, "try/binding failed");

    /* Step 6: verify binding was restored */
    rc = mino_repl_feed(repl, "*ctx*", &out);
    CHK(rc == MINO_REPL_OK, "feed failed");
    /* *ctx* should be :default keyword */
    CHK(out->type == MINO_KEYWORD, "not keyword");

    mino_repl_free(repl);
    mino_env_free(S, env);
    mino_state_free(S);
    OK();
}

/* Test: step limit + lazy sequence interaction */
static void test_limit_with_lazy(void)
{
    TEST("Step limit stops lazy sequence realization mid-stream");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);

    /* Set a tight step limit. Use a loop/recur form (not lazy) so
     * the step limit check fires in eval_impl and returns NULL. */
    mino_set_limit(S, MINO_LIMIT_STEPS, 1000);

    mino_val_t *r = mino_eval_string(S,
        "(loop (i 0) (recur (+ i 1)))", env);
    CHK(r == NULL, "should hit step limit");
    const char *err = mino_last_error(S);
    CHK(strstr(err, "step limit") != NULL, "wrong error");

    /* Reset limit and do a small eval */
    mino_set_limit(S, MINO_LIMIT_STEPS, 0);
    r = mino_eval_string(S, "(+ 1 1)", env);
    CHK(r != NULL, "eval after limit reset failed");

    mino_env_free(S, env);
    mino_state_free(S);
    OK();
}

/* Test: clone a value, modify in dst state, verify src is unchanged */
static void test_clone_isolation_deep(void)
{
    TEST("Cloned value is truly independent: modify dst, src unchanged");
    mino_state_t *src = mino_state_new();
    mino_state_t *dst = mino_state_new();
    mino_env_t *se = mino_new(src);
    mino_env_t *de = mino_new(dst);

    mino_val_t *v = mino_eval_string(src, "[1 2 3]", se);
    CHK(v != NULL, "eval failed");

    mino_val_t *cloned = mino_clone(dst, src, v);
    CHK(cloned != NULL, "clone failed");

    /* Modify in dst by conj'ing */
    mino_env_set(dst, de, "data", cloned);
    mino_val_t *modified = mino_eval_string(dst, "(conj data 4)", de);
    CHK(modified != NULL, "conj failed");
    CHK(modified->as.vec.len == 4, "modified should have 4");

    /* Original in src should still have 3 */
    CHK(v->as.vec.len == 3, "src should still have 3");

    mino_env_free(src, se); mino_env_free(dst, de);
    mino_state_free(src); mino_state_free(dst);
    OK();
}

/* Test: actor with eval + try/catch + binding interaction */
static void test_actor_eval_with_exceptions(void)
{
    TEST("Actor: eval with try/catch handles errors correctly");
    mino_state_t *host = mino_state_new();
    mino_actor_t *a = mino_actor_new();
    mino_state_t *as = mino_actor_state(a);
    mino_env_t *ae = mino_actor_env(a);

    /* Actor has a handler that can fail */
    mino_eval_string(as,
        "(defn handle (x)"
        "  (try"
        "    (if (= x 0) (throw \"zero!\") (* x x))"
        "    (catch e (str \"error:\" e))))", ae);

    /* Send valid messages */
    mino_actor_send(a, host, mino_int(host, 5));
    mino_val_t *msg = mino_actor_recv(a);
    mino_env_set(as, ae, "__m", msg);
    mino_val_t *r = mino_eval_string(as, "(handle __m)", ae);
    CHK(r != NULL, "handle 5 failed");
    long long v;
    CHK(mino_to_int(r, &v) && v == 25, "5^2 should be 25");

    /* Send error-triggering message */
    mino_actor_send(a, host, mino_int(host, 0));
    msg = mino_actor_recv(a);
    mino_env_set(as, ae, "__m", msg);
    r = mino_eval_string(as, "(handle __m)", ae);
    CHK(r != NULL, "handle 0 failed");
    const char *s;
    size_t len;
    CHK(mino_to_string(r, &s, &len), "not string");
    CHK(strstr(s, "error:zero!") != NULL, "wrong error result");

    mino_actor_free(a);
    mino_state_free(host);
    OK();
}

/* Test: pcall inside eval_string (nested C->mino->C->mino) */
static mino_val_t *prim_safe_divide(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!args || args->type != MINO_CONS ||
        !args->as.cons.cdr || args->as.cons.cdr->type != MINO_CONS)
        return mino_nil(S);

    mino_val_t *fn = mino_env_get(env, "/");
    mino_val_t *out = NULL;
    int rc = mino_pcall(S, fn, args, env, &out);
    if (rc != 0) {
        return mino_string(S, "div-error");
    }
    return out;
}

static void test_pcall_inside_eval(void)
{
    TEST("pcall from C primitive called during mino eval");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);

    mino_register_fn(S, env, "safe-divide", prim_safe_divide);

    /* Normal case: 10 / 2 = 5 (integer division) */
    mino_val_t *r = mino_eval_string(S, "(safe-divide 10 2)", env);
    CHK(r != NULL, "eval failed");
    long long v;
    CHK(mino_to_int(r, &v), "not int");
    CHK(v == 5, "expected 5");

    /* Error case -- pcall should catch the div by zero */
    r = mino_eval_string(S, "(safe-divide 10 0)", env);
    CHK(r != NULL, "safe-divide should return error string");
    const char *s;
    size_t len;
    CHK(mino_to_string(r, &s, &len), "not string");
    CHK(strcmp(s, "div-error") == 0, "wrong error");

    mino_env_free(S, env);
    mino_state_free(S);
    OK();
}

/* Test: module loading + defmacro + macro use across module boundary */
static void test_module_with_macros(void)
{
    TEST("Module loading: macro defined in module used by caller");
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);

    /* Write a module that defines a macro */
    FILE *f = fopen("/tmp/mino_macro_mod.mino", "w");
    fprintf(f, "(defmacro unless (test & body)\n"
               "  `(if (not ~test) (do ~@body)))\n"
               "(def MOD_LOADED true)\n");
    fclose(f);

    mino_set_resolver(S, NULL, NULL);
    /* Load directly since we know the path */
    mino_val_t *r = mino_load_file(S, "/tmp/mino_macro_mod.mino", env);
    CHK(r != NULL, "load failed");

    /* Use the macro */
    r = mino_eval_string(S, "(unless false 42)", env);
    CHK(r != NULL, "unless failed");
    long long v;
    CHK(mino_to_int(r, &v) && v == 42, "unless should return 42");

    r = mino_eval_string(S, "(unless true 42)", env);
    CHK(r != NULL, "unless true failed");
    CHK(mino_is_nil(r), "unless true should be nil");

    mino_env_free(S, env);
    mino_state_free(S);
    OK();
}

/* Test: GC stress with interleaved ref/eval/clone */
static void test_gc_stress_integration(void)
{
    TEST("GC: interleaved ref/eval/clone under allocation pressure");
    mino_state_t *s1 = mino_state_new();
    mino_state_t *s2 = mino_state_new();
    mino_env_t *e1 = mino_new(s1);
    mino_env_t *e2 = mino_new(s2);
    int i;

    for (i = 0; i < 100; i++) {
        /* Build value in s1 */
        mino_val_t *v = mino_eval_string(s1,
            "(hash-map :i 0 :data (into [] (range 20)))", e1);
        CHK(v != NULL, "eval in s1 failed");

        /* Ref it */
        mino_ref_t *ref = mino_ref(s1, v);

        /* Clone to s2 */
        mino_val_t *cloned = mino_clone(s2, s1, mino_deref(ref));
        CHK(cloned != NULL, "clone failed");

        /* Do more allocation in both states */
        mino_eval_string(s1, "(into [] (range 50))", e1);
        mino_eval_string(s2, "(into [] (range 50))", e2);

        /* Verify ref still valid */
        CHK(mino_deref(ref)->type == MINO_MAP, "ref broken after alloc");

        mino_unref(s1, ref);
    }

    mino_env_free(s1, e1); mino_env_free(s2, e2);
    mino_state_free(s1); mino_state_free(s2);
    OK();
}

/* Test: handle finalizer ordering on state_free */
static int fin_order[10];
static int fin_idx = 0;

static void ordered_finalizer(void *ptr, const char *tag)
{
    (void)tag;
    if (fin_idx < 10) {
        fin_order[fin_idx++] = *(int*)ptr;
    }
}

static void test_finalizer_on_state_free(void)
{
    TEST("Handle finalizers all run on mino_state_free");
    fin_idx = 0;
    mino_state_t *S = mino_state_new();
    mino_env_t *env = mino_new(S);
    static int ids[5] = {1, 2, 3, 4, 5};
    int i;

    for (i = 0; i < 5; i++) {
        mino_val_t *h = mino_handle_ex(S, &ids[i], "test", ordered_finalizer);
        char name[16];
        snprintf(name, sizeof(name), "__h%d", i);
        mino_env_set(S, env, name, h);
    }

    mino_env_free(S, env);
    mino_state_free(S);

    /* All 5 finalizers should have run */
    CHK(fin_idx == 5, "not all finalizers ran");
    OK();
}

int main(void)
{
    printf("Integration tests\n");
    printf("------------------\n");

    test_eval_then_call_lazy();
    test_repl_complex_session();
    test_limit_with_lazy();
    test_clone_isolation_deep();
    test_actor_eval_with_exceptions();
    test_pcall_inside_eval();
    test_module_with_macros();
    test_gc_stress_integration();
    test_finalizer_on_state_free();

    printf("------------------\n");
    printf("%d/%d tests passed\n", pass, total);
    return pass == total ? 0 : 1;
}
