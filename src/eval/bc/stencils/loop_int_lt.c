/*
 * stencils/loop_int_lt.c -- copy-and-patch stencil for OP_LOOP_INT_LT.
 *
 * Forward-counted single-binding loop step:
 *
 *   if regs[A] < regs[B]: regs[A]++ and back-jump (loop continues)
 *   else: fall through to next stencil (loop exits)
 *
 * The OP_LOOP_INT_LT bytecode op IS the loop entry; each iteration
 * re-executes the same instruction via `pc -= 1` in the interpreter.
 * The stencil source models this with a `for (;;)` loop -- clang
 * emits one prologue, one set of callee-saved spills, and a tight
 * body with a single back-edge from end-of-body to top-of-body. The
 * loop-exit edges use MINO_STENCIL_CHAIN_RETURN, which compiles into
 * the function's natural ret; the JIT then patches that ret into the
 * usual `b <next_stencil>` chain branch.
 *
 * Fast path: tagged-int operands with c < l. Since the tagged-int
 * range caps both operands at MAX_INT (2^60 - 1), c < l implies
 * c < MAX_INT, so c + 1 cannot overflow the inline range.
 *
 * Slow path: cons-spine + prim_lt + prim_inc through
 * mino_jit_loop_int_lt_slow. The helper returns the (possibly
 * relocated) regs pointer with bit 0 set to signal "exit" or clear
 * to signal "continue".
 */

#include "abi.h"

void stencil_op_loop_int_lt(mino_val_t **regs,
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
        mino_val_t *vc = regs[IMM_A];
        mino_val_t *vl = regs[IMM_B];
        uintptr_t uc = (uintptr_t)vc;
        uintptr_t ul = (uintptr_t)vl;
        if (__builtin_expect(((uc ^ 1) | (ul ^ 1)) & 7, 0)) {
            /* Slow path: either operand non-int or NULL. */
            regs = mino_jit_loop_int_lt_slow(S, regs, IMM_A, IMM_B);
            if (regs == NULL) {
                MINO_STENCIL_CHAIN_RETURN(NULL, consts, S);
            }
            if (((uintptr_t)regs & 1) != 0) {
                regs = (mino_val_t **)((uintptr_t)regs & ~(uintptr_t)1);
                MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
            }
            continue;
        }
        long long c = (long long)(intptr_t)uc >> 3;
        long long l = (long long)(intptr_t)ul >> 3;
        if (c >= l) {
            MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
        }
        regs[IMM_A] = (mino_val_t *)(((uintptr_t)(c + 1) << 3) | (uintptr_t)1);
        /* fall through: continue the for-loop */
    }
}
