/*
 * src/eval/bc/jit.c -- copy-and-patch JIT compile pipeline.
 *
 * Layout per JIT'd fn:
 *
 *   [code page(s)]   stencils memcpy'd in bytecode order, trailing
 *                    `ret` stripped from every stencil except the final
 *                    OP_RETURN. Adrp+ldr pairs from the GOT-style
 *                    relocations are patched to point at per-instance
 *                    8-byte slots in the literal pool below.
 *   [literal pool]   one 8-byte slot per (instruction, immediate) pair.
 *                    The runtime fills each slot with the materialised
 *                    operand value (A / B / C / Bx / sBx) at compile
 *                    time. The pool sits on a separate page so the
 *                    adrp's 4KB page-relative addressing reaches it
 *                    independent of code-page size.
 *
 * The entry ABI: a native fn pointer takes (regs, consts, S) in x0,
 * x1, x2 and returns the OP_RETURN value in x0. The interpreter's
 * register window is shared between modes -- the runtime sets up
 * regs[] (copy argv into the first n_params slots) before invoking
 * the native code, and the native code writes results back through
 * the same window pointer.
 *
 * Supported in v0.185.0: linear bytecode bodies composed of OP_MOVE,
 * OP_LOAD_K, OP_RETURN. Anything else (branches, calls, arithmetic
 * needing fallback into prim_*, captures, IC slots, multi-arity)
 * marks the fn ineligible and the interpreter keeps running it.
 * Subsequent releases expand the stencil set incrementally.
 *
 * Build flag: only compiled when MINO_CPJIT is defined. The state
 * field `jit_regions` exists unconditionally so the runtime's tear-
 * down path doesn't need to ifdef every site.
 */

#include "jit.h"

#ifdef MINO_CPJIT
/* Host detection: each (arch, os) pair we ship stencils for sets
 * MINO_CPJIT_HOST and the corresponding STENCILS_HEADER_PATH so the
 * full pipeline compiles in. Other combinations get the public API
 * (stub branch below) where every entry returns failure / NULL so
 * the runtime falls through to the interpreter.
 *
 * Today only ARM64 Darwin has a generated header. ARM64 Linux is
 * detected here so jit.c is one source change away from full
 * support once the ELF reloc extractor lands and the platform's
 * generated header is committed. */
#if defined(__aarch64__) && defined(__APPLE__)
#define MINO_CPJIT_HOST 1
#define MINO_CPJIT_STENCILS_HEADER "stencils/generated/stencils_arm64_darwin.h"
#elif defined(__aarch64__) && defined(__linux__) && defined(MINO_CPJIT_ARM64_LINUX)
#define MINO_CPJIT_HOST 1
#define MINO_CPJIT_STENCILS_HEADER "stencils/generated/stencils_arm64_linux.h"
#endif
#endif

#ifdef MINO_CPJIT_HOST

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../../mino.h"
#include "../../mino_internal.h"
#include MINO_CPJIT_STENCILS_HEADER

/* Reloc kind enum mirror -- kept in sync with the values
 * tools/stencil_extract.c writes into <sym>_relocs tables. Header
 * changes there force a change here. */
#define MINO_STENCIL_RELOC_ARM64_PAGE21              0u
#define MINO_STENCIL_RELOC_ARM64_PAGEOFF12           1u
#define MINO_STENCIL_RELOC_ARM64_BRANCH26            2u
#define MINO_STENCIL_RELOC_ABS64                     3u
#define MINO_STENCIL_RELOC_ARM64_GOT_LOAD_PAGE21     4u
#define MINO_STENCIL_RELOC_ARM64_GOT_LOAD_PAGEOFF12  5u

/* Stencil descriptor: per (opcode, variant) tuple. The selection logic
 * looks up an entry by opcode and emits its bytes after stripping the
 * trailing `ret` (for chained stencils) or keeping it (for OP_RETURN
 * which terminates the JIT'd region). */
typedef struct {
    unsigned             opcode;
    const unsigned char *bytes;
    unsigned long        size;
    const char *const   *symbols;
    unsigned long        nsymbols;
    const unsigned int (*relocs)[4];
    unsigned long        nrelocs;
    unsigned long        trim_tail_bytes;
} stencil_desc_t;

/* Symbol-name to immediate-kind lookup. Stencil sources declare extern
 * `MINO_STENCIL_IMM_*` symbols; the runtime resolves each occurrence
 * to a bytecode-operand decode (A / B / C / Bx / sBx). */
