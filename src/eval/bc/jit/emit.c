/*
 * src/eval/bc/jit/emit.c -- region bookkeeping + per-instance emit +
 * top-level compile walk.
 *
 * `mino_jit_compile_inner` (called from entry.c's `mino_jit_compile`)
 * runs the two-pass copy-and-patch pipeline:
 *
 *   pass 1 -- size pass: walk the bc, picking each pc's stencil (with
 *             fusion lookahead) or direct-emit op; accumulate code /
 *             trampoline / pool budgets and validate every required
 *             extern symbol resolves.
 *   layout  -- mmap a single RW page big enough for [code | tramps |
 *             pool]; the section boundaries are derived from the size
 *             pass's totals and aligned to 16/8 bytes.
 *   pass 2 -- emit pass: for each pc, copy the stencil bytes into the
 *             code section (or synthesise direct-emit instructions),
 *             allocate / fill pool slots, write trampolines, and
 *             patch every reloc against the resulting addresses.
 *   pass A  -- chain patch: every non-final stencil source ends
 *             each of its return paths with `__attribute__((musttail))
 *             return mino_jit_chain_continue_marker(regs, consts, S)`.
 *             clang lowers each musttail into a single branch
 *             instruction whose target field carries a relocation
 *             against the marker symbol. This pass walks each
 *             non-final stencil's reloc table, finds every
 *             chain-marker relocation, and patches the branch
 *             offset to point at the next instance's native_start.
 *             Inline fast paths that exit through multiple basic
 *             blocks each emit their own chain reloc; all of them
 *             are patched to the same next-instance target.
 *   pass B  -- direct-emit target patching: resolve JMP / JMPIFNOT
 *             instances' branch offsets through the pc -> native-offset
 *             side table built during pass 2.
 *   commit  -- mprotect to RX, attach the region to the state's
 *             jit_regions list, and store the entry pointer on the bc.
 *
 * The compile is all-or-nothing: any failure in any pass tears down
 * the partial region (free + munmap) before returning -1.
 */

#include "internal.h"
#include "../jit.h"

#ifdef MINO_CPJIT_HOST

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----- emit one stencil instance ----------------------------------------- */

/* Per-emit symbol classification slots. The fixed bound mirrors the
 * extractor's MAX_SYMS (32 in tools/stencil_extract.c); raising it
 * means raising both sides together. */
enum { MINO_JIT_MAX_SYMS_PER_STENCIL = 32 };

typedef enum {
    SYM_SLOT_IMM    = 0,  /* pool slot (8-byte literal) */
    SYM_SLOT_FN     = 1,  /* 16-byte trampoline that bl's the helper */
    SYM_SLOT_LOOP   = 2,  /* fused-loop back-jump marker: patch BRANCH26
                             relocations against this symbol to the
                             stencil instance's own self_start. */
    SYM_SLOT_CHAIN  = 3   /* chain-continue tail-call marker: emit
                             defers patching; the post-emit chain pass
                             rewrites every relocation against this
                             symbol to point at the next stencil
                             instance's native_start. */
} sym_slot_kind_t;

typedef struct {
    sym_slot_kind_t kind;
    size_t          slot_offset;  /* pool[slot_offset] (IMM) or
                                   * tramp_buf + slot_offset (FN). Unused
                                   * for SYM_SLOT_LOOP. */
} sym_slot_t;

/* Place `st`'s body at `code + pos`. Walks the stencil's symbol
 * table, allocating either a pool slot (for MINO_STENCIL_IMM_*
 * symbols) or a 16-byte trampoline (for extern helper-fn symbols)
 * per entry. Patches every reloc into the corresponding slot:
 * GOT_LOAD pairs land on pool slots, BRANCH26 lands on trampoline
 * slots, the chain-continue marker's BRANCH26 is left for the
 * post-emit chain pass. Returns the number of bytes consumed in
 * the code section (== st->size). Returns -1 on capacity overflow
 * or unknown symbol. */
