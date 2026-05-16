/*
 * src/eval/bc/jit/patcher_x86_64.c -- x86_64 instruction patchers +
 * direct-emit byte templates.
 *
 * Only compiled when the host is x86_64 (gated on
 * MINO_CPJIT_HOST_X86_64). The ARM64 counterparts live in patcher.c.
 *
 * The compile pipeline in emit.c writes raw stencil bytes into the JIT
 * region; this file then rewrites the address-dependent fields:
 *
 *   - patch_abs64:    8-byte absolute target for R_X86_64_64.
 *   - patch_pc32:     4-byte signed offset for R_X86_64_PC32 / _PLT32.
 *                     The offset is `target - (insn_addr + 4)` because
 *                     rip points at the next instruction.
 *   - patch_gotpcrel: same as patch_pc32 but the target is a pool slot
 *                     holding the absolute address (R_X86_64_GOTPCREL /
 *                     _REX_GOTPCRELX).
 *   - patch_jmp32_to: rewrite a 5-byte `e9 <rel32>` jmp's offset.
 *                     Used by the chain pass to rewrite each musttail
 *                     placeholder to its next-instance target, and
 *                     by Pass B for OP_JMP target patching.
 *   - patch_jcc32_to: rewrite a 6-byte `0f 8X <rel32>` jcc's offset.
 *                     Used by Pass B for OP_JMPIFNOT target patching.
 *
 * The direct-emit byte writers synthesise short sequences for OP_JMP
 * and OP_JMPIFNOT from scratch; the trampoline writer synthesises a
 * 12-byte slab that does `movabs rax, target; jmp rax`. The
 * trampoline indirection sidesteps the rel32's ±2 GB reach when the
 * helper fn is far from the mmap'd JIT region.
 */

#include "internal.h"

#ifdef MINO_CPJIT_HOST_X86_64

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Write an 8-byte absolute target at `insn`. The addend is normally
 * zero for R_X86_64_64 against extern fn symbols; preserved for
 * generality. Returns 0 on success; never fails for an abs reloc
 * (the int return matches the ARM64 patcher signatures so emit.c
 * dispatch reads uniformly). */
int mino_jit_patch_abs64(unsigned char *insn, uintptr_t target,
                          int32_t addend)
{
    uint64_t val = (uint64_t)((int64_t)target + (int64_t)addend);
    memcpy(insn, &val, 8);
    return 0;
}

/* Write a 4-byte signed offset at `insn` so the rip-relative read
 * lands on `target`. `insn_addr` is the address of the rel32 field
 * itself. ELF PC32 semantics: stored = S + A - P, where S=target,
 * A=addend (the assembler records -4 because rip points 4 bytes
 * past the rel32 field's start), P=insn_addr. The -4 in `addend`
 * captures the "rip points at next instruction" adjustment, so
 * the patcher does not add a separate +4. Returns -1 if the
 * offset doesn't fit in int32. */
int mino_jit_patch_pc32(unsigned char *insn, uintptr_t insn_addr,
                         uintptr_t target, int32_t addend)
{
    int64_t diff = (int64_t)target + (int64_t)addend - (int64_t)insn_addr;
    if (diff < INT32_MIN || diff > INT32_MAX) return -1;
    int32_t rel = (int32_t)diff;
    memcpy(insn, &rel, 4);
    return 0;
}

/* Same encoding as patch_pc32; named separately so emit.c dispatch
 * makes the intent clear. The target here is a pool slot whose
 * 8-byte contents hold the actual address the loaded value reads. */
int mino_jit_patch_gotpcrel(unsigned char *insn, uintptr_t insn_addr,
                             uintptr_t pool_slot_addr, int32_t addend)
{
    return mino_jit_patch_pc32(insn, insn_addr, pool_slot_addr, addend);
}

/* Rewrite the rel32 field of a 5-byte `e9 <rel32>` jmp instruction.
 * `insn` points at the `e9` opcode; the rel32 field lives at +1.
 * `insn_addr` is the address of the `e9`. The CPU computes the
 * target as `(insn_addr + 5) + rel32`. Returns -1 if out of range. */
int mino_jit_patch_jmp32_to(unsigned char *insn, uintptr_t insn_addr,
                             uintptr_t target_addr)
{
    int64_t diff = (int64_t)target_addr - (int64_t)(insn_addr + 5);
    if (diff < INT32_MIN || diff > INT32_MAX) return -1;
    int32_t rel = (int32_t)diff;
    memcpy(insn + 1, &rel, 4);
    return 0;
}

/* Rewrite the rel32 field of a 6-byte `0f 8X <rel32>` jcc
 * instruction. `insn` points at the `0f` prefix; the rel32 field
 * lives at +2. The CPU computes the target as
 * `(insn_addr + 6) + rel32`. Returns -1 if out of range. */
int mino_jit_patch_jcc32_to(unsigned char *insn, uintptr_t insn_addr,
                             uintptr_t target_addr)
{
    int64_t diff = (int64_t)target_addr - (int64_t)(insn_addr + 6);
    if (diff < INT32_MIN || diff > INT32_MAX) return -1;
    int32_t rel = (int32_t)diff;
    memcpy(insn + 2, &rel, 4);
    return 0;
}

