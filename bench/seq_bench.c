/*
 * seq_bench — exercises sequence operations (map, filter, reduce, sort)
 * on lists and vectors of varying sizes.  Reports ns/element so the
 * constant-factor cost of the strict-sequence approach is visible.
 *
 * Not part of the smoke suite; not run by CI. Invoke via `make bench-seq`.
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

/* Time (map inc (range n)) — builds a list and maps over it. ns/element. */
static double bench_map(size_t n)
{
    mino_state_t *S = mino_state_new();
    char        expr[256];
    mino_env_t *env = mino_new(S);
    mino_val_t *form;
    const char *end;
    double      t0, elapsed;
    mino_eval_string(S, "(def inc (fn (x) (+ x 1)))", env);
    snprintf(expr, sizeof(expr), "(map inc (range %zu))", n);
    t0   = now_sec();
    form = mino_read(S, expr, &end);
    if (form == NULL || mino_eval(S, form, env) == NULL) {
        fprintf(stderr, "bench_map failed: %s\n", mino_last_error(S));
        exit(1);
    }
    elapsed = now_sec() - t0;
    mino_env_free(S, env);
    mino_state_free(S);
    return elapsed * 1e9 / (double)n;
}

/* Time (filter even? (range n)). ns/element. */
static double bench_filter(size_t n)
{
    mino_state_t *S = mino_state_new();
    char        expr[256];
    mino_env_t *env = mino_new(S);
    mino_val_t *form;
    const char *end;
    double      t0, elapsed;
    mino_eval_string(S,
        "(def even? (fn (x) (= 0 (- x (* 2 (/ x 2))))))", env);
    snprintf(expr, sizeof(expr), "(filter even? (range %zu))", n);
    t0   = now_sec();
    form = mino_read(S, expr, &end);
    if (form == NULL || mino_eval(S, form, env) == NULL) {
        fprintf(stderr, "bench_filter failed: %s\n", mino_last_error(S));
        exit(1);
    }
    elapsed = now_sec() - t0;
    mino_env_free(S, env);
    mino_state_free(S);
    return elapsed * 1e9 / (double)n;
}

/* Time (reduce + 0 (range n)). ns/element. */
static double bench_reduce(size_t n)
{
    mino_state_t *S = mino_state_new();
    char        expr[256];
    mino_env_t *env = mino_new(S);
    mino_val_t *form;
    const char *end;
    double      t0, elapsed;
    snprintf(expr, sizeof(expr), "(reduce + 0 (range %zu))", n);
    t0   = now_sec();
    form = mino_read(S, expr, &end);
    if (form == NULL || mino_eval(S, form, env) == NULL) {
        fprintf(stderr, "bench_reduce failed: %s\n", mino_last_error(S));
        exit(1);
    }
    elapsed = now_sec() - t0;
    mino_env_free(S, env);
    mino_state_free(S);
    return elapsed * 1e9 / (double)n;
}

/* Time (sort (reverse (range n))). ns/element. */
static double bench_sort(size_t n)
{
    mino_state_t *S = mino_state_new();
    char        expr[256];
    mino_env_t *env = mino_new(S);
    mino_val_t *form;
    const char *end;
    double      t0, elapsed;
    snprintf(expr, sizeof(expr), "(sort (reverse (range %zu)))", n);
    t0   = now_sec();
    form = mino_read(S, expr, &end);
    if (form == NULL || mino_eval(S, form, env) == NULL) {
        fprintf(stderr, "bench_sort failed: %s\n", mino_last_error(S));
        exit(1);
    }
    elapsed = now_sec() - t0;
    mino_env_free(S, env);
    mino_state_free(S);
    return elapsed * 1e9 / (double)n;
}

int main(void)
{
    static const size_t sizes[] = { 100, 1000, 10000 };
    size_t              i;
    printf("%-10s  %-16s  %-16s  %-16s  %-16s\n",
           "n", "map ns/elem", "filter ns/elem", "reduce ns/elem",
           "sort ns/elem");
    printf("%-10s  %-16s  %-16s  %-16s  %-16s\n",
           "----------", "----------------", "----------------",
           "----------------", "----------------");
    for (i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        size_t n  = sizes[i];
        double tm = bench_map(n);
        double tf = bench_filter(n);
        double tr = bench_reduce(n);
        double ts = bench_sort(n);
        printf("%-10zu  %16.1f  %16.1f  %16.1f  %16.1f\n",
               n, tm, tf, tr, ts);
        fflush(stdout);
    }
    return 0;
}