typedef enum {
    IMM_KIND_A   = 0,
    IMM_KIND_B   = 1,
    IMM_KIND_C   = 2,
    IMM_KIND_BX  = 3,
    IMM_KIND_SBX = 4
} imm_kind_t;

static int imm_kind_from_name(const char *sym)
{
    if (strcmp(sym, "MINO_STENCIL_IMM_A")   == 0) return IMM_KIND_A;
    if (strcmp(sym, "MINO_STENCIL_IMM_B")   == 0) return IMM_KIND_B;
    if (strcmp(sym, "MINO_STENCIL_IMM_C")   == 0) return IMM_KIND_C;
    if (strcmp(sym, "MINO_STENCIL_IMM_BX")  == 0) return IMM_KIND_BX;
    if (strcmp(sym, "MINO_STENCIL_IMM_SBX") == 0) return IMM_KIND_SBX;
    return -1;
}

static uint64_t imm_value(mino_bc_insn_t insn, imm_kind_t k)
{
    switch (k) {
    case IMM_KIND_A:   return (uint64_t)A_OF(insn);
    case IMM_KIND_B:   return (uint64_t)B_OF(insn);
    case IMM_KIND_C:   return (uint64_t)C_OF(insn);
    case IMM_KIND_BX:  return (uint64_t)Bx_OF(insn);
    case IMM_KIND_SBX: return (uint64_t)(int64_t)sBx_OF(insn);
    }
    return 0;
}

/* Stencil table. Ordered by frequency in measured workloads so the
 * linear scan in find_stencil tends to hit early; the cost is
 * marginal at <20 entries but the discipline keeps the table easy to
 * extend. */
static const stencil_desc_t g_stencils[] = {
    {
        OP_MOVE,
        stencil_op_move_bytes, stencil_op_move_size,
        stencil_op_move_symbols, stencil_op_move_nsymbols,
        stencil_op_move_relocs, stencil_op_move_nrelocs,
        4u  /* strip trailing arm64 `ret` -- fall through to next */
    },
    {
        OP_LOAD_K,
        stencil_op_load_k_bytes, stencil_op_load_k_size,
        stencil_op_load_k_symbols, stencil_op_load_k_nsymbols,
        stencil_op_load_k_relocs, stencil_op_load_k_nrelocs,
        4u
    },
    {
        OP_RETURN,
        stencil_op_return_imm_bytes, stencil_op_return_imm_size,
        stencil_op_return_imm_symbols, stencil_op_return_imm_nsymbols,
        stencil_op_return_imm_relocs, stencil_op_return_imm_nrelocs,
        0u  /* keep `ret` -- this is the function exit */
    }
};
static const int g_stencils_count =
    (int)(sizeof(g_stencils) / sizeof(g_stencils[0]));

static const stencil_desc_t *find_stencil(unsigned opcode)
{
    for (int i = 0; i < g_stencils_count; i++) {
        if (g_stencils[i].opcode == opcode) return &g_stencils[i];
    }
    return NULL;
}

int mino_jit_eligible(const mino_bc_fn_t *bc)
{
    if (bc == NULL || bc->code == NULL) return 0;
    if (bc->captures)            return 0;
    if (bc->ic_slots_len > 0)    return 0;
    if (bc->n_clauses != 1)      return 0;
    if (bc->n_clauses == 1 && bc->clauses != NULL && bc->clauses[0].has_rest) {
        return 0;  /* variadic dispatch not stencilised yet */
    }
    for (size_t pc = 0; pc < bc->code_len; pc++) {
        unsigned op = OP_OF(bc->code[pc]);
        if (find_stencil(op) == NULL) return 0;
    }
    /* Need at least one OP_RETURN so the fn terminates. The compiler
     * always emits one at the end of the body; we double-check so a
     * malformed bc doesn't get a JIT'd path with no exit. */
    if (bc->code_len == 0) return 0;
    if (OP_OF(bc->code[bc->code_len - 1]) != OP_RETURN) return 0;
    return 1;
}

/* ----- ARM64 instruction patchers ---------------------------------------- */

/* Patch the adrp at *insn so that (adrp_addr.page + offset) lands in
 * `target`'s page. The encoded value is a 21-bit signed page count;
 * adrp takes immlo (2 bits, bits 30-29) and immhi (19 bits, bits
 * 23-5). The other bits stay as the compiler emitted them. */