static long emit_stencil(unsigned char *code, size_t pos, size_t code_cap,
                         uint64_t *pool, size_t pool_pos, size_t pool_cap,
                         size_t *out_pool_pos,
                         unsigned char *tramp_buf,
                         size_t tramp_pos, size_t tramp_cap,
                         size_t *out_tramp_pos,
                         const stencil_desc_t *st,
                         mino_bc_insn_t insn, mino_bc_insn_t insn2,
                         const mino_bc_fn_t *bc,
                         uintptr_t code_base, uintptr_t pool_base,
                         uintptr_t tramp_base)
{
    if (pos + st->size > code_cap) return -1;
    if (st->nsymbols > (size_t)MINO_JIT_MAX_SYMS_PER_STENCIL) return -1;
    sym_slot_t slots[MINO_JIT_MAX_SYMS_PER_STENCIL];
    size_t new_pool_pos  = pool_pos;
    size_t new_tramp_pos = tramp_pos;
    /* Walk the symbol table once: classify each entry as IMM, FN, or
     * the fused-loop back-jump marker, and allocate the corresponding
     * slot. The extractor preserves the order each name was first
     * encountered, so subsequent reloc-driven patches index slots[s]
     * by sym_index directly. */
    for (size_t s = 0; s < st->nsymbols; s++) {
        const char *name = st->symbols[s];
        int k = mino_jit_imm_kind_from_name(name);
        if (k >= 0) {
            if (new_pool_pos >= pool_cap) return -1;
            slots[s].kind        = SYM_SLOT_IMM;
            slots[s].slot_offset = new_pool_pos;
            pool[new_pool_pos]   = mino_jit_imm_value(insn, insn2,
                                                     (imm_kind_t)k, bc);
            new_pool_pos++;
        } else if (strcmp(name, MINO_JIT_LOOP_MARKER_NAME) == 0) {
            slots[s].kind        = SYM_SLOT_LOOP;
            slots[s].slot_offset = 0;
        } else if (strcmp(name, MINO_JIT_CHAIN_MARKER_NAME) == 0
                   || strcmp(name, MINO_JIT_CHAIN_RET_MARKER_NAME) == 0) {
            slots[s].kind        = SYM_SLOT_CHAIN;
            slots[s].slot_offset = 0;
        } else {
            void *addr = mino_jit_lookup_extern_fn(name);
            if (addr == NULL) return -1;
            if (new_tramp_pos + MINO_JIT_TRAMPOLINE_SIZE > tramp_cap) return -1;
            slots[s].kind        = SYM_SLOT_FN;
            slots[s].slot_offset = new_tramp_pos;
            mino_jit_write_trampoline(tramp_buf + new_tramp_pos,
                                       (uintptr_t)addr);
            new_tramp_pos += MINO_JIT_TRAMPOLINE_SIZE;
        }
    }
    memcpy(code + pos, st->bytes, st->size);
    for (size_t r = 0; r < st->nrelocs; r++) {
        unsigned int  off    = st->relocs[r][0];
        unsigned int  kind   = st->relocs[r][1];
        unsigned int  sym    = st->relocs[r][2];
        int32_t       addend = (int32_t)st->relocs[r][3];
        (void)addend;  /* ARM64 patchers don't read addend. */
        if (off >= st->size) return -1;
        if (sym >= st->nsymbols) return -1;
        uintptr_t      insn_addr = code_base + pos + off;
        unsigned char *insn_p    = code + pos + off;
        switch (slots[sym].kind) {
        case SYM_SLOT_FN: {
            uintptr_t target = tramp_base + slots[sym].slot_offset;
#if defined(MINO_CPJIT_HOST_ARM64)
            if (kind != MINO_STENCIL_RELOC_ARM64_BRANCH26) return -1;
            if (mino_jit_patch_branch26((uint32_t *)insn_p, insn_addr,
                                         target) != 0) return -1;
#elif defined(MINO_CPJIT_HOST_X86_64)
            if (kind != MINO_STENCIL_RELOC_X86_64_PC32) return -1;
            if (mino_jit_patch_pc32(insn_p, insn_addr, target, addend) != 0) {
                return -1;
            }
#endif
            break;
        }
        case SYM_SLOT_LOOP: {
            /* Back-jump: target the stencil instance's own start. */
            uintptr_t target = code_base + pos;
#if defined(MINO_CPJIT_HOST_ARM64)
            if (kind != MINO_STENCIL_RELOC_ARM64_BRANCH26) return -1;
            if (mino_jit_patch_branch26((uint32_t *)insn_p, insn_addr,
                                         target) != 0) return -1;
#elif defined(MINO_CPJIT_HOST_X86_64)
            if (kind != MINO_STENCIL_RELOC_X86_64_PC32) return -1;
            if (mino_jit_patch_pc32(insn_p, insn_addr, target, addend) != 0) {
                return -1;
            }
#endif
            break;
        }
        case SYM_SLOT_CHAIN: {
            /* Defer: the next stencil's native_start isn't known
             * during this per-instance emit; the post-emit chain
             * pass patches every chain reloc once the layout walk
             * has placed all instances. */
#if defined(MINO_CPJIT_HOST_ARM64)
            if (kind != MINO_STENCIL_RELOC_ARM64_BRANCH26) return -1;
#elif defined(MINO_CPJIT_HOST_X86_64)
            if (kind != MINO_STENCIL_RELOC_X86_64_PC32) return -1;
#endif
            break;
        }
        case SYM_SLOT_IMM: {
            uintptr_t target = pool_base
                + slots[sym].slot_offset * sizeof(uint64_t);
            switch (kind) {
#if defined(MINO_CPJIT_HOST_ARM64)
            case MINO_STENCIL_RELOC_ARM64_PAGE21:
            case MINO_STENCIL_RELOC_ARM64_GOT_LOAD_PAGE21:
                mino_jit_patch_adrp((uint32_t *)insn_p, insn_addr, target);
                break;
            case MINO_STENCIL_RELOC_ARM64_PAGEOFF12:
            case MINO_STENCIL_RELOC_ARM64_GOT_LOAD_PAGEOFF12:
                mino_jit_patch_pageoff12_ldr64((uint32_t *)insn_p, target);
                break;
#elif defined(MINO_CPJIT_HOST_X86_64)
            case MINO_STENCIL_RELOC_X86_64_GOTPCREL:
                if (mino_jit_patch_gotpcrel(insn_p, insn_addr, target,
                                             addend) != 0) return -1;
                break;
            case MINO_STENCIL_RELOC_X86_64_PC32:
                /* clang sometimes inlines extern var reads as a
                 * rip-relative read instead of the GOT indirection
                 * (-fno-pic codegen on small/medium models). The
                 * patcher still produces a valid rel32 to the pool
                 * slot. */
                if (mino_jit_patch_pc32(insn_p, insn_addr, target,
                                         addend) != 0) return -1;
                break;
            case MINO_STENCIL_RELOC_X86_64_ABS64:
                if (mino_jit_patch_abs64(insn_p, target, addend) != 0) {
                    return -1;
                }
                break;
#endif
            default:
                return -1;
            }
            break;
        }
        }
    }
    *out_pool_pos  = new_pool_pos;
    *out_tramp_pos = new_tramp_pos;
    return (long)st->size;
}

