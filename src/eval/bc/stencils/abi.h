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
typedef struct mino_bc_fn    mino_bc_fn_t;

/* Chain mechanism for non-final stencils.
 *
 * Every non-final stencil ends each of its return paths with a
 * `musttail` call to the extern chain-continue marker. clang lowers
 * the musttail to an unconditional branch (BRANCH26 on ARM64, PC32
 * jmp on x86_64) against the marker symbol; the linker emits one
 * relocation per call site. The JIT's emit pass walks each
 * non-final stencil's reloc table, finds every relocation against
 * the chain marker, and patches the branch offset to point at the
 * next stencil instance's start.
 *
 * Why a marker (and not the old struct-return + register-pin
 * trick)? The previous design leaned on three AArch64-specific
 * properties: (1) AAPCS returns 2-pointer structs in (x0, x1),
 * (2) the first three args also live in (x0, x1, x2), so the
 * previous stencil's return registers ARE the next stencil's arg
 * registers, and (3) `ret` and `b` are both 4 bytes so a patcher
 * can swap one for the other in place. None of those hold on
 * x86_64 SysV; the marker design is arch-portable because every
 * supported host has a sensible tail-call branch.
 *
 * `__attribute__((musttail))` forces clang to emit the call as a
 * jump and reuse the caller's frame -- there is no epilogue or
 * trailing ret. The marker's signature `(regs, consts, S)` is the
 * canonical chain signature; every chained stencil takes (and
 * passes through) the same three pointers so musttail's strict
 * signature-match rule holds.
 *
 * The marker function exists only so the linker resolves the
 * symbol; the runtime never executes it. The JIT rewrites every
 * call site before the region is set RX. */
extern void mino_jit_chain_continue_marker(mino_val_t **regs,
                                           mino_val_t **consts,
                                           mino_state_t *S);

