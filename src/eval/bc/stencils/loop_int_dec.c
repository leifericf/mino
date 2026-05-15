/*
 * stencils/loop_int_dec.c -- copy-and-patch stencil for OP_LOOP_INT_DEC.
 *
 * Reverse-counted single-binding loop step. See loop_int_lt.c for the
 * `for (;;)` loop pattern -- clang emits a single prologue plus a
 * tight body with a natural back-edge.
 */

#include "abi.h"

#define MINO_TAGGED_INT_MIN \
    ((uintptr_t)((unsigned long long)0x8000000000000000ull) | (uintptr_t)1)

void stencil_op_loop_int_dec(mino_val_t **regs,
                              mino_val_t **consts,
                              mino_state_t *S)
{
    for (;;) {
        mino_val_t *v = regs[IMM_A];
        uintptr_t uv = (uintptr_t)v;
        if (__builtin_expect(((uv ^ 1) & 7) == 0
                             && uv != MINO_TAGGED_INT_MIN, 1)) {
            long long t = (long long)(intptr_t)uv >> 3;
            if (t == 0) {
                MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
            }
            regs[IMM_A] =
                (mino_val_t *)(((uintptr_t)(t - 1) << 3) | (uintptr_t)1);
            continue;
        }
        regs = mino_jit_loop_int_dec_slow(S, regs, IMM_A);
        if (regs == NULL) {
            MINO_STENCIL_CHAIN_RETURN(NULL, consts, S);
        }
        if (((uintptr_t)regs & 1) != 0) {
            regs = (mino_val_t **)((uintptr_t)regs & ~(uintptr_t)1);
            MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
        }
        /* continue */
    }
}
