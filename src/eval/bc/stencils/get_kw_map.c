/*
 * stencils/get_kw_map.c -- copy-and-patch stencil for OP_GET_KW_MAP.
 *
 * Trampoline into mino_jit_get_kw_map_slow which carries the
 * MINO_MAP + MINO_RECORD/KEYWORD fast lanes and the prim_get fallback.
 *
 * Operands:
 *   IMM_A -- destination reg
 *   IMM_B -- coll reg
 *   IMM_C -- key reg
 */

#include "abi.h"
#include "runtime_layout.h"

void stencil_op_get_kw_map(mino_val_t **regs,
                            mino_val_t **consts,
                            mino_state_t *S)
{
    regs = mino_jit_get_kw_map_slow(S, regs,
                                    (unsigned)IMM_A,
                                    (unsigned)IMM_B,
                                    (unsigned)IMM_C);
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
