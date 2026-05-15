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
 *   pass A  -- chain rewrite: rewrite every `ret` inside each
 *             non-final stencil's span into a `b <next>` chain branch.
 *             Inline fast paths often emit multiple `ret` instructions
 *             (lean exit + slow-path exit); patching only the first
 *             would leave the remaining `ret` short-circuiting out of
 *             the JIT region.
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

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ----- region book-keeping ------------------------------------------------ */

static int region_track(mino_state_t *S, void *ptr, size_t size, void *aux_ptr)
{
    struct mino_jit_region *node =
        (struct mino_jit_region *)malloc(sizeof(*node));
    if (node == NULL) return -1;
    node->ptr     = ptr;
    node->size    = size;
    node->aux_ptr = aux_ptr;
    node->next    = S->jit_regions;
    S->jit_regions = node;
    return 0;
}

void mino_jit_free_all(mino_state_t *S)
{
    struct mino_jit_region *node = S->jit_regions;
    while (node != NULL) {
        struct mino_jit_region *next = node->next;
        if (node->aux_ptr != NULL) free(node->aux_ptr);
        if (node->ptr     != NULL) munmap(node->ptr, node->size);
        free(node);
        node = next;
    }
    S->jit_regions = NULL;
}

/* ----- emit one stencil instance ----------------------------------------- */

/* Per-emit symbol classification slots. The fixed bound mirrors the
 * extractor's MAX_SYMS (32 in tools/stencil_extract.c); raising it
 * means raising both sides together. */
enum { MINO_JIT_MAX_SYMS_PER_STENCIL = 32 };

