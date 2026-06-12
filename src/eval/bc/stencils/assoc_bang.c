/*
 * stencils/assoc_bang.c -- copy-and-patch stencil for OP_ASSOC_BANG.
 *
 * OP_ASSOC_BANG A B  ::=  regs[A] := (assoc! regs[B] regs[B+1] regs[B+2]).
 *
 * Three consecutive registers starting at B carry [tcoll, k, v]; the
 * compiler arranges this in the bc emit. The slow helper handles the
 * full dispatch (valid-transient fast lane via mino_assoc_bang;
 * non-transient or invalidated transient fall through to
 * prim_assoc_bang for the canonical diagnostic).
 *
 * Operands:
 *   IMM_A -- destination reg
 *   IMM_B -- tcoll reg (k at B+1, v at B+2)
 */

#include "abi.h"
#include "runtime_layout.h"

void *stencil_op_assoc_bang(mino_val **regs,
                           mino_val **consts,
                           mino_state *S)
{
    regs = mino_jit_assoc_bang_slow(S, regs,
                                    (unsigned)IMM_A,
                                    (unsigned)IMM_B);
    if (__builtin_expect(regs == NULL, 0)) return NULL;
    MINO_STENCIL_CHAIN_RETURN_PTR(regs, consts, S);
}
