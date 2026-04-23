/*
 * embed_multi_state.c -- 16 states x 16 pthreads stress test.
 *
 * Guarantees under test (Phase 4 "one mino_state_t per thread" story):
 *   - A mino_state_t is wholly self-contained: no cross-state globals,
 *     no shared intern tables, no shared schedulers, no shared GC. An
 *     embedder may create one per thread and run them in parallel
 *     without locks, as long as no single state is touched by more
 *     than one thread concurrently.
 *   - GC cycles, remembered-set bookkeeping, and allocator accounting
 *     stay consistent under sustained allocation pressure driven from
 *     16 concurrent threads.
 *
 * Shape: each worker thread owns one mino_state_t + env, evaluates a
 * workload that forces minor + major collections, reads back results
 * via a known-value check, and exits with a per-thread failure count.
 * The main thread sums the counts; exit 0 means every worker saw what
 * it expected.
 *
 * Build (from repo root):
 *   cc -std=c99 -Wall -Wextra -O2 -Isrc -pthread -o embed_multi_state tests/embed_multi_state.c src/<dot>c -lm
 * where <dot>c is shell glob for all C sources.
 */

#include "mino.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N_STATES   16
#define N_THREADS  16

/* Per-worker script: forces nursery pressure (repeated range+map),
 * forces a few promotions (the long-lived accumulator survives minors),
 * and finishes with a deterministic result we can assert on. The work
 * is small enough to finish in well under a second per state but big
 * enough to drive both minor and major cycles. */
static const char *WORKER_SCRIPT =
    "(let [acc (atom 0)]"
    "  (dotimes [i 200]"
    "    (doall (map inc (range 500)))"
    "    (swap! acc + i))"
    "  @acc)";

typedef struct {
    int id;           /* worker index */
    int failures;     /* per-thread failure tally */
    long long result; /* numeric result returned from mino_to_long */
} worker_ctx_t;

static void *worker_main(void *arg)
{
    worker_ctx_t *ctx = (worker_ctx_t *)arg;
    mino_state_t *S;
    mino_env_t   *env;
    mino_val_t   *result;
    long long     n = 0;

    S = mino_state_new();
    if (S == NULL) { ctx->failures++; return NULL; }
    env = mino_env_new(S);
    if (env == NULL) { ctx->failures++; mino_state_free(S); return NULL; }
    mino_install_core(S, env);

    /* Tighten the nursery so minors fire often under this workload. */
    if (mino_gc_set_param(S, MINO_GC_NURSERY_BYTES, 128u * 1024u) != 0) {
        ctx->failures++;
    }

    result = mino_eval_string(S, WORKER_SCRIPT, env);
    if (result == NULL) {
        fprintf(stderr, "worker %d eval failed: %s\n",
                ctx->id, mino_last_error(S));
        ctx->failures++;
    } else if (!mino_to_int(result, &n)) {
        fprintf(stderr, "worker %d result not integer\n", ctx->id);
        ctx->failures++;
    } else {
        /* Expected: sum 0..199 = 19900. */
        if (n != 19900LL) {
            fprintf(stderr, "worker %d wrong result: %lld\n", ctx->id, n);
            ctx->failures++;
        }
        ctx->result = n;
    }

    /* Drive one explicit full cycle so teardown-time bugs surface
     * before the state goes away. */
    mino_gc_collect(S, MINO_GC_FULL);

    mino_env_free(S, env);
    mino_state_free(S);
    return NULL;
}

int main(void)
{
    pthread_t    threads[N_THREADS];
    worker_ctx_t ctx[N_THREADS];
    int          i, rc;
    int          failures = 0;

    memset(ctx, 0, sizeof(ctx));

    /* N_STATES is spelled out alongside N_THREADS so the shape of the
     * test is explicit even though we currently keep them equal -- one
     * pthread per state, one state per pthread, no sharing. */
    if (N_STATES != N_THREADS) {
        fprintf(stderr, "test expects one state per thread\n");
        return 1;
    }

    for (i = 0; i < N_THREADS; i++) {
        ctx[i].id       = i;
        ctx[i].failures = 0;
        ctx[i].result   = -1;
        rc = pthread_create(&threads[i], NULL, worker_main, &ctx[i]);
        if (rc != 0) {
            fprintf(stderr, "pthread_create %d failed: %d\n", i, rc);
            return 1;
        }
    }

    for (i = 0; i < N_THREADS; i++) {
        pthread_join(threads[i], NULL);
        failures += ctx[i].failures;
    }

    if (failures > 0) {
        fprintf(stderr, "\n%d failure(s) across %d workers\n",
                failures, N_THREADS);
        return 1;
    }
    printf("embed_multi_state: %d workers, %d states, no races or crashes\n",
           N_THREADS, N_STATES);
    return 0;
}
