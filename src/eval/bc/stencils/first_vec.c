/*
 * stencils/first_vec.c -- copy-and-patch stencil for OP_FIRST_VEC.
 *
 * OP_FIRST_VEC A B  ::=  regs[A] := (first regs[B]).
 *
 * Trampoline into mino_jit_first_vec_slow which carries the
 * MINO_VECTOR fast lane + prim_first fallback for any other coll
 * type (lazy seq, list, string, map, etc.). See nth_vec.c for the
 * trampoline-only rationale.
 *
 * Operands the JIT patches:
 *   IMM_A -- destination reg
 *   IMM_B -- coll reg
 */

#include "abi.h"
#include "runtime_layout.h"

void stencil_op_first_vec(mino_val **regs,
                           mino_val **consts,
                           mino_state *S)
{
    regs = mino_jit_first_vec_slow(S, regs,
                                   (unsigned)IMM_A,
                                   (unsigned)IMM_B);
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
