/*
 * embed_api_test.c -- C-level smoke test for the embedder helpers:
 *   MINO_VERSION_* / mino_version_string
 *   mino_args_parse
 *   mino_throw
 *
 * Build (from repo root):
 *   cc -std=c99 -Wall -Wextra -Wpedantic -O2 -Isrc -o embed_api_test \
 *       tests/embed_api_test.c src/SRC.c -lm
 * Run: ./embed_api_test
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

/* Primitive that uses mino_args_parse to take (int, string) and returns a
 * vector [i, len(s)] so the test can assert both destructured values. */
static mino_val_t *prim_demo(mino_state_t *S, mino_val_t *args,
                             mino_env_t *env)
{
    long long    i;
    const char  *s;
    size_t       slen;
    mino_val_t  *items[2];
    (void)env;

    if (mino_args_parse(S, "demo", args, "iS", &i, &s, &slen) != 0) {
        return NULL;
    }
    (void)s;

    items[0] = mino_int(S, i);
    items[1] = mino_int(S, (long long)slen);
    return mino_vector(S, items, 2);
}

/* Primitive that always throws a keyword exception via mino_throw. Used
 * to prove (a) the longjmp delivers to the surrounding (try ... catch)
 * and (b) the payload is the same value we passed in. */
static mino_val_t *prim_boom(mino_state_t *S, mino_val_t *args,
                             mino_env_t *env)
{
    (void)args;
    (void)env;
    return mino_throw(S, mino_keyword(S, "boom"));
}

static void test_version(void)
{
    const char *v = mino_version_string();
    char        expected[32];
    snprintf(expected, sizeof(expected), "%d.%d.%d",
             MINO_VERSION_MAJOR, MINO_VERSION_MINOR, MINO_VERSION_PATCH);
    REQUIRE(v != NULL, "mino_version_string returned NULL");
    REQUIRE(v != NULL && strcmp(v, expected) == 0,
            "mino_version_string does not match compile-time constants");
    REQUIRE(MINO_VERSION_MAJOR == 0, "unexpected MAJOR");
    REQUIRE(MINO_VERSION_MINOR >= 48, "unexpected MINOR");
}

static void test_args_parse_ok(mino_state_t *S, mino_env_t *env)
{
    mino_val_t *r;
    long long   first, second;
    mino_register_fn(S, env, "demo", prim_demo);

    r = mino_eval_string(S, "(demo 42 \"hello\")", env);
    REQUIRE(r != NULL, "demo eval returned NULL");
    REQUIRE(r != NULL && r->type == MINO_VECTOR, "demo result not a vector");

    r = mino_eval_string(S, "(nth (demo 42 \"hello\") 0)", env);
    if (mino_to_int(r, &first)) {
        REQUIRE(first == 42, "demo: int arg not preserved");
    } else {
        REQUIRE(0, "demo: first slot not int");
    }
    r = mino_eval_string(S, "(nth (demo 42 \"hello\") 1)", env);
    if (mino_to_int(r, &second)) {
        REQUIRE(second == 5, "demo: string byte length wrong");
    } else {
        REQUIRE(0, "demo: second slot not int");
    }
}

static void test_args_parse_arity(mino_state_t *S, mino_env_t *env)
{
    mino_val_t *r = mino_eval_string(S, "(demo 1)", env);
    const char *err;
    REQUIRE(r == NULL, "arity: demo with 1 arg should error");
    err = mino_last_error(S);
    REQUIRE(err != NULL && strstr(err, "expected 2") != NULL,
            "arity: error does not mention expected 2");
}

static void test_args_parse_type(mino_state_t *S, mino_env_t *env)
{
    mino_val_t *r = mino_eval_string(S, "(demo \"not-int\" \"s\")", env);
    const char *err;
    REQUIRE(r == NULL, "type: demo with wrong first arg should error");
    err = mino_last_error(S);
    REQUIRE(err != NULL && strstr(err, "expected int") != NULL,
            "type: error does not mention expected int");
}

static void test_throw_caught(mino_state_t *S, mino_env_t *env)
{
    mino_val_t *r;
    mino_register_fn(S, env, "boom", prim_boom);

    r = mino_eval_string(S,
        "(try (boom) (catch e (pr-str e)))",
        env);
    REQUIRE(r != NULL, "throw-caught: try-catch returned NULL");
    if (r != NULL && r->type == MINO_STRING) {
        /* The runtime wraps caught exceptions in a diagnostic map that
         * carries the thrown payload under :mino/data. Assert the map
         * printed form includes the keyword we threw. */
        REQUIRE(strstr(r->as.s.data, ":boom") != NULL,
                "throw-caught: payload does not contain :boom");
    } else {
        REQUIRE(0, "throw-caught: catch result is not a string");
    }
}

static void test_throw_uncaught(mino_state_t *S, mino_env_t *env)
{
    mino_val_t *r = mino_eval_string(S, "(boom)", env);
    const char *err;
    REQUIRE(r == NULL, "throw-uncaught: should produce NULL result");
    err = mino_last_error(S);
    REQUIRE(err != NULL && strstr(err, "unhandled exception") != NULL,
            "throw-uncaught: error does not mention unhandled exception");
}

int main(void)
{
    mino_state_t *S = mino_state_new();
    mino_env_t   *env = mino_new(S);

    test_version();
    test_args_parse_ok(S, env);
    test_args_parse_arity(S, env);
    test_args_parse_type(S, env);
    test_throw_caught(S, env);
    test_throw_uncaught(S, env);

    mino_env_free(S, env);
    mino_state_free(S);

    if (failures > 0) {
        fprintf(stderr, "embed_api_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("embed_api_test: all checks passed\n");
    return 0;
}
