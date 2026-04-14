/*
 * embed.c — minimal embedding example for mino.
 *
 * Demonstrates: creating a runtime, registering a host function,
 * evaluating mino code, and extracting C values from the result.
 *
 * Build:
 *   cc -std=c99 -I.. -o embed embed.c ../mino.c
 * Run:
 *   ./embed
 */

#include "mino.h"
#include <stdio.h>

/* A host function exposed to mino as (add-tax amount). */
static mino_val_t *host_add_tax(mino_val_t *args, mino_env_t *env)
{
    long long amount;
    (void)env;
    if (!mino_is_cons(args) || !mino_to_int(args->as.cons.car, &amount)) {
        return mino_nil();
    }
    return mino_float((double)amount * 1.08);
}

int main(void)
{
    mino_env_t *env = mino_new();          /* env + core in one call */

    /* Register a host-defined function. */
    mino_register_fn(env, "add-tax", host_add_tax);

    /* Evaluate mino source that calls the host function. */
    mino_val_t *result = mino_eval_string(
        "(def prices [100 200 300])\n"
        "(loop (i 0 total 0.0)\n"
        "  (if (< i (count prices))\n"
        "      (recur (+ i 1) (+ total (add-tax (nth prices i))))\n"
        "      total))\n",
        env);

    /* Extract and use the result from C. */
    if (result == NULL) {
        fprintf(stderr, "error: %s\n", mino_last_error());
    } else {
        double total;
        if (mino_to_float(result, &total)) {
            printf("total with tax: %.2f\n", total);
        }
    }

    /* Demonstrate the in-process REPL handle: feed lines one at a time,
     * collecting results as complete forms become available. */
    {
        mino_repl_t *repl = mino_repl_new(env);
        mino_val_t  *out  = NULL;
        int          rc;

        /* Single-line form. */
        rc = mino_repl_feed(repl, "(+ 1 2)\n", &out);
        if (rc == MINO_REPL_OK && out != NULL) {
            printf("repl: ");
            mino_println(out);
        }

        /* Multi-line form: first line is incomplete. */
        rc = mino_repl_feed(repl, "(* 3\n", &out);
        if (rc == MINO_REPL_MORE) {
            printf("repl: awaiting more input...\n");
        }
        rc = mino_repl_feed(repl, "   4)\n", &out);
        if (rc == MINO_REPL_OK && out != NULL) {
            printf("repl: ");
            mino_println(out);
        }

        mino_repl_free(repl);
    }

    mino_env_free(env);
    return 0;
}
