/*
 * eval/bc/jit/internal.h -- private interface shared between the
 * copy-and-patch JIT's translation units (entry / stats / helpers /
 * patcher / emit).
 *
 * Host detection lives here so each of the five .c files can include
 * this one header and gate its body on `MINO_CPJIT_HOST`. Symbols with
 * external linkage are prefixed `mino_jit_` (no static) because the
 * stencil-side machine code resolves a few of them by name through
 * the extern-fn table populated in entry.c.
 *
 * Public entry points (`mino_jit_compile`, `mino_jit_invoke`, etc.)
 * stay in jit.h; this header is strictly TU-private.
 */

#ifndef MINO_EVAL_BC_JIT_INTERNAL_H
#define MINO_EVAL_BC_JIT_INTERNAL_H

#ifdef MINO_CPJIT
#if defined(__aarch64__) && defined(__APPLE__)
#define MINO_CPJIT_HOST 1
#define MINO_CPJIT_HOST_ARM64 1
#define MINO_CPJIT_STENCILS_HEADER "../stencils/generated/stencils_arm64_darwin.h"
#elif defined(__aarch64__) && defined(__linux__) && defined(MINO_CPJIT_ARM64_LINUX)
#define MINO_CPJIT_HOST 1
#define MINO_CPJIT_HOST_ARM64 1
#define MINO_CPJIT_STENCILS_HEADER "../stencils/generated/stencils_arm64_linux.h"
#elif defined(__x86_64__) && defined(__linux__) && defined(MINO_CPJIT_X86_64_LINUX)
#define MINO_CPJIT_HOST 1
#define MINO_CPJIT_HOST_X86_64 1
#define MINO_CPJIT_STENCILS_HEADER "../stencils/generated/stencils_x86_64_linux.h"
#elif defined(__x86_64__) && defined(__APPLE__) && defined(MINO_CPJIT_X86_64_DARWIN)
#define MINO_CPJIT_HOST 1
#define MINO_CPJIT_HOST_X86_64 1
#define MINO_CPJIT_STENCILS_HEADER "../stencils/generated/stencils_x86_64_darwin.h"
#elif defined(__x86_64__) && defined(_WIN32) && defined(MINO_CPJIT_X86_64_WINDOWS)
#define MINO_CPJIT_HOST 1
#define MINO_CPJIT_HOST_X86_64 1
#define MINO_CPJIT_STENCILS_HEADER "../stencils/generated/stencils_x86_64_windows.h"
#endif
#endif

#ifdef MINO_CPJIT_HOST

#include <stddef.h>
#include <stdint.h>

#include "../../../mino.h"
#include "../../../mino_internal.h"
#include "../internal.h"

/* Reloc kind enum mirror -- kept in sync with the values
 * tools/stencil_extract.c writes into <sym>_relocs tables. Header
 * changes there force a change here. The x86_64 entries are
 * declared here so the G3 mirror gate pins their numeric values
 * even before the x86_64 patchers ship; the patcher functions land
 * in the x86_64 platform release. */
#define MINO_STENCIL_RELOC_ARM64_PAGE21              0u
#define MINO_STENCIL_RELOC_ARM64_PAGEOFF12           1u
#define MINO_STENCIL_RELOC_ARM64_BRANCH26            2u
#define MINO_STENCIL_RELOC_ABS64                     3u
#define MINO_STENCIL_RELOC_ARM64_GOT_LOAD_PAGE21     4u
#define MINO_STENCIL_RELOC_ARM64_GOT_LOAD_PAGEOFF12  5u
#define MINO_STENCIL_RELOC_X86_64_ABS64              6u
#define MINO_STENCIL_RELOC_X86_64_PC32               7u
#define MINO_STENCIL_RELOC_X86_64_GOTPCREL           8u

/* Pseudo-opcode for the fused LOAD_K + RETURN superinstruction. Sits
 * above OP__COUNT so the find-stencil lookup never confuses it with a
 * real bytecode opcode emitted by the compiler. */
#define OP_FUSED_LOAD_K_RETURN ((unsigned)(OP__COUNT + 1))

