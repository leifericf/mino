/*
 * transient_test.c -- C-level smoke test for the transient batch-
 * mutation API declared in mino.h.
 *
 * Build (from repo root):
 *   cc -std=c99 -Wall -Wextra -O2 -Isrc -o transient_test \
 *     tests/transient_test.c src/SRC.c -lm
 * Run:
 *   ./transient_test
 * Exit 0 means every assertion passed; exit 1 prints the first
 * assertion that failed.
 */

#include "mino.h"

#include <stdio.h>
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

static mino_val_t *eval_ok(mino_state_t *S, mino_env_t *env, const char *src)
{
    mino_val_t *r = mino_eval_string(S, src, env);
    if (r == NULL) {
        fprintf(stderr, "eval failed: %s -- %s\n", src, mino_last_error(S));
    }
    return r;
}

static void test_vector_conj_bang(mino_state_t *S, mino_env_t *env)
{
    /* Start from an empty persistent vector, grow via conj! in a loop,
     * seal with persistent, and verify the count plus a spot check. */
    mino_val_t *v0 = mino_vector(S, NULL, 0);
    mino_val_t *t  = mino_transient(S, v0);
    long long   i;
    mino_val_t *final;
    long long   n;
    (void)env;
    REQUIRE(mino_is_transient(t), "vector: mino_transient returned non-transient");
    for (i = 0; i < 256; i++) {
        mino_val_t *elem = mino_int(S, i);
        mino_val_t *t2   = mino_conj_bang(S, t, elem);
        REQUIRE(t2 != NULL, "vector: conj! returned NULL");
        REQUIRE(t2 == t, "vector: conj! returned a different transient");
    }
    REQUIRE(mino_transient_count(t) == 256u,
            "vector: transient count mismatch");
    final = mino_persistent(S, t);
    REQUIRE(final != NULL, "vector: persistent! returned NULL");
    /* Verify sealing: any further conj! must error out. */
    {
        mino_val_t *t2 = mino_conj_bang(S, t, mino_int(S, -1));
        REQUIRE(t2 == NULL, "vector: conj! on sealed transient did not fail");
    }
    /* Verify final vector via a mino-level expression. */
    mino_env_t *e = env;
    (void)e;
    /* Bind the result to a var and count it. */
    (void)mino_to_int(eval_ok(S, env, "42"), &n);  /* smoke */
    REQUIRE(final->type == MINO_VECTOR, "vector: final is not a vector");
    {
        /* Persistent vector len lives in as.vec.len. */
        size_t len = final->as.vec.len;
        REQUIRE(len == 256u, "vector: final length mismatch");
    }
}

static void test_vector_assoc_bang(mino_state_t *S, mino_env_t *env)
{
    mino_val_t *v0 = eval_ok(S, env, "[10 20 30 40]");
    mino_val_t *t  = mino_transient(S, v0);
    mino_val_t *t2;
    mino_val_t *final;
    REQUIRE(t != NULL, "vector assoc: transient creation failed");
    t2 = mino_assoc_bang(S, t, mino_int(S, 1), mino_int(S, 99));
    REQUIRE(t2 == t, "vector assoc: did not return same transient");
    final = mino_persistent(S, t);
    REQUIRE(final != NULL && final->type == MINO_VECTOR,
            "vector assoc: final not a vector");
    REQUIRE(final->as.vec.len == 4u,
            "vector assoc: length changed unexpectedly");
}

static void test_vector_pop_bang(mino_state_t *S, mino_env_t *env)
{
    mino_val_t *v0 = eval_ok(S, env, "[1 2 3 4 5]");
    mino_val_t *t  = mino_transient(S, v0);
    mino_val_t *t2;
    mino_val_t *final;
    (void)env;
    t2 = mino_pop_bang(S, t);
    REQUIRE(t2 == t, "vector pop: did not return same transient");
    t2 = mino_pop_bang(S, t);
    REQUIRE(t2 == t, "vector pop: second pop returned NULL");
    final = mino_persistent(S, t);
    REQUIRE(final != NULL && final->type == MINO_VECTOR,
            "vector pop: final not a vector");
    REQUIRE(final->as.vec.len == 3u,
            "vector pop: final length wrong");
}

