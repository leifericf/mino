/*
 * main.c — standalone entry point for the mino runtime.
 *
 * v0.1 transitional: evaluates a few hard-coded expressions to demonstrate
 * the read/eval/print pipeline. The interactive REPL replaces this in the
 * next commit.
 */

#include "mino.h"

#include <stdio.h>

static int eval_and_print(const char *src, mino_env_t *env)
{
    const char *p = src;
    mino_val_t *form = mino_read(src, &p);
    mino_val_t *result;
    if (form == NULL) {
        const char *err = mino_last_error();
        fprintf(stderr, "read error: %s\n", err ? err : "unknown");
        return 1;
    }
    result = mino_eval(form, env);
    if (result == NULL) {
        const char *err = mino_last_error();
        fprintf(stderr, "eval error: %s\n", err ? err : "unknown");
        return 1;
    }
    mino_println(result);
    return 0;
}

int main(void)
{
    mino_env_t *env = mino_env_new();
    mino_install_core(env);

    eval_and_print("(+ 1 2)", env);
    eval_and_print("(cons 1 '(2 3))", env);
    eval_and_print("(def x 41)", env);
    eval_and_print("(+ x 1)", env);
    eval_and_print("(def x 100)", env);
    eval_and_print("x", env);
    eval_and_print("(list 1 2.5 'foo \"hi\")", env);
    eval_and_print("(< 1 2 3)", env);
    eval_and_print("(= (cons 1 '(2 3)) '(1 2 3))", env);

    mino_env_free(env);
    return 0;
}
