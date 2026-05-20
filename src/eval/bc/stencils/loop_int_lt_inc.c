/*
 * stencils/loop_int_lt_inc.c -- copy-and-patch stencil for
 * OP_LOOP_INT_LT_INC.
 *
 * Forward-counted two-binding loop step (counter A vs limit B, carry
 * C incremented in lockstep with A). See loop_int_lt.c for the
 * `for (;;)` loop pattern.
 */

#include "abi.h"

#define MINO_TAGGED_INT_MAX \
    ((uintptr_t)((unsigned long long)0x7ffffffffffffff8ull) | (uintptr_t)1)

void stencil_op_loop_int_lt_inc(mino_val **regs,
                                 mino_val **consts,
                                 mino_state *S)
{
    unsigned long ticks = 256;
    for (;;) {
        if (__builtin_expect(--ticks == 0, 0)) {
            if (!mino_bc_safepoint(S)) {
                MINO_STENCIL_CHAIN_RETURN(NULL, consts, S);
            }
            ticks = 256;
        }
        mino_val *vc = regs[IMM_A];
        mino_val *vl = regs[IMM_B];
        mino_val *vk = regs[IMM_C];
        uintptr_t uc = (uintptr_t)vc;
        uintptr_t ul = (uintptr_t)vl;
        uintptr_t uk = (uintptr_t)vk;
        if (__builtin_expect(((uc ^ 1) | (ul ^ 1) | (uk ^ 1)) & 7, 0)) {
            regs = mino_jit_loop_int_lt_inc_slow(S, regs,
                                                  IMM_A, IMM_B, IMM_C);
            if (regs == NULL) {
                MINO_STENCIL_CHAIN_RETURN(NULL, consts, S);
            }
            if (((uintptr_t)regs & 1) != 0) {
                regs = (mino_val **)((uintptr_t)regs & ~(uintptr_t)1);
                MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
            }
            continue;
        }
        long long c = (long long)(intptr_t)uc >> 3;
        long long l = (long long)(intptr_t)ul >> 3;
        if (c >= l) {
            MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
        }
        if (__builtin_expect(uk == MINO_TAGGED_INT_MAX, 0)) {
            regs = mino_jit_loop_int_lt_inc_slow(S, regs,
                                                  IMM_A, IMM_B, IMM_C);
            if (regs == NULL) {
                MINO_STENCIL_CHAIN_RETURN(NULL, consts, S);
            }
            if (((uintptr_t)regs & 1) != 0) {
                regs = (mino_val **)((uintptr_t)regs & ~(uintptr_t)1);
                MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
            }
            continue;
        }
        long long k = (long long)(intptr_t)uk >> 3;
        regs[IMM_A] = (mino_val *)(((uintptr_t)(c + 1) << 3) | (uintptr_t)1);
        regs[IMM_C] = (mino_val *)(((uintptr_t)(k + 1) << 3) | (uintptr_t)1);
        /* continue */
    }
}
