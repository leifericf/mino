/*
 * embed_multi_tenant_threads.c -- cross-state thread pool sharing
 * (Cycle G4.5).
 *
 * The shape this example demonstrates is a host that runs many
 * isolated mino runtimes (per-tenant scripting, per-NPC AI, per-buffer
 * linter, per-request handler) on a shared pool of OS threads. The
 * key is mino_set_thread_pool: each state delegates its (future ...)
 * spawns to the same host pool, so N threads fan out across M states
 * without each state owning its own pthread set.
 *
 * The pool here is a simple work-queue + N pthreads written from
 * scratch to keep the example self-contained. Real hosts would point
 * mino at Tokio's runtime, libuv's worker pool, ASIO's io_context,
 * or whatever scheduling primitive they already have.
 *
 * Run:
 *   ./mino task build
 *   cc -std=c99 -Isrc -o embed_multi_tenant_threads \
 *       examples/embed_multi_tenant_threads.c <objects> -lm -lpthread
 *   ./embed_multi_tenant_threads
 */

#include "mino.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* A trivial host pool                                                       */
/* ------------------------------------------------------------------------- */

typedef struct work_item {
    void (*fn)(void *);
    void  *ctx;
    struct work_item *next;
} work_item_t;

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    work_item_t    *head;
    work_item_t    *tail;
    int             shutting_down;
    int             pending;
    pthread_cond_t  drained;
} demo_pool_t;

static void *demo_worker(void *arg)
{
    demo_pool_t *pool = (demo_pool_t *)arg;
    for (;;) {
        work_item_t *w;
        pthread_mutex_lock(&pool->mu);
        while (pool->head == NULL && !pool->shutting_down) {
            pthread_cond_wait(&pool->cv, &pool->mu);
        }
        if (pool->head == NULL && pool->shutting_down) {
            pthread_mutex_unlock(&pool->mu);
            return NULL;
        }
        w = pool->head;
        pool->head = w->next;
        if (pool->head == NULL) { pool->tail = NULL; }
        pthread_mutex_unlock(&pool->mu);

        w->fn(w->ctx);
        free(w);

        pthread_mutex_lock(&pool->mu);
        pool->pending--;
        if (pool->pending == 0) { pthread_cond_broadcast(&pool->drained); }
        pthread_mutex_unlock(&pool->mu);
    }
}

static int demo_submit(struct mino_thread_pool *pool, void (*fn)(void *), void *ctx)
{
    demo_pool_t *p = (demo_pool_t *)pool->user_data;
    work_item_t *w = (work_item_t *)calloc(1, sizeof(*w));
    if (w == NULL) { return -1; }
    w->fn  = fn;
    w->ctx = ctx;

    pthread_mutex_lock(&p->mu);
    if (p->shutting_down) {
        pthread_mutex_unlock(&p->mu);
        free(w);
        return -2;
    }
    if (p->tail) { p->tail->next = w; } else { p->head = w; }
    p->tail = w;
    p->pending++;
    pthread_cond_signal(&p->cv);
    pthread_mutex_unlock(&p->mu);
    return 0;
}

static demo_pool_t *demo_pool_new(int n_workers, pthread_t *threads)
{
    demo_pool_t *p = (demo_pool_t *)calloc(1, sizeof(*p));
    int i;
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);
    pthread_cond_init(&p->drained, NULL);
    for (i = 0; i < n_workers; i++) {
        pthread_create(&threads[i], NULL, demo_worker, p);
    }
    return p;
}

static void demo_pool_drain(demo_pool_t *p)
{
    pthread_mutex_lock(&p->mu);
    while (p->pending > 0) {
        pthread_cond_wait(&p->drained, &p->mu);
    }
    pthread_mutex_unlock(&p->mu);
}

static void demo_pool_shutdown(demo_pool_t *p, int n_workers, pthread_t *threads)
{
    int i;
    pthread_mutex_lock(&p->mu);
    p->shutting_down = 1;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
    for (i = 0; i < n_workers; i++) { pthread_join(threads[i], NULL); }
    pthread_mutex_destroy(&p->mu);
    pthread_cond_destroy(&p->cv);
    pthread_cond_destroy(&p->drained);
    free(p);
}

/* ------------------------------------------------------------------------- */
/* Multi-tenant scenario                                                     */
/* ------------------------------------------------------------------------- */

#define N_TENANTS  6
#define N_WORKERS  3   /* fewer than tenants — pool fan-out is the point */

int main(void)
{
    pthread_t            workers[N_WORKERS];
    demo_pool_t         *p_impl;
    mino_thread_pool_t   pool;
    mino_state_t        *tenants[N_TENANTS];
    mino_env_t          *envs[N_TENANTS];
    mino_val_t          *results[N_TENANTS];
    int i, failures = 0;

    p_impl = demo_pool_new(N_WORKERS, workers);
    pool.submit_fn = demo_submit;
    pool.user_data = p_impl;

    /* Spin up N tenants, each with its own runtime + isolated env.
     * Wire each tenant to the same host pool. */
    for (i = 0; i < N_TENANTS; i++) {
        mino_state_t *S = mino_state_new();
        mino_env_t   *env = mino_env_new(S);
        mino_install_all(S, env);
        mino_set_thread_limit(S, 4);
        mino_set_thread_pool(S, &pool);
        tenants[i] = S;
        envs[i]    = env;
    }

    /* From each tenant, fire a (future ...) that returns its tenant id
     * times 100. This forces the host pool to multiplex N_TENANTS work
     * items across N_WORKERS pthreads. */
    for (i = 0; i < N_TENANTS; i++) {
        char form[128];
        snprintf(form, sizeof(form),
                 "(future (* %d 100))", i);
        results[i] = mino_eval_string(tenants[i], form, envs[i]);
        if (results[i] == NULL) {
            fprintf(stderr, "tenant %d: spawn failed: %s\n",
                    i, mino_last_error(tenants[i]));
            failures++;
        }
    }

    /* Deref each future. The work might have already completed on one
     * of the pool workers; if not, deref parks until it does. */
    for (i = 0; i < N_TENANTS; i++) {
        char form[64];
        long long val;
        mino_val_t *got;
        snprintf(form, sizeof(form),
                 "(deref (future (* %d 100)))", i);
        got = mino_eval_string(tenants[i], form, envs[i]);
        if (got == NULL) {
            fprintf(stderr, "tenant %d: deref failed: %s\n",
                    i, mino_last_error(tenants[i]));
            failures++;
            continue;
        }
        if (mino_to_int(got, &val) == 0 || val != (long long)i * 100) {
            fprintf(stderr, "tenant %d: expected %d\n", i, i * 100);
            failures++;
        }
    }

    /* Quiesce each tenant before tearing down its state. With a pool,
     * quiesce cv-waits (mino doesn't own the pthreads). */
    for (i = 0; i < N_TENANTS; i++) {
        mino_quiesce_threads(tenants[i]);
        mino_env_free(tenants[i], envs[i]);
        mino_state_free(tenants[i]);
    }

    /* Drain any straggler items, then shut the pool down. */
    demo_pool_drain(p_impl);
    demo_pool_shutdown(p_impl, N_WORKERS, workers);

    if (failures > 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("embed_multi_tenant_threads: ok (%d tenants on %d pool workers)\n",
           N_TENANTS, N_WORKERS);
    return 0;
}
