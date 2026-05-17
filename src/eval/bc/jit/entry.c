/*
 * src/eval/bc/jit/entry.c -- copy-and-patch JIT public entry points,
 * stencil descriptor table, eligibility classifier, and extern-helper
 * resolution table.
 *
 * The descriptor table at the bottom of this file is the single source
 * of truth for which opcodes the JIT understands. Adding a new stencil
 * means (a) extending the generated stencil header through
 * `tools/stencil-extract` over the new stencil source, and (b) adding
 * an entry here. Removing one means dropping its entry; the
 * eligibility classifier will then reject any bc that references it
 * and the fn falls back to the interpreter.
 *
 * The pipeline split is documented in src/eval/bc/jit/internal.h --
 * patcher.c, emit.c, helpers.c, and stats.c each own a slice of the
 * compile / runtime path; this file glues them together at the public
 * API surface.
 */

#include "../jit.h"
#include "internal.h"

#ifdef MINO_CPJIT_HOST

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../../../prim/internal.h"
#include "../stencils/runtime_layout.h"
#include MINO_CPJIT_STENCILS_HEADER

/* Stencil-layer runtime layout cross-check.
 *
 * stencils/runtime_layout.h exposes a curated view of the runtime
 * struct offsets the JIT's inline fast paths read, gated so the
 * canonical typedefs from the headers above still win in this
 * translation unit. The asserts below run on the dual-visibility
 * compile: each MINO_JIT_LAYOUT_OFFSET_* constant is compared to
 * `offsetof(<real struct>, <field>)`, so any field reorder in
 * runtime/internal.h or eval/bc/internal.h fires a compile error
 * here rather than corrupting a stencil read at runtime. Update both
 * sides together: bump the constant in runtime_layout.h, update the
 * matching assert here, regenerate the stencil byte tables.
 *
 * C99-compatible assert form: typedef of a negative-sized array on
 * mismatch. `_Static_assert` is C11; the runtime CFLAGS pin -std=c99
 * with -Wpedantic, so the keyword form would force the compiler to
 * emit a -Wc11-extensions diagnostic that the -Werror knob turns
 * into a hard error. */
