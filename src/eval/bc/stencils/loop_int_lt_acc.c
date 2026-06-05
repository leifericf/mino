/*
 * stencils/loop_int_lt_acc.c -- copy-and-patch stencil for
 * OP_LOOP_INT_LT_ACC.
 *
 * Forward-counted two-binding loop step where the second binding is
 * an arithmetic-step accumulator: counter A vs limit B, accumulator C
 * incremented by the value of register D each iteration. D is patched
 * from word-2's Bx via IMM_BX2.
 *
 * Fast path: all four operands are tagged ints, counter is < limit,
 * counter increment doesn't overflow MINO_TAGGED_INT_MAX, and the
 * acc + step doesn't escape MINO_TAGGED_INT range. The acc-overflow
 * check is `new_acc >= MIN && new_acc <= MAX` since step can be any
 * signed value (negative steps occur on `(+ acc -k)` shapes).
 */

#include "abi.h"
#include "runtime_layout.h"

#define MINO_TAGGED_INT_MAX \
    ((uintptr_t)((unsigned long long)0x7ffffffffffffff8ull) | (uintptr_t)1)

void stencil_op_loop_int_lt_acc(mino_val **regs,
                                 mino_val **consts,
                                 mino_state *S)
{
    unsigned long ticks = 256;
    for (;;) {
        if (__builtin_expect(--ticks == 0, 0)) {
            if (!mino_bc_safepoint_batch(S, 256)) {
                MINO_STENCIL_CHAIN_RETURN(NULL, consts, S);
            }
            ticks = 256;
        }
        mino_val *vc = regs[IMM_A];
        mino_val *vl = regs[IMM_B];
        mino_val *vk = regs[IMM_C];
        mino_val *vs = regs[IMM_BX2];
        uintptr_t uc = (uintptr_t)vc;
        uintptr_t ul = (uintptr_t)vl;
        uintptr_t uk = (uintptr_t)vk;
        uintptr_t us = (uintptr_t)vs;
        if (__builtin_expect(((uc ^ 1) | (ul ^ 1) | (uk ^ 1) | (us ^ 1)) & 7, 0)) {
            regs = mino_jit_loop_int_lt_acc_slow(S, regs,
                                                  IMM_A, IMM_B, IMM_C,
                                                  IMM_BX2);
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
        if (__builtin_expect(uc == MINO_TAGGED_INT_MAX, 0)) {
            regs = mino_jit_loop_int_lt_acc_slow(S, regs,
                                                  IMM_A, IMM_B, IMM_C,
                                                  IMM_BX2);
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
        long long s = (long long)(intptr_t)us >> 3;
        long long new_k = k + s;
        if (__builtin_expect(new_k < MINO_INT_MIN || new_k > MINO_INT_MAX, 0)) {
            regs = mino_jit_loop_int_lt_acc_slow(S, regs,
                                                  IMM_A, IMM_B, IMM_C,
                                                  IMM_BX2);
            if (regs == NULL) {
                MINO_STENCIL_CHAIN_RETURN(NULL, consts, S);
            }
            if (((uintptr_t)regs & 1) != 0) {
                regs = (mino_val **)((uintptr_t)regs & ~(uintptr_t)1);
                MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
            }
            continue;
        }
        regs[IMM_A] = (mino_val *)(((uintptr_t)(c + 1) << 3) | (uintptr_t)1);
        regs[IMM_C] = (mino_val *)(((uintptr_t)new_k << 3) | (uintptr_t)1);
        /* continue */
    }
}
