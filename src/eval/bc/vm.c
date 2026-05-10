/*
 * eval/bc/vm.c -- register-based bytecode VM dispatch.
 *
 * Switch-based interpreter. Phase 1 skeleton; opcode handlers and
 * register-stack integration arrive in subsequent commits.
 */

#include <stddef.h>

#include "mino.h"
#include "eval/bc/internal.h"

/* Phase-1 skeleton entry point. Returns NULL so apply_callable falls
 * back to the tree-walker for every call until the dispatch loop and
 * the state-side register stack land. */
mino_val_t *mino_bc_run(mino_state_t *S, mino_val_t *fn_val,
                        mino_val_t **argv, int argc, mino_env_t *env)
{
    (void)S; (void)fn_val; (void)argv; (void)argc; (void)env;
    return NULL;
}

/* GC mark hook for a compiled fn's const pool. Wired in once the const
 * array is allocated by the compiler. */
void mino_bc_fn_mark(mino_state_t *S, const mino_bc_fn_t *bc)
{
    (void)S; (void)bc;
}
