/*
 * stencils/load_k.c -- copy-and-patch stencil for OP_LOAD_K.
 *
 * OP_LOAD_K reads a value from the per-fn constants table and stores it
 * in a register: regs[A] = consts[Bx]. A is 8-bit; Bx is 16-bit. Bounds
 * checking against bc->consts_len is the JIT compiler's responsibility:
 * the JIT only emits this stencil when the compile-time Bx is in range.
 *
 * Build: compiled as part of the stencil pipeline only. The compiled .o
 * is fed to tools/stencil-extract, which writes the byte table + reloc
 * table into src/eval/bc/stencils/generated/stencils_<arch>_<os>.h.
 */

#include "abi.h"

void stencil_op_load_k(mino_val **regs, mino_val **consts,
                       mino_state *S)
{
    regs[IMM_A] = consts[IMM_BX];
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