#define MINO_STENCIL_CHAIN_RETURN(regs_, consts_, S_)                      \
    do {                                                                   \
        __attribute__((musttail))                                          \
        return mino_jit_chain_continue_marker((regs_), (consts_), (S_));   \
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
/* Per-fn bc pointer. The JIT compile pipeline writes the address of
 * the bc that owns this stencil instance; stencils that need to
 * reach the fn's ic_slots, consts, or other per-fn state read it as
 * a `mino_bc_fn_t *`. */
extern char MINO_STENCIL_IMM_BC[];
/* Bx of the trailing word for two-word ops (OP_CALL_CACHED and the
 * protocol-call siblings). The JIT compile walk consumes both words
 * together; the pool slot carries the slot-index payload from word-2
 * so the stencil reads it as a 16-bit unsigned. */
extern char MINO_STENCIL_IMM_BX2[];

#define IMM_A    ((unsigned long)(uintptr_t)MINO_STENCIL_IMM_A)
#define IMM_B    ((unsigned long)(uintptr_t)MINO_STENCIL_IMM_B)
#define IMM_C    ((unsigned long)(uintptr_t)MINO_STENCIL_IMM_C)
#define IMM_BX   ((unsigned long)(uintptr_t)MINO_STENCIL_IMM_BX)
#define IMM_SBX  ((long)(intptr_t)MINO_STENCIL_IMM_SBX)
#define IMM_KIMM ((mino_val_t *)(uintptr_t)MINO_STENCIL_IMM_KIMM)
#define IMM_BC   ((mino_bc_fn_t *)(uintptr_t)MINO_STENCIL_IMM_BC)
#define IMM_BX2  ((unsigned long)(uintptr_t)MINO_STENCIL_IMM_BX2)

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
#define STENCIL_BINOP_MOD  8u
#define STENCIL_BINOP_QUOT 9u
#define STENCIL_BINOP_REM  10u
#define STENCIL_BINOP_BAND 11u
#define STENCIL_BINOP_BOR  12u
#define STENCIL_BINOP_BXOR 13u
#define STENCIL_BINOP_SHL  14u
#define STENCIL_BINOP_SHR  15u
#define STENCIL_BINOP_USHR 16u
#define STENCIL_UNOP_INC      0u
#define STENCIL_UNOP_DEC      1u
#define STENCIL_UNOP_ZERO_P   2u
#define STENCIL_UNOP_POS_P    3u
#define STENCIL_UNOP_NEG_P    4u
#define STENCIL_UNOP_EVEN_P   5u
#define STENCIL_UNOP_ODD_P    6u
#define STENCIL_UNOP_BNOT     7u

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

/* OP_GETGLOBAL_CACHED slow helper. Resolves the bc's ic-slot at index
 * `slot_idx` through the shared `ic_resolve_global` cascade (dyn
 * binding -> lexical env -> cached var -> resolve), refills the slot
 * on a miss under the GC write barrier, and stores the resolved value
 * into regs[a]. env is passed NULL by the JIT path; JIT-eligible fns
 * have `captures == 0` so their bodies never read env-bound names
 * inside ic_resolve_global's env-lookup branch. Returns the (possibly
 * relocated) regs base on success or NULL on resolve failure. */
extern mino_val_t **mino_jit_getglobal_cached_slow(mino_state_t *S,
                                                    mino_val_t **regs,
                                                    unsigned a,
                                                    mino_bc_fn_t *bc,
                                                    unsigned slot_idx);

/* OP_CALL_CACHED slow helper. Resolves the callee through the same
 * IC cascade (dyn / env / cached / resolve), invokes
 * `apply_callable_argv` with the args sitting at
 * regs[arg_base..arg_base + argc - 1], and stores the return value
 * at regs[dst]. Refreshes the regs base on return; returns NULL on
 * resolve failure or apply-side error (longjmp-style errors fire
 * through the caller's try frame and never reach this NULL path). */
extern mino_val_t **mino_jit_call_cached_slow(mino_state_t *S,
                                               mino_val_t **regs,
                                               unsigned arg_base,
                                               unsigned argc,
                                               unsigned dst,
                                               mino_bc_fn_t *bc,
                                               unsigned slot_idx);

/* Inlined-resolve complement: the stencil's inline fast path verified
 * the IC slot is hot (cached !=NULL, gen match, no dyn binding) and
 * pre-resolved the callee. This helper skips the second IC lookup
 * and goes straight to `apply_callable_argv`. Same regs / GC
 * refresh contract as `mino_jit_call_cached_slow`. */
extern mino_val_t **mino_jit_call_resolved_slow(mino_state_t *S,
                                                 mino_val_t **regs,
                                                 mino_val_t *callee,
                                                 unsigned arg_base,
                                                 unsigned argc,
                                                 unsigned dst);

/* Known-bc-fn complement: stencil's inline fast path additionally
 * verified that the IC slot's cached_callable_kind is the
 * single-arity bc-fn shape (and that argc matches), so this helper
 * skips the var-unwrap + type-of dispatch switch inside
 * apply_callable_argv and enters at mino_apply_known_bc_fn_argv. Same
 * regs / GC refresh contract as `mino_jit_call_resolved_slow`. */
extern mino_val_t **mino_jit_call_known_fn_slow(mino_state_t *S,
                                                 mino_val_t **regs,
                                                 mino_val_t *callee,
                                                 unsigned arg_base,
                                                 unsigned argc,
                                                 unsigned dst);

/* Known-PRIM_ARGV complement: stencil's inline path verified the IC
 * slot's cached_callable_kind is MINO_IC_CALLABLE_PRIM_ARGV, so the
 * callee is a MINO_PRIM with fn2 set. Skips apply_callable_argv's
 * dispatch switch and invokes the prim directly. Same regs / GC
 * refresh contract as `mino_jit_call_resolved_slow`. */
extern mino_val_t **mino_jit_call_known_prim_slow(mino_state_t *S,
                                                   mino_val_t **regs,
                                                   mino_val_t *callee,
                                                   unsigned arg_base,
                                                   unsigned argc,
                                                   unsigned dst);

/* Cached-native complement: stencil's inline path verified the IC
 * slot's cached_callable_kind is MINO_FN_BC_SINGLE, argc matches,
 * has_rest is zero, AND slot->cached_bc is non-NULL (so the slot has
 * already classified the callable as a bc-runnable single-arity fn).
 * This helper skips invoke_bc_fn_argv's staleness rechecks (the IC
 * gen already validated them) and enters mino_bc_run directly with
 * the pre-resolved bc; mino_bc_run's own native dispatch then routes
 * to mino_jit_invoke when bc->native is set. Falls back to
 * apply_callable_argv when the cached bc has drifted out from under
 * the slot (mino_bc_run returns NULL on shape mismatch). Same regs /
 * GC refresh contract as `mino_jit_call_resolved_slow`. */
extern mino_val_t **mino_jit_call_known_native_slow(mino_state_t *S,
                                                     mino_val_t **regs,
                                                     mino_val_t *fn,
                                                     mino_bc_fn_t *bc,
                                                     unsigned arg_base,
                                                     unsigned argc,
                                                     unsigned dst);

/* OP_NTH_VEC slow helper. Inlined vector fast lane + cons-and-prim_nth
 * fallback. */
extern mino_val_t **mino_jit_nth_vec_slow(mino_state_t *S,
                                           mino_val_t **regs,
                                           unsigned a, unsigned b,
                                           unsigned c);

/* OP_FIRST_VEC slow helper. Inlined vector fast lane + cons-and-prim_first
 * fallback. */
extern mino_val_t **mino_jit_first_vec_slow(mino_state_t *S,
                                             mino_val_t **regs,
                                             unsigned a, unsigned b);

/* OP_COUNT_VEC slow helper. Tagged-int len fast lane on MINO_VECTOR,
 * cons-and-prim_count fallback. */
extern mino_val_t **mino_jit_count_vec_slow(mino_state_t *S,
                                             mino_val_t **regs,
                                             unsigned a, unsigned b);

/* OP_EMPTY_VEC slow helper. true/false fast lane on MINO_VECTOR by
 * .len comparison, cons-and-prim_empty_p fallback. */
extern mino_val_t **mino_jit_empty_vec_slow(mino_state_t *S,
                                             mino_val_t **regs,
                                             unsigned a, unsigned b);

/* OP_GET_KW_MAP slow helper. MINO_MAP + MINO_RECORD/KEYWORD fast lanes
 * mirroring the bc_run handler; falls through to prim_get for
 * sorted-maps, transients, 3-arg-default forms, and record ext-map
 * hits. */
extern mino_val_t **mino_jit_get_kw_map_slow(mino_state_t *S,
                                              mino_val_t **regs,
                                              unsigned a, unsigned b,
                                              unsigned c);

/* OP_CONJ_VEC slow helper. MINO_VECTOR fast lane via vec_conj1
 * (allocates -- regs base may relocate); cons-and-prim_conj fallback
 * for lists, sets, sorted-colls, etc. */
extern mino_val_t **mino_jit_conj_vec_slow(mino_state_t *S,
                                            mino_val_t **regs,
                                            unsigned a, unsigned b,
                                            unsigned c);

/* OP_ASSOC slow helper. 3-arg shape: [coll, k, v] sits at regs[b..b+2].
 * MINO_VECTOR+int-key fast lane via vec_assoc1; MINO_MAP fast lane
 * via mino_map_assoc1; falls through to prim_assoc otherwise. */
extern mino_val_t **mino_jit_assoc_slow(mino_state_t *S,
                                         mino_val_t **regs,
                                         unsigned a, unsigned b);
extern mino_val_t **mino_jit_dissoc_slow(mino_state_t *S,
                                          mino_val_t **regs,
                                          unsigned a, unsigned b,
                                          unsigned c);

/* OP_CALL slow helper -- uncached path. Callee comes from
 * regs[fn_reg]; args sit at regs[fn_reg + 1..fn_reg + argc]; the
 * return value lands at regs[dst]. Routes through
 * `apply_callable_argv` so every callable kind reaches its correct
 * entry. */
extern mino_val_t **mino_jit_call_slow(mino_state_t *S,
                                        mino_val_t **regs,
                                        unsigned fn_reg,
                                        unsigned argc,
                                        unsigned dst);

/* OP_TAILCALL slow helper -- builds the args cons list, publishes
 * (callee, args) on the state's tail-call sentinel, and returns
 * `&S->tail_call_sentinel`. The stencil ferries the sentinel back
 * to mino_bc_run via its natural fn ret; mino_bc_run's caller
 * (apply_callable's trampoline loop) picks up the new (fn, args)
 * without growing the C stack. Returns NULL on cons OOM. */
extern mino_val_t *mino_jit_tailcall_slow(mino_state_t *S,
                                           mino_val_t **regs,
                                           unsigned fn_reg,
                                           unsigned argc);

/* Closure / env helpers. The JIT-invoke env is published on
 * `mino_thread_ctx_t::jit_invoke_env`; every helper that mutates
 * env (`mino_jit_push_env_slow`, `mino_jit_pop_env_slow`,
 * `mino_jit_env_bind_slow`) updates that field so subsequent
 * lookups by `mino_jit_getglobal_cached_slow` see the new chain.
 * mino_jit_invoke restores the prior env on JIT-region exit so
 * neighbouring JIT regions and the surrounding interpreter frame
 * are unaffected. */
extern mino_val_t **mino_jit_closure_slow(mino_state_t *S,
                                           mino_val_t **regs,
                                           unsigned a,
                                           mino_bc_fn_t *bc,
                                           unsigned bx);
extern mino_val_t **mino_jit_make_lazy_slow(mino_state_t *S,
                                             mino_val_t **regs,
                                             unsigned a,
                                             mino_bc_fn_t *bc,
                                             unsigned bx);
extern mino_val_t **mino_jit_protocol_call_cached_slow(mino_state_t *S,
                                                        mino_val_t **regs,
                                                        unsigned a,
                                                        unsigned argn,
                                                        unsigned ret,
                                                        mino_bc_fn_t *bc,
                                                        unsigned slot_idx);
extern mino_val_t  *mino_jit_protocol_tailcall_cached_slow(mino_state_t *S,
                                                            mino_val_t **regs,
                                                            unsigned a,
                                                            unsigned argn,
                                                            mino_bc_fn_t *bc,
                                                            unsigned slot_idx);
extern mino_val_t **mino_jit_push_env_slow(mino_state_t *S,
                                            mino_val_t **regs);
extern mino_val_t **mino_jit_pop_env_slow(mino_state_t *S,
                                           mino_val_t **regs);
extern mino_val_t **mino_jit_env_bind_slow(mino_state_t *S,
                                            mino_val_t **regs,
                                            unsigned a,
                                            mino_bc_fn_t *bc,
                                            unsigned bx);

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
extern mino_val_t **mino_jit_loop_int_dec_inc_slow(mino_state_t *S,
                                                    mino_val_t **regs,
                                                    unsigned a, unsigned b);

/* The continue-marker symbol. Fused-loop stencils end their continue
 * path with `__asm__("b _mino_jit_loop_continue_marker")`. The JIT
 * scans for the BRANCH26 reloc against this name in each stencil
 * instance and overwrites the branch offset to point at the stencil's
 * own start (the back-jump destination). The function is never called
 * directly; only its address-as-symbol matters. */
extern void mino_jit_loop_continue_marker(void);

#endif /* MINO_BC_STENCIL_ABI_H */
