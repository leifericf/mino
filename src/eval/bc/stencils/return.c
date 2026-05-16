/*
 * stencils/return.c -- copy-and-patch stencil for OP_RETURN.
 *
 * OP_RETURN hands its register operand back to the calling apply layer.
 * The stencil writes the chosen register to a single out-pointer the
 * runtime supplies; A is encoded as an extern immediate that the JIT
 * patches per-instruction.
 *
 * The legacy stencil_op_return_arg0 form (no immediate) stays as the
 * minimal-shape smoke test the build pipeline pinned in v0.183.0; it
 * exercises the no-relocation extraction path. The new
 * stencil_op_return_imm form joins the byte table with one PAGE21 +
 * PAGEOFF12 relocation pair the JIT patches with the bytecode's A
 * field.
 *
 * Build: compiled as part of the stencil pipeline only. The compiled .o
 * is fed to tools/stencil-extract, which writes the byte table + reloc
 * table into src/eval/bc/stencils/generated/stencils_<arch>_<os>.h.
 */

#include "abi.h"

void *stencil_op_return_arg0(void **arg0)
{
    return arg0[0];
}

void *stencil_op_return_imm(mino_val_t **regs)
{
    return regs[IMM_A];
}
