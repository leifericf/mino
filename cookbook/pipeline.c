/*
 * pipeline.c — transform data through mino expressions.
 *
 * Demonstrates: pushing C data into mino as vectors/maps, running
 * sequence operations, extracting transformed results back to C.
 * Shows how mino can serve as a lightweight ETL / data-wrangling layer.
 *
 * Build:  cc -std=c99 -I.. -o pipeline pipeline.c ../mino.c
 * Run:    ./pipeline
 */

#include "mino.h"
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    mino_env_t *env = mino_new();
    mino_val_t *result;

    /* Build a vector of employee records from C data. */
    {
        static const char *names[]  = { "Alice", "Bob", "Carol", "Dave" };
        static const int   ages[]   = { 30, 45, 28, 52 };
        static const int salaries[] = { 75000, 92000, 68000, 110000 };
        size_t i;
        size_t n = sizeof(names) / sizeof(names[0]);
        mino_val_t **records = (mino_val_t **)malloc(n * sizeof(*records));

        for (i = 0; i < n; i++) {
            mino_val_t *keys[3], *vals[3];
            keys[0] = mino_keyword("name");
            vals[0] = mino_string(names[i]);
            keys[1] = mino_keyword("age");
            vals[1] = mino_int(ages[i]);
            keys[2] = mino_keyword("salary");
            vals[2] = mino_int(salaries[i]);
            records[i] = mino_map(keys, vals, 3);
        }
        mino_env_set(env, "employees", mino_vector(records, n));
        free(records);
    }

    /* Pipeline 1: names of employees over 35. */
    printf("Over 35:\n");
    result = mino_eval_string(
        "(->> employees\n"
        "     (filter (fn (e) (> (get e :age) 35)))\n"
        "     (map (fn (e) (get e :name))))",
        env);
    if (result != NULL) {
        printf("  ");
        mino_println(result);
    }

    /* Pipeline 2: total salary budget. */
    result = mino_eval_string(
        "(reduce + 0 (map (fn (e) (get e :salary)) employees))",
        env);
    if (result != NULL) {
        long long total;
        if (mino_to_int(result, &total)) {
            printf("Total salary: %lld\n", total);
        }
    }

    /* Pipeline 3: sorted by salary descending. */
    printf("By salary (desc):\n");
    result = mino_eval_string(
        "(->> employees\n"
        "     (sort)\n"
        "     (reverse)\n"
        "     (map (fn (e) (str (get e :name) \": $\" (get e :salary)))))",
        env);
    if (result != NULL) {
        /* Walk the result list and print each line. */
        mino_val_t *p = result;
        while (mino_is_cons(p)) {
            const char *s;
            size_t      len;
            if (mino_to_string(p->as.cons.car, &s, &len)) {
                printf("  %.*s\n", (int)len, s);
            }
            p = p->as.cons.cdr;
        }
    }

    /* Pipeline 4: group into age brackets using into + map. */
    result = mino_eval_string(
        "(let (senior   (filter (fn (e) (>= (get e :age) 40)) employees)\n"
        "      junior   (filter (fn (e) (<  (get e :age) 40)) employees))\n"
        "  {:senior (map (fn (e) (get e :name)) senior)\n"
        "   :junior (map (fn (e) (get e :name)) junior)})",
        env);
    if (result != NULL) {
        printf("Age groups: ");
        mino_println(result);
    }

    mino_env_free(env);
    return 0;
}