/* Pseudo-opcode for the side-exit / deopt-to-interp stencil. The
 * compile path inserts it after the supported native prefix when the
 * classifier returns OK_WITH_DEOPT; the stencil's body sets
 * S->jit_deopt_pending = 1 and S->jit_deopt_pc = the resume PC, then
 * returns NULL. mino_jit_invoke detects the deopt signal and continues
 * dispatch through the interpreter at the recorded PC. Never emitted
 * by the bytecode compiler. */
#define OP_DEOPT_TO_INTERP ((unsigned)(OP__COUNT + 2))

/* Continue-marker symbol names as the extractor records them
 * (after the Mach-O / ELF underscore stripping the tool does).
 * Matched in emit_stencil to short-circuit the extern-fn lookup:
 *
 *   LOOP marker  -- fused-loop back-jump; emit redirects the
 *                   branch to the stencil instance's own
 *                   self_start.
 *   CHAIN marker -- chain-continue tail-call; emit defers
 *                   patching until the post-emit pass knows the
 *                   next instance's native_start and rewrites
 *                   every chain reloc to point there. */
#define MINO_JIT_LOOP_MARKER_NAME  "mino_jit_loop_continue_marker"
#define MINO_JIT_CHAIN_MARKER_NAME "mino_jit_chain_continue_marker"

/* Direct-emit instruction sizes -- arch-specific because each host
 * encodes branches and trampolines differently:
 *
 *   ARM64    -- 4-byte fixed instructions throughout. JMP = one
 *               `b imm26`; JMPIFNOT = five instructions (ldr, cbz,
 *               sub, cmp, b.ls). Trampoline = ldr x16 + br x16 +
 *               8-byte literal = 16 bytes.
 *
 *   x86_64   -- variable-length encoding. JMP = `e9 rel32` (5
 *               bytes). JMPIFNOT = a small sequence that loads
 *               `regs[A]` and branches on NULL or the tagged
 *               nil/false range (30 bytes -- a 64-bit imm load
 *               for the slot offset, a movq from rdi, a test for
 *               NULL, a near branch, a sub, a cmp, a near
 *               branch). Trampoline = `48 b8 <8 bytes target>`
 *               + `ff e0` = 12 bytes.
 *
 * Both hosts pick the trampoline encoding that's reachable
 * regardless of where in the address space the JIT region lives
 * (ARM64's bl has ±128 MB range; x86_64's jmp rel32 has ±2 GB
 * range -- big but not guaranteed when the helper fns are far
 * from the JIT mmap). The trampoline indirection sidesteps both. */
#if defined(MINO_CPJIT_HOST_ARM64)
#define MINO_JIT_JMP_SIZE         4u
#define MINO_JIT_JMPIFNOT_SIZE   20u
#define MINO_JIT_TRAMPOLINE_SIZE 16u
#elif defined(MINO_CPJIT_HOST_X86_64)
#define MINO_JIT_JMP_SIZE         5u
#define MINO_JIT_JMPIFNOT_SIZE   30u
#define MINO_JIT_TRAMPOLINE_SIZE 12u
#endif

/* Stencil descriptor: per (opcode, variant) tuple. */
typedef struct {
    unsigned             opcode;
    const unsigned char *bytes;
    unsigned long        size;
    const char *const   *symbols;
    unsigned long        nsymbols;
    const unsigned int (*relocs)[4];
    unsigned long        nrelocs;
    /* Non-zero for the OP_RETURN-class stencil: the trailing `ret` is
     * preserved as the JIT region's exit instead of being rewritten
     * into a chain branch. */
    unsigned             is_final;
} stencil_desc_t;

/* Symbol-name to immediate-kind lookup. Stencil sources declare extern
 * `MINO_STENCIL_IMM_*` symbols; the runtime resolves each occurrence to
 * a bytecode-operand decode (A / B / C / Bx / sBx). */
typedef enum {
    IMM_KIND_A    = 0,
    IMM_KIND_B    = 1,
    IMM_KIND_C    = 2,
    IMM_KIND_BX   = 3,
    IMM_KIND_SBX  = 4,
    /* Signed 8-bit C field re-tagged as a mino_val_t* via MINO_MAKE_INT. */
    IMM_KIND_KIMM = 5,
    /* The bc pointer of the fn this stencil belongs to. */
    IMM_KIND_BC   = 6,
    /* Bx field of the SECOND instruction word for two-word ops. */
    IMM_KIND_BX2  = 7
} imm_kind_t;

