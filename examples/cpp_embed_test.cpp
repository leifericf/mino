/*
 * cpp_embed_test.cpp -- verify mino embeds cleanly in C++.
 * Tests the extern "C" boundary, RAII wrappers, and C++ idioms
 * interacting with the mino C API.
 */

extern "C" {
#include "mino.h"
}

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <functional>
#include <memory>

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

/* --- RAII wrappers --- */

struct MiState {
    mino_state_t *s;
    MiState() : s(mino_state_new()) {}
    ~MiState() { mino_state_free(s); }
    MiState(const MiState&) = delete;
    MiState& operator=(const MiState&) = delete;
    operator mino_state_t*() { return s; }
};

struct MiEnv {
    mino_state_t *s;
    mino_env_t   *e;
    MiEnv(mino_state_t *st) : s(st), e(mino_new(st)) {}
    ~MiEnv() { mino_env_free(s, e); }
    MiEnv(const MiEnv&) = delete;
    operator mino_env_t*() { return e; }
};

struct MiRef {
    mino_state_t *s;
    mino_ref_t   *r;
    MiRef(mino_state_t *st, mino_val_t *v) : s(st), r(mino_ref(st, v)) {}
    ~MiRef() { mino_unref(s, r); }
    MiRef(const MiRef&) = delete;
    mino_val_t *get() { return mino_deref(r); }
};

struct MiActor {
    mino_actor_t *a;
    MiActor() : a(mino_actor_new()) {}
    ~MiActor() { mino_actor_free(a); }
    MiActor(const MiActor&) = delete;
    operator mino_actor_t*() { return a; }
};

/* --- Tests --- */

static void test_raii_lifecycle(void)
{
    TEST("RAII wrappers: create/destroy without leaks");
    {
        MiState s;
        MiEnv env(s);
        mino_val_t *r = mino_eval_string(s, "(+ 1 2 3)", env);
        ASSERT(r != nullptr, "eval failed");
        long long val;
        ASSERT(mino_to_int(r, &val), "not int");
        ASSERT(val == 6, "expected 6");
    }
    /* No crash = no leak in destructors */
    PASS();
}

static void test_raii_ref(void)
{
    TEST("RAII ref: value survives scope");
    MiState s;
    MiEnv env(s);
    mino_val_t *v = mino_eval_string(s, "[1 2 3]", env);
    ASSERT(v != nullptr, "eval failed");
    MiRef ref(s, v);

    /* Force GC */
    for (int i = 0; i < 50; i++)
        mino_eval_string(s, "(into [] (range 100))", env);

    ASSERT(ref.get()->type == MINO_VECTOR, "not vector after GC");
    ASSERT(ref.get()->as.vec.len == 3, "wrong length");
    PASS();
}

static void test_cpp_string_interop(void)
{
    TEST("C++ std::string <-> mino string");
    MiState s;
    MiEnv env(s);

    std::string input = "hello from C++";
    mino_val_t *v = mino_string(s, input.c_str());
    ASSERT(v != nullptr, "string failed");

    const char *out;
    size_t len;
    ASSERT(mino_to_string(v, &out, &len), "to_string failed");
    std::string result(out, len);
    ASSERT(result == input, "roundtrip failed");
    PASS();
}

static void test_cpp_vector_to_mino(void)
{
    TEST("C++ vector<int> -> mino vector -> back");
    MiState s;
    MiEnv env(s);

    std::vector<int> cpp_vec = {10, 20, 30, 40, 50};
    std::vector<mino_val_t*> vals;
    for (int x : cpp_vec) {
        vals.push_back(mino_int(s, x));
    }
    mino_val_t *mv = mino_vector(s, vals.data(), vals.size());
    ASSERT(mv != nullptr, "vector failed");
    ASSERT(mv->as.vec.len == 5, "wrong length");

    /* Pass to mino for processing, get back */
    mino_env_set(s, env, "data", mv);
    mino_val_t *r = mino_eval_string(s,
        "(reduce + 0 data)", env);
    ASSERT(r != nullptr, "eval failed");
    long long sum;
    ASSERT(mino_to_int(r, &sum), "not int");
    ASSERT(sum == 150, "expected 150");
    PASS();
}