/* OP_JMP emits one near-jmp: `e9 00 00 00 00` (5 bytes). The
 * placeholder rel32 = 0 means "branch to itself"; the target-patch
 * pass rewrites it once the layout walk has placed every pc. */
void mino_jit_emit_jmp_bytes(unsigned char *code, size_t pos)
{
    static const unsigned char JMP_PLACEHOLDER[5] = {
        0xe9, 0x00, 0x00, 0x00, 0x00
    };
    memcpy(code + pos, JMP_PLACEHOLDER, sizeof(JMP_PLACEHOLDER));
}

/* OP_JMPIFNOT emits a 30-byte sequence that mirrors the ARM64 path:
 *
 *   48 8b 87 disp32        ; mov rax, qword ptr [rdi + disp32]
 *   48 85 c0               ; test rax, rax            (NULL?)
 *   0f 84 00 00 00 00      ; je   <taken>             (placeholder rel32)
 *   48 83 e8 02            ; sub  rax, 2
 *   48 83 f8 01            ; cmp  rax, 1
 *   0f 86 00 00 00 00      ; jbe  <taken>             (placeholder rel32)
 *
 * disp32 is `IMM_A * 8` -- the byte offset of regs[A] in the regs
 * array. The `regs` pointer arrives in rdi (first SysV arg), so
 * `[rdi + 8*A]` is the load site. Both branches share the same
 * target (the "taken" path); Pass B patches them in lockstep.
 *
 * The unsigned `cmp+jbe` after `sub rax, 2` catches both the
 * nil-tagged value (0x3 -> 0x1) and the false-tagged value
 * (0x2 -> 0x0); rax > 1 means the value is truthy.
 *
 * Operand layout, byte offsets:
 *   [0]  -- 48 8b 87  (mov rax, [rdi+disp32] start)
 *   [3]  -- disp32    (4 bytes, set per-instance to IMM_A * 8)
 *   [7]  -- 48 85 c0  (test rax, rax)
 *  [10]  -- 0f 84     (je rel32 start)
 *  [12]  -- rel32     (4-byte placeholder)
 *  [16]  -- 48 83 e8 02 (sub rax, 2)
 *  [20]  -- 48 83 f8 01 (cmp rax, 1)
 *  [24]  -- 0f 86     (jbe rel32 start)
 *  [26]  -- rel32     (4-byte placeholder)
 *
 * Total: 30 bytes. */
void mino_jit_emit_jmpifnot_bytes(unsigned char *code, size_t pos,
                                   mino_bc_insn_t insn)
{
    static const unsigned char TEMPLATE[30] = {
        0x48, 0x8b, 0x87, 0x00, 0x00, 0x00, 0x00,  /* mov rax, [rdi+disp32] */
        0x48, 0x85, 0xc0,                          /* test rax, rax         */
        0x0f, 0x84, 0x00, 0x00, 0x00, 0x00,        /* je   rel32            */
        0x48, 0x83, 0xe8, 0x02,                    /* sub rax, 2            */
        0x48, 0x83, 0xf8, 0x01,                    /* cmp rax, 1            */
        0x0f, 0x86, 0x00, 0x00, 0x00, 0x00         /* jbe  rel32            */
    };
    unsigned char *base = code + pos;
    memcpy(base, TEMPLATE, sizeof(TEMPLATE));
    /* Per-instance disp32 = IMM_A * 8 (byte offset into regs[]). */
    int32_t disp = (int32_t)((uint32_t)A_OF(insn) * 8u);
    memcpy(base + 3, &disp, 4);
}

/* Each PC32 reloc in a stencil's helper-fn call site targets a
 * 12-byte trampoline whose body materialises the helper's absolute
 * address and jumps through it:
 *
 *   48 b8 <8 bytes helper_addr>   ; movabs rax, helper_addr
 *   ff e0                         ; jmp    rax
 *
 * The original `call <trampoline_rel32>` in the stencil already
 * pushed the return address (one byte after the call's end); the
 * trampoline's `jmp rax` doesn't touch rsp, so when the helper
 * issues its own `ret` the CPU returns to the stencil's
 * post-call instruction unchanged. This sidesteps the rel32's
 * ±2 GB reach when the helper fn is far from the JIT region. */
void mino_jit_write_trampoline(unsigned char *slot, uintptr_t target_addr)
{
    static const unsigned char MOVABS_RAX = 0xb8;  /* movabs rax, imm64 */
    slot[0] = 0x48;       /* REX.W */
    slot[1] = MOVABS_RAX;
    memcpy(slot + 2, &target_addr, 8);
    slot[10] = 0xff;
    slot[11] = 0xe0;      /* jmp rax */
}

#endif /* MINO_CPJIT_HOST_X86_64 */

/* Keep this TU non-empty under -Werror=pedantic when the gate above
 * is false (e.g., arm64 builds). */
typedef int mino_jit_patcher_x86_64_tu_marker;