/* ----- top-level compile -------------------------------------------------- */

/* Sizing-pass symbol accounting for one stencil: bump the pool-slot
 * count per immediate symbol and the trampoline count per extern
 * helper, skipping the loop / chain markers (they get no slot).
 * Returns -1 when an extern helper does not resolve, so the compile
 * declines before any region is mapped. */
static int stencil_account_syms(const stencil_desc_t *st,
                                size_t *pool_slots, size_t *tramp_count)
{
    for (size_t s = 0; s < st->nsymbols; s++) {
        const char *name = st->symbols[s];
        int k = mino_jit_imm_kind_from_name(name);
        if (k >= 0) {
            (*pool_slots)++;
        } else if (strcmp(name, MINO_JIT_LOOP_MARKER_NAME) == 0
                   || strcmp(name, MINO_JIT_CHAIN_MARKER_NAME) == 0
                   || strcmp(name, MINO_JIT_CHAIN_RET_MARKER_NAME) == 0) {
            /* Marker symbols don't get pool / tramp slots. */
        } else {
            if (mino_jit_lookup_extern_fn(name) == NULL) return -1;
            (*tramp_count)++;
        }
    }
    return 0;
}

/* True when the direct-emit branch at `pc` is a loop back-edge. The
 * dispatcher adds sBx to the already-advanced pc, so any negative
 * offset targets pc or earlier. Mirrors the interpreter's
 * poll-on-backward-jump condition (vm.c OP_JMP / OP_JMPIFNOT). */
static int branch_is_backward(const mino_bc_fn_t *bc, size_t pc)
{
    return sBx_OF(bc->code[pc]) < 0;
}

/* True when `bc` contains any direct-emit branch op. The fused
 * LOAD_K + RETURN superinstruction is disabled in that case so a
 * mis-aimed jump never lands on the RETURN-half of a fused pair. */
static int bc_has_branches(const mino_bc_fn_t *bc)
{
    for (size_t pc = 0; pc < bc->code_len; pc++) {
        if (mino_jit_is_direct_emit_op(OP_OF(bc->code[pc]))) return 1;
    }
    return 0;
}

/* Pick the stencil + a hint about whether the current pc participates
 * in a OP_LOAD_K + OP_RETURN superinstruction fusion. Returns the
 * stencil descriptor and writes `*out_fused = 1` when fusion fires
 * (caller must advance pc by 2 in that branch). Returns NULL when no
 * stencil covers the opcode -- which for direct-emit ops is the
 * expected case so the caller knows to skip the stencil lane. */
static const stencil_desc_t *pick_stencil(const mino_bc_fn_t *bc,
                                          size_t pc, int *out_fused,
                                          int allow_fusion)
{
    unsigned op = OP_OF(bc->code[pc]);
    *out_fused  = 0;
    if (allow_fusion && op == OP_LOAD_K && pc + 1 < bc->code_len) {
        mino_bc_insn_t cur  = bc->code[pc];
        mino_bc_insn_t next = bc->code[pc + 1];
        if (OP_OF(next) == OP_RETURN && A_OF(next) == A_OF(cur)) {
            *out_fused = 1;
            return mino_jit_find_stencil(OP_FUSED_LOAD_K_RETURN);
        }
    }
    return mino_jit_find_stencil(op);
}

/* Instance-kind enum: each bytecode pc produces one entry in the
 * compile pipeline's tracking table, and the kind decides how the
 * post-emit passes treat it. Direct-emit branches skip the trailing-
 * ret rewriter; they get their own imm19 / imm26 target patching. */
typedef enum {
    INST_STENCIL_NONFINAL = 0,
    INST_STENCIL_FINAL    = 1,
    INST_JMP              = 2,
    INST_JMPIFNOT         = 3
} inst_kind_t;

typedef struct {
    size_t                native_start;  /* byte offset within the code region */
    size_t                bc_pc;         /* bytecode pc the instance materialises */
    inst_kind_t           kind;
    /* For stencil-driven instances: the descriptor whose bytes were
     * memcpy'd at native_start. The chain pass walks
     * st->relocs / st->symbols to locate every chain-marker
     * relocation in the span. NULL for INST_JMP / INST_JMPIFNOT. */
    const stencil_desc_t *st;
} inst_t;

/* Compile-pipeline context: bundles the state shared across all five
 * sub-passes so each sub-function takes one pointer rather than a
 * dozen individual arguments. Initialised incrementally -- only the
 * fields relevant to a given pass are valid when that pass runs. */
