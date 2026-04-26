/*
 * multimethod_test.c -- C-level smoke test for mino's multimethod
 * dispatch, hierarchy, and prefer-method semantics as exercised
 * through the public embedding API.
 *
 * mino ships multimethods as a mino-level construct (see core.clj
 * defmulti / defmethod / prefer-method / remove-method / methods /
 * get-method), so embedders reach them via mino_eval_string and
 * mino_call like any other user-defined fn. This file pins that path
 * so regressions in the C-visible behaviour surface loudly.
 *
 * Build:
 *   cc -std=c99 -Wall -Wextra -O2 -Isrc -o multimethod_test \
 *     tests/multimethod_test.c src/SRC.c -lm
 * Run:
 *   ./multimethod_test
 * Exit 0 means every assertion passed; non-zero prints which failed.
 */

#include "mino.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define REQUIRE(cond, msg)                                         \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "FAIL (%s:%d): %s\n",                  \
                    __FILE__, __LINE__, (msg));                    \
            failures++;                                            \
        }                                                          \
    } while (0)

static mino_val_t *eval(mino_state_t *S, mino_env_t *env, const char *src)
{
    mino_val_t *r = mino_eval_string(S, src, env);
    if (r == NULL) {
        fprintf(stderr, "eval failed: %s\n  src: %s\n",
                mino_last_error(S), src);
    }
    return r;
}

static int expect_int(mino_state_t *S, mino_env_t *env,
                      const char *src, long long want, const char *label)
{
    mino_val_t *r = eval(S, env, src);
    long long    n = 0;
    if (r == NULL) { REQUIRE(0, label); return 0; }
    if (!mino_to_int(r, &n)) { REQUIRE(0, label); return 0; }
    if (n != want) {
        fprintf(stderr, "FAIL %s: got %lld, expected %lld\n",
                label, n, want);
        failures++;
        return 0;
    }
    return 1;
}

static int expect_string(mino_state_t *S, mino_env_t *env,
                         const char *src, const char *want,
                         const char *label)
{
    mino_val_t *r = eval(S, env, src);
    const char *s  = NULL;
    size_t      len = 0;
    if (r == NULL) { REQUIRE(0, label); return 0; }
    if (r->type != MINO_KEYWORD && !mino_to_string(r, &s, &len)) {
        REQUIRE(0, label);
        return 0;
    }
    if (r->type == MINO_KEYWORD) {
        s   = r->as.s.data;
        len = r->as.s.len;
    }
    if (strlen(want) != len || memcmp(s, want, len) != 0) {
        fprintf(stderr, "FAIL %s: got '%.*s', expected '%s'\n",
                label, (int)len, s, want);
        failures++;
        return 0;
    }
    return 1;
}

static void test_dispatch_by_keyword(mino_state_t *S, mino_env_t *env)
{
    eval(S, env,
         "(defmulti area :shape)"
         "(defmethod area :circle [{:keys [r]}] (* 314 r r))"
         "(defmethod area :rect [{:keys [w h]}] (* w h))"
         "(defmethod area :default [_] :unknown-shape)");
    expect_int(S, env, "(area {:shape :rect :w 3 :h 4})", 12,
               "dispatch: rect area");
    expect_int(S, env, "(area {:shape :circle :r 2})", 1256,
               "dispatch: circle area");
    expect_string(S, env, "(area {:shape :hexagon})", "unknown-shape",
                  "dispatch: :default fallback");
}

static void test_methods_api(mino_state_t *S, mino_env_t *env)
{
    expect_int(S, env, "(count (methods area))", 3,
               "methods: registered count including :default");
    eval(S, env, "(remove-method area :rect)");
    expect_int(S, env, "(count (methods area))", 2,
               "remove-method: one fewer after removal");
    expect_string(S, env, "(area {:shape :rect :w 3 :h 4})", "unknown-shape",
                  "remove-method: rect now falls through to :default");
}

static void test_hierarchy_dispatch(mino_state_t *S, mino_env_t *env)
{
    eval(S, env,
         "(derive :child :parent)"
         "(derive :grandchild :child)"
         "(defmulti tag-of identity)"
         "(defmethod tag-of :parent [_] :from-parent)"
         "(defmethod tag-of :grandchild [_] :from-grandchild)");
    expect_string(S, env, "(tag-of :child)", "from-parent",
                  "hierarchy: child dispatches to :parent");
    expect_string(S, env, "(tag-of :grandchild)", "from-grandchild",
                  "hierarchy: exact match beats ancestor");
    expect_string(S, env, "(tag-of :parent)", "from-parent",
                  "hierarchy: exact match on ancestor");
}

static void test_prefer_method(mino_state_t *S, mino_env_t *env)
{
    /* Ambiguous dispatch: :triangle is both :has-sides and :flat.
     * Without a preference, dispatch throws. With prefer-method we
     * resolve the ambiguity. */
    eval(S, env,
         "(derive :triangle :has-sides)"
         "(derive :triangle :flat)"
         "(defmulti feature identity)"
         "(defmethod feature :has-sides [_] :sided)"
         "(defmethod feature :flat [_] :flat-tag)");
    /* Ambiguous now throws; catch via try. */
    expect_string(S, env,
                  "(try (feature :triangle) (catch e :ambiguous))",
                  "ambiguous", "ambiguity: throws before prefer-method");
    eval(S, env, "(prefer-method feature :has-sides :flat)");
    expect_string(S, env, "(feature :triangle)", "sided",
                  "prefer-method: preferred wins");
}

int main(void)
{
    mino_state_t *S   = mino_state_new();
    mino_env_t   *env = mino_env_new(S);
    if (S == NULL || env == NULL) {
        fprintf(stderr, "setup failed\n");
        return 1;
    }
    mino_install_core(S, env);

    test_dispatch_by_keyword(S, env);
    test_methods_api(S, env);
    test_hierarchy_dispatch(S, env);
    test_prefer_method(S, env);

    mino_env_free(S, env);
    mino_state_free(S);

    if (failures == 0) {
        printf("multimethod_test: OK (all assertions passed)\n");
        return 0;
    }
    fprintf(stderr, "multimethod_test: %d FAILURE(S)\n", failures);
    return 1;
}