#define MINO_JIT_LAYOUT_CAT_(a, b)         a##b
#define MINO_JIT_LAYOUT_CAT(a, b)          MINO_JIT_LAYOUT_CAT_(a, b)
#define MINO_JIT_LAYOUT_ASSERT(cond, tag)                                  \
    typedef char MINO_JIT_LAYOUT_CAT(                                      \
        mino_jit_layout_assert_##tag##_, __LINE__)[(cond) ? 1 : -1]

MINO_JIT_LAYOUT_ASSERT(MINO_JIT_LAYOUT_OFFSET_STATE_IC_GEN ==
                           offsetof(struct mino_state, ic_gen),
                       state_ic_gen);
MINO_JIT_LAYOUT_ASSERT(MINO_JIT_LAYOUT_OFFSET_STATE_BC_REGS ==
                           offsetof(struct mino_state, bc_regs),
                       state_bc_regs);
MINO_JIT_LAYOUT_ASSERT(MINO_JIT_LAYOUT_OFFSET_STATE_JIT_INVOKE_CTX ==
                           offsetof(struct mino_state, jit_invoke_ctx),
                       state_jit_invoke_ctx);
MINO_JIT_LAYOUT_ASSERT(MINO_JIT_LAYOUT_OFFSET_CTX_DYN_STACK ==
                           offsetof(struct mino_thread_ctx, dyn_stack),
                       ctx_dyn_stack);
MINO_JIT_LAYOUT_ASSERT(MINO_JIT_LAYOUT_OFFSET_BC_IC_SLOTS ==
                           offsetof(struct mino_bc_fn, ic_slots),
                       bc_ic_slots);
MINO_JIT_LAYOUT_ASSERT(sizeof(mino_bc_ic_slot_t) == 56,
                       ic_slot_size);
MINO_JIT_LAYOUT_ASSERT(offsetof(mino_bc_ic_slot_t, sym) == 0,
                       ic_slot_sym);
MINO_JIT_LAYOUT_ASSERT(offsetof(mino_bc_ic_slot_t, cached) == 8,
                       ic_slot_cached);
MINO_JIT_LAYOUT_ASSERT(offsetof(mino_bc_ic_slot_t, gen) == 16,
                       ic_slot_gen);
MINO_JIT_LAYOUT_ASSERT(offsetof(mino_bc_ic_slot_t, kind) == 20,
                       ic_slot_kind);
MINO_JIT_LAYOUT_ASSERT(offsetof(mino_bc_ic_slot_t,
                                cached_callable_kind) == 21,
                       ic_slot_cached_callable_kind);
MINO_JIT_LAYOUT_ASSERT(offsetof(mino_bc_ic_slot_t,
                                cached_fn_has_rest) == 22,
                       ic_slot_cached_fn_has_rest);
MINO_JIT_LAYOUT_ASSERT(offsetof(mino_bc_ic_slot_t, atom) == 24,
                       ic_slot_atom);
MINO_JIT_LAYOUT_ASSERT(offsetof(mino_bc_ic_slot_t, cached_map) == 32,
                       ic_slot_cached_map);
MINO_JIT_LAYOUT_ASSERT(offsetof(mino_bc_ic_slot_t, cached_type) == 40,
                       ic_slot_cached_type);
MINO_JIT_LAYOUT_ASSERT(offsetof(mino_bc_ic_slot_t,
                                cached_fn_n_params) == 48,
                       ic_slot_cached_fn_n_params);

int mino_jit_imm_kind_from_name(const char *sym)
{
    if (strcmp(sym, "MINO_STENCIL_IMM_A")    == 0) return IMM_KIND_A;
    if (strcmp(sym, "MINO_STENCIL_IMM_B")    == 0) return IMM_KIND_B;
    if (strcmp(sym, "MINO_STENCIL_IMM_C")    == 0) return IMM_KIND_C;
    if (strcmp(sym, "MINO_STENCIL_IMM_BX")   == 0) return IMM_KIND_BX;
    if (strcmp(sym, "MINO_STENCIL_IMM_SBX")  == 0) return IMM_KIND_SBX;
    if (strcmp(sym, "MINO_STENCIL_IMM_KIMM") == 0) return IMM_KIND_KIMM;
    if (strcmp(sym, "MINO_STENCIL_IMM_BC")   == 0) return IMM_KIND_BC;
    if (strcmp(sym, "MINO_STENCIL_IMM_BX2")  == 0) return IMM_KIND_BX2;
    return -1;
}

uint64_t mino_jit_imm_value(mino_bc_insn_t insn, mino_bc_insn_t insn2,
                            imm_kind_t k, const mino_bc_fn_t *bc)
{
    switch (k) {
    case IMM_KIND_A:    return (uint64_t)A_OF(insn);
    case IMM_KIND_B:    return (uint64_t)B_OF(insn);
    case IMM_KIND_C:    return (uint64_t)C_OF(insn);
    case IMM_KIND_BX:   return (uint64_t)Bx_OF(insn);
    case IMM_KIND_SBX:  return (uint64_t)(int64_t)sBx_OF(insn);
    case IMM_KIND_KIMM: {
        long long lit = (long long)(int8_t)C_OF(insn);
        return (uint64_t)(uintptr_t)MINO_MAKE_INT(lit);
    }
    case IMM_KIND_BC:   return (uint64_t)(uintptr_t)bc;
    case IMM_KIND_BX2:  return (uint64_t)Bx_OF(insn2);
    }
    return 0;
}

/* Number of extra instruction words a given opcode reads after its
 * primary word. Today only the IC-mediated call ops fetch a second
 * word; the primary dispatch never sees the slot-bearing word as a
 * free-standing op. The eligibility loop and the JIT compile walk
 * both consult this so they skip the slot word in lockstep with the
 * interpreter's `code[pc++]` in vm.c. */
int mino_jit_op_extra_words(unsigned op)
{
    switch (op) {
    case OP_CALL_CACHED:
    case OP_PROTOCOL_CALL_CACHED:
    case OP_PROTOCOL_TAILCALL_CACHED:
        return 1;
    default:
        return 0;
    }
}

/* Stencil table. Ordered by frequency in measured workloads so the
 * linear scan in find_stencil tends to hit early; the cost is
 * marginal at <30 entries but the discipline keeps the table easy
 * to extend. */
const stencil_desc_t mino_jit_stencils[] = {
    {
        OP_MOVE,
        stencil_op_move_bytes, stencil_op_move_size,
        stencil_op_move_symbols, stencil_op_move_nsymbols,
        stencil_op_move_relocs, stencil_op_move_nrelocs,
        0u  /* non-final: trailing `ret` is patched to b <next> */
    },
    {
        OP_LOAD_K,
        stencil_op_load_k_bytes, stencil_op_load_k_size,
        stencil_op_load_k_symbols, stencil_op_load_k_nsymbols,
        stencil_op_load_k_relocs, stencil_op_load_k_nrelocs,
        0u
    },
    {
        OP_RETURN,
        stencil_op_return_imm_bytes, stencil_op_return_imm_size,
        stencil_op_return_imm_symbols, stencil_op_return_imm_nsymbols,
        stencil_op_return_imm_relocs, stencil_op_return_imm_nrelocs,
        1u  /* keep `ret` -- this is the function exit */
    },
    {
        OP_FUSED_LOAD_K_RETURN,
        stencil_op_load_k_return_bytes, stencil_op_load_k_return_size,
        stencil_op_load_k_return_symbols, stencil_op_load_k_return_nsymbols,
        stencil_op_load_k_return_relocs, stencil_op_load_k_return_nrelocs,
        1u  /* keep `ret` -- fused stencil exits the fn */
    },
    {
        OP_ADD_II,
        stencil_op_add_ii_bytes, stencil_op_add_ii_size,
        stencil_op_add_ii_symbols, stencil_op_add_ii_nsymbols,
        stencil_op_add_ii_relocs, stencil_op_add_ii_nrelocs,
        0u
    },
    {
        OP_SUB_II,
        stencil_op_sub_ii_bytes, stencil_op_sub_ii_size,
        stencil_op_sub_ii_symbols, stencil_op_sub_ii_nsymbols,
        stencil_op_sub_ii_relocs, stencil_op_sub_ii_nrelocs,
        0u
    },
    {
        OP_MUL_II,
        stencil_op_mul_ii_bytes, stencil_op_mul_ii_size,
        stencil_op_mul_ii_symbols, stencil_op_mul_ii_nsymbols,
        stencil_op_mul_ii_relocs, stencil_op_mul_ii_nrelocs,
        0u
    },
    {
        OP_LT_II,
        stencil_op_lt_ii_bytes, stencil_op_lt_ii_size,
        stencil_op_lt_ii_symbols, stencil_op_lt_ii_nsymbols,
        stencil_op_lt_ii_relocs, stencil_op_lt_ii_nrelocs,
        0u
    },
    {
        OP_LE_II,
        stencil_op_le_ii_bytes, stencil_op_le_ii_size,
        stencil_op_le_ii_symbols, stencil_op_le_ii_nsymbols,
        stencil_op_le_ii_relocs, stencil_op_le_ii_nrelocs,
        0u
    },
    {
        OP_GT_II,
        stencil_op_gt_ii_bytes, stencil_op_gt_ii_size,
        stencil_op_gt_ii_symbols, stencil_op_gt_ii_nsymbols,
        stencil_op_gt_ii_relocs, stencil_op_gt_ii_nrelocs,
        0u
    },
    {
        OP_GE_II,
        stencil_op_ge_ii_bytes, stencil_op_ge_ii_size,
        stencil_op_ge_ii_symbols, stencil_op_ge_ii_nsymbols,
        stencil_op_ge_ii_relocs, stencil_op_ge_ii_nrelocs,
        0u
    },
    {
        OP_EQ_II,
        stencil_op_eq_ii_bytes, stencil_op_eq_ii_size,
        stencil_op_eq_ii_symbols, stencil_op_eq_ii_nsymbols,
        stencil_op_eq_ii_relocs, stencil_op_eq_ii_nrelocs,
        0u
    },
    {
        OP_INC_I,
        stencil_op_inc_i_bytes, stencil_op_inc_i_size,
        stencil_op_inc_i_symbols, stencil_op_inc_i_nsymbols,
        stencil_op_inc_i_relocs, stencil_op_inc_i_nrelocs,
        0u
    },
    {
        OP_DEC_I,
        stencil_op_dec_i_bytes, stencil_op_dec_i_size,
        stencil_op_dec_i_symbols, stencil_op_dec_i_nsymbols,
        stencil_op_dec_i_relocs, stencil_op_dec_i_nrelocs,
        0u
    },
    {
        OP_ZERO_INT_P,
        stencil_op_zero_int_p_bytes, stencil_op_zero_int_p_size,
        stencil_op_zero_int_p_symbols, stencil_op_zero_int_p_nsymbols,
        stencil_op_zero_int_p_relocs, stencil_op_zero_int_p_nrelocs,
        0u
    },
    {
        OP_ADD_IK,
        stencil_op_add_ik_bytes, stencil_op_add_ik_size,
        stencil_op_add_ik_symbols, stencil_op_add_ik_nsymbols,
        stencil_op_add_ik_relocs, stencil_op_add_ik_nrelocs,
        0u
    },
    {
        OP_SUB_IK,
        stencil_op_sub_ik_bytes, stencil_op_sub_ik_size,
        stencil_op_sub_ik_symbols, stencil_op_sub_ik_nsymbols,
        stencil_op_sub_ik_relocs, stencil_op_sub_ik_nrelocs,
        0u
    },
    {
        OP_LT_IK,
        stencil_op_lt_ik_bytes, stencil_op_lt_ik_size,
        stencil_op_lt_ik_symbols, stencil_op_lt_ik_nsymbols,
        stencil_op_lt_ik_relocs, stencil_op_lt_ik_nrelocs,
        0u
    },
    {
        OP_LE_IK,
        stencil_op_le_ik_bytes, stencil_op_le_ik_size,
        stencil_op_le_ik_symbols, stencil_op_le_ik_nsymbols,
        stencil_op_le_ik_relocs, stencil_op_le_ik_nrelocs,
        0u
    },
    {
        OP_EQ_IK,
        stencil_op_eq_ik_bytes, stencil_op_eq_ik_size,
        stencil_op_eq_ik_symbols, stencil_op_eq_ik_nsymbols,
        stencil_op_eq_ik_relocs, stencil_op_eq_ik_nrelocs,
        0u
    },
    {
        OP_MOD_II,
        stencil_op_mod_ii_bytes, stencil_op_mod_ii_size,
        stencil_op_mod_ii_symbols, stencil_op_mod_ii_nsymbols,
        stencil_op_mod_ii_relocs, stencil_op_mod_ii_nrelocs,
        0u
    },
    {
        OP_QUOT_II,
        stencil_op_quot_ii_bytes, stencil_op_quot_ii_size,
        stencil_op_quot_ii_symbols, stencil_op_quot_ii_nsymbols,
        stencil_op_quot_ii_relocs, stencil_op_quot_ii_nrelocs,
        0u
    },
    {
        OP_REM_II,
        stencil_op_rem_ii_bytes, stencil_op_rem_ii_size,
        stencil_op_rem_ii_symbols, stencil_op_rem_ii_nsymbols,
        stencil_op_rem_ii_relocs, stencil_op_rem_ii_nrelocs,
        0u
    },
    /* OP_LOOP_INT_LT intentionally absent: measurement against the
     * interpreter's inline fast path showed a 17% regression on the
     * canonical `count-loop` workload. The bytecode compiler still
     * emits OP_LOOP_INT_LT for the matching shape; without a stencil
     * the JIT eligibility check rejects the body and the interpreter
     * runs the loop at its (slightly faster) native rate. The
     * stencil source is kept under src/eval/bc/stencils/ in case a
     * future cycle finds a way to beat the interpreter on this
     * shape. */
    {
        OP_LOOP_INT_DEC,
        stencil_op_loop_int_dec_bytes, stencil_op_loop_int_dec_size,
        stencil_op_loop_int_dec_symbols, stencil_op_loop_int_dec_nsymbols,
        stencil_op_loop_int_dec_relocs, stencil_op_loop_int_dec_nrelocs,
        0u
    },
    {
        OP_LOOP_INT_DEC_INC,
        stencil_op_loop_int_dec_inc_bytes, stencil_op_loop_int_dec_inc_size,
        stencil_op_loop_int_dec_inc_symbols, stencil_op_loop_int_dec_inc_nsymbols,
        stencil_op_loop_int_dec_inc_relocs, stencil_op_loop_int_dec_inc_nrelocs,
        0u
    },
    {
        OP_LOOP_INT_LT_INC,
        stencil_op_loop_int_lt_inc_bytes, stencil_op_loop_int_lt_inc_size,
        stencil_op_loop_int_lt_inc_symbols, stencil_op_loop_int_lt_inc_nsymbols,
        stencil_op_loop_int_lt_inc_relocs, stencil_op_loop_int_lt_inc_nrelocs,
        0u
    },
    {
        OP_GETGLOBAL_CACHED,
        stencil_op_getglobal_cached_bytes, stencil_op_getglobal_cached_size,
        stencil_op_getglobal_cached_symbols, stencil_op_getglobal_cached_nsymbols,
        stencil_op_getglobal_cached_relocs, stencil_op_getglobal_cached_nrelocs,
        0u
    },
    {
        OP_CALL_CACHED,
        stencil_op_call_cached_bytes, stencil_op_call_cached_size,
        stencil_op_call_cached_symbols, stencil_op_call_cached_nsymbols,
        stencil_op_call_cached_relocs, stencil_op_call_cached_nrelocs,
        0u
    },
    {
        OP_CALL,
        stencil_op_call_bytes, stencil_op_call_size,
        stencil_op_call_symbols, stencil_op_call_nsymbols,
        stencil_op_call_relocs, stencil_op_call_nrelocs,
        0u
    },
    {
        OP_TAILCALL,
        stencil_op_tailcall_bytes, stencil_op_tailcall_size,
        stencil_op_tailcall_symbols, stencil_op_tailcall_nsymbols,
        stencil_op_tailcall_relocs, stencil_op_tailcall_nrelocs,
        1u  /* FINAL: returns the tail-call sentinel; ret stays as the
             * fn's natural exit so subsequent stencils after this pc
             * are dead. */
    },
    {
        OP_CLOSURE,
        stencil_op_closure_bytes, stencil_op_closure_size,
        stencil_op_closure_symbols, stencil_op_closure_nsymbols,
        stencil_op_closure_relocs, stencil_op_closure_nrelocs,
        0u
    },
    {
        OP_PUSH_ENV,
        stencil_op_push_env_bytes, stencil_op_push_env_size,
        stencil_op_push_env_symbols, stencil_op_push_env_nsymbols,
        stencil_op_push_env_relocs, stencil_op_push_env_nrelocs,
        0u
    },
    {
        OP_POP_ENV,
        stencil_op_pop_env_bytes, stencil_op_pop_env_size,
        stencil_op_pop_env_symbols, stencil_op_pop_env_nsymbols,
        stencil_op_pop_env_relocs, stencil_op_pop_env_nrelocs,
        0u
    },
    {
        OP_ENV_BIND,
        stencil_op_env_bind_bytes, stencil_op_env_bind_size,
        stencil_op_env_bind_symbols, stencil_op_env_bind_nsymbols,
        stencil_op_env_bind_relocs, stencil_op_env_bind_nrelocs,
        0u
    },
    {
        OP_NTH_VEC,
        stencil_op_nth_vec_bytes, stencil_op_nth_vec_size,
        stencil_op_nth_vec_symbols, stencil_op_nth_vec_nsymbols,
        stencil_op_nth_vec_relocs, stencil_op_nth_vec_nrelocs,
        0u
    },
    {
        OP_FIRST_VEC,
        stencil_op_first_vec_bytes, stencil_op_first_vec_size,
        stencil_op_first_vec_symbols, stencil_op_first_vec_nsymbols,
        stencil_op_first_vec_relocs, stencil_op_first_vec_nrelocs,
        0u
    },
    {
        OP_COUNT_VEC,
        stencil_op_count_vec_bytes, stencil_op_count_vec_size,
        stencil_op_count_vec_symbols, stencil_op_count_vec_nsymbols,
        stencil_op_count_vec_relocs, stencil_op_count_vec_nrelocs,
        0u
    },
    {
        OP_EMPTY_VEC,
        stencil_op_empty_vec_bytes, stencil_op_empty_vec_size,
        stencil_op_empty_vec_symbols, stencil_op_empty_vec_nsymbols,
        stencil_op_empty_vec_relocs, stencil_op_empty_vec_nrelocs,
        0u
    },
    {
        OP_GET_KW_MAP,
        stencil_op_get_kw_map_bytes, stencil_op_get_kw_map_size,
        stencil_op_get_kw_map_symbols, stencil_op_get_kw_map_nsymbols,
        stencil_op_get_kw_map_relocs, stencil_op_get_kw_map_nrelocs,
        0u
    },
    {
        OP_CONJ_VEC,
        stencil_op_conj_vec_bytes, stencil_op_conj_vec_size,
        stencil_op_conj_vec_symbols, stencil_op_conj_vec_nsymbols,
        stencil_op_conj_vec_relocs, stencil_op_conj_vec_nrelocs,
        0u
    },
    {
        OP_ASSOC,
        stencil_op_assoc_bytes, stencil_op_assoc_size,
        stencil_op_assoc_symbols, stencil_op_assoc_nsymbols,
        stencil_op_assoc_relocs, stencil_op_assoc_nrelocs,
        0u
    }
};
const int mino_jit_stencils_count =
    (int)(sizeof(mino_jit_stencils) / sizeof(mino_jit_stencils[0]));

const stencil_desc_t *mino_jit_find_stencil(unsigned opcode)
{
    for (int i = 0; i < mino_jit_stencils_count; i++) {
        if (mino_jit_stencils[i].opcode == opcode) return &mino_jit_stencils[i];
    }
    return NULL;
}

/* Direct-emit ops live alongside the stencil-driven ones in the
 * eligibility list. The compile pipeline produces ARM64 instructions
 * for these by hand (see patcher.c's emit_jmp_bytes / emit_jmpifnot_bytes)
 * instead of memcpy'ing a compiled stencil. The encoded forms are
 * short and free of pool / trampoline slots so the cost of bypassing
 * the generic stencil path is small. */
int mino_jit_is_direct_emit_op(unsigned op)
{
    return op == OP_JMP || op == OP_JMPIFNOT;
}

const char *mino_jit_reason_name(cpjit_reason_t r)
{
    switch (r) {
    case CPJIT_REASON_OK:             return "ok";
    case CPJIT_REASON_NULL_BC:        return "null-bc";
    case CPJIT_REASON_CAPTURES:       return "captures";
    case CPJIT_REASON_IC_SLOTS:       return "ic-slots";
    case CPJIT_REASON_N_CLAUSES:      return "n-clauses";
    case CPJIT_REASON_HAS_REST:       return "has-rest";
    case CPJIT_REASON_UNKNOWN_OP:     return "unknown-op";
    case CPJIT_REASON_EMPTY:          return "empty";
    case CPJIT_REASON_BAD_TERMINATOR: return "bad-terminator";
    default:                          return "?";
    }
}

/* Sets *first_unknown_op to the op number that triggered an
 * UNKNOWN_OP rejection; left untouched for any other reason. */
cpjit_reason_t mino_jit_classify_eligibility(const mino_bc_fn_t *bc,
                                              unsigned *first_unknown_op)
{
    if (bc == NULL || bc->code == NULL) return CPJIT_REASON_NULL_BC;
    /* captures no longer blocks: OP_CLOSURE / OP_PUSH_ENV / OP_POP_ENV /
     * OP_ENV_BIND all have stencils that route through the
     * jit_invoke_env publish point so closures pick up the right
     * lexical chain. */
    /* ic_slots_len > 0 no longer blocks: OP_GETGLOBAL_CACHED has a
     * stencil. PROTOCOL-kind slots are still rejected via their
     * unstencilised ops (OP_PROTOCOL_*_CACHED) in the loop below.
     *
     * Multi-arity and variadic fns are now JIT-eligible: bc_run's
     * outer dispatch picks the matching clause and sets up regs
     * (including the rest-collection list at regs[n_params]) before
     * the JIT entry check fires. For multi-arity, the JIT invoke
     * only enters when the matched clause's entry_pc == 0 (i.e.,
     * the body starts at the JIT region's front); calls into later
     * clauses fall back to the interpreter. */
    for (size_t pc = 0; pc < bc->code_len; pc++) {
        unsigned op = OP_OF(bc->code[pc]);
        if (mino_jit_is_direct_emit_op(op)) continue;
        if (mino_jit_find_stencil(op) == NULL) {
            if (first_unknown_op != NULL) *first_unknown_op = op;
            return CPJIT_REASON_UNKNOWN_OP;
        }
        /* Skip the trailing word of multi-word ops so the eligibility
         * scan doesn't classify the slot's encoded NOP byte as an
         * "unknown op". The interpreter consumes the same word via
         * `code[pc++]` from inside the handler. */
        pc += (size_t)mino_jit_op_extra_words(op);
    }
    /* Need at least one OP_RETURN so the fn terminates. The compiler
     * always emits one at the end of the body; we double-check so a
     * malformed bc doesn't get a JIT'd path with no exit. */
    if (bc->code_len == 0) return CPJIT_REASON_EMPTY;
    if (OP_OF(bc->code[bc->code_len - 1]) != OP_RETURN) {
        return CPJIT_REASON_BAD_TERMINATOR;
    }
    return CPJIT_REASON_OK;
}

int mino_jit_eligible(const mino_bc_fn_t *bc)
{
    return mino_jit_classify_eligibility(bc, NULL) == CPJIT_REASON_OK;
}

/* ----- Extern-symbol resolution table ------------------------------------ */

/* Stencils call into a small fixed set of host helpers. Each entry
 * maps the linker-visible symbol name (without the Mach-O leader
 * underscore) to the C address. Adding a new helper means adding it
 * to this table and to any stencil source that references it. */
typedef struct {
    const char *name;
    void       *addr;
} extern_fn_t;

static const extern_fn_t g_extern_fns[] = {
    {"binop_int_fast",              (void *)(uintptr_t)binop_int_fast},
    {"unop_int_fast",               (void *)(uintptr_t)unop_int_fast},
    {"mino_jit_binop_slow",         (void *)(uintptr_t)mino_jit_binop_slow},
    {"mino_jit_binop_k_slow",       (void *)(uintptr_t)mino_jit_binop_k_slow},
    {"mino_jit_unop_slow",          (void *)(uintptr_t)mino_jit_unop_slow},
    {"mino_jit_loop_int_lt_slow",     (void *)(uintptr_t)mino_jit_loop_int_lt_slow},
    {"mino_jit_loop_int_dec_slow",    (void *)(uintptr_t)mino_jit_loop_int_dec_slow},
    {"mino_jit_loop_int_lt_inc_slow", (void *)(uintptr_t)mino_jit_loop_int_lt_inc_slow},
    {"mino_jit_loop_int_dec_inc_slow",(void *)(uintptr_t)mino_jit_loop_int_dec_inc_slow},
    {"mino_jit_getglobal_cached_slow", (void *)(uintptr_t)mino_jit_getglobal_cached_slow},
    {"mino_jit_call_cached_slow",      (void *)(uintptr_t)mino_jit_call_cached_slow},
    {"mino_jit_call_resolved_slow",    (void *)(uintptr_t)mino_jit_call_resolved_slow},
    {"mino_jit_call_known_fn_slow",    (void *)(uintptr_t)mino_jit_call_known_fn_slow},
    {"mino_jit_call_known_prim_slow",  (void *)(uintptr_t)mino_jit_call_known_prim_slow},
    {"mino_jit_nth_vec_slow",          (void *)(uintptr_t)mino_jit_nth_vec_slow},
    {"mino_jit_first_vec_slow",        (void *)(uintptr_t)mino_jit_first_vec_slow},
    {"mino_jit_count_vec_slow",        (void *)(uintptr_t)mino_jit_count_vec_slow},
    {"mino_jit_empty_vec_slow",        (void *)(uintptr_t)mino_jit_empty_vec_slow},
    {"mino_jit_get_kw_map_slow",       (void *)(uintptr_t)mino_jit_get_kw_map_slow},
    {"mino_jit_conj_vec_slow",         (void *)(uintptr_t)mino_jit_conj_vec_slow},
    {"mino_jit_assoc_slow",            (void *)(uintptr_t)mino_jit_assoc_slow},
    {"mino_jit_call_slow",             (void *)(uintptr_t)mino_jit_call_slow},
    {"mino_jit_tailcall_slow",         (void *)(uintptr_t)mino_jit_tailcall_slow},
    {"mino_jit_closure_slow",          (void *)(uintptr_t)mino_jit_closure_slow},
    {"mino_jit_push_env_slow",         (void *)(uintptr_t)mino_jit_push_env_slow},
    {"mino_jit_pop_env_slow",          (void *)(uintptr_t)mino_jit_pop_env_slow},
    {"mino_jit_env_bind_slow",         (void *)(uintptr_t)mino_jit_env_bind_slow},
    {NULL, NULL}
};

void *mino_jit_lookup_extern_fn(const char *name)
{
    for (int i = 0; g_extern_fns[i].name != NULL; i++) {
        if (strcmp(g_extern_fns[i].name, name) == 0) {
            return g_extern_fns[i].addr;
        }
    }
    return NULL;
}

/* ----- Public entry points ----------------------------------------------- */

int mino_jit_compile(mino_state_t *S, mino_val_t *fn_val)
{
    if (fn_val == NULL || mino_type_of(fn_val) != MINO_FN) return -1;
    mino_bc_fn_t *bc = fn_val->as.fn.bc;
    if (bc == NULL) return -1;
    if (bc->native != NULL) return 0;

    unsigned       first_unknown_op = 0;
    cpjit_reason_t reason = mino_jit_classify_eligibility(bc, &first_unknown_op);
    if (reason != CPJIT_REASON_OK) {
        mino_jit_stats_record(bc, reason, first_unknown_op, 0, 0);
        return -1;
    }

    int rc = mino_jit_compile_inner(S, fn_val);
    mino_jit_stats_record(bc, CPJIT_REASON_OK, 0,
                          rc == 0, rc == 0 ? bc->native_size : 0);
    return rc;
}

mino_val_t *mino_jit_invoke(mino_state_t *S, mino_bc_fn_t *bc,
                            mino_val_t **regs, mino_val_t **consts,
                            mino_env_t *env)
{
    typedef mino_val_t *(*native_t)(mino_val_t **, mino_val_t **,
                                     mino_state_t *);
    native_t f = (native_t)bc->native;
    /* Publish env on the current thread ctx so slow helpers running
     * from inside the JIT region can read it. Save / restore around
     * the call to support re-entry from a nested JIT'd callee in a
     * later release. */
    mino_thread_ctx_t *ctx        = mino_current_ctx(S);
    mino_env_t        *saved_env  = ctx->jit_invoke_env;
    mino_thread_ctx_t *saved_ctx  = S->jit_invoke_ctx;
    ctx->jit_invoke_env = env;
    /* Publish ctx on S so stencil-emitted code can reach
     * `ctx->dyn_stack` via a fixed offset from S without touching
     * the Darwin __thread machinery the extractor doesn't model. */
    S->jit_invoke_ctx = ctx;
    mino_val_t *r = f(regs, consts, S);
    S->jit_invoke_ctx = saved_ctx;
    ctx->jit_invoke_env = saved_env;
    return r;
}

void mino_jit_invalidate(mino_state_t *S, mino_val_t *fn_val)
{
    (void)S;
    if (fn_val == NULL || mino_type_of(fn_val) != MINO_FN) return;
    mino_bc_fn_t *bc = fn_val->as.fn.bc;
    if (bc == NULL) return;
    if (bc->native == NULL) return;
    /* The mmap'd region and offset table stay owned by the state's
     * jit_regions list -- they're reaped at state teardown. Dropping
     * the runtime-visible pointers is the publication-visible deopt
     * step; the hot counter rewinds so the next compile attempt is
     * gated by the full threshold. */
    bc->native            = NULL;
    bc->native_size       = 0;
    bc->native_pc_offsets = NULL;
    bc->hot_counter       = 0;
}

/* Reverse-lookup: which bytecode pc owns the stencil that contains
 * `native_off` within bc->native? Used by stack-trace formatting to
 * attribute a native instruction back to its bytecode position (and
 * thus, through source_map, to a source line / column). Returns -1
 * when the offset is out of range or the fn has no offset table. */
long mino_jit_offset_to_pc(const mino_bc_fn_t *bc, unsigned native_off)
{
    if (bc == NULL || bc->native_pc_offsets == NULL) return -1;
    if (bc->code_len == 0) return -1;
    /* Linear scan suffices: code_len is typically <100 for hot fns,
     * and this helper runs only on the cold error / introspection
     * path. A binary search becomes interesting once large bodies are
     * JIT-eligible. */
    for (size_t i = 0; i + 1 < bc->code_len; i++) {
        if (native_off >= bc->native_pc_offsets[i]
            && native_off <  bc->native_pc_offsets[i + 1]) {
            return (long)i;
        }
    }
    if (native_off >= bc->native_pc_offsets[bc->code_len - 1]
        && native_off <  bc->native_size) {
        return (long)(bc->code_len - 1);
    }
    return -1;
}

#elif defined(MINO_CPJIT)

/* MINO_CPJIT defined but not on a supported host. Provide stubs
 * with the same signatures so the linker resolves them; every
 * entry reports failure / no-op so the runtime keeps using the
 * interpreter. The state's jit_regions list stays NULL.
 *
 * stdlib.h is the only dep -- the stubs reference NULL through
 * `(void)0` casts to avoid pulling in the runtime headers. */
#include <stddef.h>
#include "../../../mino.h"
#include "../../../mino_internal.h"

int mino_jit_eligible(const mino_bc_fn_t *bc)
{
    (void)bc; return 0;
}

int mino_jit_compile(mino_state_t *S, mino_val_t *fn_val)
{
    (void)S; (void)fn_val; return -1;
}

mino_val_t *mino_jit_invoke(mino_state_t *S, mino_bc_fn_t *bc,
                            mino_val_t **regs, mino_val_t **consts,
                            mino_env_t *env)
{
    (void)S; (void)bc; (void)regs; (void)consts; (void)env; return NULL;
}

void mino_jit_invalidate(mino_state_t *S, mino_val_t *fn)
{
    (void)S; (void)fn;
}

long mino_jit_offset_to_pc(const mino_bc_fn_t *bc, unsigned native_off)
{
    (void)bc; (void)native_off; return -1;
}

void mino_jit_free_all(mino_state_t *S)
{
    /* Nothing tracked when the host is unsupported. */
    (void)S;
}

#else /* !MINO_CPJIT */

/* Header's inline stubs cover this case; nothing to compile. */

#endif /* MINO_CPJIT */