typedef struct {
    /* set by compile_inner before any pass */
    mino_bc_fn_t         *bc;
    int                   is_deopt_compile;
    size_t                effective_code_len;
    int                   allow_fusion;
    size_t                deopt_at_pc;
    const stencil_desc_t *deopt_st;
    const stencil_desc_t *safepoint_st;
    /* written by jit_size_pass */
    size_t code_size;
    size_t pool_slots;
    size_t tramp_count;
    size_t n_backjumps;
    /* written by jit_layout */
    struct mino_jit_slab *slab;
    size_t                slot_offset;
    size_t                slot_size;
    void                 *region;
    size_t                total_size;
    size_t                need_bytes;
    unsigned             *pc_offsets;
    inst_t               *insts;
    size_t                n_inst;
    /* derived buffer pointers (set in jit_layout) */
    unsigned char        *code;
    unsigned char        *tramp_buf;
    uint64_t             *pool;
    uintptr_t             code_base;
    uintptr_t             tramp_base;
    uintptr_t             pool_base;
    size_t                tramp_offset;
    size_t                pool_offset;
    size_t                tramp_bytes;
    size_t                pool_bytes;
    /* mutable positions written/read by jit_emit_pass */
    size_t pos;
    size_t pool_pos;
    size_t tramp_pos;
} jit_compile_ctx_t;

/* Size pass: walk every pc in [0, ctx->effective_code_len) and
 * accumulate code / pool / trampoline budgets. Also validates that
 * every extern symbol resolves so the mmap below never fails mid-pass.
 * Returns 0 on success, -1 on unresolvable symbol or overflow. */
static int jit_size_pass(jit_compile_ctx_t *ctx)
{
    const mino_bc_fn_t   *bc               = ctx->bc;
    size_t                effective_code_len = ctx->effective_code_len;
    int                   is_deopt_compile  = ctx->is_deopt_compile;
    const stencil_desc_t *deopt_st          = ctx->deopt_st;
    const stencil_desc_t *safepoint_st      = ctx->safepoint_st;
    int                   allow_fusion      = ctx->allow_fusion;
    size_t code_size   = 0;
    size_t pool_slots  = 0;
    size_t tramp_count = 0;
    size_t n_backjumps = 0;
    for (size_t pc = 0; pc < effective_code_len; pc++) {
        unsigned op = OP_OF(bc->code[pc]);
        if (op == OP_JMP || op == OP_JMPIFNOT) {
            if (branch_is_backward(bc, pc)) {
                if (code_size > SIZE_MAX - safepoint_st->size) return -1;
                code_size += safepoint_st->size;
                if (stencil_account_syms(safepoint_st, &pool_slots,
                                         &tramp_count) != 0) return -1;
                n_backjumps++;
            }
            size_t branch_sz = (op == OP_JMP) ? MINO_JIT_JMP_SIZE
                                               : MINO_JIT_JMPIFNOT_SIZE;
            if (code_size > SIZE_MAX - branch_sz) return -1;
            code_size += branch_sz;
            continue;
        }
        int fused;
        const stencil_desc_t *st = pick_stencil(bc, pc, &fused, allow_fusion);
        if (st == NULL) return -1;
        if (code_size > SIZE_MAX - st->size) return -1;
        code_size += st->size;
        if (stencil_account_syms(st, &pool_slots, &tramp_count) != 0) return -1;
        if (fused) pc++;
        pc += (size_t)mino_jit_op_extra_words(op);
    }
    /* Account for the deopt stencil when compiling a prefix. */
    if (is_deopt_compile) {
        if (code_size > SIZE_MAX - deopt_st->size) return -1;
        code_size += deopt_st->size;
        if (stencil_account_syms(deopt_st, &pool_slots, &tramp_count) != 0)
            return -1;
    }
    ctx->code_size   = code_size;
    ctx->pool_slots  = pool_slots;
    ctx->tramp_count = tramp_count;
    ctx->n_backjumps = n_backjumps;
    return 0;
}

/* Layout pass: compute section offsets, pick slab vs mmap, allocate
 * the region and the two side tables (pc_offsets, insts).
 * On failure the function tears down whatever it managed to allocate
 * before returning -1, so the caller needs no cleanup for this phase. */
