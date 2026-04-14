/*
 * map_bench — exercises HAMT-backed maps: bulk build via assoc, get, and
 * update across sizes that straddle the trie's branching boundaries.
 * Reports ns/op so the O(log32 n) scaling is visible.
 *
 * Not part of the smoke suite; not run by CI. Invoke via `make bench-map`.
 */

#include "mino.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Build a map {0 0, 1 1, ..., n-1 n-1} via repeated assoc. ns/element. */
static double bench_build(size_t n)
{
    char        expr[256];
    mino_env_t *env = mino_env_new();
    mino_val_t *form;
    const char *end;
    double      t0, elapsed;
    mino_install_core(env);
    snprintf(expr, sizeof(expr),
             "(loop (i 0 m {})"
             "  (if (< i %zu) (recur (+ i 1) (assoc m i i)) m))",
             n);
    t0   = now_sec();
    form = mino_read(expr, &end);
    if (form == NULL || mino_eval(form, env) == NULL) {
        fprintf(stderr, "bench_build failed: %s\n", mino_last_error());
        exit(1);
    }
    elapsed = now_sec() - t0;
    mino_env_free(env);
    return elapsed * 1e9 / (double)n;
}

/* Build map of size n, then time `reps` (get m key) lookups. ns/op. */
static double bench_get(size_t n, size_t reps)
{
    char        expr[256];
    mino_env_t *env = mino_env_new();
    mino_val_t *form;
    const char *end;
    double      t0;
    size_t      r;
    mino_install_core(env);
    snprintf(expr, sizeof(expr),
             "(def m (loop (i 0 m {})"
             "  (if (< i %zu) (recur (+ i 1) (assoc m i i)) m)))",
             n);
    form = mino_read(expr, &end);
    if (form == NULL || mino_eval(form, env) == NULL) {
        fprintf(stderr, "bench_get build failed: %s\n", mino_last_error());
        exit(1);
    }
    t0 = now_sec();
    for (r = 0; r < reps; r++) {
        size_t      key = (size_t)((unsigned long)r * 2654435761u) % n;
        char        prog[64];
        const char *e2;
        mino_val_t *f2;
        snprintf(prog, sizeof(prog), "(get m %zu)", key);
        f2 = mino_read(prog, &e2);
        (void)mino_eval(f2, env);
    }
    {
        double elapsed = now_sec() - t0;
        mino_env_free(env);
        return elapsed * 1e9 / (double)reps;
    }
}

/* Build map, then time `reps` (assoc m key 999) updates. ns/op. */
static double bench_assoc(size_t n, size_t reps)
{
    char        expr[256];
    mino_env_t *env = mino_env_new();
    mino_val_t *form;
    const char *end;
    double      t0;
    size_t      r;
    mino_install_core(env);
    snprintf(expr, sizeof(expr),
             "(def m (loop (i 0 m {})"
             "  (if (< i %zu) (recur (+ i 1) (assoc m i i)) m)))",
             n);
    form = mino_read(expr, &end);
    if (form == NULL || mino_eval(form, env) == NULL) {
        fprintf(stderr, "bench_assoc build failed: %s\n", mino_last_error());
        exit(1);
    }
    t0 = now_sec();
    for (r = 0; r < reps; r++) {
        size_t      key = (size_t)((unsigned long)r * 2654435761u) % n;
        char        prog[96];
        const char *e2;
        mino_val_t *f2;
        snprintf(prog, sizeof(prog), "(assoc m %zu 999)", key);
        f2 = mino_read(prog, &e2);
        (void)mino_eval(f2, env);
    }
    {
        double elapsed = now_sec() - t0;
        mino_env_free(env);
        return elapsed * 1e9 / (double)reps;
    }
}

int main(void)
{
    static const size_t sizes[] = { 32, 1024, 32768 };
    size_t              i;
    printf("%-10s  %-16s  %-14s  %-14s\n",
           "n", "build ns/element", "get ns/op", "assoc ns/op");
    printf("%-10s  %-16s  %-14s  %-14s\n",
           "----------", "----------------", "--------------",
           "--------------");
    for (i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        size_t n    = sizes[i];
        size_t reps = n >= 32768 ? 100000 : 1000000;
        double tb   = bench_build(n);
        double tg   = bench_get(n, reps);
        double ta   = bench_assoc(n, reps);
        printf("%-10zu  %16.1f  %14.1f  %14.1f\n", n, tb, tg, ta);
        fflush(stdout);
    }
    return 0;
}
