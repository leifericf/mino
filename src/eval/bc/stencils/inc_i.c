/*
 * stencils/inc_i.c -- copy-and-patch stencil for OP_INC_I.
 *
 * OP_INC_I A B is the specialised int form of `(inc x)`. The stencil
 * tries unop_int_fast with subop UNOP_INC; on a tag miss or overflow
 * it falls back to the cons-spine + prim_inc path through
 * mino_jit_unop_slow, which mirrors the interpreter's OP_INC_I
 * handler exactly.
 *
 * The stencil returns the (possibly relocated) regs pointer so the
 * chain can carry the updated base through to the next stencil.
 */

#include "abi.h"

mino_val_t **stencil_op_inc_i(mino_val_t **regs, mino_val_t **consts,
                               mino_state_t *S)
{
    (void)consts;
    mino_val_t *r = unop_int_fast(S, regs[IMM_B], STENCIL_UNOP_INC);
    if (__builtin_expect(r == NULL, 0)) {
        return mino_jit_unop_slow(S, regs, IMM_A, IMM_B,
                                  STENCIL_UNOP_INC);
    }
    regs[IMM_A] = r;
    return regs;
}