static int jit_layout(mino_state *S, jit_compile_ctx_t *ctx)
{
    size_t code_size   = ctx->code_size;
    size_t pool_slots  = ctx->pool_slots;
    size_t tramp_count = ctx->tramp_count;
    size_t n_backjumps = ctx->n_backjumps;
    const mino_bc_fn_t *bc = ctx->bc;

    if (tramp_count > SIZE_MAX / MINO_JIT_TRAMPOLINE_SIZE) return -1;
    if (pool_slots  > SIZE_MAX / sizeof(uint64_t))         return -1;
    /* Overflow-checked section-offset arithmetic. Each additive term is
     * checked before the addition so a pathological bc can never wrap
     * need_bytes around to a smaller-than-required allocation. */
    if (code_size > SIZE_MAX - 15u) return -1;
    size_t tramp_offset    = (code_size + 15u) & ~(size_t)15u;
    size_t tramp_bytes     = tramp_count * MINO_JIT_TRAMPOLINE_SIZE;
    if (tramp_offset > SIZE_MAX - tramp_bytes) return -1;
    size_t pool_offset_raw = tramp_offset + tramp_bytes;
    if (pool_offset_raw > SIZE_MAX - 7u) return -1;
    size_t pool_offset     = (pool_offset_raw + 7u) & ~(size_t)7u;
    size_t pool_bytes      = pool_slots * sizeof(uint64_t);
    if (pool_offset > SIZE_MAX - pool_bytes) return -1;
    size_t need_bytes      = pool_offset + pool_bytes;
    if (need_bytes == 0) need_bytes = 1;

    long page_l = jit_region_page_size();
    if (page_l <= 0) return -1;
    size_t page       = (size_t)page_l;
    size_t total_size = (need_bytes + page - 1) & ~(page - 1);

    struct mino_jit_slab *slab        = NULL;
    size_t                slot_offset = 0;
    size_t                slot_size   = 0;
    void                 *region;
    if (need_bytes <= MINO_JIT_SLAB_CUTOFF) {
        slab = jit_slab_acquire(S, need_bytes);
        if (slab == NULL) return -1;
        if (jit_slab_make_rw(slab) != 0) {
            /* Re-seal to RX so the slab doesn't stay in a writable state
             * on the failure path (a newly-allocated slab starts RW from
             * the mmap call; an existing one may have been mid-transition).
             * Ignore the return value -- this is best-effort cleanup. */
            (void)jit_slab_make_rx(slab);
            return -1;
        }
        slot_offset = slab->bump_offset;
        slot_size   = (need_bytes + MINO_JIT_SLAB_SLOT_ALIGN - 1)
                      & ~(MINO_JIT_SLAB_SLOT_ALIGN - 1);
        region      = (unsigned char *)slab->page + slot_offset;
    } else {
        region = jit_region_alloc(total_size);
        if (region == MINO_JIT_REGION_ALLOC_FAILED) return -1;
    }

    if (bc->code_len > SIZE_MAX / sizeof(unsigned)) {
        jit_compile_cleanup(slab, region, total_size);
        return -1;
    }
    unsigned *pc_offsets = (unsigned *)malloc(bc->code_len * sizeof(unsigned));
    if (pc_offsets == NULL) {
        if (slab != NULL) (void)jit_slab_make_rx(slab);
        else              jit_compile_cleanup(slab, region, total_size);
        return -1;
    }

    if (n_backjumps > SIZE_MAX - bc->code_len
        || (bc->code_len + n_backjumps) > SIZE_MAX / sizeof(inst_t)) {
        free(pc_offsets);
        jit_compile_cleanup(slab, region, total_size);
        return -1;
    }
    size_t  n_insts_alloc = bc->code_len + n_backjumps;
    inst_t *insts         = (inst_t *)malloc(n_insts_alloc * sizeof(*insts));
    if (insts == NULL) {
        free(pc_offsets);
        jit_compile_cleanup(slab, region, total_size);
        return -1;
    }

    unsigned char *code      = (unsigned char *)region;
    unsigned char *tramp_buf = code + tramp_offset;
    uint64_t      *pool      = (uint64_t *)(code + pool_offset);

    ctx->slab         = slab;
    ctx->slot_offset  = slot_offset;
    ctx->slot_size    = slot_size;
    ctx->region       = region;
    ctx->total_size   = total_size;
    ctx->need_bytes   = need_bytes;
    ctx->pc_offsets   = pc_offsets;
    ctx->insts        = insts;
    ctx->n_inst       = 0;
    ctx->code         = code;
    ctx->tramp_buf    = tramp_buf;
    ctx->pool         = pool;
    ctx->code_base    = (uintptr_t)code;
    ctx->tramp_base   = (uintptr_t)tramp_buf;
    ctx->pool_base    = (uintptr_t)pool;
    ctx->tramp_offset = tramp_offset;
    ctx->pool_offset  = pool_offset;
    ctx->tramp_bytes  = tramp_bytes;
    ctx->pool_bytes   = pool_bytes;
    ctx->pos          = 0;
    ctx->pool_pos     = 0;
    ctx->tramp_pos    = 0;
    return 0;
}

/* Emit pass: copy stencil bytes and direct-emit branch bytes into the
 * code region, filling insts[] and pc_offsets[] for the chain /
 * direct-emit passes that follow.  Returns -1 on stencil failure; the
 * caller must free insts, pc_offsets, and tear down the region. */
