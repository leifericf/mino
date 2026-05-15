/*
 * stencils/move.c -- copy-and-patch stencil for OP_MOVE.
 *
 * OP_MOVE copies one register to another: regs[A] = regs[B]. Both A and
 * B are 8-bit operands encoded in the bytecode instruction word. The
 * stencil reads them from extern immediate symbols; the runtime patches
 * the load sites with the actual A / B values when materialising the
 * stencil for a specific OP_MOVE instruction.
 *
 * Build: compiled as part of the stencil pipeline only. The compiled .o
 * is fed to tools/stencil_extract, which writes the byte table + reloc
 * table into src/eval/bc/stencils/generated/stencils_<arch>_<os>.h.
 */

#include "abi.h"

void stencil_op_move(mino_val_t **regs, mino_val_t **consts, mino_state_t *S)
{
    regs[IMM_A] = regs[IMM_B];
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
