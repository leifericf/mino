/*
 * embed_host_threads.c -- foundation-slice smoke test for the
 * host-thread grant API (cycle G4).
 *
 * Creates two states: one without a thread grant, one with. Confirms
 *
 *   - default thread limit is 1
 *   - mino_set_thread_limit raises it
 *   - mino_get_thread_limit reads it
 *   - mino_thread_count is 0 in this slice (no spawn yet)
 *   - script-side `(future ...)` throws :mino/unsupported
 *     with a message that distinguishes the two grant states
 *
 * The full host-thread runtime lands across upcoming versions; this
 * program is the contract test for the v0.84.0 surface.
 *
 * Build (from repo root):
 *   ./mino task build
 *   cc -std=c99 -Isrc -o embed_host_threads \
 *       examples/embed_host_threads.c \
 *       <objects from `mino task build`> -lm
 */

#include "mino.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Wrap `form` in (try ... (catch e (:mino/message e))) so the throw
 * message is returned as a value instead of surfacing through
 * mino_last_error (which loses the structured payload). */
static int run_throw(mino_state_t *S, mino_env_t *env,
                     const char *form, const char *expect_substring)
{
    char buf[1024];
    mino_val_t *result;
    const char *src;
    size_t len;
    int n = snprintf(buf, sizeof(buf),
                     "(try %s (catch e (:mino/message e)))", form);
    if (n < 0 || n >= (int)sizeof(buf)) {
        fprintf(stderr, "form too large: %s\n", form);
        return 1;
    }
    result = mino_eval_string(S, buf, env);
    if (result == NULL) {
        fprintf(stderr, "wrapper eval failed for: %s -> %s\n",
                form, mino_last_error(S));
        return 1;
    }
    if (mino_to_string(result, &src, &len) == 0) {
        fprintf(stderr, "expected string message, got non-string for: %s\n",
                form);
        return 1;
    }
    if (strstr(src, expect_substring) == NULL) {
        fprintf(stderr, "expected substring %s, got: %.*s\n",
                expect_substring, (int)len, src);
        return 1;
    }
    return 0;
}

int main(void)
{
    int failures = 0;

    /* State A: no thread grant. */
    {
        mino_state_t *S = mino_state_new();
        mino_env_t   *env = mino_env_new(S);
        mino_install_all(S, env);

        if (mino_get_thread_limit(S) != 1) {
            fprintf(stderr, "default limit not 1\n");
            failures++;
        }
        if (mino_thread_count(S) != 0) {
            fprintf(stderr, "default count not 0\n");
            failures++;
        }
        failures += run_throw(S, env, "(future (+ 1 1))", "not granted");
        failures += run_throw(S, env, "(promise)",        "not granted");
        failures += run_throw(S, env, "(thread (+ 1 1))", "not granted");

        mino_quiesce_threads(S);
        mino_env_free(S, env);
        mino_state_free(S);
    }

    /* State B: limit = 4 grant. */
    {
        mino_state_t *S = mino_state_new();
        mino_env_t   *env = mino_env_new(S);
        mino_install_all(S, env);
        mino_set_thread_limit(S, 4);

        if (mino_get_thread_limit(S) != 4) {
            fprintf(stderr, "set limit not honored\n");
            failures++;
        }
        failures += run_throw(S, env, "(future (+ 1 1))", "in flight");
        failures += run_throw(S, env, "(thread (+ 1 1))", "in flight");

        mino_quiesce_threads(S);
        mino_env_free(S, env);
        mino_state_free(S);
    }

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("embed_host_threads: ok\n");
    return 0;
}
