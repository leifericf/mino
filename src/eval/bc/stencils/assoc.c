/*
 * stencils/assoc.c -- copy-and-patch stencil for OP_ASSOC.
 *
 * OP_ASSOC A B  ::=  regs[A] := (assoc regs[B] regs[B+1] regs[B+2]).
 *
 * Three consecutive registers starting at B carry [coll, k, v]; the
 * compiler arranges this in the bc emit. The slow helper handles all
 * dispatch (MINO_VECTOR+int-k vec_assoc1 fast lane, MINO_MAP
 * mino_map_assoc1 fast lane, prim_assoc fallback for everything
 * else).
 *
 * Operands:
 *   IMM_A -- destination reg
 *   IMM_B -- coll reg (k at B+1, v at B+2)
 */

#include "abi.h"
#include "runtime_layout.h"

void *stencil_op_assoc(mino_val **regs,
                       mino_val **consts,
                       mino_state *S)
{
    regs = mino_jit_assoc_slow(S, regs,
                               (unsigned)IMM_A,
                               (unsigned)IMM_B);
    if (__builtin_expect(regs == NULL, 0)) return NULL;
    MINO_STENCIL_CHAIN_RETURN_PTR(regs, consts, S);
}
