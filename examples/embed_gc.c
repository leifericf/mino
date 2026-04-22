/*
 * embed_gc.c -- minimal embedder sanity program for the public GC API.
 *
 * Creates a mino state, evaluates a script that allocates pressure,
 * queries collector stats, tunes a parameter, and triggers each kind
 * of collection. Used as a smoke test that mino.h stays host-friendly.
 *
 * Build (from repo root):
 *   cc -std=c99 -Isrc -o embed_gc examples/embed_gc.c src/xxx.o ... -lm
 * or link against the objects produced by `mino task build`.
 */

#include "mino.h"

#include <stdio.h>
#include <stdlib.h>

static void report(mino_state_t *S, const char *label)
{
    mino_gc_stats_t st;
    mino_gc_stats(S, &st);
    printf("%-18s minor=%zu major=%zu live=%zu young=%zu old=%zu rem=%zu phase=%d\n",
           label,
           st.collections_minor, st.collections_major,
           st.bytes_live, st.bytes_young, st.bytes_old,
           st.remset_entries, st.phase);
}

int main(void)
{
    mino_state_t *S;
    mino_env_t   *env;
    mino_val_t   *result;

    S = mino_state_new();
    if (S == NULL) {
        fprintf(stderr, "state_new failed\n");
        return 1;
    }
    env = mino_env_new(S);
    mino_install_core(S, env);

    report(S, "initial");

    /* Tune: tighten the nursery so minors fire often under light load. */
    if (mino_gc_set_param(S, MINO_GC_NURSERY_BYTES, 128u * 1024u) != 0) {
        fprintf(stderr, "set_param nursery failed\n");
        return 1;
    }

    /* Out-of-range parameter must be rejected. */
    if (mino_gc_set_param(S, MINO_GC_PROMOTION_AGE, 99u) == 0) {
        fprintf(stderr, "set_param promotion-age should have rejected 99\n");
        return 1;
    }

    /* Allocate pressure: build a vector of vectors inside mino so the
     * conservative stack scan sees no raw pointers we depend on. */
    result = mino_eval_string(S,
        "(do (dotimes [i 200] (doall (map inc (range 200)))) :done)",
        env);
    if (result == NULL) {
        fprintf(stderr, "eval failed: %s\n", mino_last_error(S));
        return 1;
    }
    report(S, "after-eval");

    mino_gc_collect(S, MINO_GC_MINOR);
    report(S, "after-minor");

    mino_gc_collect(S, MINO_GC_MAJOR);
    report(S, "after-major");

    mino_gc_collect(S, MINO_GC_FULL);
    report(S, "after-full");

    mino_env_free(S, env);
    mino_state_free(S);
    return 0;
}
