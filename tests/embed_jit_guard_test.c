/*
 * embed_jit_guard_test.c -- the JIT slow-helper entry points are the
 * boundary where native stencil code hands control back to the
 * runtime. A stencil-chain register defect can deliver a state
 * pointer that is not the state (observed: a tagged Lisp value in
 * the S argument register). Every mino_jit_*_slow entry must detect
 * a foreign S and refuse it as data (return NULL, the documented
 * error propagate) before any field is read or written through it.
 *
 * Build (from repo root):
 *   cc -std=c99 -Wall -Wextra -Wpedantic -O2 -DMINO_CPJIT=1 -Isrc \
 *       -o embed_jit_guard_test tests/embed_jit_guard_test.c \
 *       src/SRC.c -lm
 * Run: ./embed_jit_guard_test
 */

#include "mino.h"
#include "runtime/internal.h"
#include "eval/bc/internal.h"
#include "eval/bc/jit/internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define REQUIRE(cond, msg)                                         \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "FAIL (%s:%d): %s\n",                  \
                    __FILE__, __LINE__, (msg));                    \
            failures++;                                            \
        }                                                          \
    } while (0)

int main(void)
{
    /* A buffer shaped like nothing: zeroed memory standing in for the
     * tagged values and heap pointers the register defect delivers.
     * A correct guard rejects it on the magic field alone. */
    static mino_state fake_storage;
    mino_state *fake = &fake_storage;

    mino_state *S = mino_state_new();
    REQUIRE(S != NULL, "state creation");
    REQUIRE(S->state_magic == MINO_STATE_MAGIC,
            "fresh state carries the magic");

    /* NULL state: refused, not dereferenced. */
#ifdef MINO_CPJIT
    {
        mino_val **r;
        extern mino_val **mino_jit_getglobal_cached_slow(
            mino_state *S, mino_val **regs, unsigned a,
            mino_bc_fn_t *bc, unsigned slot_idx);
        mino_val *regs_stub[4] = {0};

        r = mino_jit_getglobal_cached_slow(NULL, regs_stub, 0, NULL, 0);
        REQUIRE(r == NULL, "NULL state refused");

        r = mino_jit_getglobal_cached_slow(fake, regs_stub, 0, NULL, 0);
        REQUIRE(r == NULL, "foreign state refused before any field use");
    }
#endif

    if (failures == 0) {
        printf("embed_jit_guard_test: all checks passed\n");
        return 0;
    }
    return 1;
}
