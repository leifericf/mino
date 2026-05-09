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

#include "mino.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

    mino_env_free(S, env);
    mino_state_free(S);

    if (failures > 0) {
        fprintf(stderr, "embed_stm_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("embed_stm_test: all checks passed\n");
    return 0;
}
