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

void stencil_op_assoc_bang(mino_val_t **regs,
                           mino_val_t **consts,
                           mino_state_t *S)
{
    regs = mino_jit_assoc_bang_slow(S, regs,
                                    (unsigned)IMM_A,
                                    (unsigned)IMM_B);
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
