/*
 * stencils/pop_env.c -- copy-and-patch stencil for OP_POP_ENV.
 *
 * Bracket-end op paired with OP_PUSH_ENV. Walks the JIT-invoke env
 * up one frame so the let scope's bindings drop out of resolve
 * cascades after the bracketed body finishes.
 *
 * No operands.
 */

#include "abi.h"

void *stencil_op_pop_env(mino_val **regs,
                         mino_val **consts,
                         mino_state *S)
{
    regs = mino_jit_pop_env_slow(S, regs);
    if (__builtin_expect(regs == NULL, 0)) return NULL;
    MINO_STENCIL_CHAIN_RETURN_PTR(regs, consts, S);
}
