/*
 * stencils/protocol_call_cached.c -- copy-and-patch stencil for
 * OP_PROTOCOL_CALL_CACHED.
 *
 * Two-word op: word-1 carries A=arg_base / B=argn / C=ret_dst,
 * word-2 carries the IC slot index in Bx. Unlike OP_CALL_CACHED
 * (which uses a vararg shape regs[A]=callee + regs[A+1..A+B]=args),
 * protocol dispatch packs the protocol method's args contiguously
 * starting at regs[A] -- regs[A] IS the first arg (the type-disc
 * source) and there is no separate callee register; the impl is
 * resolved through the IC slot's atom.
 *
 * The full inline IC fast lane (atom-deref + type-disc compute +
 * triple-pointer-compare) is intentionally NOT inlined here: the
 * type-disc compute branches on the operand's runtime type
 * (record / non-record special cases) and the throw-on-no-impl
 * path is large enough that inlining the whole resolver bloats the
 * stencil for marginal win. The slow helper is a thin wrapper
 * around mino_bc_ic_resolve_protocol + apply_callable_argv;
 * eligibility unlock is the architecturally relevant win here.
 *
 * Operands the JIT patches:
 *   IMM_A   -- arg_base (also first-arg register)
 *   IMM_B   -- argn (8-bit)
 *   IMM_C   -- ret_dst (8-bit)
 *   IMM_BX2 -- IC slot index (16-bit, from word-2)
 *   IMM_BC  -- owning bc fn pointer
 */

#include "abi.h"

void stencil_op_protocol_call_cached(mino_val_t **regs,
                                      mino_val_t **consts,
                                      mino_state_t *S)
{
    regs = mino_jit_protocol_call_cached_slow(S, regs,
                                              (unsigned)IMM_A,
                                              (unsigned)IMM_B,
                                              (unsigned)IMM_C,
                                              IMM_BC,
                                              (unsigned)IMM_BX2);
    MINO_STENCIL_CHAIN_RETURN(regs, consts, S);
}
