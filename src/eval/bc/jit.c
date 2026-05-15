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
#elif defined(__x86_64__) && defined(__linux__) && defined(MINO_CPJIT_X86_64_LINUX)
#define MINO_CPJIT_HOST 1
#define MINO_CPJIT_STENCILS_HEADER "stencils/generated/stencils_x86_64_linux.h"
#elif defined(__x86_64__) && defined(__APPLE__) && defined(MINO_CPJIT_X86_64_DARWIN)
#define MINO_CPJIT_HOST 1
#define MINO_CPJIT_STENCILS_HEADER "stencils/generated/stencils_x86_64_darwin.h"
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
#include "../../prim/internal.h"
#include "internal.h"
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
 * looks up an entry by opcode and emits its bytes; the JIT runtime
 * then replaces the trailing `ret` with a `b <next>` chain branch for
 * non-final stencils. OP_RETURN keeps its `ret` so the JIT region
 * exits to the caller through it. */
typedef struct {
    unsigned             opcode;
    const unsigned char *bytes;
    unsigned long        size;
    const char *const   *symbols;
    unsigned long        nsymbols;
    const unsigned int (*relocs)[4];
    unsigned long        nrelocs;
    /* Non-zero for the OP_RETURN-class stencil: the trailing `ret`
     * is preserved as the JIT region's exit instead of being
     * rewritten into a chain branch. */
    unsigned             is_final;
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

/* Pseudo-opcode for the fused LOAD_K + RETURN superinstruction. Sits
 * above OP__COUNT so the find_stencil lookup never confuses it with
 * a real bytecode opcode emitted by the compiler. The JIT-compile
 * walk pattern-matches the source pair and selects this entry
 * directly; the bytecode tier never sees it. */
#define OP_FUSED_LOAD_K_RETURN ((unsigned)(OP__COUNT + 1))

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

/* ----- Cold helpers stencils call ---------------------------------------- */

/* Slow path for the arith stencils. Mirrors the interpreter's OP_*_II
 * fallback: build a two-element cons list rooted in the bytecode
 * register window, dispatch to the matching numeric prim, and store
 * the result through the (possibly relocated) regs base before
 * returning that base to the caller. Returns NULL only when the prim
 * itself returns NULL -- in practice prims raise through longjmp on
 * type errors, so the NULL return is the defensive case the stencil's
 * caller propagates back up the JIT region. */
mino_val_t **mino_jit_binop_slow(mino_state_t *S, mino_val_t **regs,
                                 unsigned a, unsigned b, unsigned c,
                                 unsigned subop)
{
    ptrdiff_t base = regs - S->bc_regs;
    mino_val_t *list = mino_nil(S);
    if (list == NULL) return NULL;
    /* Read regs[b] / regs[c] through the freshly-rebased pointer at
     * every step: a GC inside mino_cons can reallocate bc_regs and
     * leave the C local stale. The base offset stays valid. */
    list = mino_cons(S, S->bc_regs[base + c], list);
    if (list == NULL) return NULL;
    list = mino_cons(S, S->bc_regs[base + b], list);
    if (list == NULL) return NULL;
    mino_val_t *r;
    switch (subop) {
    case BINOP_ADD: r = prim_add(S, list, NULL); break;
    case BINOP_SUB: r = prim_sub(S, list, NULL); break;
    case BINOP_MUL: r = prim_mul(S, list, NULL); break;
    default:        r = NULL;                    break;
    }
    if (r == NULL) return NULL;
    regs = S->bc_regs + base;
    regs[a] = r;
    return regs;
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
    {"binop_int_fast",      (void *)(uintptr_t)binop_int_fast},
    {"mino_jit_binop_slow", (void *)(uintptr_t)mino_jit_binop_slow},
    {NULL, NULL}
};

static void *lookup_extern_fn(const char *name)
{
    for (int i = 0; g_extern_fns[i].name != NULL; i++) {
        if (strcmp(g_extern_fns[i].name, name) == 0) {
            return g_extern_fns[i].addr;
        }
    }
    return NULL;
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

/* Patch a `bl` (or any BRANCH26-encoded branch) at *insn so it
 * targets `target_addr`. The 26-bit signed offset (in 4-byte units)
 * has ±128 MB range -- always reachable when target is within the
 * JIT region's own mmap. Returns 0 on success, -1 if the target is
 * unreachable or unaligned. */
static int patch_branch26(uint32_t *insn, uintptr_t insn_addr,
                          uintptr_t target_addr)
{
    int64_t diff = (int64_t)(target_addr - insn_addr);
    if ((diff & 3) != 0) return -1;
    int64_t imm26 = diff >> 2;
    if (imm26 < -(int64_t)(1 << 25) || imm26 >= (int64_t)(1 << 25)) {
        return -1;
    }
    uint32_t base = *insn;
    base &= ~(uint32_t)0x03ffffffu;
    base |= (uint32_t)((uint64_t)imm26 & 0x03ffffffu);
    *insn = base;
    return 0;
}

/* Overwrite a 32-bit slot with an unconditional `b <target>`. Used to
 * replace the trailing `ret` of a non-final stencil with a chain
 * branch to the next stencil's first instruction. */
static int patch_b_unconditional(uint32_t *insn, uintptr_t insn_addr,
                                 uintptr_t target_addr)
{
    int64_t diff = (int64_t)(target_addr - insn_addr);
    if ((diff & 3) != 0) return -1;
    int64_t imm26 = diff >> 2;
    if (imm26 < -(int64_t)(1 << 25) || imm26 >= (int64_t)(1 << 25)) {
        return -1;
    }
    *insn = (uint32_t)0x14000000u
          | (uint32_t)((uint64_t)imm26 & 0x03ffffffu);
    return 0;
}

/* Locate the first `ret` (encoded as 0xd65f03c0) within the stencil
 * bytes. The compiler-emitted layout for our stencils places exactly
 * one ret at the natural function exit; cold blocks merge back into
 * the epilogue via an intra-stencil branch before the ret. Returns
 * the offset of the ret or -1 when none is found. */
static long find_first_ret(const unsigned char *bytes, unsigned long size)
{
    for (unsigned long i = 0; i + 4 <= size; i += 4) {
        uint32_t insn;
        memcpy(&insn, bytes + i, 4);
        if (insn == 0xd65f03c0u) return (long)i;
    }
    return -1;
}

/* Each BRANCH26 reloc in a stencil bl-calls a 16-byte trampoline
 * whose body loads an absolute target address from an embedded
 * literal pool and branches to it. The trampoline is self-contained:
 *   00:  ldr x16, [pc, #8]   (0x58000050)
 *   04:  br  x16              (0xd61f0200)
 *   08:  <8-byte absolute target address, little-endian>
 *
 * This sidesteps the bl's 26-bit signed offset limit (±128 MB) when
 * the host fn is far from the mmap'd region. Trampolines live in a
 * dedicated slab between the code region and the per-instruction
 * literal pool. */
#define MINO_JIT_TRAMPOLINE_SIZE 16u

static void write_trampoline(unsigned char *slot, uintptr_t target_addr)
{
    static const uint32_t LDR_X16_PC_REL_8 = 0x58000050u;
    static const uint32_t BR_X16           = 0xd61f0200u;
    memcpy(slot + 0,  &LDR_X16_PC_REL_8, 4);
    memcpy(slot + 4,  &BR_X16,           4);
    memcpy(slot + 8,  &target_addr,      8);
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

/* Per-emit symbol classification slots. The fixed bound mirrors the
 * extractor's MAX_SYMS (32 in tools/stencil_extract.c); raising it
 * means raising both sides together. */
enum { MINO_JIT_MAX_SYMS_PER_STENCIL = 32 };

typedef struct {
    int    is_fn;        /* 0 = IMM pool slot, 1 = FN trampoline slot */
    size_t slot_offset;  /* pool[slot_offset] (IMM) or
                          * tramp_buf + slot_offset (FN, byte-indexed). */
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
                         const stencil_desc_t *st, mino_bc_insn_t insn,
                         uintptr_t code_base, uintptr_t pool_base,
                         uintptr_t tramp_base)
{
    if (pos + st->size > code_cap) return -1;
    if (st->nsymbols > (unsigned long)MINO_JIT_MAX_SYMS_PER_STENCIL) return -1;
    sym_slot_t slots[MINO_JIT_MAX_SYMS_PER_STENCIL];
    size_t new_pool_pos  = pool_pos;
    size_t new_tramp_pos = tramp_pos;
    /* Walk the symbol table once: classify each entry as IMM or FN
     * and allocate the corresponding slot. The extractor preserves
     * the order each name was first encountered, so subsequent
     * reloc-driven patches index slots[s] by sym_index directly. */
    for (unsigned long s = 0; s < st->nsymbols; s++) {
        const char *name = st->symbols[s];
        int k = imm_kind_from_name(name);
        if (k >= 0) {
            if (new_pool_pos >= pool_cap) return -1;
            slots[s].is_fn       = 0;
            slots[s].slot_offset = new_pool_pos;
            pool[new_pool_pos]   = imm_value(insn, (imm_kind_t)k);
            new_pool_pos++;
        } else {
            void *addr = lookup_extern_fn(name);
            if (addr == NULL) return -1;
            if (new_tramp_pos + MINO_JIT_TRAMPOLINE_SIZE > tramp_cap) return -1;
            slots[s].is_fn       = 1;
            slots[s].slot_offset = new_tramp_pos;
            write_trampoline(tramp_buf + new_tramp_pos, (uintptr_t)addr);
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
        if (slots[sym].is_fn) {
            uintptr_t target = tramp_base + slots[sym].slot_offset;
            if (kind != MINO_STENCIL_RELOC_ARM64_BRANCH26) return -1;
            if (patch_branch26(insn_p, insn_addr, target) != 0) return -1;
        } else {
            uintptr_t target = pool_base
                + slots[sym].slot_offset * sizeof(uint64_t);
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
    }
    *out_pool_pos  = new_pool_pos;
    *out_tramp_pos = new_tramp_pos;
    return (long)st->size;
}

/* ----- top-level compile -------------------------------------------------- */

/* Pick the stencil + a hint about whether the current pc participates
 * in a OP_LOAD_K + OP_RETURN superinstruction fusion. Returns the
 * stencil descriptor and writes `*out_fused = 1` when fusion fires
 * (caller must advance pc by 2 in that branch). Returns NULL when no
 * stencil covers the opcode. */
static const stencil_desc_t *pick_stencil(const mino_bc_fn_t *bc,
                                          size_t pc, int *out_fused)
{
    unsigned op = OP_OF(bc->code[pc]);
    *out_fused  = 0;
    if (op == OP_LOAD_K && pc + 1 < bc->code_len) {
        mino_bc_insn_t cur  = bc->code[pc];
        mino_bc_insn_t next = bc->code[pc + 1];
        if (OP_OF(next) == OP_RETURN && A_OF(next) == A_OF(cur)) {
            *out_fused = 1;
            return find_stencil(OP_FUSED_LOAD_K_RETURN);
        }
    }
    return find_stencil(op);
}

int mino_jit_compile(mino_state_t *S, mino_val_t *fn_val)
{
    if (fn_val == NULL || mino_type_of(fn_val) != MINO_FN) return -1;
    mino_bc_fn_t *bc = fn_val->as.fn.bc;
    if (bc == NULL) return -1;
    if (bc->native != NULL) return 0;
    if (!mino_jit_eligible(bc)) return -1;

    /* First pass: classify every symbol of every stencil to size the
     * code / trampoline / pool regions. The pass also validates that
     * every extern fn symbol resolves to a known host helper, so the
     * mmap below has zero chance of failing partway through. */
    size_t code_size   = 0;
    size_t pool_slots  = 0;
    size_t tramp_count = 0;
    for (size_t pc = 0; pc < bc->code_len; pc++) {
        int fused;
        const stencil_desc_t *st = pick_stencil(bc, pc, &fused);
        if (st == NULL) return -1;
        code_size += st->size;
        for (unsigned long s = 0; s < st->nsymbols; s++) {
            const char *name = st->symbols[s];
            int k = imm_kind_from_name(name);
            if (k >= 0) {
                pool_slots++;
            } else {
                if (lookup_extern_fn(name) == NULL) return -1;
                tramp_count++;
            }
        }
        if (fused) pc++;
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

    /* Per-stencil-instance tracking so the post-emit ret patcher
     * knows which spans are non-final and where the next instance
     * begins. Sized to code_len (upper bound; fusion strictly
     * reduces the count). */
    size_t *inst_starts = (size_t *)malloc(bc->code_len * sizeof(size_t));
    unsigned char *inst_is_final =
        (unsigned char *)malloc(bc->code_len * sizeof(unsigned char));
    if (inst_starts == NULL || inst_is_final == NULL) {
        free(inst_starts);
        free(inst_is_final);
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
        int fused;
        const stencil_desc_t *st = pick_stencil(bc, pc, &fused);
        size_t new_pool_pos  = pool_pos;
        size_t new_tramp_pos = tramp_pos;
        long n = emit_stencil(code, pos, code_size,
                              pool, pool_pos, pool_slots, &new_pool_pos,
                              tramp_buf, tramp_pos, tramp_bytes, &new_tramp_pos,
                              st, bc->code[pc],
                              code_base, pool_base, tramp_base);
        if (n < 0) {
            free(inst_starts);
            free(inst_is_final);
            free(pc_offsets);
            munmap(region, total_size);
            return -1;
        }
        inst_starts[n_inst]   = pos;
        inst_is_final[n_inst] = (unsigned char)st->is_final;
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
    }

    /* Second pass: rewrite each non-final stencil's trailing `ret`
     * into a `b <next_stencil_start>` chain branch. clang lays the
     * cold blocks out after the natural exit; the first ret in the
     * byte stream is always the success-path exit. */
    for (size_t i = 0; i + 1 < n_inst; i++) {
        if (inst_is_final[i]) continue;
        size_t span_size = inst_starts[i + 1] - inst_starts[i];
        long   ret_off   = find_first_ret(code + inst_starts[i], span_size);
        if (ret_off < 0) {
            free(inst_starts);
            free(inst_is_final);
            free(pc_offsets);
            munmap(region, total_size);
            return -1;
        }
        uintptr_t ret_addr  = code_base + inst_starts[i] + (size_t)ret_off;
        uintptr_t next_addr = code_base + inst_starts[i + 1];
        uint32_t *insn_p    =
            (uint32_t *)(code + inst_starts[i] + (size_t)ret_off);
        if (patch_b_unconditional(insn_p, ret_addr, next_addr) != 0) {
            free(inst_starts);
            free(inst_is_final);
            free(pc_offsets);
            munmap(region, total_size);
            return -1;
        }
    }
    free(inst_starts);
    free(inst_is_final);

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
