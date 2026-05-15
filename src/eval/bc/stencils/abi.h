/*
 * src/eval/bc/stencils/abi.h
 *
 * Copy-and-patch stencil immediate ABI.
 *
 * Each stencil source is compiled as a standalone .o file; the resulting
 * machine code is memcpy'd into RWX memory at runtime. Stencils reference
 * extern symbols declared here for the immediate operands the JIT patches:
 *
 *   MINO_STENCIL_IMM_A   -- A field of the bytecode instruction (8 bits)
 *   MINO_STENCIL_IMM_B   -- B field                              (8 bits)
 *   MINO_STENCIL_IMM_C   -- C field                              (8 bits)
 *   MINO_STENCIL_IMM_BX  -- Bx field (constant / ic-slot index, 16 bits)
 *   MINO_STENCIL_IMM_SBX -- signed Bx (branch offset)
 *
 * The compiler emits address-of expressions for these symbols; the linker
 * leaves the address fields unresolved as relocations. The extractor
 * records each relocation as (offset, kind, sym_index, addend) so the
 * runtime knows which bytes to overwrite when materialising the stencil
 * for a particular bytecode instruction.
 *
 * The address-of read is wrapped in IMM_* macros so the stencil sources
 * stay readable: `regs[IMM_A]` reads as a 1-D register access while the
 * underlying expression is `(unsigned long)(uintptr_t)&MINO_STENCIL_IMM_A`.
 *
 * No mino runtime headers are included here. Stencil sources are
 * intentionally hermetic so the build pipeline doesn't bring in compiler
 * options or symbol references that would muddy the emitted code.
 */

#ifndef MINO_BC_STENCIL_ABI_H
#define MINO_BC_STENCIL_ABI_H

#include <stddef.h>
#include <stdint.h>

/* Forward-declared types kept opaque to the stencil compilation unit so
 * the bytes the compiler emits never depend on the runtime layout. */
typedef struct mino_state    mino_state_t;
typedef struct mino_val      mino_val_t;

/* Chain-return type for non-final stencils. ARM64 AAPCS returns
 * structs of two 8-byte pointers in (x0, x1), so a stencil that
 * names this struct as its return type forces clang to spill
 * `consts` (x1 at entry) to a callee-saved register across any
 * helper call and reload it into x1 before the trailing ret.
 *
 * The JIT patches that ret into `b <next>`, so the next stencil
 * sees x0=regs, x1=consts, x2=S — even when the current stencil
 * called into a helper that clobbered x1. The fused / OP_RETURN
 * (final) stencils keep their `mino_val_t *` scalar return; they
 * are not chained out of so x1 preservation is moot for them. */
typedef struct {
    mino_val_t **regs;
    mino_val_t **consts;
} mino_stencil_chain_t;

/* Chain-exit invariant: at the patched-`ret` boundary, x0=regs,
 * x1=consts, x2=S. The struct return enforces (x0, x1) via AAPCS;
 * the register-asm pin forces clang to materialise S in x2 right
 * before the ret. The AArch64 function epilogue itself touches
 * only x19/x20/x29/x30 and sp, so x2 carries through to the
 * patched chain branch unchanged. */
#define MINO_STENCIL_CHAIN_RETURN(regs_, consts_, S_)                      \
    do {                                                                   \
        register mino_state_t *_mino_s_in_x2 __asm__("x2") = (S_);         \
        __asm__ volatile("" : : "r"(_mino_s_in_x2));                       \
        return (mino_stencil_chain_t){(regs_), (consts_)};                 \
    } while (0)

/* Extern immediate slots. Their addresses encode the patchable operands.
 * The linker emits one R_UNSIGNED / PAGE21+PAGEOFF12 relocation pair per
 * read site; the extractor records the offsets and the runtime overwrites
 * those bytes with the materialised immediate.
 *
 * MINO_STENCIL_IMM_KIMM holds a pre-tagged mino_val_t* for the signed
 * 8-bit immediate carried by the OP_*_IK opcodes. The pool slot stores
 * the result of MINO_MAKE_INT((int8_t)C_OF(insn)) so the stencil can
 * pass it through to binop_int_fast / the slow helper exactly the way
 * the II stencils pass regs[c]. */
extern char MINO_STENCIL_IMM_A[];
extern char MINO_STENCIL_IMM_B[];
extern char MINO_STENCIL_IMM_C[];
extern char MINO_STENCIL_IMM_BX[];
extern char MINO_STENCIL_IMM_SBX[];
extern char MINO_STENCIL_IMM_KIMM[];

