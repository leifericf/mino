/*
 * stencils/dissoc.c -- copy-and-patch stencil for OP_DISSOC.
 *
 * OP_DISSOC A B C  ::=  regs[A] := (dissoc regs[B] regs[C]).
 *
 * Unlike OP_ASSOC (which packs [coll k v] in three consecutive
 * registers starting at B), OP_DISSOC uses three independent register
 * slots: A=dst, B=coll, C=key. The slow helper handles the MINO_MAP
 * fast lane via mino_map_dissoc1 and falls through to prim_dissoc
 * for other types (which raises a type diagnostic).
 *
 * Operands:
 *   IMM_A -- destination reg
 *   IMM_B -- coll reg
 *   IMM_C -- key reg
 */

#include "abi.h"
#include "runtime_layout.h"

void stencil_op_dissoc(mino_val **regs,
                        mino_val **consts,
                        mino_state *S)
{
    regs = mino_jit_dissoc_slow(S, regs,
                                (unsigned)IMM_A,
                                (unsigned)IMM_B,
                                (unsigned)IMM_C);
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
