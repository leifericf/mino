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

/* Extern immediate slots. Their addresses encode the patchable operands.
 * The linker emits one R_UNSIGNED / PAGE21+PAGEOFF12 relocation pair per
 * read site; the extractor records the offsets and the runtime overwrites
 * those bytes with the materialised immediate. */
extern char MINO_STENCIL_IMM_A[];
extern char MINO_STENCIL_IMM_B[];
extern char MINO_STENCIL_IMM_C[];
extern char MINO_STENCIL_IMM_BX[];
extern char MINO_STENCIL_IMM_SBX[];

#define IMM_A   ((unsigned long)(uintptr_t)MINO_STENCIL_IMM_A)
#define IMM_B   ((unsigned long)(uintptr_t)MINO_STENCIL_IMM_B)
#define IMM_C   ((unsigned long)(uintptr_t)MINO_STENCIL_IMM_C)
#define IMM_BX  ((unsigned long)(uintptr_t)MINO_STENCIL_IMM_BX)
#define IMM_SBX ((long)(intptr_t)MINO_STENCIL_IMM_SBX)

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
 * binop_int_fast: tagged-int speculative fast lane (NULL on miss).
 * mino_jit_binop_slow: cold helper that builds a cons list, dispatches
 *   to the matching prim, and stores the result through regs[a]. May
 *   trigger GC; returns the (possibly relocated) regs pointer so the
 *   caller can fall through with the latest base. Returns NULL when
 *   the prim itself returns NULL (the prim normally raises through
 *   longjmp on type errors -- a NULL return is the defensive case). */
extern mino_val_t *binop_int_fast(mino_state_t *S, mino_val_t *lhs,
                                  mino_val_t *rhs, unsigned subop);
extern mino_val_t **mino_jit_binop_slow(mino_state_t *S, mino_val_t **regs,
                                        unsigned a, unsigned b, unsigned c,
                                        unsigned subop);

#endif /* MINO_BC_STENCIL_ABI_H */