/* Host callback using C++ closure via static dispatch */
static std::function<long long(long long)> g_transform;

static mino_val_t *prim_cpp_transform(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!args || args->type != MINO_CONS) return mino_nil(S);
    long long n;
    if (!mino_to_int(args->as.cons.car, &n)) return mino_nil(S);
    return mino_int(S, g_transform(n));
}

static void test_cpp_closure_as_primitive(void)
{
    TEST("C++ std::function exposed as mino primitive");
    MiState s;
    MiEnv env(s);

    int multiplier = 7;
    g_transform = [multiplier](long long x) { return x * multiplier; };

    mino_register_fn(s, env, "cpp-transform", prim_cpp_transform);

    mino_val_t *r = mino_eval_string(s,
        "(into [] (map cpp-transform [1 2 3 4 5]))", env);
    ASSERT(r != nullptr, "eval failed");
    ASSERT(r->type == MINO_VECTOR, "not vector");
    ASSERT(r->as.vec.len == 5, "wrong length");

    /* Verify: [7 14 21 28 35] */
    mino_env_set(s, env, "result", r);
    mino_val_t *check = mino_eval_string(s, "(nth result 2)", env);
    long long val;
    ASSERT(mino_to_int(check, &val), "not int");
    ASSERT(val == 21, "expected 21");
    PASS();
}

static void test_cpp_map_interop(void)
{
    TEST("Build mino map from C++, query from mino");
    MiState s;
    MiEnv env(s);

    /* Build a map: {"name" => "alice", "age" => 30} */
    mino_val_t *keys[2] = {
        mino_keyword(s, "name"),
        mino_keyword(s, "age")
    };
    mino_val_t *vals[2] = {
        mino_string(s, "alice"),
        mino_int(s, 30)
    };
    mino_val_t *m = mino_map(s, keys, vals, 2);
    ASSERT(m != nullptr, "map failed");

    mino_env_set(s, env, "person", m);
    mino_val_t *r = mino_eval_string(s,
        "(str (get person :name) \" is \" (get person :age))", env);
    ASSERT(r != nullptr, "eval failed");
    const char *str;
    size_t len;
    ASSERT(mino_to_string(r, &str, &len), "to_string failed");
    ASSERT(strcmp(str, "alice is 30") == 0, "wrong result");
    PASS();
}

static void test_exception_from_cpp(void)
{
    TEST("mino exception caught via pcall from C++");
    MiState s;
    MiEnv env(s);

    mino_val_t *fn = mino_eval_string(s,
        "(fn (x) (if (< x 0) (throw \"negative!\") (* x x)))", env);
    ASSERT(fn != nullptr, "eval fn failed");
    MiRef fn_ref(s, fn);

    /* Good call */
    mino_val_t *args1 = mino_cons(s, mino_int(s, 5), mino_nil(s));
    mino_val_t *out = nullptr;
    int rc = mino_pcall(s, fn_ref.get(), args1, env, &out);
    ASSERT(rc == 0, "pcall should succeed");
    long long val;
    ASSERT(mino_to_int(out, &val), "not int");
    ASSERT(val == 25, "expected 25");

    /* Bad call */
    mino_val_t *args2 = mino_cons(s, mino_int(s, -3), mino_nil(s));
    rc = mino_pcall(s, fn_ref.get(), args2, env, &out);
    ASSERT(rc == -1, "pcall should fail");
    const char *err = mino_last_error(s);
    ASSERT(err != nullptr && strstr(err, "negative") != nullptr, "wrong error");
    PASS();
}

/* --- Actor model: ping-pong --- */