typedef enum {
    SYM_SLOT_IMM    = 0,  /* pool slot (8-byte literal) */
    SYM_SLOT_FN     = 1,  /* 16-byte trampoline that bl's the helper */
    SYM_SLOT_LOOP   = 2   /* fused-loop back-jump marker: patch BRANCH26
                             relocations against this symbol to the
                             stencil instance's own self_start. */
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
 * slots. Returns the number of bytes consumed in the code section
 * (== st->size; the trailing `ret` is rewritten in a later pass).
 * Returns -1 on capacity overflow or unknown symbol. */
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
    if (st->nsymbols > (unsigned long)MINO_JIT_MAX_SYMS_PER_STENCIL) return -1;
    sym_slot_t slots[MINO_JIT_MAX_SYMS_PER_STENCIL];
    size_t new_pool_pos  = pool_pos;
    size_t new_tramp_pos = tramp_pos;
    /* Walk the symbol table once: classify each entry as IMM, FN, or
     * the fused-loop back-jump marker, and allocate the corresponding
     * slot. The extractor preserves the order each name was first
     * encountered, so subsequent reloc-driven patches index slots[s]
     * by sym_index directly. */
    for (unsigned long s = 0; s < st->nsymbols; s++) {
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
    for (unsigned long r = 0; r < st->nrelocs; r++) {
        unsigned int off  = st->relocs[r][0];
        unsigned int kind = st->relocs[r][1];
        unsigned int sym  = st->relocs[r][2];
        if (off >= st->size) return -1;
        if (sym >= st->nsymbols) return -1;
        uintptr_t   insn_addr = code_base + pos + off;
        uint32_t   *insn_p    = (uint32_t *)(code + pos + off);
        switch (slots[sym].kind) {
        case SYM_SLOT_FN: {
            uintptr_t target = tramp_base + slots[sym].slot_offset;
            if (kind != MINO_STENCIL_RELOC_ARM64_BRANCH26) return -1;
            if (mino_jit_patch_branch26(insn_p, insn_addr, target) != 0) return -1;
            break;
        }
        case SYM_SLOT_LOOP: {
            /* Back-jump: target the stencil instance's own start. */
            uintptr_t target = code_base + pos;
            if (kind != MINO_STENCIL_RELOC_ARM64_BRANCH26) return -1;
            if (mino_jit_patch_branch26(insn_p, insn_addr, target) != 0) return -1;
            break;
        }
        case SYM_SLOT_IMM: {
            uintptr_t target = pool_base
                + slots[sym].slot_offset * sizeof(uint64_t);
            switch (kind) {
            case MINO_STENCIL_RELOC_ARM64_PAGE21:
            case MINO_STENCIL_RELOC_ARM64_GOT_LOAD_PAGE21:
                mino_jit_patch_adrp(insn_p, insn_addr, target);
                break;
            case MINO_STENCIL_RELOC_ARM64_PAGEOFF12:
            case MINO_STENCIL_RELOC_ARM64_GOT_LOAD_PAGEOFF12:
                mino_jit_patch_pageoff12_ldr64(insn_p, target);
                break;
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
    size_t      native_start;  /* byte offset within the code region */
    size_t      bc_pc;         /* bytecode pc the instance materialises */
    inst_kind_t kind;
} inst_t;

int mino_jit_compile_inner(mino_state_t *S, mino_val_t *fn_val)
{
    mino_bc_fn_t *bc = fn_val->as.fn.bc;
    /* Caller (mino_jit_compile) has already validated fn_val / bc and
     * confirmed eligibility, so the head guards from the public entry
     * point are not repeated here. */

    /* Fusion fires only on branch-free bodies: a fused LOAD_K + RETURN
     * is atomic, so any direct-emit branch landing on the RETURN-half
     * would re-execute the LOAD_K. Disabling the optimisation when
     * branches are present keeps the case impossible by construction
     * without a pre-scan of every branch's target. */
    int allow_fusion = !bc_has_branches(bc);

    /* First pass: classify every symbol of every stencil to size the
     * code / trampoline / pool regions. The pass also validates that
     * every extern fn symbol resolves to a known host helper, so the
     * mmap below has zero chance of failing partway through. */
    size_t code_size   = 0;
    size_t pool_slots  = 0;
    size_t tramp_count = 0;
    for (size_t pc = 0; pc < bc->code_len; pc++) {
        unsigned op = OP_OF(bc->code[pc]);
        if (op == OP_JMP) {
            code_size += MINO_JIT_JMP_SIZE;
            continue;
        }
        if (op == OP_JMPIFNOT) {
            code_size += MINO_JIT_JMPIFNOT_SIZE;
            continue;
        }
        int fused;
        const stencil_desc_t *st = pick_stencil(bc, pc, &fused, allow_fusion);
        if (st == NULL) return -1;
        code_size += st->size;
        for (unsigned long s = 0; s < st->nsymbols; s++) {
            const char *name = st->symbols[s];
            int k = mino_jit_imm_kind_from_name(name);
            if (k >= 0) {
                pool_slots++;
            } else if (strcmp(name, MINO_JIT_LOOP_MARKER_NAME) == 0) {
                /* The back-jump marker resolves to the stencil's own
                 * self_start at emit time -- no pool / trampoline
                 * slot needed. */
            } else {
                if (mino_jit_lookup_extern_fn(name) == NULL) return -1;
                tramp_count++;
            }
        }
        if (fused) pc++;
        pc += (size_t)mino_jit_op_extra_words(op);
    }

    /* Layout: [code] [trampolines, 16-byte aligned] [pool, 8-byte
     * aligned]. Trampolines live in the same RX mmap as the code
     * (they're branched into by the in-stencil bl instructions) and
     * each carries its own 8-byte target literal. The pool follows
     * trampolines and holds the per-instruction immediate values
     * loaded via GOT-style adrp+ldr pairs. */
    size_t tramp_offset    = (code_size + 15u) & ~(size_t)15u;
    size_t tramp_bytes     = tramp_count * MINO_JIT_TRAMPOLINE_SIZE;
    size_t pool_offset_raw = tramp_offset + tramp_bytes;
    size_t pool_offset     = (pool_offset_raw + 7u) & ~(size_t)7u;
    size_t pool_bytes      = pool_slots * sizeof(uint64_t);
    size_t need_bytes      = pool_offset + pool_bytes;
    if (need_bytes == 0) need_bytes = 1;  /* mmap with size 0 is an error */

    long page_l = sysconf(_SC_PAGESIZE);
    if (page_l <= 0) return -1;
    size_t page       = (size_t)page_l;
    size_t total_size = (need_bytes + page - 1) & ~(page - 1);

    void *region = mmap(NULL, total_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON, -1, 0);
    if (region == MAP_FAILED) return -1;

    /* Per-pc → native offset side table. Sized to code_len; written
     * during the layout walk below. Held in a malloc'd buffer because
     * the JIT module is the only writer and the GC's value-pointer
     * scan would otherwise treat the integer slots as live edges. */
    unsigned *pc_offsets = (unsigned *)malloc(bc->code_len * sizeof(unsigned));
    if (pc_offsets == NULL) {
        munmap(region, total_size);
        return -1;
    }

    /* Per-instance tracking. Sized to code_len -- the upper bound;
     * fusion strictly reduces the count and direct-emit branches
     * each take one entry. */
    inst_t *insts = (inst_t *)malloc(bc->code_len * sizeof(*insts));
    if (insts == NULL) {
        free(pc_offsets);
        munmap(region, total_size);
        return -1;
    }
    size_t n_inst = 0;

    unsigned char *code      = (unsigned char *)region;
    unsigned char *tramp_buf = code + tramp_offset;
    uint64_t      *pool      = (uint64_t *)(code + pool_offset);
    uintptr_t      code_base  = (uintptr_t)code;
    uintptr_t      tramp_base = (uintptr_t)tramp_buf;
    uintptr_t      pool_base  = (uintptr_t)pool;

    size_t pos       = 0;
    size_t pool_pos  = 0;
    size_t tramp_pos = 0;
    for (size_t pc = 0; pc < bc->code_len; pc++) {
        pc_offsets[pc] = (unsigned)pos;
        unsigned op = OP_OF(bc->code[pc]);
        if (op == OP_JMP) {
            mino_jit_emit_jmp_bytes(code, pos);
            insts[n_inst].native_start = pos;
            insts[n_inst].bc_pc        = pc;
            insts[n_inst].kind         = INST_JMP;
            n_inst++;
            pos += MINO_JIT_JMP_SIZE;
            continue;
        }
        if (op == OP_JMPIFNOT) {
            mino_jit_emit_jmpifnot_bytes(code, pos, bc->code[pc]);
            insts[n_inst].native_start = pos;
            insts[n_inst].bc_pc        = pc;
            insts[n_inst].kind         = INST_JMPIFNOT;
            n_inst++;
            pos += MINO_JIT_JMPIFNOT_SIZE;
            continue;
        }
        int fused;
        const stencil_desc_t *st = pick_stencil(bc, pc, &fused, allow_fusion);
        size_t new_pool_pos  = pool_pos;
        size_t new_tramp_pos = tramp_pos;
        /* insn2 carries the trailing word for two-word ops. Single-
         * word ops never read insn2 -- their stencil descriptors
         * declare no IMM_KIND_BX2 symbols -- so the value passed when
         * pc+1 is out of range is irrelevant. */
        mino_bc_insn_t insn2 = 0;
        if (mino_jit_op_extra_words(op) > 0 && pc + 1 < bc->code_len) {
            insn2 = bc->code[pc + 1];
        }
        long n = emit_stencil(code, pos, code_size,
                              pool, pool_pos, pool_slots, &new_pool_pos,
                              tramp_buf, tramp_pos, tramp_bytes, &new_tramp_pos,
                              st, bc->code[pc], insn2, bc,
                              code_base, pool_base, tramp_base);
        if (n < 0) {
            free(insts);
            free(pc_offsets);
            munmap(region, total_size);
            return -1;
        }
        insts[n_inst].native_start = pos;
        insts[n_inst].bc_pc        = pc;
        insts[n_inst].kind         = st->is_final ? INST_STENCIL_FINAL
                                                  : INST_STENCIL_NONFINAL;
        n_inst++;
        pos       += (size_t)n;
        pool_pos   = new_pool_pos;
        tramp_pos  = new_tramp_pos;
        if (fused) {
            /* Both pcs map to the start of the fused chunk; the fused
             * stencil is atomic, so deopt mid-fusion is not a
             * representable state. */
            pc_offsets[pc + 1] = pc_offsets[pc];
            pc++;
        }
        if (mino_jit_op_extra_words(op) > 0) {
            /* Two-word op: the slot-bearing word shares the stencil's
             * native span with its primary word. */
            pc_offsets[pc + 1] = pc_offsets[pc];
            pc++;
        }
    }

    /* Pass A: rewrite every `ret` inside each non-final stencil's
     * span into a `b <next_inst_start>` chain branch. Stencils whose
     * fast path inlines a check without calling a helper (e.g., the
     * inline OP_GETGLOBAL_CACHED hit path) end up with two basic
     * blocks: the lean fast-path exit (no prologue) and the cold
     * slow-path exit (with prologue). clang emits a `ret` at the
     * end of each. Patching only the first would leave the
     * remaining ret(s) as real returns that would short-circuit
     * out of the JIT region. We walk the span and rewrite each
     * encountered `ret` so every exit path chains to the next
     * stencil. Direct-emit instances skip this pass -- they have no
     * ret to patch. */
    for (size_t i = 0; i + 1 < n_inst; i++) {
        if (insts[i].kind != INST_STENCIL_NONFINAL) continue;
        size_t    span_size = insts[i + 1].native_start - insts[i].native_start;
        uintptr_t next_addr = code_base + insts[i + 1].native_start;
        int       any       = 0;
        for (size_t off = 0; off + 4 <= span_size; off += 4) {
            uint32_t insn;
            memcpy(&insn, code + insts[i].native_start + off, 4);
            if (insn != 0xd65f03c0u) continue;
            uintptr_t ret_addr =
                code_base + insts[i].native_start + off;
            uint32_t *insn_p   =
                (uint32_t *)(code + insts[i].native_start + off);
            if (mino_jit_patch_b_unconditional(insn_p, ret_addr, next_addr) != 0) {
                free(insts);
                free(pc_offsets);
                munmap(region, total_size);
                return -1;
            }
            any = 1;
        }
        if (!any) {
            free(insts);
            free(pc_offsets);
            munmap(region, total_size);
            return -1;
        }
    }

    /* Pass B: target patching for direct-emit branches. The bytecode
     * dispatcher reads `code[pc++]` before adding the offset, so the
     * target_pc is pc + 1 + sBx. Both JMP and JMPIFNOT instances
     * resolve to a native address via pc_offsets. */
    for (size_t i = 0; i < n_inst; i++) {
        if (insts[i].kind != INST_JMP && insts[i].kind != INST_JMPIFNOT) {
            continue;
        }
        size_t      bc_pc      = insts[i].bc_pc;
        mino_bc_insn_t ins     = bc->code[bc_pc];
        long        target_pc  = (long)bc_pc + 1 + sBx_OF(ins);
        if (target_pc < 0 || (size_t)target_pc >= bc->code_len) {
            free(insts);
            free(pc_offsets);
            munmap(region, total_size);
            return -1;
        }
        uintptr_t target_addr = code_base + pc_offsets[target_pc];
        if (insts[i].kind == INST_JMP) {
            uintptr_t insn_addr = code_base + insts[i].native_start;
            uint32_t *insn_p    = (uint32_t *)(code + insts[i].native_start);
            if (mino_jit_patch_b_unconditional(insn_p, insn_addr, target_addr) != 0) {
                free(insts);
                free(pc_offsets);
                munmap(region, total_size);
                return -1;
            }
        } else { /* INST_JMPIFNOT */
            /* Two conditional branches share the same target. The CBZ
             * at offset +4 catches NULL; the B.LS at offset +16
             * catches the (v - 2) <= 1 nil / false range. */
            unsigned char *base_p = code + insts[i].native_start;
            uintptr_t      base_a = code_base + insts[i].native_start;
            uint32_t *cbz_p = (uint32_t *)(base_p + 4);
            uint32_t *bls_p = (uint32_t *)(base_p + 16);
            if (mino_jit_patch_imm19(cbz_p, base_a + 4,  target_addr) != 0
             || mino_jit_patch_imm19(bls_p, base_a + 16, target_addr) != 0) {
                free(insts);
                free(pc_offsets);
                munmap(region, total_size);
                return -1;
            }
        }
    }
    free(insts);

    /* Flush the I-cache so the CPU sees the freshly written
     * instructions. On ARM64 the data and instruction caches are
     * coherent only after explicit maintenance; clang's builtin
     * issues `dc cvau` + `dsb ish` + `ic ivau` + `dsb ish` + `isb`
     * the way the architecture requires. */
    __builtin___clear_cache((char *)region, (char *)region + total_size);

    /* Switch the whole region to RX. The literal pool is read-only at
     * this point too -- the patcher already populated it. */
    if (mprotect(region, total_size, PROT_READ | PROT_EXEC) != 0) {
        munmap(region, total_size);
        return -1;
    }

    if (region_track(S, region, total_size, pc_offsets) != 0) {
        free(pc_offsets);
        munmap(region, total_size);
        return -1;
    }

    bc->native            = region;
    bc->native_size       = total_size;
    bc->native_gen        = S->ic_gen;
    /* The offsets table is owned by the jit_regions node so a prior
     * compile's table is still reachable through the list. Pointing
     * bc here to the fresh table is the visible-to-runtime atomic. */
    bc->native_pc_offsets = pc_offsets;
    /* Optional diagnostic: emit a stderr line on each compile when
     * `MINO_CPJIT_TRACE` is set in the environment. The check fires
     * once per compile, off the hot path. */
    {
        const char *trace = getenv("MINO_CPJIT_TRACE");
        if (trace != NULL && trace[0] != '\0' && trace[0] != '0') {
            fprintf(stderr,
                    "[cpjit] compiled bc (code_len=%zu, region=%p, "
                    "total_size=%zu, code_used=%zu, tramp_used=%zu, "
                    "pool_used=%zu)\n",
                    bc->code_len, region, total_size, pos,
                    tramp_pos, pool_pos);
            if (trace[0] == '2') {
                for (size_t i = 0; i < pos; i += 4) {
                    uint32_t insn;
                    memcpy(&insn, (unsigned char *)region + i, 4);
                    fprintf(stderr, "  %04zx: %08x\n", i, insn);
                }
            }
        }
    }
    return 0;
}

#endif /* MINO_CPJIT_HOST */