static void patch_adrp(uint32_t *insn, uintptr_t insn_addr, uintptr_t target)
{
    uintptr_t insn_page   = insn_addr & ~(uintptr_t)0xfff;
    uintptr_t target_page = target    & ~(uintptr_t)0xfff;
    int64_t   page_diff   = (int64_t)(target_page - insn_page) >> 12;
    uint32_t  immlo = (uint32_t)((uint64_t)page_diff & 0x3u);
    uint32_t  immhi = (uint32_t)(((uint64_t)page_diff >> 2) & 0x7ffffu);
    uint32_t  base  = *insn;
    base &= ~((uint32_t)0x60000000u | ((uint32_t)0x7ffffu << 5));
    base |= (immlo << 29) | (immhi << 5);
    *insn = base;
}

/* Patch the ldr-immediate at *insn (or add-immediate) so its 12-bit
 * page-offset field encodes the low bits of `target`. For a 64-bit
 * load the offset is scaled by 8 in the encoding; the compiler emits
 * size=11 ldr so we apply >> 3 on the byte offset. */
static void patch_pageoff12_ldr64(uint32_t *insn, uintptr_t target)
{
    uint32_t off12  = (uint32_t)(target & 0xfffu);
    uint32_t scaled = off12 >> 3;
    uint32_t base   = *insn;
    base &= ~((uint32_t)0xfffu << 10);
    base |= (scaled & 0xfffu) << 10;
    *insn = base;
}

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

/* Place `st`'s body at `code + pos`, populate `nsymbols` consecutive
 * 8-byte slots in the literal pool with the bytecode operand values,
 * and patch every reloc to address its slot. Returns the number of
 * bytes consumed in the code section (post-trim) or -1 on overflow. */
static long emit_stencil(unsigned char *code, size_t pos,
                         size_t code_cap,
                         uint64_t *pool, size_t pool_pos, size_t pool_cap,
                         size_t *out_pool_pos,
                         const stencil_desc_t *st, mino_bc_insn_t insn,
                         uintptr_t code_base, uintptr_t pool_base)
{
    size_t emit_size = st->size - st->trim_tail_bytes;
    if (pos + emit_size > code_cap) return -1;
    if (pool_pos + st->nsymbols > pool_cap) return -1;
    /* Populate the literal pool slots for this instance. The
     * extractor's symbol-table ordering matches the order each name
     * was first encountered in the reloc table; the stencil's reloc
     * slot index then keys directly into our pool. */
    for (unsigned long s = 0; s < st->nsymbols; s++) {
        int kind = imm_kind_from_name(st->symbols[s]);
        if (kind < 0) return -1;
        pool[pool_pos + s] = imm_value(insn, (imm_kind_t)kind);
    }
    memcpy(code + pos, st->bytes, emit_size);
    for (unsigned long r = 0; r < st->nrelocs; r++) {
        unsigned int off  = st->relocs[r][0];
        unsigned int kind = st->relocs[r][1];
        unsigned int sym  = st->relocs[r][2];
        if (off >= emit_size) continue;       /* points into the trimmed tail */
        if (sym >= st->nsymbols) return -1;
        uintptr_t   insn_addr = code_base + pos + off;
        uintptr_t   target    = pool_base + (pool_pos + sym) * sizeof(uint64_t);
        uint32_t   *insn_p    = (uint32_t *)(code + pos + off);
        switch (kind) {
        case MINO_STENCIL_RELOC_ARM64_PAGE21:
        case MINO_STENCIL_RELOC_ARM64_GOT_LOAD_PAGE21:
            patch_adrp(insn_p, insn_addr, target);
            break;
        case MINO_STENCIL_RELOC_ARM64_PAGEOFF12:
        case MINO_STENCIL_RELOC_ARM64_GOT_LOAD_PAGEOFF12:
            patch_pageoff12_ldr64(insn_p, target);
            break;
        default:
            return -1;
        }
    }
    *out_pool_pos = pool_pos + st->nsymbols;
    return (long)emit_size;
}

/* ----- top-level compile -------------------------------------------------- */

