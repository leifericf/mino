/*
 * stencils/loop_int_dec_inc.c -- copy-and-patch stencil for
 * OP_LOOP_INT_DEC_INC.
 *
 * Two-binding reverse-counted loop step:
 *
 *   if (regs[A] == 0) exit
 *   regs[A]-- (the test counter)
 *   regs[B]++ (the lockstep carry)
 *   back-jump
 *
 * See loop_int_lt.c for the `for (;;)` loop pattern; clang emits one
 * prologue plus a tight body with a natural back-edge.
 *
 * Fast path: both operands are tagged ints, the counter is non-zero,
 * neither op overflows the tagged-int range. Decrement-by-one cannot
 * overflow downward from any non-zero positive counter, but the inline
 * range is symmetric around zero so we still guard the test reg's
 * decrement when it sits at MINO_TAGGED_INT_MIN -- and the inc reg
 * cannot exceed MINO_TAGGED_INT_MAX.
 *
 * Slow path: cons + prim_zero_p + prim_dec + prim_inc via
 * mino_jit_loop_int_dec_inc_slow. Same low-bit-tagged signal as
 * mino_jit_loop_int_lt_inc_slow: NULL on cons OOM, regs|1 on exit,
 * regs (low-bit-clear) on continue.
 */

#include "abi.h"

#define MINO_TAGGED_INT_MIN \
    ((uintptr_t)((unsigned long long)0x8000000000000000ull) | (uintptr_t)1)
#define MINO_TAGGED_INT_MAX \
    ((uintptr_t)((unsigned long long)0x7ffffffffffffff8ull) | (uintptr_t)1)

void *stencil_op_loop_int_dec_inc(mino_val **regs,
                                   mino_val **consts,
                                   mino_state *S)
{
    unsigned long ticks = 256;
    for (;;) {
        if (__builtin_expect(--ticks == 0, 0)) {
            if (!mino_bc_safepoint_batch(S, 256)) {
                return NULL;
            }
            ticks = 256;
        }
        mino_val *vt = regs[IMM_A];
        mino_val *vi = regs[IMM_B];
        uintptr_t ut = (uintptr_t)vt;
        uintptr_t ui = (uintptr_t)vi;
        if (__builtin_expect(((ut ^ 1) | (ui ^ 1)) & 7, 0)) {
            regs = mino_jit_loop_int_dec_inc_slow(S, regs, IMM_A, IMM_B);
            if (regs == NULL) {
                return NULL;
            }
            if (((uintptr_t)regs & 1) != 0) {
                regs = (mino_val **)((uintptr_t)regs & ~(uintptr_t)1);
                MINO_STENCIL_CHAIN_RETURN_PTR(regs, consts, S);
            }
            continue;
        }
        long long t = (long long)(intptr_t)ut >> 3;
        if (t == 0) {
            MINO_STENCIL_CHAIN_RETURN_PTR(regs, consts, S);
        }
        if (__builtin_expect(ut == MINO_TAGGED_INT_MIN
                             || ui == MINO_TAGGED_INT_MAX, 0)) {
            regs = mino_jit_loop_int_dec_inc_slow(S, regs, IMM_A, IMM_B);
            if (regs == NULL) {
                return NULL;
            }
            if (((uintptr_t)regs & 1) != 0) {
                regs = (mino_val **)((uintptr_t)regs & ~(uintptr_t)1);
                MINO_STENCIL_CHAIN_RETURN_PTR(regs, consts, S);
            }
            continue;
        }
        long long i = (long long)(intptr_t)ui >> 3;
        regs[IMM_A] = (mino_val *)(((uintptr_t)(t - 1) << 3) | (uintptr_t)1);
        regs[IMM_B] = (mino_val *)(((uintptr_t)(i + 1) << 3) | (uintptr_t)1);
        /* continue */
    }
}
