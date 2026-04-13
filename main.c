/*
 * main.c — standalone entry point for the mino runtime.
 *
 * v0.1 stub: link-only check. Subsequent commits add the REPL.
 */

#include "mino.h"

#include <stdio.h>

int main(void)
{
    mino_val_t *v = mino_cons(mino_int(1), mino_cons(mino_int(2), mino_nil()));
    (void)v;
    fputs("mino\n", stdout);
    return 0;
}