/* Eligibility reason taxonomy. Returned by classify_eligibility so the
 * stats dumper can attribute every rejection to a specific blocker.
 *
 * OK_WITH_DEOPT marks a fn whose first unstenciled op sits at PC > 0:
 * the native prefix is worth compiling and the unstenciled tail can
 * run through the interpreter via a side-exit hand-off. The actual
 * compile-with-deopt path lands separately; the classifier surfaces
 * the annotation here so the tracing dashboard can rank pending wins. */
typedef enum {
    CPJIT_REASON_OK = 0,
    CPJIT_REASON_OK_WITH_DEOPT,
    CPJIT_REASON_NULL_BC,
    CPJIT_REASON_CAPTURES,
    CPJIT_REASON_IC_SLOTS,
    CPJIT_REASON_N_CLAUSES,
    CPJIT_REASON_HAS_REST,
    CPJIT_REASON_UNKNOWN_OP,
    CPJIT_REASON_EMPTY,
    CPJIT_REASON_BAD_TERMINATOR,
    CPJIT_REASON__COUNT
} cpjit_reason_t;

/* Descriptor table + lookup (defined in entry.c). */
extern const stencil_desc_t mino_jit_stencils[];
extern const int            mino_jit_stencils_count;

const stencil_desc_t *mino_jit_find_stencil(unsigned opcode);
int                   mino_jit_op_extra_words(unsigned op);
int                   mino_jit_is_direct_emit_op(unsigned op);

/* Eligibility classifier (defined in entry.c). Both out-params are
 * optional (pass NULL to skip). *first_unknown_op records the op that
 * tripped the classifier; *first_unknown_pc records its PC so a future
 * compile-with-deopt path can plant a side-exit at exactly that offset. */
cpjit_reason_t mino_jit_classify_eligibility(const mino_bc_fn_t *bc,
                                              unsigned *first_unknown_op,
                                              size_t   *first_unknown_pc);
const char    *mino_jit_reason_name(cpjit_reason_t r);

/* Symbol-name decoding (defined in entry.c). */
int      mino_jit_imm_kind_from_name(const char *sym);
uint64_t mino_jit_imm_value(mino_bc_insn_t insn, mino_bc_insn_t insn2,
                            imm_kind_t k, const mino_bc_fn_t *bc);

/* Extern-helper resolution (defined in entry.c). */
void *mino_jit_lookup_extern_fn(const char *name);

/* Host-arch patchers and direct-emit byte writers. The
 * ARM64 versions live in patcher.c, the x86_64 versions in
 * patcher_x86_64.c. Each TU gates its body on the matching
 * MINO_CPJIT_HOST_* macro so only one set is compiled per binary;
 * declarations stay visible to emit.c either way so the conditional
 * dispatch in the compile pipeline reads cleanly without
 * #ifdef-ing every call site. */
#if defined(MINO_CPJIT_HOST_ARM64)
void mino_jit_patch_adrp(uint32_t *insn, uintptr_t insn_addr,
                          uintptr_t target);
void mino_jit_patch_pageoff12_ldr64(uint32_t *insn, uintptr_t target);
int  mino_jit_patch_branch26(uint32_t *insn, uintptr_t insn_addr,
                              uintptr_t target_addr);
int  mino_jit_patch_b_unconditional(uint32_t *insn, uintptr_t insn_addr,
                                     uintptr_t target_addr);
int  mino_jit_patch_imm19(uint32_t *insn, uintptr_t insn_addr,
                           uintptr_t target_addr);
#elif defined(MINO_CPJIT_HOST_X86_64)
/* x86_64 patchers.
 *
 * patch_abs64       -- 8-byte absolute target (R_X86_64_64).
 * patch_pc32        -- 4-byte signed offset (R_X86_64_PC32). The
 *                       offset is `target - (insn + 4)`, i.e. rip
 *                       points at the next instruction.
 * patch_gotpcrel    -- 4-byte signed offset to a pool slot holding
 *                       the absolute address (R_X86_64_GOTPCREL).
 *                       Same encoding as patch_pc32 but target is
 *                       the pool slot, not the helper fn directly.
 * patch_jmp32_to    -- rewrite a 5-byte `e9 <rel32>` jmp to point
 *                       at target_addr. Used by the chain pass.
 * patch_jcc32_to    -- rewrite a 6-byte `0f 8X <rel32>` jcc to
 *                       point at target_addr. Used by JMPIFNOT
 *                       target patching. */
