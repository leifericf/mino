/*
 * fuzz_reader.c — fuzz target for the mino reader.
 *
 * Two modes:
 *   1. libFuzzer:  cc -fsanitize=fuzzer,address -I.. fuzz_reader.c ../mino.c
 *   2. stdin:      cc -DFUZZ_STDIN -I.. -o fuzz_reader fuzz_reader.c ../mino.c
 *                  echo '(+ 1 2)' | ./fuzz_reader
 *
 * The reader must never crash, regardless of input. It should either
 * return a valid value or return NULL with an error message set.
 */

#include "mino.h"
#include <stdlib.h>
#include <string.h>

static int fuzz_one(const char *data, size_t size)
{
    /* NUL-terminate — mino_read expects a C string. */
    char *buf = (char *)malloc(size + 1);
    if (buf == NULL) return 0;
    memcpy(buf, data, size);
    buf[size] = '\0';

    /* Read all forms until EOF or error. */
    {
        const char *pos = buf;
        while (*pos != '\0') {
            const char *end = NULL;
            mino_val_t *val = mino_read(pos, &end);
            if (val == NULL) {
                if (end != NULL && end > pos) {
                    /* EOF: only whitespace/comments remaining. */
                    break;
                }
                /* Parse error: reader rejected this input. Fine. */
                break;
            }
            if (end == NULL || end <= pos) {
                /* Safety: avoid infinite loop if reader doesn't advance. */
                break;
            }
            pos = end;
        }
    }

    free(buf);
    return 0;
}

#ifdef FUZZ_STDIN
#include <stdio.h>
int main(void)
{
    char   buf[1024 * 1024];
    size_t n = fread(buf, 1, sizeof(buf), stdin);
    return fuzz_one(buf, n);
}
#else
int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    /* Cap input to avoid slow units. */
    if (size > 64 * 1024) return 0;
    return fuzz_one((const char *)data, size);
}
#endif