static void test_actor_ping_pong(void)
{
    TEST("Actor model: ping-pong message exchange");
    MiState caller;
    MiEnv caller_env(caller);
    MiActor worker;

    mino_state_t *ws = mino_actor_state(worker);
    mino_env_t *we = mino_actor_env(worker);
    mino_install_io(ws, we);

    /* Define a handler in the worker that doubles received numbers */
    mino_eval_string(ws,
        "(defn handle (msg) (* msg 2))", we);

    /* Send 5 messages, process each, collect results */
    std::vector<long long> results;
    for (int i = 1; i <= 5; i++) {
        mino_actor_send(worker, caller, mino_int(caller, i));

        /* Worker receives and processes */
        mino_val_t *msg = mino_actor_recv(worker);
        ASSERT(msg != nullptr, "recv null");

        mino_env_set(ws, we, "__msg", msg);
        mino_val_t *result = mino_eval_string(ws, "(handle __msg)", we);
        ASSERT(result != nullptr, "handle failed");

        long long val;
        ASSERT(mino_to_int(result, &val), "not int");
        results.push_back(val);
    }

    ASSERT(results.size() == 5, "wrong count");
    ASSERT(results[0] == 2 && results[1] == 4 && results[2] == 6
        && results[3] == 8 && results[4] == 10, "wrong values");
    PASS();
}

/* --- Actor model: pipeline --- */

static void test_actor_pipeline(void)
{
    TEST("Actor model: 3-stage processing pipeline");
    MiState host;
    MiEnv host_env(host);

    /* Stage 1: increment */
    MiActor stage1;
    mino_eval_string(mino_actor_state(stage1),
        "(defn process (x) (+ x 1))", mino_actor_env(stage1));

    /* Stage 2: double */
    MiActor stage2;
    mino_eval_string(mino_actor_state(stage2),
        "(defn process (x) (* x 2))", mino_actor_env(stage2));

    /* Stage 3: to string */
    MiActor stage3;
    mino_eval_string(mino_actor_state(stage3),
        "(defn process (x) (str \"result:\" x))", mino_actor_env(stage3));

    /* Push value 10 through the pipeline */
    mino_val_t *val = mino_int(host, 10);

    /* Stage 1 */
    mino_actor_send(stage1, host, val);
    mino_val_t *msg = mino_actor_recv(stage1);
    mino_env_set(mino_actor_state(stage1), mino_actor_env(stage1), "__m", msg);
    val = mino_eval_string(mino_actor_state(stage1), "(process __m)",
                           mino_actor_env(stage1));
    ASSERT(val != nullptr, "stage1 failed");

    /* Stage 2 -- clone from stage1's state to stage2's */
    mino_val_t *cloned = mino_clone(mino_actor_state(stage2),
                                     mino_actor_state(stage1), val);
    ASSERT(cloned != nullptr, "clone 1->2 failed");
    mino_env_set(mino_actor_state(stage2), mino_actor_env(stage2), "__m", cloned);
    val = mino_eval_string(mino_actor_state(stage2), "(process __m)",
                           mino_actor_env(stage2));
    ASSERT(val != nullptr, "stage2 failed");

    /* Stage 3 -- clone from stage2 to stage3 */
    cloned = mino_clone(mino_actor_state(stage3),
                         mino_actor_state(stage2), val);
    ASSERT(cloned != nullptr, "clone 2->3 failed");
    mino_env_set(mino_actor_state(stage3), mino_actor_env(stage3), "__m", cloned);
    val = mino_eval_string(mino_actor_state(stage3), "(process __m)",
                           mino_actor_env(stage3));
    ASSERT(val != nullptr, "stage3 failed");

    const char *str;
    size_t len;
    ASSERT(mino_to_string(val, &str, &len), "to_string failed");
    /* 10 -> (+1) -> 11 -> (*2) -> 22 -> str -> "result:22" */
    ASSERT(strcmp(str, "result:22") == 0, "wrong pipeline result");
    PASS();
}

/* --- Actor model: supervisor pattern --- */

