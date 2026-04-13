/*
 * main.c — standalone entry point for the mino runtime.
 *
 * v0.1 stub: round-trips a fixed string through the reader and printer.
 * The interactive REPL replaces this in a later commit.
 */

#include "mino.h"

#include <stdio.h>

int main(void)
{
    const char *src = "(+ 1 2.5 \"hi\\n\" 'foo (a b c))";
    const char *end = NULL;
    mino_val_t *v = mino_read(src, &end);
    if (v == NULL) {
        const char *err = mino_last_error();
        fprintf(stderr, "read error: %s\n", err ? err : "unknown");
        return 1;
    }
    mino_println(v);
    return 0;
}