static int jit_emit_pass(jit_compile_ctx_t *ctx)
{
    mino_bc_fn_t         *bc               = ctx->bc;
    size_t                effective_code_len = ctx->effective_code_len;
    int                   is_deopt_compile  = ctx->is_deopt_compile;
    int                   allow_fusion      = ctx->allow_fusion;
    size_t                deopt_at_pc       = ctx->deopt_at_pc;
    const stencil_desc_t *safepoint_st      = ctx->safepoint_st;
    const stencil_desc_t *deopt_st          = ctx->deopt_st;
    unsigned char        *code              = ctx->code;
    uint64_t             *pool              = ctx->pool;
    unsigned char        *tramp_buf         = ctx->tramp_buf;
    uintptr_t             code_base         = ctx->code_base;
    uintptr_t             pool_base         = ctx->pool_base;
    uintptr_t             tramp_base        = ctx->tramp_base;
    size_t                code_size         = ctx->code_size;
    size_t                pool_slots        = ctx->pool_slots;
    size_t                tramp_bytes       = ctx->tramp_bytes;
    unsigned             *pc_offsets        = ctx->pc_offsets;
    inst_t               *insts             = ctx->insts;
    size_t pos       = ctx->pos;
    size_t pool_pos  = ctx->pool_pos;
    size_t tramp_pos = ctx->tramp_pos;
    size_t n_inst    = ctx->n_inst;
    for (size_t pc = 0; pc < effective_code_len; pc++) {
        if (pos > (size_t)UINT_MAX) return -1;
        pc_offsets[pc] = (unsigned)pos;
        unsigned op = OP_OF(bc->code[pc]);
        if (op == OP_JMP || op == OP_JMPIFNOT) {
            if (branch_is_backward(bc, pc)) {
                size_t new_pool_pos  = pool_pos;
                size_t new_tramp_pos = tramp_pos;
                long n = emit_stencil(code, pos, code_size,
                                      pool, pool_pos, pool_slots, &new_pool_pos,
                                      tramp_buf, tramp_pos, tramp_bytes,
                                      &new_tramp_pos, safepoint_st,
                                      bc->code[pc], 0, bc,
                                      code_base, pool_base, tramp_base);
                if (n < 0) return -1;
                insts[n_inst].native_start = pos;
                insts[n_inst].bc_pc        = pc;
                insts[n_inst].kind         = INST_STENCIL_NONFINAL;
                insts[n_inst].st           = safepoint_st;
                n_inst++;
                pos       += (size_t)n;
                pool_pos   = new_pool_pos;
                tramp_pos  = new_tramp_pos;
            }
            insts[n_inst].native_start = pos;
            insts[n_inst].bc_pc        = pc;
            insts[n_inst].st           = NULL;
            if (op == OP_JMP) {
                mino_jit_emit_jmp_bytes(code, pos);
                insts[n_inst].kind = INST_JMP;
                pos += MINO_JIT_JMP_SIZE;
            } else {
                mino_jit_emit_jmpifnot_bytes(code, pos, bc->code[pc]);
                insts[n_inst].kind = INST_JMPIFNOT;
                pos += MINO_JIT_JMPIFNOT_SIZE;
            }
            n_inst++;
            continue;
        }
        int fused;
        const stencil_desc_t *st = pick_stencil(bc, pc, &fused, allow_fusion);
        size_t new_pool_pos  = pool_pos;
        size_t new_tramp_pos = tramp_pos;
        mino_bc_insn_t insn2 = 0;
        if (mino_jit_op_extra_words(op) > 0 && pc + 1 < bc->code_len)
            insn2 = bc->code[pc + 1];
        long n = emit_stencil(code, pos, code_size,
                              pool, pool_pos, pool_slots, &new_pool_pos,
                              tramp_buf, tramp_pos, tramp_bytes, &new_tramp_pos,
                              st, bc->code[pc], insn2, bc,
                              code_base, pool_base, tramp_base);
        if (n < 0) return -1;
        insts[n_inst].native_start = pos;
        insts[n_inst].bc_pc        = pc;
        insts[n_inst].kind         = st->is_final ? INST_STENCIL_FINAL
                                                  : INST_STENCIL_NONFINAL;
        insts[n_inst].st           = st;
        n_inst++;
        pos       += (size_t)n;
        pool_pos   = new_pool_pos;
        tramp_pos  = new_tramp_pos;
        if (fused) { pc_offsets[pc + 1] = pc_offsets[pc]; pc++; }
        if (mino_jit_op_extra_words(op) > 0) {
            pc_offsets[pc + 1] = pc_offsets[pc];
            pc++;
        }
    }
    if (is_deopt_compile) {
        if (pos > (size_t)UINT_MAX) return -1;
        pc_offsets[deopt_at_pc] = (unsigned)pos;
        mino_bc_insn_t deopt_insn =
            (mino_bc_insn_t)(OP_DEOPT_TO_INTERP & 0xFFu)
            | (mino_bc_insn_t)((deopt_at_pc & 0xFFFFu) << 16);
        size_t new_pool_pos  = pool_pos;
        size_t new_tramp_pos = tramp_pos;
        long n = emit_stencil(code, pos, code_size,
                              pool, pool_pos, pool_slots, &new_pool_pos,
                              tramp_buf, tramp_pos, tramp_bytes, &new_tramp_pos,
                              deopt_st, deopt_insn, 0, bc,
                              code_base, pool_base, tramp_base);
        if (n < 0) return -1;
        insts[n_inst].native_start = pos;
        insts[n_inst].bc_pc        = deopt_at_pc;
        insts[n_inst].kind         = INST_STENCIL_FINAL;
        insts[n_inst].st           = deopt_st;
        n_inst++;
        pos       += (size_t)n;
        pool_pos   = new_pool_pos;
        tramp_pos  = new_tramp_pos;
        for (size_t pc = deopt_at_pc + 1; pc < bc->code_len; pc++)
            pc_offsets[pc] = (unsigned)pos;
    }
    ctx->pos      = pos;
    ctx->pool_pos = pool_pos;
    ctx->tramp_pos = tramp_pos;
    ctx->n_inst   = n_inst;
    return 0;
}

/* Chain pass (Pass A): patch each non-final stencil's chain-continue
 * tail call(s) to branch into the next instance's native_start.
 * Returns -1 on reloc error; caller frees insts/pc_offsets and tears
 * down the region. */