int  mino_jit_patch_abs64(unsigned char *insn, uintptr_t target,
                           int32_t addend);
int  mino_jit_patch_pc32(unsigned char *insn, uintptr_t insn_addr,
                          uintptr_t target, int32_t addend);
int  mino_jit_patch_gotpcrel(unsigned char *insn, uintptr_t insn_addr,
                              uintptr_t pool_slot_addr, int32_t addend);
int  mino_jit_patch_jmp32_to(unsigned char *insn, uintptr_t insn_addr,
                              uintptr_t target_addr);
int  mino_jit_patch_jcc32_to(unsigned char *insn, uintptr_t insn_addr,
                              uintptr_t target_addr);
#endif

void mino_jit_write_trampoline(unsigned char *slot, uintptr_t target_addr);
void mino_jit_emit_jmp_bytes(unsigned char *code, size_t pos);
void mino_jit_emit_jmpifnot_bytes(unsigned char *code, size_t pos,
                                   mino_bc_insn_t insn);

/* Stats hook (defined in stats.c; called from entry.c). */
void mino_jit_stats_record(const mino_bc_fn_t *bc,
                            cpjit_reason_t reason,
                            unsigned first_unknown_op,
                            size_t first_unknown_pc,
                            int compiled, size_t native_bytes);

/* Compile pipeline (defined in emit.c; called from entry.c). The
 * `deopt_at_pc` argument is (size_t)-1 for a full-body compile and a
 * PC < bc->code_len for a compile-with-deopt: the compile path emits
 * stencils for pcs [0, deopt_at_pc) and an OP_DEOPT_TO_INTERP stencil
 * at deopt_at_pc; the resulting native region falls through to deopt
 * if execution reaches the unstenciled tail. */
int mino_jit_compile_inner(mino_state_t *S, mino_val_t *fn_val,
                            size_t deopt_at_pc);

/* Slow-path helpers (defined in helpers.c; referenced by name from
 * stencils through the extern-fn table in entry.c, and addressed
 * directly by entry.c when building that table). */
mino_val_t **mino_jit_binop_slow(mino_state_t *S, mino_val_t **regs,
                                 unsigned a, unsigned b, unsigned c,
                                 unsigned subop);
mino_val_t **mino_jit_binop_k_slow(mino_state_t *S, mino_val_t **regs,
                                   unsigned a, unsigned b,
                                   mino_val_t *kimm, unsigned subop);
mino_val_t **mino_jit_unop_slow(mino_state_t *S, mino_val_t **regs,
                                unsigned a, unsigned b, unsigned subop);
mino_val_t **mino_jit_getglobal_cached_slow(mino_state_t *S,
                                            mino_val_t **regs,
                                            unsigned a,
                                            mino_bc_fn_t *bc,
                                            unsigned slot_idx);
mino_val_t **mino_jit_call_slow(mino_state_t *S, mino_val_t **regs,
                                unsigned fn_reg, unsigned argc,
                                unsigned dst);
mino_val_t **mino_jit_call_cached_slow(mino_state_t *S, mino_val_t **regs,
                                       unsigned arg_base, unsigned argc,
                                       unsigned dst,
                                       mino_bc_fn_t *bc, unsigned slot_idx);
mino_val_t **mino_jit_call_resolved_slow(mino_state_t *S, mino_val_t **regs,
                                         mino_val_t *callee,
                                         unsigned arg_base, unsigned argc,
                                         unsigned dst);
mino_val_t **mino_jit_call_known_fn_slow(mino_state_t *S, mino_val_t **regs,
                                         mino_val_t *callee,
                                         unsigned arg_base, unsigned argc,
                                         unsigned dst);
mino_val_t **mino_jit_call_known_prim_slow(mino_state_t *S,
                                           mino_val_t **regs,
                                           mino_val_t *callee,
                                           unsigned arg_base,
                                           unsigned argc,
                                           unsigned dst);
mino_val_t **mino_jit_call_known_native_slow(mino_state_t *S,
                                             mino_val_t **regs,
                                             mino_val_t *fn,
                                             mino_bc_fn_t *bc,
                                             unsigned arg_base,
                                             unsigned argc,
                                             unsigned dst);
