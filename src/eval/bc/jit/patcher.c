/*
 * src/eval/bc/jit/patcher.c -- ARM64 instruction patchers + direct-emit
 * byte templates.
 *
 * Only compiled when the host is ARM64. The x86_64 counterparts live in
 * patcher_x86_64.c with the same set of public signatures (where they
 * still make sense for the arch); emit.c picks between them through
 * the `MINO_CPJIT_HOST_*` macros and the per-reloc-kind dispatch in
 * the per-instance emit walk.
 *
 * The compile pipeline in emit.c writes raw stencil bytes into the JIT
 * region; this file then rewrites the address-dependent fields:
 *
 *   - `patch_adrp`, `patch_pageoff12_ldr64`: page21 + pageoff12 pair
 *     for the GOT-style adrp+ldr that loads a per-instruction pool
 *     slot.
 *   - `patch_branch26`: the bl / b targeting a 16-byte trampoline (for
 *     extern helper calls) or, for the back-jump marker, the stencil
 *     instance's own start.
 *   - `patch_b_unconditional`: the trailing `ret` of a non-final
 *     stencil is overwritten with a `b <next_stencil_start>` chain
 *     branch.
 *   - `patch_imm19`: 19-bit signed offset for B.cond / CBZ used by the
 *     direct-emit JMPIFNOT template.
 *
 * The direct-emit byte writers (`emit_jmp_bytes`, `emit_jmpifnot_bytes`)
 * synthesise short sequences from scratch instead of memcpy'ing a
 * compiled stencil. The trampoline writer (`write_trampoline`)
 * synthesises a fixed 16-byte slab that loads an absolute helper-fn
 * address and branches into it, sidestepping the bl's 26-bit reach
 * when the host fn is far from the mmap'd region.
 *
 * Splitting this from the compile pipeline isolates the seam the
 * non-ARM64 patcher cycle will graft onto: a new patcher.c per host
 * arch can land here without touching emit.c.
 */

#include "internal.h"

#ifdef MINO_CPJIT_HOST_ARM64

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Patch the adrp at *insn so that (adrp_addr.page + offset) lands in
 * `target`'s page. The encoded value is a 21-bit signed page count;
 * adrp takes immlo (2 bits, bits 30-29) and immhi (19 bits, bits
 * 23-5). The other bits stay as the compiler emitted them. */
void mino_jit_patch_adrp(uint32_t *insn, uintptr_t insn_addr, uintptr_t target)
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
void mino_jit_patch_pageoff12_ldr64(uint32_t *insn, uintptr_t target)
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
int mino_jit_patch_branch26(uint32_t *insn, uintptr_t insn_addr,
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
int mino_jit_patch_b_unconditional(uint32_t *insn, uintptr_t insn_addr,
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

/* Patch a CBZ / B.cond imm19 field so the encoded offset reaches
 * `target_addr`. Both instructions place imm19 at bits 23:5; the
 * caller controls the rest of the encoding. Returns -1 if the target
 * is unaligned or outside the ±1 MB reach. */
int mino_jit_patch_imm19(uint32_t *insn, uintptr_t insn_addr,
                         uintptr_t target_addr)
{
    int64_t diff = (int64_t)(target_addr - insn_addr);
    if ((diff & 3) != 0) return -1;
    int64_t imm19 = diff >> 2;
    if (imm19 < -(int64_t)(1 << 18) || imm19 >= (int64_t)(1 << 18)) {
        return -1;
    }
    uint32_t base = *insn;
    base &= ~((uint32_t)0x7ffffu << 5);
    base |= ((uint32_t)((uint64_t)imm19 & 0x7ffffu)) << 5;
    *insn = base;
    return 0;
}

/* OP_JMP emits one unconditional `b <target>`. The target offset is
 * unknown until the layout pass completes, so the initial bytes are
 * a placeholder `b .` (self-branch) that the target-patch pass
 * rewrites. */
void mino_jit_emit_jmp_bytes(unsigned char *code, size_t pos)
{
    static const uint32_t B_SELF = 0x14000000u;  /* b . (offset = 0) */
    memcpy(code + pos, &B_SELF, 4);
}

/* OP_JMPIFNOT emits five ARM64 instructions: load regs[A] into x9,
 * branch on NULL, then `(v - 2) <= 1` covers both nil-tagged (0x3)
 * and false-tagged (0x2). The truthy fall-through case proceeds to
 * the next stencil with x0 / x1 / x2 untouched.
 *
 *   ldr  x9,  [x0, #(IMM_A * 8)]
 *   cbz  x9,  <taken>           ; NULL  -> take branch
 *   sub  x10, x9, #2
 *   cmp  x10, #1                ; unsigned: covers x10 == 0 (v=2/false)
 *                               ; and x10 == 1 (v=3/nil), leaves all
 *                               ; other tagged values above 1.
 *   b.ls <taken>                ; LS = unsigned <=
 *
 * The branch-target placeholders point at the instruction itself
 * (imm19 = 0) until the target-patch pass rewrites them. */
void mino_jit_emit_jmpifnot_bytes(unsigned char *code, size_t pos,
                                  mino_bc_insn_t insn)
{
    uint32_t a = A_OF(insn);
    /* ldr x9, [x0, #(a*8)]: unsigned-imm offset is scaled by 8
     * for 64-bit loads, so the encoded imm12 == a directly. */
    uint32_t ldr  = 0xf9400009u | ((a & 0xfffu) << 10);
    /* cbz x9, .  -- target patched later. */
    uint32_t cbz  = 0xb4000009u;
    /* sub x10, x9, #2 */
    uint32_t sub_ = 0xd100092au;
    /* cmp x10, #1  (subs xzr, x10, #1) */
    uint32_t cmp_ = 0xf100055fu;
    /* b.ls .   (cond = 9 = LS) */
    uint32_t bls  = 0x54000009u;
    memcpy(code + pos + 0,  &ldr,  4);
    memcpy(code + pos + 4,  &cbz,  4);
    memcpy(code + pos + 8,  &sub_, 4);
    memcpy(code + pos + 12, &cmp_, 4);
    memcpy(code + pos + 16, &bls,  4);
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
void mino_jit_write_trampoline(unsigned char *slot, uintptr_t target_addr)
{
    static const uint32_t LDR_X16_PC_REL_8 = 0x58000050u;
    static const uint32_t BR_X16           = 0xd61f0200u;
    memcpy(slot + 0,  &LDR_X16_PC_REL_8, 4);
    memcpy(slot + 4,  &BR_X16,           4);
    memcpy(slot + 8,  &target_addr,      8);
}

#endif /* MINO_CPJIT_HOST_ARM64 */

/* Keep this TU non-empty under -Werror=pedantic when the gate above
 * is false (e.g., x86_64 builds). */
typedef int mino_jit_patcher_arm64_tu_marker;
