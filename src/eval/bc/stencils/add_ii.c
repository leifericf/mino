/*
 * stencils/add_ii.c -- copy-and-patch stencil for OP_ADD_II.
 *
 * OP_ADD_II A B C is the specialised int+int form of `(+ a b)`. The
 * stencil tries the tagged-int fast lane (binop_int_fast with subop
 * BINOP_ADD); on a tag miss or overflow it falls back to the cons-
 * spine + prim_add path through mino_jit_binop_slow, which mirrors
 * the interpreter's OP_*_II handler exactly.
 *
 * The stencil returns the (possibly relocated) regs pointer so the
 * chain can carry the updated base through to the next stencil. The
 * trailing `ret` is patched to a `b <next_stencil>` by the JIT
 * compiler at materialisation time so the body falls through to the
 * next op's bytes.
 *
 * Build: compiled as part of the stencil pipeline only. The compiled
 * .o is fed to tools/stencil_extract, which writes the byte table +
 * reloc table into src/eval/bc/stencils/generated/<arch>_<os>.h.
 */

#include "abi.h"

mino_val_t **stencil_op_add_ii(mino_val_t **regs, mino_val_t **consts,
                                mino_state_t *S)
{
    (void)consts;
    mino_val_t *r = binop_int_fast(S, regs[IMM_B], regs[IMM_C],
                                   STENCIL_BINOP_ADD);
    if (__builtin_expect(r == NULL, 0)) {
        return mino_jit_binop_slow(S, regs, IMM_A, IMM_B, IMM_C,
                                   STENCIL_BINOP_ADD);
    }
    regs[IMM_A] = r;
    return regs;
}