#define IMM_A    ((unsigned long)(uintptr_t)MINO_STENCIL_IMM_A)
#define IMM_B    ((unsigned long)(uintptr_t)MINO_STENCIL_IMM_B)
#define IMM_C    ((unsigned long)(uintptr_t)MINO_STENCIL_IMM_C)
#define IMM_BX   ((unsigned long)(uintptr_t)MINO_STENCIL_IMM_BX)
#define IMM_SBX  ((long)(intptr_t)MINO_STENCIL_IMM_SBX)
#define IMM_KIMM ((mino_val_t *)(uintptr_t)MINO_STENCIL_IMM_KIMM)

/* Sub-op constants the BINOP_INT family shares with the bytecode VM.
 * Kept in numeric sync with mino_bc_binop_t in src/eval/bc/internal.h;
 * the runtime side has compile-time asserts elsewhere that the values
 * agree. Duplicated here so the stencil sources stay hermetic. */
#define STENCIL_BINOP_ADD  0u
#define STENCIL_BINOP_SUB  1u
#define STENCIL_BINOP_MUL  2u
#define STENCIL_BINOP_LT   3u
#define STENCIL_BINOP_LE   4u
#define STENCIL_BINOP_GT   5u
#define STENCIL_BINOP_GE   6u
#define STENCIL_BINOP_EQ   7u
#define STENCIL_UNOP_INC      0u
#define STENCIL_UNOP_DEC      1u
#define STENCIL_UNOP_ZERO_P   2u

/* Runtime helpers stencils call. Declarations stay minimal: opaque
 * forward-declared types only, no runtime headers. The compiler
 * emits ARM64_RELOC_BRANCH26 against each name; the JIT runtime
 * resolves the symbol to the host C address and patches each bl
 * through a 16-byte trampoline appended to the JIT region.
 *
 * binop_int_fast / unop_int_fast: tagged-int speculative fast lanes
 *   (NULL on miss).
 * mino_jit_binop_slow / mino_jit_unop_slow: cold helpers that build a
 *   cons list, dispatch to the matching prim, and store the result
 *   through regs[a]. May trigger GC; return the (possibly relocated)
 *   regs pointer so the caller can fall through with the latest base.
 *   Return NULL when the prim itself returns NULL (the prim normally
 *   raises through longjmp on type errors -- a NULL return is the
 *   defensive case).
 * mino_jit_binop_k_slow: like _binop_slow but the rhs is a pre-tagged
 *   immediate (passed by value, not by register index). The helper
 *   conses the literal directly onto the spine so OP_*_IK stencils
 *   reach the same prim fallback path as their II siblings. */
extern mino_val_t *binop_int_fast(mino_state_t *S, mino_val_t *lhs,
                                  mino_val_t *rhs, unsigned subop);
extern mino_val_t *unop_int_fast(mino_state_t *S, mino_val_t *v,
                                 unsigned subop);
extern mino_val_t **mino_jit_binop_slow(mino_state_t *S, mino_val_t **regs,
                                        unsigned a, unsigned b, unsigned c,
                                        unsigned subop);
extern mino_val_t **mino_jit_binop_k_slow(mino_state_t *S, mino_val_t **regs,
                                          unsigned a, unsigned b,
                                          mino_val_t *kimm, unsigned subop);
extern mino_val_t **mino_jit_unop_slow(mino_state_t *S, mino_val_t **regs,
                                       unsigned a, unsigned b, unsigned subop);

/* Fused counted-loop step helpers. Each takes the loop's register
 * indices, runs one iteration's slow path (prim_lt / prim_inc / etc.
 * with cons-spine args), and returns the (possibly relocated) regs
 * pointer. The low bit of the return tags the exit signal:
 *
 *   (ret_ptr & 1) == 0  -> loop continues; ret_ptr is the fresh regs base
 *   (ret_ptr & 1) == 1  -> loop exits; (ret_ptr & ~1) is the regs base
 *
 * The low-bit tag is safe because regs always points to 8-byte-aligned
 * storage. NULL is reserved for hard errors (e.g., a cons OOM that the
 * caller propagates back up the JIT region as a NULL return). */
extern mino_val_t **mino_jit_loop_int_lt_slow(mino_state_t *S,
                                               mino_val_t **regs,
                                               unsigned a, unsigned b);
extern mino_val_t **mino_jit_loop_int_dec_slow(mino_state_t *S,
                                                mino_val_t **regs,
                                                unsigned a);
extern mino_val_t **mino_jit_loop_int_lt_inc_slow(mino_state_t *S,
                                                   mino_val_t **regs,
                                                   unsigned a, unsigned b,
                                                   unsigned c);

/* The continue-marker symbol. Fused-loop stencils end their continue
 * path with `__asm__("b _mino_jit_loop_continue_marker")`. The JIT
 * scans for the BRANCH26 reloc against this name in each stencil
 * instance and overwrites the branch offset to point at the stencil's
 * own start (the back-jump destination). The function is never called
 * directly; only its address-as-symbol matters. */
extern void mino_jit_loop_continue_marker(void);

#endif /* MINO_BC_STENCIL_ABI_H */
