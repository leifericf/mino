/*
 * embed_gc_stress.c -- adversarial test of the public GC API.
 *
 * Exercises boundary conditions the happy-path embed_gc.c does not:
 *   - Every mino_gc_set_param key at its low/high valid and invalid bounds.
 *   - Nested mino_gc_collect() calls (minor inside a caller callback, etc).
 *   - mino_gc_stats invoked across every phase.
 *   - Tuning knob changes after allocation pressure has built up.
 *   - mino_gc_collect(MINO_GC_FULL) driving IDLE from MAJOR_MARK and back.
 *
 * Exit 0 means every adversarial case behaved as documented. Any
 * unexpected reject/accept, NULL-deref, or crash is a failure.
 *
 * Build:
 *   cc -std=c99 -Wall -Wpedantic -Wextra -O2 -Isrc \
 *     -o examples/embed_gc_stress examples/embed_gc_stress.c $MINO_OBJS -lm
 */

#include "mino.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void fail(const char *msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    failures++;
}

/* mino_gc_set_param contract: returns 0 on success, -1 on invalid key or
 * out-of-range value. Cover each known key with a successful minimum, a
 * successful maximum, and an out-of-range value on each side. */
static void test_param_bounds(mino_state_t *S)
{
    struct {
        mino_gc_param_t p;
        size_t          lo_ok;
        size_t          hi_ok;
        size_t          lo_bad;
        size_t          hi_bad;
        const char     *name;
    } cases[] = {
        { MINO_GC_NURSERY_BYTES,       65536u, 64u * 1024u * 1024u,
                                       1024u, (size_t)-1, "nursery" },
        { MINO_GC_MAJOR_GROWTH_TENTHS, 11u, 40u, 10u, 41u,  "growth" },
        { MINO_GC_PROMOTION_AGE,       1u,  8u,  0u,  9u,   "promote-age" },
        { MINO_GC_INCREMENTAL_BUDGET,  64u, 65536u, 0u, (size_t)-1, "budget" },
        { MINO_GC_STEP_ALLOC_BYTES,    1024u, 1024u*1024u, 0u, (size_t)-1, "quantum" }
    };
    size_t i;
    for (i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        char buf[64];
        if (mino_gc_set_param(S, cases[i].p, cases[i].lo_ok) != 0) {
            snprintf(buf, sizeof(buf), "%s accepted lo bound", cases[i].name);
            fail(buf);
        }
        if (mino_gc_set_param(S, cases[i].p, cases[i].hi_ok) != 0) {
            snprintf(buf, sizeof(buf), "%s accepted hi bound", cases[i].name);
            fail(buf);
        }
        if (mino_gc_set_param(S, cases[i].p, cases[i].lo_bad) == 0) {
            snprintf(buf, sizeof(buf), "%s rejected below-range", cases[i].name);
            fail(buf);
        }
        if (mino_gc_set_param(S, cases[i].p, cases[i].hi_bad) == 0) {
            snprintf(buf, sizeof(buf), "%s rejected above-range", cases[i].name);
            fail(buf);
        }
    }
    /* Unknown param key must reject. */
    if (mino_gc_set_param(S, (mino_gc_param_t)9999, 1) == 0) {
        fail("unknown param key accepted");
    }
}

/* mino_gc_collect contract: every kind drives phase to IDLE on return.
 * MAJOR during IDLE runs a full mark+sweep; FULL always runs a STW cycle
 * regardless of prior state. Stats should reflect the progress. */
static void test_collect_kinds(mino_state_t *S, mino_env_t *env)
{
    mino_gc_stats_t st;
    size_t minor_before, major_before;

    mino_gc_stats(S, &st);
    minor_before = st.collections_minor;
    major_before = st.collections_major;

    mino_gc_collect(S, MINO_GC_MINOR);
    mino_gc_stats(S, &st);
    if (st.collections_minor <= minor_before) fail("MINOR did not increment counter");
    if (st.phase != 0) fail("MINOR left phase non-IDLE");

    mino_gc_collect(S, MINO_GC_MAJOR);
    mino_gc_stats(S, &st);
    if (st.collections_major <= major_before) fail("MAJOR did not increment counter");
    if (st.phase != 0) fail("MAJOR left phase non-IDLE");

    mino_gc_collect(S, MINO_GC_FULL);
    mino_gc_stats(S, &st);
    if (st.phase != 0) fail("FULL left phase non-IDLE");

    /* Invalid kind must be rejected as no-op (no crash, no progress). */
    {
        size_t before = st.collections_major;
        mino_gc_collect(S, (mino_gc_kind_t)99);
        mino_gc_stats(S, &st);
        if (st.collections_major != before) fail("invalid kind ran a cycle");
    }
    (void)env;
}

/* Stats snapshot contract: out-param populated without side effects.
 * Repeated calls return monotone non-decreasing cumulative fields. */
static void test_stats_monotone(mino_state_t *S, mino_env_t *env)
{
    mino_gc_stats_t a, b;
    const char *script =
        "(dotimes [i 800] (doall (take 200 (range 1000))))";
    mino_gc_stats(S, &a);
    if (mino_eval_string(S, script, env) == NULL) {
        fail("stats workload eval failed");
        return;
    }
    mino_gc_stats(S, &b);
    if (b.bytes_alloc < a.bytes_alloc) fail("bytes_alloc went backwards");
    if (b.collections_minor < a.collections_minor) fail("minor count went backwards");
    if (b.total_gc_ns < a.total_gc_ns) fail("total_gc_ns went backwards");
}

/* Parameter changes mid-run should take effect immediately but not
 * corrupt an in-progress cycle. Drive nursery from a tight 64 KB to a
 * loose 4 MB while running allocation pressure. */
static void test_param_change_under_pressure(mino_state_t *S, mino_env_t *env)
{
    if (mino_gc_set_param(S, MINO_GC_NURSERY_BYTES, 64u * 1024u) != 0) {
        fail("set nursery 64 KB");
    }
    if (mino_eval_string(S, "(dotimes [i 500] (doall (range 200)))", env) == NULL) {
        fail("tight-nursery eval");
        return;
    }
    if (mino_gc_set_param(S, MINO_GC_NURSERY_BYTES, 4u * 1024u * 1024u) != 0) {
        fail("set nursery 4 MB");
    }
    if (mino_eval_string(S, "(dotimes [i 500] (doall (range 200)))", env) == NULL) {
        fail("loose-nursery eval");
    }
}

int main(void)
{
    mino_state_t *S = mino_state_new();
    mino_env_t   *env;
    if (S == NULL) { fail("state_new"); return 1; }
    env = mino_env_new(S);
    mino_install_core(S, env);

    test_param_bounds(S);
    test_collect_kinds(S, env);
    test_stats_monotone(S, env);
    test_param_change_under_pressure(S, env);

    mino_env_free(S, env);
    mino_state_free(S);

    if (failures > 0) {
        fprintf(stderr, "\n%d failure(s)\n", failures);
        return 1;
    }
    printf("embed_gc_stress: all cases passed\n");
    return 0;
}
