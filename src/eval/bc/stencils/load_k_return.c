/*
 * stencils/load_k_return.c -- copy-and-patch fused superinstruction.
 *
 * The bytecode pattern OP_LOAD_K (A=R, Bx=K) immediately followed by
 * OP_RETURN A=R compiles into two separate stencils:
 *
 *   LOAD_K:  load consts[Bx] into a temp, store it into regs[A]
 *   RETURN:  load regs[A] into x0 (return register), ret
 *
 * A constant-returning fn -- `(fn [] 42)`, `(fn [_] :tag)`, every
 * arity-stub that wraps a literal -- hits this pattern. The fused
 * stencil collapses both to a single load: skip the intermediate
 * regs write, put consts[Bx] straight in x0, ret. The JIT compile
 * path detects the pattern and emits this stencil in place of the
 * pair when A matches between the two instructions.
 *
 * On ARM64 with default -O2 codegen the fused body is four
 * instructions (adrp / ldr-from-GOT / ldr-from-consts / ret); the
 * pair it replaces totals nine. The reloc surface is one PAGE21 +
 * one PAGEOFF12 against MINO_STENCIL_IMM_BX.
 *
 * Build: compiled as part of the stencil pipeline only.
 */

#include "abi.h"

void *stencil_op_load_k_return(mino_val **regs, mino_val **consts)
{
    (void)regs;
    return consts[IMM_BX];
}
