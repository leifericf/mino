/*
 * stencils/eq_ii.c -- copy-and-patch stencil for OP_EQ_II.
 *
 * OP_EQ_II A B C is the specialised int==int form of `(= a b)`.
 * Same template as lt_ii.c with BINOP_EQ as the subop. The fast
 * lane is sound for two tagged ints; mixed-type equality routes
 * through the prim to honour Clojure's cross-type comparison rules.
 *
 * Build: compiled as part of the stencil pipeline only.
 */

#include "abi.h"

mino_val_t **stencil_op_eq_ii(mino_val_t **regs, mino_val_t **consts,
                               mino_state_t *S)
{
    (void)consts;
    mino_val_t *r = binop_int_fast(S, regs[IMM_B], regs[IMM_C],
                                   STENCIL_BINOP_EQ);
    if (__builtin_expect(r == NULL, 0)) {
        return mino_jit_binop_slow(S, regs, IMM_A, IMM_B, IMM_C,
                                   STENCIL_BINOP_EQ);
    }
    regs[IMM_A] = r;
    return regs;
}