mino_val_t **mino_jit_nth_vec_slow(mino_state_t *S, mino_val_t **regs,
                                   unsigned a, unsigned b, unsigned c);
mino_val_t **mino_jit_first_vec_slow(mino_state_t *S, mino_val_t **regs,
                                     unsigned a, unsigned b);
mino_val_t **mino_jit_count_vec_slow(mino_state_t *S, mino_val_t **regs,
                                     unsigned a, unsigned b);
mino_val_t **mino_jit_empty_vec_slow(mino_state_t *S, mino_val_t **regs,
                                     unsigned a, unsigned b);
mino_val_t **mino_jit_get_kw_map_slow(mino_state_t *S, mino_val_t **regs,
                                      unsigned a, unsigned b, unsigned c);
mino_val_t **mino_jit_conj_vec_slow(mino_state_t *S, mino_val_t **regs,
                                    unsigned a, unsigned b, unsigned c);
mino_val_t **mino_jit_assoc_slow(mino_state_t *S, mino_val_t **regs,
                                 unsigned a, unsigned b);
mino_val_t **mino_jit_assoc_bang_slow(mino_state_t *S, mino_val_t **regs,
                                      unsigned a, unsigned b);
mino_val_t **mino_jit_dissoc_slow(mino_state_t *S, mino_val_t **regs,
                                   unsigned a, unsigned b, unsigned c);
mino_val_t  *mino_jit_tailcall_slow(mino_state_t *S, mino_val_t **regs,
                                    unsigned fn_reg, unsigned argc);
mino_val_t **mino_jit_closure_slow(mino_state_t *S, mino_val_t **regs,
                                   unsigned a, mino_bc_fn_t *bc,
                                   unsigned bx);
mino_val_t **mino_jit_make_lazy_slow(mino_state_t *S, mino_val_t **regs,
                                      unsigned a, mino_bc_fn_t *bc,
                                      unsigned bx);
mino_val_t **mino_jit_protocol_call_cached_slow(mino_state_t *S,
                                                 mino_val_t **regs,
                                                 unsigned a,
                                                 unsigned argn,
                                                 unsigned ret,
                                                 mino_bc_fn_t *bc,
                                                 unsigned slot_idx);
mino_val_t  *mino_jit_protocol_tailcall_cached_slow(mino_state_t *S,
                                                     mino_val_t **regs,
                                                     unsigned a,
                                                     unsigned argn,
                                                     mino_bc_fn_t *bc,
                                                     unsigned slot_idx);
mino_val_t **mino_jit_push_env_slow(mino_state_t *S, mino_val_t **regs);
mino_val_t **mino_jit_pop_env_slow(mino_state_t *S, mino_val_t **regs);
mino_val_t **mino_jit_env_bind_slow(mino_state_t *S, mino_val_t **regs,
                                    unsigned a, mino_bc_fn_t *bc,
                                    unsigned bx);
mino_val_t **mino_jit_loop_int_lt_slow(mino_state_t *S, mino_val_t **regs,
                                       unsigned a, unsigned b);
mino_val_t **mino_jit_loop_int_dec_slow(mino_state_t *S, mino_val_t **regs,
                                        unsigned a);
mino_val_t **mino_jit_loop_int_lt_inc_slow(mino_state_t *S, mino_val_t **regs,
                                           unsigned a, unsigned b,
                                           unsigned c);
mino_val_t **mino_jit_loop_int_dec_inc_slow(mino_state_t *S, mino_val_t **regs,
                                            unsigned a, unsigned b);
void         mino_jit_loop_continue_marker(void);
void         mino_jit_chain_continue_marker(mino_val_t **regs,
                                             mino_val_t **consts,
                                             mino_state_t *S);
/* Side-exit runtime helper. The deopt stencil tail-calls into this
 * function: it sets S->jit_deopt_pending = 1, writes the resume PC to
 * S->jit_deopt_pc, then returns NULL. mino_jit_invoke checks the
 * pending flag after the native region returns. */
mino_val_t  *mino_jit_deopt_exit(mino_state_t *S, size_t resume_pc);

#endif /* MINO_CPJIT_HOST */

#endif /* MINO_EVAL_BC_JIT_INTERNAL_H */
