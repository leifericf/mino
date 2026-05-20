/*
 * stencils/protocol_tailcall_cached.c -- copy-and-patch stencil for
 * OP_PROTOCOL_TAILCALL_CACHED.
 *
 * Tail-position protocol-method call. Same shape as the non-tail
 * variant (two-word op, contiguous args at regs[A..A+B-1]) but the
 * impl's return value becomes the fn's return value: the stencil
 * is FINAL (returns mino_val * directly, no chain). Self-tail-
 * recursive protocol methods grow the C stack linearly here -- the
 * deliberate trade-off matches the interpreter's behaviour and
 * avoids the cons-spine build the MINO_TAIL_CALL sentinel would
 * otherwise force.
 *
 * Operands the JIT patches:
 *   IMM_A   -- arg_base (also first-arg register)
 *   IMM_B   -- argn (8-bit)
 *   IMM_BX2 -- IC slot index (16-bit, from word-2)
 *   IMM_BC  -- owning bc fn pointer
 *
 * IMM_C is unused (no destination register; the value is returned).
 */

#include "abi.h"

mino_val *stencil_op_protocol_tailcall_cached(mino_val **regs,
                                                 mino_val **consts,
                                                 mino_state *S)
{
    (void)consts;
    return mino_jit_protocol_tailcall_cached_slow(S, regs,
                                                  (unsigned)IMM_A,
                                                  (unsigned)IMM_B,
                                                  IMM_BC,
                                                  (unsigned)IMM_BX2);
}
