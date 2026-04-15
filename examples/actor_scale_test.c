/*
 * actor_scale_test.c -- find the practical limits of actor count.
 * Spawns actors in increasing batches, measures memory and timing,
 * reports where things break or get impractical.
 */

#include "mino.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void test_scale(int count)
{
    mino_actor_t **actors;
    mino_state_t *host;
    double t0, t1, t2, t3;
    int i;

    printf("  %6d actors: ", count);
    fflush(stdout);

    actors = (mino_actor_t **)calloc((size_t)count, sizeof(*actors));
    if (!actors) {
        printf("FAIL: calloc for actor array\n");
        return;
    }
    host = mino_state_new();

    /* Phase 1: create all actors */
    t0 = now_ms();
    for (i = 0; i < count; i++) {
        actors[i] = mino_actor_new();
        if (!actors[i]) {
            printf("FAIL: mino_actor_new at %d\n", i);
            /* Clean up what we have */
            int j;
            for (j = 0; j < i; j++) mino_actor_free(actors[j]);
            mino_state_free(host);
            free(actors);
            return;
        }
    }
    t1 = now_ms();

    /* Phase 2: send a message to each */
    for (i = 0; i < count; i++) {
        mino_actor_send(actors[i], host, mino_int(host, (long long)i));
    }
    t2 = now_ms();

    /* Phase 3: receive from each and verify */
    for (i = 0; i < count; i++) {
        mino_val_t *msg = mino_actor_recv(actors[i]);
        if (!msg) {
            printf("FAIL: recv null at %d\n", i);
            break;
        }
        long long v;
        if (!mino_to_int(msg, &v) || v != (long long)i) {
            printf("FAIL: wrong value at %d\n", i);
            break;
        }
    }

    /* Phase 4: free all */
    for (i = 0; i < count; i++) {
        mino_actor_free(actors[i]);
    }
    t3 = now_ms();

    mino_state_free(host);
    free(actors);

    printf("create %.0fms  send+recv %.0fms  free %.0fms  total %.0fms\n",
           t1 - t0, t2 - t1, t3 - t2, t3 - t0);
}

/* Test with eval in each actor -- heavier per-actor cost */
static void test_scale_with_eval(int count)
{
    mino_actor_t **actors;
    mino_state_t *host;
    double t0, t1;
    int i;
    long long total = 0;

    printf("  %6d actors+eval: ", count);
    fflush(stdout);

    actors = (mino_actor_t **)calloc((size_t)count, sizeof(*actors));
    if (!actors) {
        printf("FAIL: calloc\n");
        return;
    }
    host = mino_state_new();

    t0 = now_ms();
    for (i = 0; i < count; i++) {
        actors[i] = mino_actor_new();
        if (!actors[i]) {
            printf("FAIL: mino_actor_new at %d\n", i);
            int j;
            for (j = 0; j < i; j++) mino_actor_free(actors[j]);
            mino_state_free(host);
            free(actors);
            return;
        }

        /* Send, receive, eval, clone back */
        mino_actor_send(actors[i], host, mino_int(host, (long long)i));
        mino_val_t *msg = mino_actor_recv(actors[i]);
        mino_state_t *as = mino_actor_state(actors[i]);
        mino_env_t *ae = mino_actor_env(actors[i]);
        mino_env_set(as, ae, "__x", msg);
        mino_val_t *r = mino_eval_string(as, "(* __x __x)", ae);
        if (r) {
            mino_val_t *cloned = mino_clone(host, as, r);
            if (cloned) {
                long long v;
                if (mino_to_int(cloned, &v)) total += v;
            }
        }
    }
    t1 = now_ms();

    /* Free all */
    for (i = 0; i < count; i++) {
        mino_actor_free(actors[i]);
    }

    mino_state_free(host);
    free(actors);

    printf("%.0fms  (sum of squares: %lld)\n", t1 - t0, total);
}

int main(void)
{
    printf("Actor scale test\n");
    printf("-----------------\n");
    printf("\nPhase 1: spawn + send/recv + free (lightweight)\n");
    test_scale(100);
    test_scale(1000);
    test_scale(5000);
    test_scale(10000);
    test_scale(25000);
    test_scale(50000);

    printf("\nPhase 2: spawn + send + eval + clone + free (heavyweight)\n");
    test_scale_with_eval(100);
    test_scale_with_eval(1000);
    test_scale_with_eval(5000);
    test_scale_with_eval(10000);

    printf("\n-----------------\n");
    printf("Done. Review timings to find the practical ceiling.\n");
    return 0;
}
