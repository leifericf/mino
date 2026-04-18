/*
 * vector_bench — exercises bulk build, nth, and assoc across vector
 * sizes that straddle the trie's level boundaries (32, 1024, 32768, 2^20).
 * Reports ns/op so the O(log32 n) scaling of the persistent layout is
 * visible: lookup and update costs should grow as a slow staircase while
 * bulk-build cost per element stays nearly flat.
 *
 * Parses and evaluates inside the measured loop, so absolute numbers
 * include reader+eval overhead. That overhead is constant across sizes,
 * so relative scaling is the meaningful signal. Not part of the smoke
 * suite; not run by CI. Invoke via `make bench`.
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

/* Build a vector of [0, n) through vec_from_array and return ns/element. */
static double bench_build(size_t n)
{
    mino_state_t *S = mino_state_new();
    mino_val_t  **items = (mino_val_t **)malloc(n * sizeof(*items));
    mino_val_t   *v;
    double        t0;
    double        elapsed;
    size_t        i;
    if (items == NULL) {
        perror("malloc");
        exit(1);
    }
    for (i = 0; i < n; i++) {
        items[i] = mino_int(S, (long long)i);
    }
    t0 = now_sec();
    v  = mino_vector(S, items, n);
    elapsed = now_sec() - t0;
    free(items);
    (void)v;
    mino_state_free(S);
    return elapsed * 1e9 / (double)n;
}

/* Build v of length n in the env, then time `reps` (nth v idx) evaluations
 * at pseudo-random indices. Reports ns/op. */
static double bench_nth(size_t n, size_t reps)
{
    mino_state_t *S = mino_state_new();
    char        expr[256];
    mino_env_t *env = mino_new(S);
    mino_val_t *form;
    const char *end;
    double      t0;
    size_t      r;
    snprintf(expr, sizeof(expr),
             "(def v (loop (i 0 v []) "
             "  (if (< i %zu) (recur (+ i 1) (conj v i)) v)))",
             n);
    form = mino_read(S, expr, &end);
    if (form == NULL || mino_eval(S, form, env) == NULL) {
        fprintf(stderr, "bench_nth build failed: %s\n", mino_last_error(S));
        exit(1);
    }
    t0 = now_sec();
    for (r = 0; r < reps; r++) {
        size_t      idx = (size_t)((unsigned long)r * 2654435761u) % n;
        char        prog[64];
        const char *e2;
        mino_val_t *f2;
        snprintf(prog, sizeof(prog), "(nth v %zu)", idx);
        f2 = mino_read(S, prog, &e2);
        (void)mino_eval(S, f2, env);
    }
    {
        double elapsed = now_sec() - t0;
        mino_env_free(S, env);
        mino_state_free(S);
        return elapsed * 1e9 / (double)reps;
    }
}

/* Build v and time `reps` (assoc v idx 999) calls, each path-copying. */
static double bench_assoc(size_t n, size_t reps)
{
    mino_state_t *S = mino_state_new();
    char        expr[256];
    mino_env_t *env = mino_new(S);
    mino_val_t *form;
    const char *end;
    double      t0;
    size_t      r;
    snprintf(expr, sizeof(expr),
             "(def v (loop (i 0 v []) "
             "  (if (< i %zu) (recur (+ i 1) (conj v i)) v)))",
             n);
    form = mino_read(S, expr, &end);
    if (form == NULL || mino_eval(S, form, env) == NULL) {
        fprintf(stderr, "bench_assoc build failed: %s\n", mino_last_error(S));
        exit(1);
    }
    t0 = now_sec();
    for (r = 0; r < reps; r++) {
        size_t      idx = (size_t)((unsigned long)r * 2654435761u) % n;
        char        prog[96];
        const char *e2;
        mino_val_t *f2;
        snprintf(prog, sizeof(prog), "(assoc v %zu 999)", idx);
        f2 = mino_read(S, prog, &e2);
        (void)mino_eval(S, f2, env);
    }
    {
        double elapsed = now_sec() - t0;
        mino_env_free(S, env);
        mino_state_free(S);
        return elapsed * 1e9 / (double)reps;
    }
}

int main(void)
{
    static const size_t sizes[] = { 32, 1024, 32768, 1048576 };
    size_t              i;
    printf("%-10s  %-16s  %-14s  %-14s\n",
           "n", "build ns/element", "nth ns/op", "assoc ns/op");
    printf("%-10s  %-16s  %-14s  %-14s\n",
           "----------", "----------------", "--------------",
           "--------------");
    for (i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        size_t n    = sizes[i];
        size_t reps = n >= 32768 ? 100000 : 1000000;
        double tb   = bench_build(n);
        double tn   = bench_nth(n, reps);
        double ta   = bench_assoc(n, reps);
        printf("%-10zu  %16.1f  %14.1f  %14.1f\n", n, tb, tn, ta);
        fflush(stdout);
    }
    return 0;
}