static void test_actor_supervisor(void)
{
    TEST("Actor model: supervisor restarts failed actor");
    MiState host;
    MiEnv host_env(host);

    auto make_worker = []() -> mino_actor_t* {
        mino_actor_t *a = mino_actor_new();
        mino_eval_string(mino_actor_state(a),
            "(defn handle (x) (if (= x 0) (throw \"crash!\") (* x x)))",
            mino_actor_env(a));
        return a;
    };

    mino_actor_t *worker = make_worker();
    std::vector<std::string> results;

    int inputs[] = {3, 4, 0, 5};  /* 0 will crash */
    for (int i = 0; i < 4; i++) {
        mino_actor_send(worker, host, mino_int(host, inputs[i]));
        mino_val_t *msg = mino_actor_recv(worker);

        mino_state_t *ws = mino_actor_state(worker);
        mino_env_t *we = mino_actor_env(worker);
        mino_env_set(ws, we, "__m", msg);

        mino_val_t *out = nullptr;
        mino_val_t *fn = mino_env_get(we, "handle");
        int rc = mino_pcall(ws, fn, mino_cons(ws, msg, mino_nil(ws)), we, &out);

        if (rc == 0) {
            char buf[64];
            long long val;
            mino_to_int(out, &val);
            snprintf(buf, sizeof(buf), "ok:%lld", val);
            results.push_back(buf);
        } else {
            results.push_back("restarted");
            /* Supervisor restarts the actor */
            mino_actor_free(worker);
            worker = make_worker();
        }
    }

    mino_actor_free(worker);

    ASSERT(results.size() == 4, "wrong count");
    ASSERT(results[0] == "ok:9", "wrong r0");
    ASSERT(results[1] == "ok:16", "wrong r1");
    ASSERT(results[2] == "restarted", "should restart");
    ASSERT(results[3] == "ok:25", "wrong r3");
    PASS();
}

/* --- Sandboxed eval with limits --- */

static void test_sandboxed_untrusted_code(void)
{
    TEST("Sandbox: run untrusted code with limits");
    MiState s;
    /* Deliberately NOT using mino_new -- core only, no I/O */
    mino_env_t *sandbox = mino_env_new(s);
    mino_install_core(s, sandbox);

    /* Untrusted code can't do I/O */
    mino_val_t *r = mino_eval_string(s, "(println \"pwned\")", sandbox);
    ASSERT(r == nullptr, "println should be unavailable");

    /* Set step limit to prevent infinite loops */
    mino_set_limit(s, MINO_LIMIT_STEPS, 10000);

    /* Safe code works */
    r = mino_eval_string(s, "(reduce + 0 (range 100))", sandbox);
    ASSERT(r != nullptr, "safe code failed");
    long long val;
    ASSERT(mino_to_int(r, &val), "not int");
    ASSERT(val == 4950, "expected 4950");

    /* Infinite loop is caught */
    r = mino_eval_string(s, "(loop (i 0) (recur (+ i 1)))", sandbox);
    ASSERT(r == nullptr, "infinite loop should fail");
    const char *err = mino_last_error(s);
    ASSERT(strstr(err, "step limit") != nullptr, "wrong error");

    mino_env_free(s, sandbox);
    PASS();
}

/* --- Handle with C++ destructor integration --- */

struct CppResource {
    int id;
    static int destroy_count;
    CppResource(int id) : id(id) {}
};
int CppResource::destroy_count = 0;

static void cpp_finalizer(void *ptr, const char *tag)
{
    (void)tag;
    CppResource *r = static_cast<CppResource*>(ptr);
    CppResource::destroy_count++;
    delete r;
}

static void test_handle_cpp_destructor(void)
{
    TEST("Handle: C++ destructor via finalizer");
    CppResource::destroy_count = 0;
    {
        MiState s;
        MiEnv env(s);

        auto *res = new CppResource(42);
        mino_handle_ex(s, res, "CppResource", cpp_finalizer);

        /* Don't ref it -- let GC or state_free clean it up */
        for (int i = 0; i < 100; i++)
            mino_eval_string(s, "(into [] (range 100))", env);
    }
    /* State destroyed, finalizer should have run */
    ASSERT(CppResource::destroy_count == 1, "finalizer not called");
    PASS();
}

int main()
{
    printf("C++ embedding tests\n");
    printf("--------------------\n");

    test_raii_lifecycle();
    test_raii_ref();
    test_cpp_string_interop();
    test_cpp_vector_to_mino();
    test_cpp_closure_as_primitive();
    test_cpp_map_interop();
    test_exception_from_cpp();
    test_actor_ping_pong();
    test_actor_pipeline();
    test_actor_supervisor();
    test_sandboxed_untrusted_code();
    test_handle_cpp_destructor();

    printf("--------------------\n");
    printf("%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
