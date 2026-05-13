/*
 * embed_caps_test.c -- exercises the capability-gated install API.
 *
 * 1. mino_install_minimal: floor C prims only, no core.clj eval. Verify
 *    `+` and basic primitives work but `defn`, `defmulti`, `re-find`,
 *    `slurp` raise MNS001 (defn is in floor core.clj which we didn't
 *    eval) or MNS002 (defmulti/re-find/slurp would be MNS002 once
 *    core.clj is loaded with a partial cap set).
 *
 * 2. mino_install_floor_with_core: minimal + selectively wire only the
 *    multimethods cap, then evaluate core.clj. Verify defmulti is
 *    available but defprotocol (gated on :protocols) is not, and the
 *    diagnostic reports `capability 'protocols' disabled by host`.
 *
 * 3. mino_install_all: full surface. Verify every capability bit is set.
 */

#include "mino_internal.h"

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

static int diag_contains(mino_state_t *S, const char *needle)
{
    const char *err = mino_last_error(S);
    if (err == NULL) return 0;
    return strstr(err, needle) != NULL;
}

static void test_minimal_floor(void)
{
    mino_state_t *S   = mino_state_new();
    mino_env_t   *env = mino_env_new(S);

    mino_install_minimal(S, env);

    REQUIRE(mino_capability_installed(S, MINO_CAP_FLOOR),
            "minimal install sets MINO_CAP_FLOOR");
    REQUIRE(!mino_capability_installed(S, MINO_CAP_REGEX),
            "minimal does not install regex");
    REQUIRE(!mino_capability_installed(S, MINO_CAP_BIGNUM),
            "minimal does not install bignum");
    REQUIRE(!mino_capability_installed(S, MINO_CAP_MULTIMETHODS),
            "minimal does not install multimethods");
    REQUIRE(!mino_capability_installed(S, MINO_CAP_IO),
            "minimal does not install io");

    /* Floor numeric works without core.clj. */
    {
        long long n = 0;
        mino_val_t *v = mino_eval_string(S, "(+ 1 2 3)", env);
        REQUIRE(v != NULL && mino_to_int(v, &n) && n == 6,
                "floor (+ 1 2 3) returns 6");
    }

    /* re-find is unbound and reports the regex capability. */
    {
        mino_val_t *v = mino_eval_string(S, "re-find", env);
        REQUIRE(v == NULL, "re-find resolution without :regex returns NULL");
        REQUIRE(diag_contains(S, "capability 'regex' disabled by host"),
                "re-find diagnostic mentions regex capability");
    }

    /* slurp reports io. */
    {
        mino_val_t *v = mino_eval_string(S, "slurp", env);
        REQUIRE(v == NULL, "slurp resolution without :io returns NULL");
        REQUIRE(diag_contains(S, "capability 'io' disabled by host"),
                "slurp diagnostic mentions io capability");
    }

    mino_env_free(S, env);
    mino_state_free(S);
}

static void test_minimal_plus_multimethods(void)
{
    mino_state_t *S   = mino_state_new();
    mino_env_t   *env = mino_env_new(S);

    /* Install only the multimethods bit; protocols, transducers,
     * regex, bignum stay off. mino_install evaluates core.clj once the
     * cap is set, so the gated defmulti section fires. */
    mino_install(S, env, MINO_CAP_MULTIMETHODS);

    REQUIRE(mino_capability_installed(S, MINO_CAP_MULTIMETHODS),
            "multimethods cap is set after install");
    REQUIRE(!mino_capability_installed(S, MINO_CAP_PROTOCOLS),
            "protocols cap not set when only multimethods requested");
    REQUIRE(!mino_capability_installed(S, MINO_CAP_REGEX),
            "regex cap stays off when not requested");

    /* defmulti is available since multimethods cap is on. */
    {
        mino_val_t *v = mino_eval_string(S,
            "(defmulti shape :kind) (defmethod shape :default [x] :unknown) (shape {:kind :foo})",
            env);
        REQUIRE(v != NULL, "defmulti+defmethod works with multimethods on");
    }

    /* defprotocol is NOT available -- protocols cap is off. */
    {
        mino_val_t *v = mino_eval_string(S, "defprotocol", env);
        REQUIRE(v == NULL, "defprotocol resolution fails when :protocols off");
        REQUIRE(diag_contains(S, "capability 'protocols' disabled by host"),
                "defprotocol diagnostic mentions protocols capability");
    }

    mino_env_free(S, env);
    mino_state_free(S);
}

static void test_install_all(void)
{
    mino_state_t *S   = mino_state_new();
    mino_env_t   *env = mino_env_new(S);
    const mino_capability_info_t *p;

    mino_install_all(S, env);

    for (p = mino_capability_list(); p->name != NULL; p++) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "install_all sets capability '%s'", p->name);
        REQUIRE(mino_capability_installed(S, p->bit), msg);
    }
    {
        mino_val_t *v = mino_eval_string(S, "(re-find \"foo\" \"foobar\")",
                                          env);
        REQUIRE(v != NULL,
                "install_all enables re-find");
    }
    {
        long long n = 0;
        mino_val_t *v = mino_eval_string(S, "(transduce (map inc) + 0 [1 2 3])",
                                          env);
        REQUIRE(v != NULL && mino_to_int(v, &n) && n == 9,
                "install_all enables transduce");
    }

    mino_env_free(S, env);
    mino_state_free(S);
}

int main(void)
{
    test_minimal_floor();
    test_minimal_plus_multimethods();
    test_install_all();

    if (failures == 0) {
        printf("embed_caps_test: PASS\n");
        return 0;
    }
    printf("embed_caps_test: FAIL (%d)\n", failures);
    return 1;
}