int mino_jit_compile(mino_state_t *S, mino_val_t *fn_val)
{
    if (fn_val == NULL || mino_type_of(fn_val) != MINO_FN) return -1;
    mino_bc_fn_t *bc = fn_val->as.fn.bc;
    if (bc == NULL) return -1;
    if (bc->native != NULL) return 0;
    if (!mino_jit_eligible(bc)) return -1;

    /* Estimate worst-case sizes (sum of stencil sizes; pool sized to
     * worst nsymbols per opcode). */
    size_t code_size = 0;
    size_t pool_slots = 0;
    for (size_t pc = 0; pc < bc->code_len; pc++) {
        unsigned op = OP_OF(bc->code[pc]);
        const stencil_desc_t *st = find_stencil(op);
        code_size  += st->size - st->trim_tail_bytes;
        pool_slots += st->nsymbols;
    }
    if (code_size == 0 || pool_slots == 0) {
        /* Pool may legitimately be zero for an OP_RETURN-only body,
         * but the literal pool page is still required so the patchers
         * have an addressable target page. Falling through here keeps
         * the layout uniform; an unused page is cheap. */
    }
    long page_l = sysconf(_SC_PAGESIZE);
    if (page_l <= 0) return -1;
    size_t page = (size_t)page_l;
    size_t pool_bytes = pool_slots * sizeof(uint64_t);
    size_t code_pages = (code_size + page - 1) / page;
    if (code_pages == 0) code_pages = 1;
    size_t pool_pages = (pool_bytes + page - 1) / page;
    if (pool_pages == 0) pool_pages = 1;
    size_t total_size = (code_pages + pool_pages) * page;

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

    unsigned char *code = (unsigned char *)region;
    uint64_t      *pool = (uint64_t *)((unsigned char *)region
                                        + code_pages * page);
    size_t         code_cap = code_pages * page;
    size_t         pool_cap = (pool_pages * page) / sizeof(uint64_t);
    uintptr_t      code_base = (uintptr_t)code;
    uintptr_t      pool_base = (uintptr_t)pool;

    size_t pos = 0;
    size_t pool_pos = 0;
    for (size_t pc = 0; pc < bc->code_len; pc++) {
        pc_offsets[pc] = (unsigned)pos;
        unsigned op = OP_OF(bc->code[pc]);
        const stencil_desc_t *st = find_stencil(op);
        size_t new_pool_pos = pool_pos;
        long n = emit_stencil(code, pos, code_cap, pool, pool_pos, pool_cap,
                              &new_pool_pos,
                              st, bc->code[pc], code_base, pool_base);
        if (n < 0) {
            free(pc_offsets);
            munmap(region, total_size);
            return -1;
        }
        pos += (size_t)n;
        pool_pos = new_pool_pos;
    }

    /* Flush the I-cache so the CPU sees the freshly written
     * instructions. On ARM64 the data and instruction caches are
     * coherent only after explicit maintenance; clang's builtin
     * issues `dc cvau` + `dsb ish` + `ic ivau` + `dsb ish` + `isb`
     * the way the architecture requires. */
    __builtin___clear_cache((char *)region, (char *)region + code_pages * page);

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
     * once per compile, off the hot path. Used by the v0.185.0
     * smoke verification to confirm the JIT'd path is exercised. */
    {
        const char *trace = getenv("MINO_CPJIT_TRACE");
        if (trace != NULL && trace[0] != '\0' && trace[0] != '0') {
            fprintf(stderr,
                    "[cpjit] compiled bc (code_len=%zu, region=%p, "
                    "code_pages=%zu, pool_slots=%zu)\n",
                    bc->code_len, region, code_pages, pool_slots);
        }
    }
    return 0;
}

mino_val_t *mino_jit_invoke(mino_state_t *S, mino_bc_fn_t *bc,
                            mino_val_t **regs, mino_val_t **consts)
{
    typedef mino_val_t *(*native_t)(mino_val_t **, mino_val_t **,
                                     mino_state_t *);
    native_t f = (native_t)bc->native;
    return f(regs, consts, S);
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
#include "../../mino.h"
#include "../../mino_internal.h"

int mino_jit_eligible(const mino_bc_fn_t *bc)
{
    (void)bc; return 0;
}

int mino_jit_compile(mino_state_t *S, mino_val_t *fn_val)
{
    (void)S; (void)fn_val; return -1;
}

mino_val_t *mino_jit_invoke(mino_state_t *S, mino_bc_fn_t *bc,
                            mino_val_t **regs, mino_val_t **consts)
{
    (void)S; (void)bc; (void)regs; (void)consts; return NULL;
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