static void test_map_assoc_bang(mino_state_t *S, mino_env_t *env)
{
    mino_val_t *m0 = eval_ok(S, env, "{}");
    mino_val_t *t  = mino_transient(S, m0);
    long long   i;
    mino_val_t *final;
    REQUIRE(t != NULL, "map: transient creation failed");
    for (i = 0; i < 50; i++) {
        mino_val_t *k = mino_int(S, i);
        mino_val_t *v = mino_int(S, i * 10);
        mino_val_t *t2 = mino_assoc_bang(S, t, k, v);
        REQUIRE(t2 == t, "map: assoc! returned different transient");
    }
    REQUIRE(mino_transient_count(t) == 50u,
            "map: transient count wrong");
    final = mino_persistent(S, t);
    REQUIRE(final != NULL && final->type == MINO_MAP,
            "map: final not a map");
    REQUIRE(final->as.map.len == 50u,
            "map: final size wrong");
}

static void test_map_dissoc_bang(mino_state_t *S, mino_env_t *env)
{
    mino_val_t *m0 = eval_ok(S, env, "{:a 1 :b 2 :c 3}");
    mino_val_t *t  = mino_transient(S, m0);
    mino_val_t *k  = mino_keyword(S, "a");
    mino_val_t *t2;
    mino_val_t *final;
    REQUIRE(t != NULL, "map dissoc: transient creation failed");
    t2 = mino_dissoc_bang(S, t, k);
    REQUIRE(t2 == t, "map dissoc: did not return same transient");
    final = mino_persistent(S, t);
    REQUIRE(final != NULL && final->type == MINO_MAP,
            "map dissoc: final not a map");
    REQUIRE(final->as.map.len == 2u,
            "map dissoc: expected 2 entries after removing :a");
}

static void test_set_conj_disj_bang(mino_state_t *S, mino_env_t *env)
{
    mino_val_t *s0 = eval_ok(S, env, "#{}");
    mino_val_t *t  = mino_transient(S, s0);
    mino_val_t *t2;
    mino_val_t *final;
    long long   i;
    REQUIRE(t != NULL, "set: transient creation failed");
    for (i = 0; i < 20; i++) {
        mino_val_t *elem = mino_int(S, i);
        t2 = mino_conj_bang(S, t, elem);
        REQUIRE(t2 == t, "set: conj! returned different transient");
    }
    REQUIRE(mino_transient_count(t) == 20u,
            "set: size after conj wrong");
    t2 = mino_disj_bang(S, t, mino_int(S, 5));
    REQUIRE(t2 == t, "set: disj! returned different transient");
    REQUIRE(mino_transient_count(t) == 19u,
            "set: size after disj wrong");
    final = mino_persistent(S, t);
    REQUIRE(final != NULL && final->type == MINO_SET,
            "set: final not a set");
    REQUIRE(final->as.set.len == 19u,
            "set: final size wrong");
}

static void test_type_mismatch(mino_state_t *S, mino_env_t *env)
{
    /* mino_transient on a non-collection must fail. */
    mino_val_t *t = mino_transient(S, mino_int(S, 42));
    (void)env;
    REQUIRE(t == NULL, "type mismatch: transient on int should fail");
}

static void test_invalid_after_persistent(mino_state_t *S, mino_env_t *env)
{
    mino_val_t *v = mino_vector(S, NULL, 0);
    mino_val_t *t = mino_transient(S, v);
    mino_val_t *p;
    mino_val_t *t2;
    (void)env;
    p = mino_persistent(S, t);
    REQUIRE(p != NULL, "invalidation: first persistent! failed");
    t2 = mino_conj_bang(S, t, mino_int(S, 1));
    REQUIRE(t2 == NULL,
            "invalidation: conj! on sealed transient must fail");
    t2 = mino_persistent(S, t);
    REQUIRE(t2 == NULL,
            "invalidation: double persistent! must fail");
}

int main(void)
{
    mino_state_t *S   = mino_state_new();
    mino_env_t   *env = mino_env_new(S);
    if (S == NULL || env == NULL) {
        fprintf(stderr, "state/env setup failed\n");
        return 1;
    }
    mino_install_core(S, env);

    test_vector_conj_bang(S, env);
    test_vector_assoc_bang(S, env);
    test_vector_pop_bang(S, env);
    test_map_assoc_bang(S, env);
    test_map_dissoc_bang(S, env);
    test_set_conj_disj_bang(S, env);
    test_type_mismatch(S, env);
    test_invalid_after_persistent(S, env);

    mino_env_free(S, env);
    mino_state_free(S);

    if (failures == 0) {
        printf("transient_test: OK (all assertions passed)\n");
        return 0;
    }
    fprintf(stderr, "transient_test: %d FAILURE(S)\n", failures);
    return 1;
}
