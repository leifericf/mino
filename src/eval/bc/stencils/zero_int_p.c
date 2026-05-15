/*
 * stencils/zero_int_p.c -- copy-and-patch stencil for OP_ZERO_INT_P.
 *
 * Mirrors inc_i.c with UNOP_ZERO_P; prim_zero_p covers the non-int
 * diagnostic on a tag miss.
 */

#include "abi.h"

mino_val_t **stencil_op_zero_int_p(mino_val_t **regs, mino_val_t **consts,
                                    mino_state_t *S)
{
    (void)consts;
    mino_val_t *r = unop_int_fast(S, regs[IMM_B], STENCIL_UNOP_ZERO_P);
    if (__builtin_expect(r == NULL, 0)) {
        return mino_jit_unop_slow(S, regs, IMM_A, IMM_B,
                                  STENCIL_UNOP_ZERO_P);
    }
    regs[IMM_A] = r;
    return regs;
}