static int jit_chain_pass(jit_compile_ctx_t *ctx)
{
    unsigned char        *code      = ctx->code;
    uintptr_t             code_base = ctx->code_base;
    const inst_t         *insts     = ctx->insts;
    size_t                n_inst    = ctx->n_inst;
    for (size_t i = 0; i + 1 < n_inst; i++) {
        if (insts[i].kind != INST_STENCIL_NONFINAL) continue;
        const stencil_desc_t *st = insts[i].st;
        uintptr_t next_addr      = code_base + insts[i + 1].native_start;
        int any                  = 0;
        for (size_t r = 0; r < st->nrelocs; r++) {
            unsigned int off  = st->relocs[r][0];
            unsigned int kind = st->relocs[r][1];
            unsigned int sym  = st->relocs[r][2];
            if (sym >= st->nsymbols) return -1;
            if (strcmp(st->symbols[sym], MINO_JIT_CHAIN_MARKER_NAME) != 0
                && strcmp(st->symbols[sym],
                          MINO_JIT_CHAIN_RET_MARKER_NAME) != 0) {
                continue;
            }
            uintptr_t      insn_addr = code_base + insts[i].native_start + off;
            unsigned char *insn_p    = code + insts[i].native_start + off;
#if defined(MINO_CPJIT_HOST_ARM64)
            if (kind != MINO_STENCIL_RELOC_ARM64_BRANCH26) return -1;
            if (mino_jit_patch_b_unconditional((uint32_t *)insn_p, insn_addr,
                                                next_addr) != 0) return -1;
#elif defined(MINO_CPJIT_HOST_X86_64)
            if (kind != MINO_STENCIL_RELOC_X86_64_PC32) return -1;
            /* The musttail call landed as `e9 <rel32>` (5 bytes).
             * The reloc offset points at the rel32 field; back up one
             * byte to land on the opcode. Guard against off==0, which
             * would make insn_p-1 underrun the code buffer. */
            if (off < 1) return -1;
            if (mino_jit_patch_jmp32_to(insn_p - 1, insn_addr - 1,
                                          next_addr) != 0) return -1;
#endif
            any = 1;
        }
        if (!any) return -1;
    }
    return 0;
}

/* Direct-emit pass (Pass B): resolve JMP / JMPIFNOT target addresses
 * through the pc_offsets side table and patch each branch in place.
 * Returns -1 on out-of-range target or patcher error; caller frees
 * insts/pc_offsets and tears down the region. */
static int jit_direct_emit_pass(jit_compile_ctx_t *ctx)
{
    unsigned char        *code              = ctx->code;
    uintptr_t             code_base         = ctx->code_base;
    const inst_t         *insts             = ctx->insts;
    size_t                n_inst            = ctx->n_inst;
    const mino_bc_fn_t   *bc               = ctx->bc;
    size_t                effective_code_len = ctx->effective_code_len;
    const unsigned       *pc_offsets        = ctx->pc_offsets;
    for (size_t i = 0; i < n_inst; i++) {
        if (insts[i].kind != INST_JMP && insts[i].kind != INST_JMPIFNOT)
            continue;
        size_t         bc_pc     = insts[i].bc_pc;
        mino_bc_insn_t ins       = bc->code[bc_pc];
        ptrdiff_t      target_pc = (ptrdiff_t)bc_pc + 1 + sBx_OF(ins);
        if (target_pc < 0 || (size_t)target_pc >= effective_code_len) return -1;
        uintptr_t target_addr = code_base + pc_offsets[target_pc];
        if (insts[i].kind == INST_JMP) {
            uintptr_t      insn_addr = code_base + insts[i].native_start;
            unsigned char *insn_p    = code + insts[i].native_start;
#if defined(MINO_CPJIT_HOST_ARM64)
            if (mino_jit_patch_b_unconditional((uint32_t *)insn_p,
                                                insn_addr, target_addr) != 0)
                return -1;
#elif defined(MINO_CPJIT_HOST_X86_64)
            if (mino_jit_patch_jmp32_to(insn_p, insn_addr, target_addr) != 0)
                return -1;
#endif
        } else { /* INST_JMPIFNOT */
            unsigned char *base_p = code + insts[i].native_start;
            uintptr_t      base_a = code_base + insts[i].native_start;
#if defined(MINO_CPJIT_HOST_ARM64)
            uint32_t *cbz_p = (uint32_t *)(base_p + 4);
            uint32_t *bls_p = (uint32_t *)(base_p + 16);
            if (mino_jit_patch_imm19(cbz_p, base_a + 4,  target_addr) != 0
             || mino_jit_patch_imm19(bls_p, base_a + 16, target_addr) != 0)
                return -1;
#elif defined(MINO_CPJIT_HOST_X86_64)
            if (mino_jit_patch_jcc32_to(base_p + 10, base_a + 10,
                                         target_addr) != 0
             || mino_jit_patch_jcc32_to(base_p + 24, base_a + 24,
                                         target_addr) != 0)
                return -1;
#endif
        }
    }
    return 0;
}

