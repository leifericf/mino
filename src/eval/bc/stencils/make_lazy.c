/*
 * stencils/make_lazy.c -- copy-and-patch stencil for OP_MAKE_LAZY.
 *
 * OP_MAKE_LAZY A Bx allocates a fresh MINO_LAZY value over the child
 * bc at bc->consts[Bx], capturing the current JIT-invoke env, and
 * stores it at regs[A]. The slow helper mirrors the interpreter's
 * OP_MAKE_LAZY cold-op handler bit-for-bit so the resulting thunk is
 * indistinguishable from one the interpreter would build.
 *
 * The op has no useful fast-path (every invocation hits the
 * allocator), so the stencil is just a helper call with the chain
 * return -- the JIT-side win is fn-level eligibility: any body that
 * uses `lazy-seq` (which expands to OP_MAKE_LAZY) was previously
 * rejected outright and now JITs the surrounding control flow.
 *
 * Operands the JIT patches:
 *   IMM_A  -- destination register
 *   IMM_BX -- child body const-pool index
 *   IMM_BC -- pointer to the owning bc fn
 */

#include "abi.h"

void *stencil_op_make_lazy(mino_val **regs,
                           mino_val **consts,
                           mino_state *S)
{
    regs = mino_jit_make_lazy_slow(S, regs,
                                   (unsigned)IMM_A,
                                   IMM_BC,
                                   (unsigned)IMM_BX);
    if (__builtin_expect(regs == NULL, 0)) return NULL;
    MINO_STENCIL_CHAIN_RETURN_PTR(regs, consts, S);
}
