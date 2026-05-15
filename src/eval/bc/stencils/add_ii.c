/*
 * stencils/add_ii.c -- copy-and-patch stencil for OP_ADD_II.
 *
 * OP_ADD_II A B C is the specialised int+int form of `(+ a b)`. The
 * stencil tries the tagged-int fast lane (binop_int_fast with subop
 * BINOP_ADD); on a tag miss or overflow it falls back to the cons-
 * spine + prim_add path through mino_jit_binop_slow, which mirrors
 * the interpreter's OP_*_II handler exactly.
 *
 * Returns the chain tuple (regs, consts) so the JIT chain pattern
 * preserves x1 = consts across the patched ret; the matching macro
 * also pins x2 = S so subsequent stencils that call helpers see the
 * correct state pointer.
 */

#include "abi.h"

mino_stencil_chain_t stencil_op_add_ii(mino_val_t **regs, mino_val_t **consts,
                                        mino_state_t *S)
{
    mino_val_t *r = binop_int_fast(S, regs[IMM_B], regs[IMM_C],
                                   STENCIL_BINOP_ADD);
    if (__builtin_expect(r == NULL, 0)) {
        regs = mino_jit_binop_slow(S, regs, IMM_A, IMM_B, IMM_C,
                                   STENCIL_BINOP_ADD);
    } else {
        regs[IMM_A] = r;
    }
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
