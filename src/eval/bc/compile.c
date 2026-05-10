/*
 * eval/bc/compile.c -- AST-to-bytecode compiler entry point.
 *
 * Per the cycle plan, compilation is lazy and per-fn: on first call
 * apply_callable invokes mino_bc_compile_fn with the MINO_FN value;
 * the compiler walks the macroexpanded body and either populates
 * MINO_FN.bc with a runnable program or marks the slot as declined
 * via the mino_bc_declined sentinel so subsequent calls don't retry.
 *
 * Phase 1 baseline: this entry point always declines. Form-by-form
 * coverage lands in subsequent commits. The plumbing here is what
 * apply_callable will key its compile-then-dispatch path off of.
 */

#include <stddef.h>

#include "mino.h"
#include "eval/bc/internal.h"

/* Sentinel value. Zero-initialised: code == NULL, code_len == 0, etc.
 * apply_callable compares fn->as.fn.bc against this pointer to
 * detect "compile already declined, skip the retry". */
const mino_bc_fn_t mino_bc_declined = {0};

int mino_bc_compile_fn(mino_state_t *S, mino_val_t *fn)
{
    (void)S;
    if (fn == NULL || fn->type != MINO_FN) {
        return MINO_BC_ERROR;
    }
    /* Phase 1 baseline: decline every fn. The first form-coverage commit
     * replaces this body with an actual AST walk. Until then, every fn
     * runs through the tree-walker as before. */
    fn->as.fn.bc = &mino_bc_declined;
    return MINO_BC_UNSUPPORTED;
}
