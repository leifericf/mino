/*
 * stencils/nth_vec.c -- copy-and-patch stencil for OP_NTH_VEC.
 *
 * OP_NTH_VEC A B C  ::=  regs[A] := (nth regs[B] regs[C]).
 *
 * The interpreter handler inlines a tagged-int + MINO_VECTOR fast
 * lane and falls back to prim_nth when either operand misses. The
 * stencil source can't see the MINO_VECTOR layout (stencils are
 * compiled hermetically against opaque mino_val_t), so the inline
 * fast lane lives inside mino_jit_nth_vec_slow; the stencil itself
 * is a trampoline.
 *
 * The chain-ABI macro pins x2=S at the trailing ret so the chain
 * branch hits the next stencil with the right state pointer even
 * though the helper clobbered every caller-saved register.
 *
 * Operands the JIT patches:
 *   IMM_A -- destination reg
 *   IMM_B -- vector reg
 *   IMM_C -- index reg
 */

#include "abi.h"
#include "runtime_layout.h"

mino_stencil_chain_t stencil_op_nth_vec(mino_val_t **regs,
                                         mino_val_t **consts,
                                         mino_state_t *S)
{
    regs = mino_jit_nth_vec_slow(S, regs,
                                 (unsigned)IMM_A,
                                 (unsigned)IMM_B,
                                 (unsigned)IMM_C);
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
