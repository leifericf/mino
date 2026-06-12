/*
 * stencils/push_env.c -- copy-and-patch stencil for OP_PUSH_ENV.
 *
 * Bracket-start op for compiled-let scopes when the enclosing fn
 * captures (an inner fn literal needs to see the let-binding's
 * value through env lookup). The slow helper extends the
 * JIT-invoke env on the current thread ctx so subsequent
 * OP_ENV_BIND helpers bind into the fresh frame and the inner
 * OP_CLOSURE captures it.
 *
 * No operands.
 */

#include "abi.h"

void *stencil_op_push_env(mino_val **regs,
                          mino_val **consts,
                          mino_state *S)
{
    regs = mino_jit_push_env_slow(S, regs);
    if (__builtin_expect(regs == NULL, 0)) return NULL;
    MINO_STENCIL_CHAIN_RETURN_PTR(regs, consts, S);
}