int mino_jit_compile_inner(mino_state *S, mino_val *fn_val,
                            size_t deopt_at_pc)
{
    mino_bc_fn_t *bc = fn_val->as.fn.bc;
    if (bc->code_len == 0) return -1;

    jit_compile_ctx_t ctx;
    ctx.bc                = bc;
    ctx.is_deopt_compile  = (deopt_at_pc != (size_t)-1);
    ctx.effective_code_len = ctx.is_deopt_compile ? deopt_at_pc : bc->code_len;
    ctx.allow_fusion      = !bc_has_branches(bc);
    ctx.deopt_at_pc       = deopt_at_pc;
    ctx.deopt_st          = ctx.is_deopt_compile
                            ? mino_jit_find_stencil(OP_DEOPT_TO_INTERP) : NULL;
    if (ctx.is_deopt_compile && ctx.deopt_st == NULL) return -1;
    ctx.safepoint_st = mino_jit_find_stencil(OP_SAFEPOINT_POLL);
    if (ctx.safepoint_st == NULL) return -1;

    if (jit_size_pass(&ctx) != 0) return -1;
    if (jit_layout(S, &ctx)  != 0) return -1;

    /* From here ctx.insts and ctx.pc_offsets are live; any failure
     * path must free them and tear down the region. */
#define JIT_FAIL do { \
    free(ctx.insts); free(ctx.pc_offsets); \
    jit_compile_cleanup(ctx.slab, ctx.region, ctx.total_size); \
    return -1; \
} while (0)

    if (jit_emit_pass(&ctx)        != 0) { JIT_FAIL; }
    if (jit_chain_pass(&ctx)       != 0) { JIT_FAIL; }
    if (jit_direct_emit_pass(&ctx) != 0) { JIT_FAIL; }
    free(ctx.insts);

    /* Flush the I-cache so the CPU sees the freshly written instructions. */
    {
        size_t flush_bytes = (ctx.slab != NULL) ? ctx.slot_size : ctx.total_size;
#if defined(__GNUC__) || defined(__clang__)
        __builtin___clear_cache((char *)ctx.region,
                                (char *)ctx.region + flush_bytes);
#elif defined(_WIN32)
#include <windows.h>
        FlushInstructionCache(GetCurrentProcess(), ctx.region, flush_bytes);
#else
#error "MINO_CPJIT_HOST requires an I-cache flush primitive; add one for this compiler"
#endif
    }

    /* Re-seal to RX. */
    if (ctx.slab != NULL) {
        if (jit_slab_make_rx(ctx.slab) != 0) {
            free(ctx.pc_offsets);
            return -1;
        }
    } else {
        if (jit_region_make_rx(ctx.region, ctx.total_size) != 0) {
            free(ctx.pc_offsets);
            jit_compile_cleanup(ctx.slab, ctx.region, ctx.total_size);
            return -1;
        }
    }

    /* Track the region and commit to bc. */
    if (ctx.slab == NULL) {
        if (region_track(S, ctx.region, ctx.total_size, ctx.pc_offsets) != 0) {
            free(ctx.pc_offsets);
            jit_compile_cleanup(ctx.slab, ctx.region, ctx.total_size);
            return -1;
        }
    } else {
        if (region_track(S, NULL, 0, ctx.pc_offsets) != 0) {
            free(ctx.pc_offsets);
            return -1;
        }
    }

    if (ctx.slab != NULL) {
        ctx.slab->bump_offset = ctx.slot_offset + ctx.slot_size;
        ctx.slab->live_slots++;
    }
    bc->native            = ctx.region;
    bc->native_size       = (ctx.slab != NULL) ? ctx.slot_size : ctx.total_size;
    bc->native_gen        = S->ns_vars.ic_gen;
    bc->native_slab       = ctx.slab;
    bc->native_pc_offsets = ctx.pc_offsets;
    {
        size_t used  = ctx.pos + ctx.tramp_pos + ctx.pool_pos;
        size_t total = (ctx.slab != NULL) ? ctx.slot_size : ctx.total_size;
        bc->jit_code_bytes = (ctx.pos > UINT32_MAX) ? UINT32_MAX
                                                     : (uint32_t)ctx.pos;
        bc->jit_code_region_dead = (total > used)
            ? ((total - used > UINT32_MAX) ? UINT32_MAX
                                            : (uint32_t)(total - used))
            : 0u;
    }
    {
        const char *trace = getenv("MINO_CPJIT_TRACE");
        if (trace != NULL && trace[0] != '\0' && trace[0] != '0') {
            fprintf(stderr,
                    "[cpjit] compiled bc (code_len=%zu, region=%p, "
                    "total_size=%zu, code_used=%zu, tramp_used=%zu, "
                    "pool_used=%zu)\n",
                    bc->code_len, ctx.region, ctx.total_size, ctx.pos,
                    ctx.tramp_pos, ctx.pool_pos);
            if (trace[0] == '2') {
                for (size_t i = 0; i < ctx.pos; i += 4) {
                    uint32_t insn;
                    memcpy(&insn, (unsigned char *)ctx.region + i, 4);
                    fprintf(stderr, "  %04zx: %08x\n", i, insn);
                }
            }
        }
    }
#undef JIT_FAIL
    return 0;
}

#endif /* MINO_CPJIT_HOST */

/* Keep this TU non-empty under -Werror=pedantic when the gate above
 * is false. */
typedef int mino_jit_emit_tu_marker;
