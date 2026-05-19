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

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Host RWX region API. POSIX uses mmap / mprotect / munmap; Windows
 * uses VirtualAlloc / VirtualProtect / VirtualFree. The wrappers
 * below give the rest of this file a uniform allocate/protect/free
 * surface and a uniform `MAP_FAILED` sentinel so the size-pass +
 * commit-pass logic doesn't fork on host. */
#ifdef _WIN32
#include <windows.h>
static void *jit_region_alloc(size_t size)
{
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE,
                        PAGE_READWRITE);
}
static int jit_region_make_rx(void *p, size_t size)
{
    DWORD old;
    return VirtualProtect(p, size, PAGE_EXECUTE_READ, &old) ? 0 : -1;
}
static void jit_region_free(void *p, size_t size)
{
    (void)size;  /* MEM_RELEASE expects size = 0 paired with original ptr */
    VirtualFree(p, 0, MEM_RELEASE);
}
static long jit_region_page_size(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (long)si.dwPageSize;
}
#define MINO_JIT_REGION_ALLOC_FAILED  NULL
#else
#include <sys/mman.h>
#include <unistd.h>
static void *jit_region_alloc(size_t size)
{
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
static int jit_region_make_rx(void *p, size_t size)
{
    return mprotect(p, size, PROT_READ | PROT_EXEC);
}
static void jit_region_free(void *p, size_t size)
{
    munmap(p, size);
}
static long jit_region_page_size(void)
{
    return sysconf(_SC_PAGESIZE);
}
#define MINO_JIT_REGION_ALLOC_FAILED  NULL
#endif

/* ----- region book-keeping ------------------------------------------------ */

static int region_track(mino_state_t *S, void *ptr, size_t size, void *aux_ptr)
{
    struct mino_jit_region *node =
        (struct mino_jit_region *)malloc(sizeof(*node));
    if (node == NULL) return -1;
    node->ptr     = ptr;
    node->size    = size;
    node->aux_ptr = aux_ptr;
    node->next    = S->jit.jit_regions;
    S->jit.jit_regions = node;
    return 0;
}

/* ----- slab pool ----------------------------------------------------------- */

/* Cutoff for slab-pool eligibility. Fns whose pre-page-rounding
 * `need_bytes` fits under this size go through the slab pool; larger
 * fns keep the one-page-per-fn allocator. Sized just below the
 * smallest host page (Linux x86_64 = 4 KB) so the same cutoff is
 * meaningful across all hosts; on macOS arm64 with a 16 KB page,
 * multiple slots fit per slab. */
#define MINO_JIT_SLAB_CUTOFF      ((size_t)4096)

/* Per-slot alignment. Stencil-emitted code is read by the host CPU;
 * keeping slots 16-byte-aligned matches the alignment trampoline /
 * pool layout already assumes inside a single fn. */
#define MINO_JIT_SLAB_SLOT_ALIGN  ((size_t)16)

static struct mino_jit_slab *jit_slab_alloc_new(mino_state_t *S, size_t need)
{
    struct mino_jit_slab *slab;
    long                  page_l;
    size_t                page;
    void                 *p;
    page_l = jit_region_page_size();
    if (page_l <= 0) return NULL;
    page = (size_t)page_l;
    /* For requests larger than one host page, span enough pages. */
    if (need > page) {
        page = (need + page - 1) & ~(page - 1);
    }
    p = jit_region_alloc(page);
    if (p == MINO_JIT_REGION_ALLOC_FAILED) return NULL;
    slab = (struct mino_jit_slab *)malloc(sizeof(*slab));
    if (slab == NULL) {
        jit_region_free(p, page);
        return NULL;
    }
    slab->page        = p;
    slab->page_size   = page;
    slab->bump_offset = 0;
    slab->live_slots  = 0;
    slab->next        = S->jit_slabs;
    S->jit_slabs      = slab;
    return slab;
}

/* Find a slab with room for `need` aligned bytes; allocate a new
 * slab when no fit. Returns the slab whose `page` + current
 * `bump_offset` is the slot start. Caller is responsible for the
 * RW/RX cycle around the fill. */
static struct mino_jit_slab *jit_slab_acquire(mino_state_t *S, size_t need)
{
    struct mino_jit_slab *slab;
    size_t                aligned;
    aligned = (need + MINO_JIT_SLAB_SLOT_ALIGN - 1)
              & ~(MINO_JIT_SLAB_SLOT_ALIGN - 1);
    for (slab = S->jit_slabs; slab != NULL; slab = slab->next) {
        if (slab->bump_offset + aligned <= slab->page_size) {
            return slab;
        }
    }
    return jit_slab_alloc_new(S, aligned);
}

static int jit_slab_make_rw(struct mino_jit_slab *slab)
{
#ifdef _WIN32
    DWORD old;
    return VirtualProtect(slab->page, slab->page_size,
                          PAGE_READWRITE, &old) ? 0 : -1;
#else
    return mprotect(slab->page, slab->page_size,
                    PROT_READ | PROT_WRITE);
#endif
}

static int jit_slab_make_rx(struct mino_jit_slab *slab)
{
#ifdef _WIN32
    DWORD old;
    return VirtualProtect(slab->page, slab->page_size,
                          PAGE_EXECUTE_READ, &old) ? 0 : -1;
#else
    return mprotect(slab->page, slab->page_size,
                    PROT_READ | PROT_EXEC);
#endif
}

/* Compile failure cleanup: release the JIT memory acquired for the
 * compile. Slab path: re-seal the page to RX (the bump cursor stays
 * unchanged, so the just-attempted slot bytes are reusable by the
 * next compile). Legacy path: munmap the fn's dedicated page. */
static void jit_compile_cleanup(struct mino_jit_slab *slab, void *region,
                                size_t total_size)
{
    if (slab != NULL) {
        (void)jit_slab_make_rx(slab);
    } else {
        jit_region_free(region, total_size);
    }
}

/* Per-fn slot release. Called from mino_jit_invalidate when a bc
 * record gives up its native slot (deopt, IC-gen mismatch, redef).
 * Decrements the owning slab's live_slots refcount; on the last
 * release, unlinks the slab from S->jit_slabs and munmaps the page.
 * The bump cursor inside the slab is never rewound -- slots are
 * append-only within a slab, and reclamation happens at slab
 * granularity, not slot granularity. */
void mino_jit_slab_release(mino_state_t *S, struct mino_jit_slab *slab)
{
    struct mino_jit_slab **pp;
    if (slab == NULL) return;
    if (slab->live_slots > 0) slab->live_slots--;
    if (slab->live_slots != 0) return;
    for (pp = &S->jit_slabs; *pp != NULL; pp = &(*pp)->next) {
        if (*pp == slab) {
            *pp = slab->next;
            break;
        }
    }
    if (slab->page != NULL) jit_region_free(slab->page, slab->page_size);
    free(slab);
}

void mino_jit_free_all(mino_state_t *S)
{
    struct mino_jit_region *node = S->jit.jit_regions;
    while (node != NULL) {
        struct mino_jit_region *next = node->next;
        if (node->aux_ptr != NULL) free(node->aux_ptr);
        if (node->ptr     != NULL) jit_region_free(node->ptr, node->size);
        free(node);
        node = next;
    }
    S->jit.jit_regions = NULL;
    {
        struct mino_jit_slab *slab = S->jit_slabs;
        while (slab != NULL) {
            struct mino_jit_slab *next = slab->next;
            if (slab->page != NULL) jit_region_free(slab->page, slab->page_size);
            free(slab);
            slab = next;
        }
        S->jit_slabs = NULL;
    }
}

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
        } else if (strcmp(name, MINO_JIT_CHAIN_MARKER_NAME) == 0) {
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
    for (unsigned long r = 0; r < st->nrelocs; r++) {
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

int mino_jit_compile_inner(mino_state_t *S, mino_val_t *fn_val,
                            size_t deopt_at_pc)
{
    mino_bc_fn_t *bc = fn_val->as.fn.bc;
    /* Caller (mino_jit_compile) has already validated fn_val / bc and
     * confirmed eligibility, so the head guards from the public entry
     * point are not repeated here. */

    /* effective_code_len caps the compile walk at deopt_at_pc when
     * the caller asked for a compile-with-deopt; the deopt stencil is
     * emitted right after the prefix loop below. (size_t)-1 marks a
     * plain full-body compile. */
    int    is_deopt_compile  = (deopt_at_pc != (size_t)-1);
    size_t effective_code_len = is_deopt_compile ? deopt_at_pc
                                                  : bc->code_len;

    /* Fusion fires only on branch-free bodies: a fused LOAD_K + RETURN
     * is atomic, so any direct-emit branch landing on the RETURN-half
     * would re-execute the LOAD_K. Disabling the optimisation when
     * branches are present keeps the case impossible by construction
     * without a pre-scan of every branch's target. */
    int allow_fusion = !bc_has_branches(bc);

    /* Locate the deopt stencil descriptor once; the size + symbols are
     * added to the budget below when is_deopt_compile is set. */
    const stencil_desc_t *deopt_st = is_deopt_compile
        ? mino_jit_find_stencil(OP_DEOPT_TO_INTERP) : NULL;
    if (is_deopt_compile && deopt_st == NULL) return -1;

    /* First pass: classify every symbol of every stencil to size the
     * code / trampoline / pool regions. The pass also validates that
     * every extern fn symbol resolves to a known host helper, so the
     * mmap below has zero chance of failing partway through. */
    size_t code_size   = 0;
    size_t pool_slots  = 0;
    size_t tramp_count = 0;
    for (size_t pc = 0; pc < effective_code_len; pc++) {
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
            } else if (strcmp(name, MINO_JIT_CHAIN_MARKER_NAME) == 0) {
                /* The chain-continue marker is patched by the
                 * post-emit chain pass; like the loop marker, no
                 * pool / trampoline slot is needed. */
            } else {
                if (mino_jit_lookup_extern_fn(name) == NULL) return -1;
                tramp_count++;
            }
        }
        if (fused) pc++;
        pc += (size_t)mino_jit_op_extra_words(op);
    }
    /* Account for the deopt stencil (final): same pool / tramp
     * bookkeeping as a regular stencil. The deopt stencil reads
     * IMM_BX (one pool slot) and calls mino_jit_deopt_exit (one
     * trampoline). The marker counts are derived from its symbol
     * table so a future stencil refactor stays consistent. */
    if (is_deopt_compile) {
        code_size += deopt_st->size;
        for (unsigned long s = 0; s < deopt_st->nsymbols; s++) {
            const char *name = deopt_st->symbols[s];
            int k = mino_jit_imm_kind_from_name(name);
            if (k >= 0) {
                pool_slots++;
            } else if (strcmp(name, MINO_JIT_LOOP_MARKER_NAME) == 0
                       || strcmp(name, MINO_JIT_CHAIN_MARKER_NAME) == 0) {
                /* Marker symbols don't get pool / tramp slots. */
            } else {
                if (mino_jit_lookup_extern_fn(name) == NULL) return -1;
                tramp_count++;
            }
        }
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

    long page_l = jit_region_page_size();
    if (page_l <= 0) return -1;
    size_t page       = (size_t)page_l;
    size_t total_size = (need_bytes + page - 1) & ~(page - 1);

    /* Allocation: small fns (need_bytes <= MINO_JIT_SLAB_CUTOFF) come
     * out of the slab pool; the slab is flipped RW for the duration
     * of this fill and back to RX after the I-cache flush. Larger
     * fns take the one-page-per-fn mmap path so a single oversized
     * compile cannot lock a slab into RW for an extended window. */
    struct mino_jit_slab *slab        = NULL;
    size_t                slot_offset = 0;
    size_t                slot_size   = 0;
    void                 *region;
    if (need_bytes <= MINO_JIT_SLAB_CUTOFF) {
        slab = jit_slab_acquire(S, need_bytes);
        if (slab == NULL) return -1;
        if (jit_slab_make_rw(slab) != 0) return -1;
        slot_offset = slab->bump_offset;
        slot_size   = (need_bytes + MINO_JIT_SLAB_SLOT_ALIGN - 1)
                      & ~(MINO_JIT_SLAB_SLOT_ALIGN - 1);
        region      = (unsigned char *)slab->page + slot_offset;
    } else {
        region = jit_region_alloc(total_size);
        if (region == MINO_JIT_REGION_ALLOC_FAILED) return -1;
    }

    /* Per-pc → native offset side table. Sized to code_len; written
     * during the layout walk below. Held in a malloc'd buffer because
     * the JIT module is the only writer and the GC's value-pointer
     * scan would otherwise treat the integer slots as live edges. */
    unsigned *pc_offsets = (unsigned *)malloc(bc->code_len * sizeof(unsigned));
    if (pc_offsets == NULL) {
        if (slab != NULL) {
            (void)jit_slab_make_rx(slab);
        } else {
            jit_compile_cleanup(slab, region, total_size);
        }
        return -1;
    }

    /* Per-instance tracking. Sized to code_len -- the upper bound;
     * fusion strictly reduces the count and direct-emit branches
     * each take one entry. */
    inst_t *insts = (inst_t *)malloc(bc->code_len * sizeof(*insts));
    if (insts == NULL) {
        free(pc_offsets);
        jit_compile_cleanup(slab, region, total_size);
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
    for (size_t pc = 0; pc < effective_code_len; pc++) {
        pc_offsets[pc] = (unsigned)pos;
        unsigned op = OP_OF(bc->code[pc]);
        if (op == OP_JMP) {
            mino_jit_emit_jmp_bytes(code, pos);
            insts[n_inst].native_start = pos;
            insts[n_inst].bc_pc        = pc;
            insts[n_inst].kind         = INST_JMP;
            insts[n_inst].st           = NULL;
            n_inst++;
            pos += MINO_JIT_JMP_SIZE;
            continue;
        }
        if (op == OP_JMPIFNOT) {
            mino_jit_emit_jmpifnot_bytes(code, pos, bc->code[pc]);
            insts[n_inst].native_start = pos;
            insts[n_inst].bc_pc        = pc;
            insts[n_inst].kind         = INST_JMPIFNOT;
            insts[n_inst].st           = NULL;
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
            jit_compile_cleanup(slab, region, total_size);
            return -1;
        }
        insts[n_inst].native_start = pos;
        insts[n_inst].bc_pc        = pc;
        insts[n_inst].kind         = st->is_final ? INST_STENCIL_FINAL
                                                  : INST_STENCIL_NONFINAL;
        insts[n_inst].st           = st;
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

    /* Emit the deopt stencil at deopt_at_pc when compiling a prefix.
     * The synthesized insn carries the resume PC in its Bx field; the
     * stencil's IMM_BX read pulls it out and passes it to the runtime
     * helper that flags the side-exit. Pass A (chain patch) walks the
     * prefix's last non-final stencil and patches its chain-marker
     * relocation to land here automatically. */
    if (is_deopt_compile) {
        pc_offsets[deopt_at_pc] = (unsigned)pos;
        /* Synthesize (OP_DEOPT_TO_INTERP, Bx=deopt_at_pc). The Bx
         * field is 16 bits; the caller (mino_jit_compile) gated
         * eligibility on deopt_at_pc fitting. */
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
        if (n < 0) {
            free(insts);
            free(pc_offsets);
            jit_compile_cleanup(slab, region, total_size);
            return -1;
        }
        insts[n_inst].native_start = pos;
        insts[n_inst].bc_pc        = deopt_at_pc;
        insts[n_inst].kind         = INST_STENCIL_FINAL;
        insts[n_inst].st           = deopt_st;
        n_inst++;
        pos       += (size_t)n;
        pool_pos   = new_pool_pos;
        tramp_pos  = new_tramp_pos;
        /* Initialise pc_offsets for pcs past deopt_at_pc to the same
         * deopt-stencil offset so mino_jit_offset_to_pc lookups on
         * uncompiled tails still resolve to something defined; no
         * code emit reads these slots, but a stack-trace introspection
         * on an out-of-range native_off shouldn't read uninitialized
         * memory. */
        for (size_t pc = deopt_at_pc + 1; pc < bc->code_len; pc++) {
            pc_offsets[pc] = (unsigned)pos;
        }
    }

    /* Pass A: patch each non-final stencil's chain-continue tail
     * call(s) to branch into the next instance's native_start.
     *
     * Every non-final stencil source ends each of its return paths
     * with `__attribute__((musttail)) return
     * mino_jit_chain_continue_marker(regs, consts, S)`. clang
     * lowers the musttail into a single branch instruction
     * (BRANCH26 on ARM64) whose target field carries a relocation
     * against the marker symbol; the extractor records one such
     * relocation per call site. This pass walks each non-final
     * stencil's reloc table, looks up the symbol name for each
     * reloc, and -- for every chain-marker relocation -- patches
     * the branch offset to point at the next instance's
     * native_start.
     *
     * Stencils whose fast path inlines a check before deciding
     * which slow helper to call produce multiple basic blocks
     * (e.g., the OP_LOOP_INT_LT slow-path exit vs. its fall-
     * through). Each block ends with its own musttail call, so
     * each emits its own chain-marker relocation; the pass patches
     * them all. Direct-emit instances (INST_JMP / INST_JMPIFNOT)
     * skip this pass -- their target patching is Pass B's job. */
    for (size_t i = 0; i + 1 < n_inst; i++) {
        if (insts[i].kind != INST_STENCIL_NONFINAL) continue;
        const stencil_desc_t *st = insts[i].st;
        uintptr_t next_addr      = code_base + insts[i + 1].native_start;
        int any                  = 0;
        for (unsigned long r = 0; r < st->nrelocs; r++) {
            unsigned int off  = st->relocs[r][0];
            unsigned int kind = st->relocs[r][1];
            unsigned int sym  = st->relocs[r][2];
            if (sym >= st->nsymbols) {
                free(insts);
                free(pc_offsets);
                jit_compile_cleanup(slab, region, total_size);
                return -1;
            }
            if (strcmp(st->symbols[sym], MINO_JIT_CHAIN_MARKER_NAME) != 0) {
                continue;
            }
            uintptr_t      insn_addr = code_base + insts[i].native_start + off;
            unsigned char *insn_p    = code + insts[i].native_start + off;
#if defined(MINO_CPJIT_HOST_ARM64)
            if (kind != MINO_STENCIL_RELOC_ARM64_BRANCH26) {
                free(insts);
                free(pc_offsets);
                jit_compile_cleanup(slab, region, total_size);
                return -1;
            }
            if (mino_jit_patch_b_unconditional((uint32_t *)insn_p, insn_addr,
                                                next_addr) != 0) {
                free(insts);
                free(pc_offsets);
                jit_compile_cleanup(slab, region, total_size);
                return -1;
            }
#elif defined(MINO_CPJIT_HOST_X86_64)
            if (kind != MINO_STENCIL_RELOC_X86_64_PC32) {
                free(insts);
                free(pc_offsets);
                jit_compile_cleanup(slab, region, total_size);
                return -1;
            }
            /* The musttail call landed as `e9 <rel32>` (5 bytes).
             * The reloc offset points at the rel32 field; back up
             * one byte to land on the opcode so patch_jmp32_to can
             * rewrite the rel32 in place. */
            if (mino_jit_patch_jmp32_to(insn_p - 1,
                                          insn_addr - 1,
                                          next_addr) != 0) {
                free(insts);
                free(pc_offsets);
                jit_compile_cleanup(slab, region, total_size);
                return -1;
            }
#endif
            any = 1;
        }
        if (!any) {
            free(insts);
            free(pc_offsets);
            jit_compile_cleanup(slab, region, total_size);
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
        /* Branches must land inside the compiled prefix. For a plain
         * compile that's bc->code_len; for a compile-with-deopt the
         * caller's prefix_has_escaping_branch already validated this,
         * but the cap also stops mis-compiles cold on the unlikely
         * path where the input bc is inconsistent. */
        if (target_pc < 0 || (size_t)target_pc >= effective_code_len) {
            free(insts);
            free(pc_offsets);
            jit_compile_cleanup(slab, region, total_size);
            return -1;
        }
        uintptr_t target_addr = code_base + pc_offsets[target_pc];
        if (insts[i].kind == INST_JMP) {
            uintptr_t      insn_addr = code_base + insts[i].native_start;
            unsigned char *insn_p    = code + insts[i].native_start;
#if defined(MINO_CPJIT_HOST_ARM64)
            if (mino_jit_patch_b_unconditional((uint32_t *)insn_p,
                                                insn_addr, target_addr) != 0) {
                free(insts);
                free(pc_offsets);
                jit_compile_cleanup(slab, region, total_size);
                return -1;
            }
#elif defined(MINO_CPJIT_HOST_X86_64)
            if (mino_jit_patch_jmp32_to(insn_p, insn_addr,
                                         target_addr) != 0) {
                free(insts);
                free(pc_offsets);
                jit_compile_cleanup(slab, region, total_size);
                return -1;
            }
#endif
        } else { /* INST_JMPIFNOT */
            unsigned char *base_p = code + insts[i].native_start;
            uintptr_t      base_a = code_base + insts[i].native_start;
#if defined(MINO_CPJIT_HOST_ARM64)
            /* Two conditional branches share the same target. The CBZ
             * at offset +4 catches NULL; the B.LS at offset +16
             * catches the (v - 2) <= 1 nil / false range. */
            uint32_t *cbz_p = (uint32_t *)(base_p + 4);
            uint32_t *bls_p = (uint32_t *)(base_p + 16);
            if (mino_jit_patch_imm19(cbz_p, base_a + 4,  target_addr) != 0
             || mino_jit_patch_imm19(bls_p, base_a + 16, target_addr) != 0) {
                free(insts);
                free(pc_offsets);
                jit_compile_cleanup(slab, region, total_size);
                return -1;
            }
#elif defined(MINO_CPJIT_HOST_X86_64)
            /* Two conditional branches share the same target. The JE
             * at offset +10 catches NULL; the JBE at offset +24
             * catches the (v - 2) <= 1 nil / false range. Both use
             * a 6-byte `0f 8X <rel32>` encoding. */
            if (mino_jit_patch_jcc32_to(base_p + 10, base_a + 10,
                                         target_addr) != 0
             || mino_jit_patch_jcc32_to(base_p + 24, base_a + 24,
                                         target_addr) != 0) {
                free(insts);
                free(pc_offsets);
                jit_compile_cleanup(slab, region, total_size);
                return -1;
            }
#endif
        }
    }
    free(insts);

    /* Flush the I-cache so the CPU sees the freshly written
     * instructions. On ARM64 the data and instruction caches are
     * coherent only after explicit maintenance; clang's builtin
     * issues `dc cvau` + `dsb ish` + `ic ivau` + `dsb ish` + `isb`
     * the way the architecture requires. The range is the slot for
     * slab fns (so a slab past slot 0 doesn't clear past its page)
     * and the whole region for mmap'd fns. */
    {
        size_t flush_bytes = (slab != NULL) ? slot_size : total_size;
        __builtin___clear_cache((char *)region,
                                (char *)region + flush_bytes);
    }

    /* Re-seal the JIT memory. Slab path: mprotect the whole slab page
     * back to RX (one syscall covers every slot in the page, so the
     * cost amortises across compiles). Mmap path: mprotect the fn's
     * dedicated page to RX. The literal pool is read-only at this
     * point too -- the patcher already populated it. */
    if (slab != NULL) {
        if (jit_slab_make_rx(slab) != 0) {
            free(pc_offsets);
            return -1;
        }
    } else {
        if (jit_region_make_rx(region, total_size) != 0) {
            jit_compile_cleanup(slab, region, total_size);
            return -1;
        }
    }

    /* Mmap path: track the region for state-teardown munmap
     * (region_track also takes ownership of pc_offsets via aux_ptr).
     * Slab path: track only pc_offsets so state teardown frees it; the
     * slab page itself is on S->jit_slabs. */
    if (slab == NULL) {
        if (region_track(S, region, total_size, pc_offsets) != 0) {
            free(pc_offsets);
            jit_compile_cleanup(slab, region, total_size);
            return -1;
        }
    } else {
        if (region_track(S, NULL, 0, pc_offsets) != 0) {
            free(pc_offsets);
            return -1;
        }
    }

    if (slab != NULL) {
        slab->bump_offset = slot_offset + slot_size;
        slab->live_slots++;
    }
    bc->native            = region;
    bc->native_size       = (slab != NULL) ? slot_size : total_size;
    bc->native_gen        = S->ns_vars.ic_gen;
    bc->native_slab       = slab;
    /* The offsets table is owned by the jit_regions node (mmap path)
     * or the bc record itself (slab path; freed by mino_jit_invalidate).
     * Pointing bc here to the fresh table is the visible-to-runtime
     * atomic. */
    bc->native_pc_offsets = pc_offsets;
    /* Instrumentation: record the code-stream size (no tramp / pool /
     * slack) and the dead-byte slack at the end of the region. The
     * slack is the per-slot intra-alignment padding for slab fns and
     * the page-rounding waste for mmap'd fns. */
    {
        size_t used  = pos + tramp_pos + pool_pos;
        size_t total = (slab != NULL) ? slot_size : total_size;
        bc->jit_code_bytes        = (pos > UINT32_MAX) ? UINT32_MAX
                                                       : (uint32_t)pos;
        bc->jit_code_region_dead  = (total > used)
            ? ((total - used > UINT32_MAX) ? UINT32_MAX
                                            : (uint32_t)(total - used))
            : 0u;
    }
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

/* Keep this TU non-empty under -Werror=pedantic when the gate above
 * is false. */
typedef int mino_jit_emit_tu_marker;
