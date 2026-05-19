/*
 * stencils/loop_int_dec_acc.c -- copy-and-patch stencil for
 * OP_LOOP_INT_DEC_ACC.
 *
 * Reverse-counted two-binding loop step where the second binding is
 * an arithmetic-step accumulator: test register A decremented until
 * zero, accumulator C incremented by the value of register D each
 * iteration. D is patched from word-2's Bx via IMM_BX2.
 */

#include "abi.h"
#include "runtime_layout.h"

#define MINO_TAGGED_INT_MIN \
    ((uintptr_t)((unsigned long long)0x8000000000000000ull) | (uintptr_t)1)

void stencil_op_loop_int_dec_acc(mino_val_t **regs,
                                  mino_val_t **consts,
                                  mino_state_t *S)
{
    unsigned long ticks = 256;
    for (;;) {
        if (__builtin_expect(--ticks == 0, 0)) {
            if (!mino_bc_safepoint(S)) {
                MINO_STENCIL_CHAIN_RETURN(NULL, consts, S);
            }
            ticks = 256;
        }
        mino_val_t *vt = regs[IMM_A];
        mino_val_t *vk = regs[IMM_C];
        mino_val_t *vs = regs[IMM_BX2];
        uintptr_t ut = (uintptr_t)vt;
        uintptr_t uk = (uintptr_t)vk;
        uintptr_t us = (uintptr_t)vs;
        if (__builtin_expect(((ut ^ 1) | (uk ^ 1) | (us ^ 1)) & 7, 0)) {
            regs = mino_jit_loop_int_dec_acc_slow(S, regs,
                                                   IMM_A, IMM_C, IMM_BX2);
            if (regs == NULL) {
                MINO_STENCIL_CHAIN_RETURN(NULL, consts, S);
            }
            if (((uintptr_t)regs & 1) != 0) {
                regs = (mino_val_t **)((uintptr_t)regs & ~(uintptr_t)1);
                MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
            }
            continue;
        }
        long long t = (long long)(intptr_t)ut >> 3;
        if (t == 0) {
            MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
        }
        if (__builtin_expect(ut == MINO_TAGGED_INT_MIN, 0)) {
            regs = mino_jit_loop_int_dec_acc_slow(S, regs,
                                                   IMM_A, IMM_C, IMM_BX2);
            if (regs == NULL) {
                MINO_STENCIL_CHAIN_RETURN(NULL, consts, S);
            }
            if (((uintptr_t)regs & 1) != 0) {
                regs = (mino_val_t **)((uintptr_t)regs & ~(uintptr_t)1);
                MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
            }
            continue;
        }
        long long k = (long long)(intptr_t)uk >> 3;
        long long s = (long long)(intptr_t)us >> 3;
        long long new_k = k + s;
        if (__builtin_expect(new_k < MINO_INT_MIN || new_k > MINO_INT_MAX, 0)) {
            regs = mino_jit_loop_int_dec_acc_slow(S, regs,
                                                   IMM_A, IMM_C, IMM_BX2);
            if (regs == NULL) {
                MINO_STENCIL_CHAIN_RETURN(NULL, consts, S);
            }
            if (((uintptr_t)regs & 1) != 0) {
                regs = (mino_val_t **)((uintptr_t)regs & ~(uintptr_t)1);
                MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
            }
            continue;
        }
        regs[IMM_A] = (mino_val_t *)(((uintptr_t)(t - 1) << 3) | (uintptr_t)1);
        regs[IMM_C] = (mino_val_t *)(((uintptr_t)new_k << 3) | (uintptr_t)1);
        /* continue */
    }
}
