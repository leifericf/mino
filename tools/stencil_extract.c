/*
 * stencil_extract -- copy-and-patch stencil extraction utility.
 *
 * Reads an object file produced by the host C compiler for a single
 * stencil source (one C function per .o), locates the function body
 * in the .text / __text section, walks the symbol table for the
 * function's start and end addresses, walks the relocation table for
 * the immediates patched at runtime, and emits a C header that the
 * mino runtime compiles in. The runtime memcpy's the bytes into RWX
 * memory and patches the recorded reloc offsets with the bytecode's
 * actual operand values.
 *
 * This cut handles 64-bit Mach-O (Darwin arm64 and Darwin x86_64)
 * and 64-bit ELF (Linux arm64) and emits the stencil header in a
 * format the JIT can #include directly. The x86_64 ELF parser
 * shares the ELF code path and only adds a reloc-kind table.
 * COFF parsing lands in the Windows platform release.
 *
 * Build: cc -std=c99 -O2 -Wall -Wpedantic -o tools/stencil_extract
 *         tools/stencil_extract.c
 *
 * Usage:
 *   stencil_extract --selftest                  -- run built-in tests
 *   stencil_extract <obj_file> <symbol> <out>   -- extract one stencil
 *   stencil_extract <obj_file> --list           -- list symbols
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stencil_extract/core.h"
#include "stencil_extract/macho.h"

/* ------------------------------------------------------------------------- */
/* Format-magic constants                                                    */
/* ------------------------------------------------------------------------- */

/* ELF64 magic: ELF (0x7f) "E" "L" "F" in the first four bytes. The
 * extractor sniffs the first four bytes to pick a parser; the full
 * ELF parsing pipeline (sections walk, symtab, rela tables, ARM64
 * reloc-kind mapping) lands in the platform release that runs the
 * build on an ARM64 Linux host. The constants below are the
 * load-bearing identifiers the sniffing path consults. */
#define ELF_MAGIC_BYTE_0  0x7fu
#define ELF_MAGIC_BYTE_1  'E'
#define ELF_MAGIC_BYTE_2  'L'
#define ELF_MAGIC_BYTE_3  'F'

/* COFF object-file magic: amd64 COFF starts with the 2-byte machine
 * ID 0x8664 in little-endian, followed by the section count. The
 * Windows x86_64 platform release wires the COFF parser; this
 * release scaffolds the sniff so the dispatch surface is settled. */
#define COFF_MACHINE_AMD64  0x8664u

/* x86_64 COFF reloc kinds (`<winnt.h>` enum). */
#define IMAGE_REL_AMD64_ABSOLUTE  0x0000u
#define IMAGE_REL_AMD64_ADDR64    0x0001u
#define IMAGE_REL_AMD64_ADDR32    0x0002u
#define IMAGE_REL_AMD64_REL32     0x0004u
#define IMAGE_REL_AMD64_REL32_1   0x0005u

/* ARM64 ELF reloc kinds the JIT patcher consumes. The numeric values
 * come from `<elf.h>` and the AArch64 ELF ABI. Mapping to the
 * runtime-stable `MINO_STENCIL_RELOC_*` enum lives below alongside
 * the Mach-O mapping; both feed the same patcher. */
#define R_AARCH64_ABS64               257u
#define R_AARCH64_CALL26              283u
#define R_AARCH64_JUMP26              282u
#define R_AARCH64_ADR_PREL_PG_HI21    275u
#define R_AARCH64_ADD_ABS_LO12_NC     277u
#define R_AARCH64_LDST64_ABS_LO12_NC  286u
#define R_AARCH64_ADR_GOT_PAGE        311u
#define R_AARCH64_LD64_GOT_LO12_NC    312u

/* x86_64 ELF reloc kinds. Same enum convention as AArch64 above;
 * the platform release that lands the ELF parser maps each to a
 * runtime-stable `MINO_STENCIL_RELOC_X86_64_*` entry. */
#define R_X86_64_64           1u   /* direct 64-bit                  */
#define R_X86_64_PC32         2u   /* 32-bit pc-relative             */
#define R_X86_64_PLT32        4u   /* 32-bit PLT call                */
#define R_X86_64_GOTPCREL     9u   /* GOT entry, RIP-relative        */
#define R_X86_64_GOTPCRELX    41u  /* relaxed GOT lookup, RIP-rel    */
#define R_X86_64_REX_GOTPCRELX 42u /* relaxed GOT lookup w/ REX, RIP-rel */

/* Mach-O header types, reloc-kind constants, and reloc maps live in
 * stencil_extract/macho.h. The MINO_STENCIL_RELOC_* host enum and
 * stencil_reloc_t live in core.h. */

static int reloc_x86_64_elf_kind_map(uint32_t r_type);
static int reloc_x86_64_coff_kind_map(uint32_t coff_kind,
                                       int32_t *out_implicit_addend);

/* ------------------------------------------------------------------------- */
/* ELF64 type definitions and constants (shared by selftest + parser)        */
/* ------------------------------------------------------------------------- */

/* Section header types from the ELF spec (subset used by the extractor). */
#define SHT_NULL      0u
#define SHT_PROGBITS  1u
#define SHT_SYMTAB    2u
#define SHT_STRTAB    3u
#define SHT_RELA      4u
#define SHT_NOBITS    8u
#define SHT_REL       9u

/* ELF machine values relevant to the stencils. */
#define EM_X86_64   62u
#define EM_AARCH64  183u

/* ELF identification indexes + class. The extractor only handles 64-bit
 * little-endian. */
#define EI_CLASS    4
#define EI_DATA     5
#define ELFCLASS64  2
#define ELFDATA2LSB 1

/* Symbol info packing -- the low nibble is the type, the high nibble is
 * the binding. */
#define ELF64_ST_TYPE(info) ((unsigned)((info) & 0xfu))
#define ELF64_ST_BIND(info) ((unsigned)(((info) >> 4) & 0xfu))
#define STT_FUNC 2u
#define STB_GLOBAL 1u

/* Reloc info packing -- top 32 bits hold the symbol index, low 32 bits
 * hold the type. The accessors below extract each. */
static uint32_t elf64_r_sym(uint64_t info)  { return (uint32_t)(info >> 32); }
static uint32_t elf64_r_type(uint64_t info) { return (uint32_t)(info & 0xffffffffu); }

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_ehdr_t;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} elf64_shdr_t;

typedef struct {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} elf64_sym_t;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} elf64_rela_t;

/* Map an AArch64 ELF reloc type to the runtime-stable
 * MINO_STENCIL_RELOC_*. Returns -1 for unsupported kinds so the build
 * fails loudly rather than silently dropping a patch site. */
static int reloc_arm64_elf_kind_map(uint32_t r_type)
{
    switch (r_type) {
    case R_AARCH64_ABS64:               return (int)MINO_STENCIL_RELOC_ABS64;
    case R_AARCH64_CALL26:
    case R_AARCH64_JUMP26:              return (int)MINO_STENCIL_RELOC_ARM64_BRANCH26;
    case R_AARCH64_ADR_PREL_PG_HI21:    return (int)MINO_STENCIL_RELOC_ARM64_PAGE21;
    case R_AARCH64_ADD_ABS_LO12_NC:
    case R_AARCH64_LDST64_ABS_LO12_NC:  return (int)MINO_STENCIL_RELOC_ARM64_PAGEOFF12;
    case R_AARCH64_ADR_GOT_PAGE:        return (int)MINO_STENCIL_RELOC_ARM64_GOT_LOAD_PAGE21;
    case R_AARCH64_LD64_GOT_LO12_NC:    return (int)MINO_STENCIL_RELOC_ARM64_GOT_LOAD_PAGEOFF12;
    default: return -1;
    }
}

/* Map an x86_64 ELF reloc type to the runtime-stable
 * MINO_STENCIL_RELOC_X86_64_*. PLT32 collapses to PC32 because the
 * runtime patcher writes both as 32-bit pc-relative; the linker-only
 * PLT indirection doesn't apply once the stencils are flattened into
 * the JIT region. REX_GOTPCRELX collapses to GOTPCREL for the same
 * reason -- it's a peephole hint to the linker, not a different
 * patcher instruction. */
static int reloc_x86_64_elf_kind_map(uint32_t r_type)
{
    switch (r_type) {
    case R_X86_64_64:               return (int)MINO_STENCIL_RELOC_X86_64_ABS64;
    case R_X86_64_PC32:
    case R_X86_64_PLT32:            return (int)MINO_STENCIL_RELOC_X86_64_PC32;
    case R_X86_64_GOTPCREL:
    case R_X86_64_GOTPCRELX:
    case R_X86_64_REX_GOTPCRELX:    return (int)MINO_STENCIL_RELOC_X86_64_GOTPCREL;
    default: return -1;
    }
}

/* The Mach-O parser (macho_open, macho_strtab, macho_nlist, the
 * reloc bit-field accessors, macho_list_symbols, macho_find_symbol,
 * macho_extract_relocs, macho_emit_stencil_header, and the ARM64 +
 * x86_64 reloc-kind maps) lives in stencil_extract/macho.{h,c}. */


/* ------------------------------------------------------------------------- */
/* Self-test                                                                 */
/* ------------------------------------------------------------------------- */

static int selftest(void)
{
    /* Confirm sizeof layouts are right; on Darwin arm64 / x86_64 the
     * Mach-O 64 structs are tightly packed and these sizes are
     * documented as part of the file format. A mismatch here means the
     * compiler reordered or padded our structs. */
    int failed = 0;
    if (sizeof(macho_header_64_t) != 32) {
        fprintf(stderr, "selftest: header_64 size %zu != 32\n",
                sizeof(macho_header_64_t));
        failed++;
    }
    if (sizeof(macho_load_command_t) != 8) {
        fprintf(stderr, "selftest: load_command size %zu != 8\n",
                sizeof(macho_load_command_t));
        failed++;
    }
    if (sizeof(macho_segment_64_t) != 72) {
        fprintf(stderr, "selftest: segment_64 size %zu != 72\n",
                sizeof(macho_segment_64_t));
        failed++;
    }
    if (sizeof(macho_section_64_t) != 80) {
        fprintf(stderr, "selftest: section_64 size %zu != 80\n",
                sizeof(macho_section_64_t));
        failed++;
    }
    if (sizeof(macho_symtab_t) != 24) {
        fprintf(stderr, "selftest: symtab size %zu != 24\n",
                sizeof(macho_symtab_t));
        failed++;
    }
    if (sizeof(macho_nlist_64_t) != 16) {
        fprintf(stderr, "selftest: nlist_64 size %zu != 16\n",
                sizeof(macho_nlist_64_t));
        failed++;
    }
    if (sizeof(macho_reloc_info_t) != 8) {
        fprintf(stderr, "selftest: reloc_info size %zu != 8\n",
                sizeof(macho_reloc_info_t));
        failed++;
    }
    /* Reloc bit-field accessors: pack a known r_info and confirm the
     * helpers decode each slot independently. The reference value
     * encodes symbolnum=0x123456, pcrel=1, length=2, extern=1,
     * type=ARM64_RELOC_PAGE21. */
    macho_reloc_info_t probe;
    probe.r_address = 0;
    probe.r_info = 0x123456u
        | (1u << 24)
        | (2u << 25)
        | (1u << 27)
        | (ARM64_RELOC_PAGE21 << 28);
    if (macho_reloc_symbolnum(&probe) != 0x123456u) {
        fprintf(stderr, "selftest: reloc_symbolnum decode wrong\n"); failed++;
    }
    if (macho_reloc_pcrel(&probe) != 1u) {
        fprintf(stderr, "selftest: reloc_pcrel decode wrong\n"); failed++;
    }
    if (macho_reloc_length(&probe) != 2u) {
        fprintf(stderr, "selftest: reloc_length decode wrong\n"); failed++;
    }
    if (macho_reloc_extern(&probe) != 1u) {
        fprintf(stderr, "selftest: reloc_extern decode wrong\n"); failed++;
    }
    if (macho_reloc_type(&probe) != ARM64_RELOC_PAGE21) {
        fprintf(stderr, "selftest: reloc_type decode wrong\n"); failed++;
    }
    if (macho_reloc_arm64_kind_map(ARM64_RELOC_PAGE21, 2) !=
        (int)MINO_STENCIL_RELOC_ARM64_PAGE21) {
        fprintf(stderr, "selftest: kind_map PAGE21 wrong\n"); failed++;
    }
    if (macho_reloc_arm64_kind_map(ARM64_RELOC_UNSIGNED, 3) !=
        (int)MINO_STENCIL_RELOC_ABS64) {
        fprintf(stderr, "selftest: kind_map UNSIGNED-quad wrong\n"); failed++;
    }
    if (macho_reloc_arm64_kind_map(0xff, 3) != -1) {
        fprintf(stderr, "selftest: kind_map should reject unknown\n"); failed++;
    }
    /* ELF struct layouts: the ELF64 ABI fixes these exactly. A
     * mismatch means the compiler reordered or padded the structs. */
    if (sizeof(elf64_ehdr_t) != 64) {
        fprintf(stderr, "selftest: elf64_ehdr_t size %zu != 64\n",
                sizeof(elf64_ehdr_t));
        failed++;
    }
    if (sizeof(elf64_shdr_t) != 64) {
        fprintf(stderr, "selftest: elf64_shdr_t size %zu != 64\n",
                sizeof(elf64_shdr_t));
        failed++;
    }
    if (sizeof(elf64_sym_t) != 24) {
        fprintf(stderr, "selftest: elf64_sym_t size %zu != 24\n",
                sizeof(elf64_sym_t));
        failed++;
    }
    if (sizeof(elf64_rela_t) != 24) {
        fprintf(stderr, "selftest: elf64_rela_t size %zu != 24\n",
                sizeof(elf64_rela_t));
        failed++;
    }
    /* ELF reloc info decode: sym in high 32, type in low 32. */
    uint64_t r_info_probe = ((uint64_t)0xdeadbeefu << 32) | (uint64_t)R_AARCH64_CALL26;
    if (elf64_r_sym(r_info_probe) != 0xdeadbeefu) {
        fprintf(stderr, "selftest: elf64_r_sym decode wrong\n"); failed++;
    }
    if (elf64_r_type(r_info_probe) != R_AARCH64_CALL26) {
        fprintf(stderr, "selftest: elf64_r_type decode wrong\n"); failed++;
    }
    /* ELF AArch64 reloc-kind map: every recorded constant maps to a
     * runtime-stable MINO_STENCIL_RELOC_*, and unknown rejects. */
    if (reloc_arm64_elf_kind_map(R_AARCH64_ABS64) !=
        (int)MINO_STENCIL_RELOC_ABS64) {
        fprintf(stderr, "selftest: elf kind_map ABS64 wrong\n"); failed++;
    }
    if (reloc_arm64_elf_kind_map(R_AARCH64_CALL26) !=
        (int)MINO_STENCIL_RELOC_ARM64_BRANCH26) {
        fprintf(stderr, "selftest: elf kind_map CALL26 wrong\n"); failed++;
    }
    if (reloc_arm64_elf_kind_map(R_AARCH64_JUMP26) !=
        (int)MINO_STENCIL_RELOC_ARM64_BRANCH26) {
        fprintf(stderr, "selftest: elf kind_map JUMP26 wrong\n"); failed++;
    }
    if (reloc_arm64_elf_kind_map(R_AARCH64_ADR_PREL_PG_HI21) !=
        (int)MINO_STENCIL_RELOC_ARM64_PAGE21) {
        fprintf(stderr, "selftest: elf kind_map ADR_PREL_PG_HI21 wrong\n"); failed++;
    }
    if (reloc_arm64_elf_kind_map(R_AARCH64_ADD_ABS_LO12_NC) !=
        (int)MINO_STENCIL_RELOC_ARM64_PAGEOFF12) {
        fprintf(stderr, "selftest: elf kind_map ADD_ABS_LO12_NC wrong\n"); failed++;
    }
    if (reloc_arm64_elf_kind_map(R_AARCH64_LDST64_ABS_LO12_NC) !=
        (int)MINO_STENCIL_RELOC_ARM64_PAGEOFF12) {
        fprintf(stderr, "selftest: elf kind_map LDST64_ABS_LO12_NC wrong\n"); failed++;
    }
    if (reloc_arm64_elf_kind_map(R_AARCH64_ADR_GOT_PAGE) !=
        (int)MINO_STENCIL_RELOC_ARM64_GOT_LOAD_PAGE21) {
        fprintf(stderr, "selftest: elf kind_map ADR_GOT_PAGE wrong\n"); failed++;
    }
    if (reloc_arm64_elf_kind_map(R_AARCH64_LD64_GOT_LO12_NC) !=
        (int)MINO_STENCIL_RELOC_ARM64_GOT_LOAD_PAGEOFF12) {
        fprintf(stderr, "selftest: elf kind_map LD64_GOT_LO12_NC wrong\n"); failed++;
    }
    if (reloc_arm64_elf_kind_map(0xffffffffu) != -1) {
        fprintf(stderr, "selftest: elf kind_map should reject unknown\n"); failed++;
    }
    /* x86_64 ELF reloc-kind map: ABS64, PC32 (covers PLT32),
     * GOTPCREL (covers GOTPCRELX and REX_GOTPCRELX), unknown rejects. */
    if (reloc_x86_64_elf_kind_map(R_X86_64_64) !=
        (int)MINO_STENCIL_RELOC_X86_64_ABS64) {
        fprintf(stderr, "selftest: x86_64 kind_map ABS64 wrong\n"); failed++;
    }
    if (reloc_x86_64_elf_kind_map(R_X86_64_PC32) !=
        (int)MINO_STENCIL_RELOC_X86_64_PC32) {
        fprintf(stderr, "selftest: x86_64 kind_map PC32 wrong\n"); failed++;
    }
    if (reloc_x86_64_elf_kind_map(R_X86_64_PLT32) !=
        (int)MINO_STENCIL_RELOC_X86_64_PC32) {
        fprintf(stderr, "selftest: x86_64 kind_map PLT32 wrong\n"); failed++;
    }
    if (reloc_x86_64_elf_kind_map(R_X86_64_GOTPCREL) !=
        (int)MINO_STENCIL_RELOC_X86_64_GOTPCREL) {
        fprintf(stderr, "selftest: x86_64 kind_map GOTPCREL wrong\n"); failed++;
    }
    if (reloc_x86_64_elf_kind_map(R_X86_64_GOTPCRELX) !=
        (int)MINO_STENCIL_RELOC_X86_64_GOTPCREL) {
        fprintf(stderr, "selftest: x86_64 kind_map GOTPCRELX wrong\n"); failed++;
    }
    if (reloc_x86_64_elf_kind_map(R_X86_64_REX_GOTPCRELX) !=
        (int)MINO_STENCIL_RELOC_X86_64_GOTPCREL) {
        fprintf(stderr, "selftest: x86_64 kind_map REX_GOTPCRELX wrong\n"); failed++;
    }
    if (reloc_x86_64_elf_kind_map(0xffffffffu) != -1) {
        fprintf(stderr, "selftest: x86_64 kind_map should reject unknown\n"); failed++;
    }
    /* x86_64 Mach-O reloc-kind map: BRANCH+SIGNED+SIGNED_4 -> PC32,
     * GOT_LOAD+GOT -> GOTPCREL, UNSIGNED length=3 -> ABS64, others
     * reject. Implicit addend = -4 for the rip-relative kinds. */
    int32_t imp_addend = 0;
    if (macho_reloc_x86_64_kind_map(X86_64_RELOC_UNSIGNED, 3, &imp_addend) !=
        (int)MINO_STENCIL_RELOC_X86_64_ABS64 || imp_addend != 0) {
        fprintf(stderr, "selftest: macho x86_64 kind_map UNSIGNED wrong\n"); failed++;
    }
    if (macho_reloc_x86_64_kind_map(X86_64_RELOC_BRANCH, 2, &imp_addend) !=
        (int)MINO_STENCIL_RELOC_X86_64_PC32 || imp_addend != -4) {
        fprintf(stderr, "selftest: macho x86_64 kind_map BRANCH wrong\n"); failed++;
    }
    if (macho_reloc_x86_64_kind_map(X86_64_RELOC_SIGNED, 2, &imp_addend) !=
        (int)MINO_STENCIL_RELOC_X86_64_PC32 || imp_addend != -4) {
        fprintf(stderr, "selftest: macho x86_64 kind_map SIGNED wrong\n"); failed++;
    }
    if (macho_reloc_x86_64_kind_map(X86_64_RELOC_SIGNED_1, 2, &imp_addend) !=
        (int)MINO_STENCIL_RELOC_X86_64_PC32 || imp_addend != -1) {
        fprintf(stderr, "selftest: macho x86_64 kind_map SIGNED_1 wrong\n"); failed++;
    }
    if (macho_reloc_x86_64_kind_map(X86_64_RELOC_SIGNED_2, 2, &imp_addend) !=
        (int)MINO_STENCIL_RELOC_X86_64_PC32 || imp_addend != -2) {
        fprintf(stderr, "selftest: macho x86_64 kind_map SIGNED_2 wrong\n"); failed++;
    }
    if (macho_reloc_x86_64_kind_map(X86_64_RELOC_SIGNED_4, 2, &imp_addend) !=
        (int)MINO_STENCIL_RELOC_X86_64_PC32 || imp_addend != -4) {
        fprintf(stderr, "selftest: macho x86_64 kind_map SIGNED_4 wrong\n"); failed++;
    }
    if (macho_reloc_x86_64_kind_map(X86_64_RELOC_GOT_LOAD, 2, &imp_addend) !=
        (int)MINO_STENCIL_RELOC_X86_64_GOTPCREL || imp_addend != -4) {
        fprintf(stderr, "selftest: macho x86_64 kind_map GOT_LOAD wrong\n"); failed++;
    }
    if (macho_reloc_x86_64_kind_map(X86_64_RELOC_GOT, 2, &imp_addend) !=
        (int)MINO_STENCIL_RELOC_X86_64_GOTPCREL || imp_addend != -4) {
        fprintf(stderr, "selftest: macho x86_64 kind_map GOT wrong\n"); failed++;
    }
    if (macho_reloc_x86_64_kind_map(0xffu, 2, &imp_addend) != -1) {
        fprintf(stderr, "selftest: macho x86_64 kind_map should reject unknown\n"); failed++;
    }
    /* COFF amd64 reloc-kind map: REL32 -> PC32 with addend -4,
     * ADDR64 -> ABS64 with addend 0, REL32_1 -> PC32 with addend -5,
     * unknowns reject. */
    int32_t coff_addend = 0;
    if (reloc_x86_64_coff_kind_map(IMAGE_REL_AMD64_REL32, &coff_addend) !=
        (int)MINO_STENCIL_RELOC_X86_64_PC32 || coff_addend != -4) {
        fprintf(stderr, "selftest: coff REL32 wrong\n"); failed++;
    }
    if (reloc_x86_64_coff_kind_map(IMAGE_REL_AMD64_ADDR64, &coff_addend) !=
        (int)MINO_STENCIL_RELOC_X86_64_ABS64 || coff_addend != 0) {
        fprintf(stderr, "selftest: coff ADDR64 wrong\n"); failed++;
    }
    if (reloc_x86_64_coff_kind_map(IMAGE_REL_AMD64_REL32_1, &coff_addend) !=
        (int)MINO_STENCIL_RELOC_X86_64_PC32 || coff_addend != -5) {
        fprintf(stderr, "selftest: coff REL32_1 wrong\n"); failed++;
    }
    if (reloc_x86_64_coff_kind_map(0xfffu, &coff_addend) != -1) {
        fprintf(stderr, "selftest: coff kind_map should reject unknown\n");
        failed++;
    }
    if (failed > 0) return 1;
    printf("stencil_extract selftest: OK\n");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Header emission                                                           */
/* ------------------------------------------------------------------------- */

/* stencil_reloc_t + sym_table_intern + write_stencil_header all live
 * in stencil_extract/core.{h,c}. The per-format extract_relocs feed
 * them. */


/* ------------------------------------------------------------------------- */
/* ELF64 parser                                                              */
/* ------------------------------------------------------------------------- */

typedef struct {
    const mblob_t      *blob;
    const elf64_ehdr_t *hdr;
    const elf64_shdr_t *shdrs;       /* section header array              */
    const char         *shstrtab;    /* section-header string table       */
    uint16_t            text_shndx;  /* index of .text in shdrs           */
    const elf64_shdr_t *text_section;
    const elf64_shdr_t *symtab_section;
    const elf64_shdr_t *strtab_section;
    const elf64_shdr_t *rela_text_section;  /* .rela.text (NULL on miss)  */
} elf_view_t;

static int elf_open(const mblob_t *blob, elf_view_t *out)
{
    memset(out, 0, sizeof(*out));
    if (blob->len < sizeof(elf64_ehdr_t)) {
        fprintf(stderr, "stencil_extract: file too small for ELF64 header\n");
        return -1;
    }
    const elf64_ehdr_t *hdr = (const elf64_ehdr_t *)blob->data;
    if (hdr->e_ident[0] != ELF_MAGIC_BYTE_0
        || hdr->e_ident[1] != ELF_MAGIC_BYTE_1
        || hdr->e_ident[2] != ELF_MAGIC_BYTE_2
        || hdr->e_ident[3] != ELF_MAGIC_BYTE_3) {
        fprintf(stderr, "stencil_extract: not an ELF object\n");
        return -1;
    }
    if (hdr->e_ident[EI_CLASS] != ELFCLASS64) {
        fprintf(stderr, "stencil_extract: ELF class %u not supported (need 64-bit)\n",
                hdr->e_ident[EI_CLASS]);
        return -1;
    }
    if (hdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        fprintf(stderr, "stencil_extract: ELF data %u not supported (need LSB)\n",
                hdr->e_ident[EI_DATA]);
        return -1;
    }
    if (hdr->e_shentsize != sizeof(elf64_shdr_t)) {
        fprintf(stderr, "stencil_extract: unexpected ELF shentsize %u (want %zu)\n",
                hdr->e_shentsize, sizeof(elf64_shdr_t));
        return -1;
    }
    if (hdr->e_shoff == 0 || hdr->e_shnum == 0) {
        fprintf(stderr, "stencil_extract: ELF has no section table\n");
        return -1;
    }
    size_t shdrs_end = (size_t)hdr->e_shoff
        + (size_t)hdr->e_shnum * sizeof(elf64_shdr_t);
    if (shdrs_end > blob->len) {
        fprintf(stderr, "stencil_extract: ELF section table out of range\n");
        return -1;
    }
    const elf64_shdr_t *shdrs = (const elf64_shdr_t *)(blob->data + hdr->e_shoff);
    if (hdr->e_shstrndx >= hdr->e_shnum) {
        fprintf(stderr, "stencil_extract: ELF shstrndx %u out of range\n",
                hdr->e_shstrndx);
        return -1;
    }
    const elf64_shdr_t *shstr = &shdrs[hdr->e_shstrndx];
    if ((size_t)shstr->sh_offset + (size_t)shstr->sh_size > blob->len) {
        fprintf(stderr, "stencil_extract: ELF shstrtab out of range\n");
        return -1;
    }
    const char *shstrtab = (const char *)(blob->data + shstr->sh_offset);
    out->blob     = blob;
    out->hdr      = hdr;
    out->shdrs    = shdrs;
    out->shstrtab = shstrtab;
    /* Walk sections. .text is SHT_PROGBITS named ".text"; .symtab is the
     * unique SHT_SYMTAB section; .rela.text is the SHT_RELA section whose
     * sh_info points at the .text section index. */
    for (uint16_t i = 0; i < hdr->e_shnum; i++) {
        const elf64_shdr_t *sh = &shdrs[i];
        if (sh->sh_name >= shstr->sh_size) continue;
        const char *name = shstrtab + sh->sh_name;
        if (sh->sh_type == SHT_PROGBITS && strcmp(name, ".text") == 0) {
            out->text_section = sh;
            out->text_shndx   = i;
        } else if (sh->sh_type == SHT_SYMTAB) {
            out->symtab_section = sh;
        }
    }
    if (out->text_section == NULL) {
        fprintf(stderr, "stencil_extract: ELF has no .text section\n");
        return -1;
    }
    if (out->symtab_section == NULL) {
        fprintf(stderr, "stencil_extract: ELF has no symbol table\n");
        return -1;
    }
    if (out->symtab_section->sh_link >= hdr->e_shnum) {
        fprintf(stderr, "stencil_extract: ELF symtab sh_link out of range\n");
        return -1;
    }
    out->strtab_section = &shdrs[out->symtab_section->sh_link];
    /* Second pass: find .rela.text by sh_info matching text section index. */
    for (uint16_t i = 0; i < hdr->e_shnum; i++) {
        const elf64_shdr_t *sh = &shdrs[i];
        if (sh->sh_type != SHT_RELA) continue;
        if (sh->sh_info == out->text_shndx) {
            out->rela_text_section = sh;
            break;
        }
    }
    /* .rel.text without addends -- ARM64 / x86_64 use RELA so reject. */
    for (uint16_t i = 0; i < hdr->e_shnum; i++) {
        const elf64_shdr_t *sh = &shdrs[i];
        if (sh->sh_type != SHT_REL) continue;
        if (sh->sh_info == out->text_shndx) {
            fprintf(stderr, "stencil_extract: ELF .rel.text without addends "
                    "is not supported; rebuild with RELA-emitting toolchain\n");
            return -1;
        }
    }
    if ((size_t)out->text_section->sh_offset + (size_t)out->text_section->sh_size
        > blob->len) {
        fprintf(stderr, "stencil_extract: ELF .text bytes out of range\n");
        return -1;
    }
    if ((size_t)out->strtab_section->sh_offset
        + (size_t)out->strtab_section->sh_size > blob->len) {
        fprintf(stderr, "stencil_extract: ELF strtab out of range\n");
        return -1;
    }
    return 0;
}

static const char *elf_strtab(const elf_view_t *v) {
    return (const char *)(v->blob->data + v->strtab_section->sh_offset);
}

static const elf64_sym_t *elf_symtab(const elf_view_t *v) {
    if ((size_t)v->symtab_section->sh_offset
        + (size_t)v->symtab_section->sh_size > v->blob->len) return NULL;
    return (const elf64_sym_t *)(v->blob->data + v->symtab_section->sh_offset);
}

static uint64_t elf_symtab_nsyms(const elf_view_t *v) {
    return v->symtab_section->sh_size / sizeof(elf64_sym_t);
}

static int elf_list_symbols(const elf_view_t *v)
{
    const char        *strtab = elf_strtab(v);
    const elf64_sym_t *syms   = elf_symtab(v);
    if (syms == NULL) {
        fprintf(stderr, "stencil_extract: ELF symbol table out of range\n");
        return -1;
    }
    uint64_t n = elf_symtab_nsyms(v);
    for (uint64_t i = 0; i < n; i++) {
        const elf64_sym_t *s = &syms[i];
        if (s->st_shndx != v->text_shndx) continue;
        if (ELF64_ST_TYPE(s->st_info) != STT_FUNC) continue;
        const char *name = strtab + s->st_name;
        printf("  %-32s 0x%08" PRIx64 "  (size=%" PRIu64 " bind=%u)\n",
               name, s->st_value, s->st_size, ELF64_ST_BIND(s->st_info));
    }
    return 0;
}

static int elf_find_symbol(const elf_view_t *v, const char *name,
                           uint64_t *out_offset, uint64_t *out_size)
{
    const char        *strtab = elf_strtab(v);
    const elf64_sym_t *syms   = elf_symtab(v);
    if (syms == NULL) return -1;
    uint64_t n = elf_symtab_nsyms(v);
    for (uint64_t i = 0; i < n; i++) {
        const elf64_sym_t *s = &syms[i];
        if (s->st_shndx != v->text_shndx) continue;
        if (ELF64_ST_TYPE(s->st_info) != STT_FUNC) continue;
        const char *en = strtab + s->st_name;
        if (strcmp(en, name) == 0) {
            *out_offset = s->st_value;
            *out_size   = s->st_size;
            return 0;
        }
    }
    return -1;
}

static int elf_extract_relocs(const elf_view_t *v, uint64_t offset, uint64_t size,
                              stencil_reloc_t *out, int out_cap, int *out_count,
                              const char **sym_table, int *sym_count, int sym_cap)
{
    *out_count = 0;
    if (v->rela_text_section == NULL) return 0;  /* no relocs at all */
    const elf64_shdr_t *rs = v->rela_text_section;
    if (rs->sh_entsize != sizeof(elf64_rela_t)) {
        fprintf(stderr,
                "stencil_extract: ELF .rela.text entsize %" PRIu64
                " unexpected\n", rs->sh_entsize);
        return -1;
    }
    if ((size_t)rs->sh_offset + (size_t)rs->sh_size > v->blob->len) {
        fprintf(stderr, "stencil_extract: ELF rela table out of range\n");
        return -1;
    }
    if (v->hdr->e_machine != EM_AARCH64
        && v->hdr->e_machine != EM_X86_64) {
        fprintf(stderr,
                "stencil_extract: ELF e_machine %u not supported "
                "(AArch64 + x86_64 wired today)\n",
                v->hdr->e_machine);
        return -1;
    }
    const elf64_rela_t *relas =
        (const elf64_rela_t *)(v->blob->data + rs->sh_offset);
    uint64_t n_relas = rs->sh_size / sizeof(elf64_rela_t);
    const char        *strtab = elf_strtab(v);
    const elf64_sym_t *syms   = elf_symtab(v);
    if (syms == NULL) return -1;
    uint64_t n_syms = elf_symtab_nsyms(v);
    for (uint64_t i = 0; i < n_relas; i++) {
        const elf64_rela_t *r = &relas[i];
        if (r->r_offset < offset || r->r_offset >= offset + size) continue;
        uint32_t snum  = elf64_r_sym(r->r_info);
        uint32_t rtype = elf64_r_type(r->r_info);
        if (snum >= n_syms) {
            fprintf(stderr, "stencil_extract: ELF reloc sym %u out of range\n",
                    snum);
            return -1;
        }
        int mapped = (v->hdr->e_machine == EM_AARCH64)
            ? reloc_arm64_elf_kind_map(rtype)
            : reloc_x86_64_elf_kind_map(rtype);
        if (mapped < 0) {
            const char *arch = (v->hdr->e_machine == EM_AARCH64) ? "AArch64"
                                                                  : "x86_64";
            fprintf(stderr,
                    "stencil_extract: unsupported ELF %s reloc %u "
                    "at offset 0x%" PRIx64 "\n",
                    arch, rtype, r->r_offset);
            return -1;
        }
        const elf64_sym_t *s = &syms[snum];
        const char *name = strtab + s->st_name;
        /* ELF strtab carries the source-level name without the Mach-O
         * leading underscore. The downstream resolver expects the
         * source-level form, so pass it through verbatim. */
        int sym_idx = sym_table_intern(name, sym_table, sym_count, sym_cap);
        if (sym_idx < 0) {
            fprintf(stderr, "stencil_extract: too many distinct symbols\n");
            return -1;
        }
        if (*out_count >= out_cap) {
            fprintf(stderr, "stencil_extract: too many relocations per stencil\n");
            return -1;
        }
        out[*out_count].offset    = (uint32_t)(r->r_offset - offset);
        out[*out_count].kind      = (uint32_t)mapped;
        out[*out_count].sym_index = (uint32_t)sym_idx;
        /* ELF carries explicit addends in RELA entries. */
        out[*out_count].addend    = (int32_t)r->r_addend;
        (*out_count)++;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* COFF (PE/COFF) amd64 parser                                               */
/* ------------------------------------------------------------------------- */

/* COFF file header (IMAGE_FILE_HEADER). Microsoft documents the
 * layout in the PE/COFF specification. The layout is 20 bytes and
 * uses 16-bit / 32-bit little-endian fields throughout. */
typedef struct {
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
} coff_file_header_t;

/* IMAGE_SECTION_HEADER, 40 bytes. */
typedef struct {
    char     Name[8];
    uint32_t VirtualSize;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint32_t PointerToRelocations;
    uint32_t PointerToLinenumbers;
    uint16_t NumberOfRelocations;
    uint16_t NumberOfLinenumbers;
    uint32_t Characteristics;
} coff_section_t;

/* IMAGE_SYMBOL, 18 bytes. Packed: the standard `struct` layout would
 * align the fields to 4 bytes; COFF specifies 18 bytes per entry, so
 * the parser indexes raw byte offsets rather than using a packed
 * struct (which is non-portable). The accessors below pull each
 * field via memcpy to avoid alignment issues. */
enum { COFF_SYM_ENTRY_SIZE = 18 };

/* IMAGE_RELOCATION, 10 bytes. Same packing concern as IMAGE_SYMBOL. */
enum { COFF_RELOC_ENTRY_SIZE = 10 };

/* Storage classes (subset). */
#define IMAGE_SYM_CLASS_EXTERNAL  2u
#define IMAGE_SYM_CLASS_STATIC    3u

/* Section number specials. */
#define IMAGE_SYM_UNDEFINED   0
#define IMAGE_SYM_ABSOLUTE   -1
#define IMAGE_SYM_DEBUG      -2

typedef struct {
    const mblob_t            *blob;
    const coff_file_header_t *hdr;
    const coff_section_t     *sections;        /* section header array  */
    uint32_t                  text_index;      /* 1-based section index */
    const coff_section_t     *text_section;
    const uint8_t            *symtab;          /* raw 18-byte entries   */
    uint32_t                  nsyms;
    const char               *strtab;          /* points at strtab start*/
    uint32_t                  strtab_size;
} coff_view_t;

/* COFF symbol-name reader: returns a pointer into either the inline
 * 8-byte Name field (which we copy into a small static buffer because
 * it's not null-terminated when exactly 8 chars long) or into the
 * string table. The caller doesn't free; the returned pointer is
 * stable for the lifetime of the coff_view_t. */
static const char *coff_symbol_name(const coff_view_t *v, uint32_t idx,
                                     char inline_buf[9])
{
    const uint8_t *sym = v->symtab + (size_t)idx * COFF_SYM_ENTRY_SIZE;
    uint32_t zero_check;
    memcpy(&zero_check, sym, 4);
    if (zero_check == 0) {
        uint32_t off;
        memcpy(&off, sym + 4, 4);
        if (off >= v->strtab_size) return NULL;
        return v->strtab + off;
    }
    memcpy(inline_buf, sym, 8);
    inline_buf[8] = '\0';
    return inline_buf;
}

static int32_t coff_symbol_section_number(const coff_view_t *v, uint32_t idx)
{
    const uint8_t *sym = v->symtab + (size_t)idx * COFF_SYM_ENTRY_SIZE;
    int16_t sn;
    memcpy(&sn, sym + 12, 2);
    return (int32_t)sn;
}

static uint32_t coff_symbol_value(const coff_view_t *v, uint32_t idx)
{
    const uint8_t *sym = v->symtab + (size_t)idx * COFF_SYM_ENTRY_SIZE;
    uint32_t v32;
    memcpy(&v32, sym + 8, 4);
    return v32;
}

static uint8_t coff_symbol_storage_class(const coff_view_t *v, uint32_t idx)
{
    const uint8_t *sym = v->symtab + (size_t)idx * COFF_SYM_ENTRY_SIZE;
    return sym[16];
}

static uint8_t coff_symbol_num_aux(const coff_view_t *v, uint32_t idx)
{
    const uint8_t *sym = v->symtab + (size_t)idx * COFF_SYM_ENTRY_SIZE;
    return sym[17];
}

static int coff_open(const mblob_t *blob, coff_view_t *out)
{
    memset(out, 0, sizeof(*out));
    if (blob->len < sizeof(coff_file_header_t)) {
        fprintf(stderr, "stencil_extract: file too small for COFF header\n");
        return -1;
    }
    const coff_file_header_t *hdr = (const coff_file_header_t *)blob->data;
    if (hdr->Machine != COFF_MACHINE_AMD64) {
        fprintf(stderr,
                "stencil_extract: COFF machine 0x%04x not supported "
                "(amd64 only)\n", hdr->Machine);
        return -1;
    }
    if (hdr->SizeOfOptionalHeader != 0) {
        fprintf(stderr,
                "stencil_extract: COFF object should not carry "
                "an optional header (got %u bytes)\n",
                hdr->SizeOfOptionalHeader);
        return -1;
    }
    out->blob = blob;
    out->hdr  = hdr;
    size_t sect_off = sizeof(coff_file_header_t);
    size_t sect_end = sect_off
        + (size_t)hdr->NumberOfSections * sizeof(coff_section_t);
    if (sect_end > blob->len) {
        fprintf(stderr, "stencil_extract: COFF section table out of range\n");
        return -1;
    }
    out->sections = (const coff_section_t *)(blob->data + sect_off);
    /* Locate the __text section by name. COFF uses ".text" (5 chars,
     * null-padded). The section number stored in symtab entries is
     * 1-based; we record both the index and the section pointer. */
    for (uint16_t i = 0; i < hdr->NumberOfSections; i++) {
        const coff_section_t *s = &out->sections[i];
        if (strncmp(s->Name, ".text", 8) == 0) {
            out->text_index   = (uint32_t)(i + 1);
            out->text_section = s;
            break;
        }
    }
    if (out->text_section == NULL) {
        fprintf(stderr, "stencil_extract: COFF .text section missing\n");
        return -1;
    }
    /* Symbol table follows the section data; the file header points
     * at it. Each entry is 18 bytes (no padding). The string table
     * follows immediately; its first 4 bytes are the total size. */
    size_t symtab_off = hdr->PointerToSymbolTable;
    size_t symtab_end = symtab_off
        + (size_t)hdr->NumberOfSymbols * COFF_SYM_ENTRY_SIZE;
    if (symtab_off == 0 || symtab_end > blob->len) {
        fprintf(stderr, "stencil_extract: COFF symbol table out of range\n");
        return -1;
    }
    out->symtab = blob->data + symtab_off;
    out->nsyms  = hdr->NumberOfSymbols;
    size_t strtab_off = symtab_end;
    if (strtab_off + 4 > blob->len) {
        fprintf(stderr, "stencil_extract: COFF string table missing\n");
        return -1;
    }
    uint32_t strtab_size;
    memcpy(&strtab_size, blob->data + strtab_off, 4);
    if (strtab_off + strtab_size > blob->len) {
        fprintf(stderr,
                "stencil_extract: COFF string table out of range "
                "(size=%u)\n", strtab_size);
        return -1;
    }
    /* String table starts at strtab_off; offsets in symbol entries
     * are relative to strtab_off (the size field counts toward
     * itself, so byte 4 is the first real character). */
    out->strtab      = (const char *)(blob->data + strtab_off);
    out->strtab_size = strtab_size;
    return 0;
}

static int coff_list_symbols(const coff_view_t *v)
{
    printf("symbols in .text (raw size=%u, raw offset=0x%x):\n",
           v->text_section->SizeOfRawData,
           v->text_section->PointerToRawData);
    for (uint32_t i = 0; i < v->nsyms; ) {
        char inline_buf[9];
        const char *name = coff_symbol_name(v, i, inline_buf);
        int32_t  snum    = coff_symbol_section_number(v, i);
        uint32_t val     = coff_symbol_value(v, i);
        uint8_t  cls     = coff_symbol_storage_class(v, i);
        uint8_t  nax     = coff_symbol_num_aux(v, i);
        if ((uint32_t)snum == v->text_index
            && (cls == IMAGE_SYM_CLASS_EXTERNAL
                || cls == IMAGE_SYM_CLASS_STATIC)) {
            printf("  %-32s 0x%08x  (class=%u)\n",
                   name ? name : "<?>", val, cls);
        }
        i += (uint32_t)(1u + nax);
    }
    return 0;
}

static int coff_find_symbol(const coff_view_t *v, const char *name,
                             uint64_t *out_offset, uint64_t *out_size)
{
    /* COFF doesn't record symbol size in the IMAGE_SYMBOL entry; we
     * derive `size` by scanning the next symbol in __text. The
     * scanned-from value bumps to the section end if no later
     * symbol is found. */
    uint32_t target_idx = 0xffffffffu;
    uint32_t target_val = 0;
    for (uint32_t i = 0; i < v->nsyms; ) {
        char        inline_buf[9];
        const char *sname = coff_symbol_name(v, i, inline_buf);
        int32_t     snum  = coff_symbol_section_number(v, i);
        if ((uint32_t)snum == v->text_index && sname != NULL
            && strcmp(sname, name) == 0) {
            target_idx = i;
            target_val = coff_symbol_value(v, i);
            break;
        }
        uint8_t nax = coff_symbol_num_aux(v, i);
        i += (uint32_t)(1u + nax);
    }
    if (target_idx == 0xffffffffu) return -1;
    uint32_t next_val = v->text_section->SizeOfRawData;
    for (uint32_t i = 0; i < v->nsyms; ) {
        if (i != target_idx) {
            int32_t snum = coff_symbol_section_number(v, i);
            if ((uint32_t)snum == v->text_index) {
                uint32_t val = coff_symbol_value(v, i);
                if (val > target_val && val < next_val) {
                    next_val = val;
                }
            }
        }
        uint8_t nax = coff_symbol_num_aux(v, i);
        i += (uint32_t)(1u + nax);
    }
    *out_offset = (uint64_t)target_val;
    *out_size   = (uint64_t)(next_val - target_val);
    return 0;
}

/* Mach-O REL relocations and COFF use implicit addends. The COFF
 * reloc-kind map returns the addend for each REL32 variant in the
 * second out-param. UNSIGNED / ADDR64 are non-pcrel so addend = 0. */
static int reloc_x86_64_coff_kind_map(uint32_t coff_kind,
                                       int32_t *out_implicit_addend)
{
    *out_implicit_addend = 0;
    switch (coff_kind) {
    case IMAGE_REL_AMD64_ADDR64:   return (int)MINO_STENCIL_RELOC_X86_64_ABS64;
    case IMAGE_REL_AMD64_REL32:    *out_implicit_addend = -4;
                                   return (int)MINO_STENCIL_RELOC_X86_64_PC32;
    case IMAGE_REL_AMD64_REL32_1:  *out_implicit_addend = -5;
                                   return (int)MINO_STENCIL_RELOC_X86_64_PC32;
    default: return -1;
    }
}

static int coff_extract_relocs(const coff_view_t *v,
                                uint64_t offset, uint64_t size,
                                stencil_reloc_t *out, int out_cap,
                                int *out_count,
                                const char **sym_table, int *sym_count,
                                int sym_cap)
{
    *out_count = 0;
    const coff_section_t *sect = v->text_section;
    if (sect->NumberOfRelocations == 0) return 0;
    size_t reloc_off = sect->PointerToRelocations;
    size_t reloc_end = reloc_off
        + (size_t)sect->NumberOfRelocations * COFF_RELOC_ENTRY_SIZE;
    if (reloc_off == 0 || reloc_end > v->blob->len) {
        fprintf(stderr, "stencil_extract: COFF reloc table out of range\n");
        return -1;
    }
    const uint8_t *r_base = v->blob->data + reloc_off;
    for (uint16_t i = 0; i < sect->NumberOfRelocations; i++) {
        const uint8_t *r = r_base + (size_t)i * COFF_RELOC_ENTRY_SIZE;
        uint32_t addr; memcpy(&addr, r,     4);
        uint32_t snum; memcpy(&snum, r + 4, 4);
        uint16_t kind; memcpy(&kind, r + 8, 2);
        if ((uint64_t)addr < offset || (uint64_t)addr >= offset + size) continue;
        if (snum >= v->nsyms) {
            fprintf(stderr,
                    "stencil_extract: COFF reloc symbol index %u out of range\n",
                    snum);
            return -1;
        }
        int32_t implicit_addend = 0;
        int mapped = reloc_x86_64_coff_kind_map((uint32_t)kind,
                                                 &implicit_addend);
        if (mapped < 0) {
            fprintf(stderr,
                    "stencil_extract: unsupported COFF reloc kind %u "
                    "at offset 0x%x\n", kind, addr);
            return -1;
        }
        char        inline_buf[9];
        const char *name = coff_symbol_name(v, snum, inline_buf);
        if (name == NULL) {
            fprintf(stderr,
                    "stencil_extract: COFF reloc symbol name missing "
                    "(index=%u)\n", snum);
            return -1;
        }
        /* COFF (PE) symbol naming on Windows ABI: function symbols
         * have no leading underscore (unlike i386 PE). Pass through
         * verbatim. */
        int sym_idx = sym_table_intern(name, sym_table, sym_count, sym_cap);
        if (sym_idx < 0) {
            fprintf(stderr, "stencil_extract: too many distinct symbols\n");
            return -1;
        }
        if (*out_count >= out_cap) {
            fprintf(stderr, "stencil_extract: too many relocations per stencil\n");
            return -1;
        }
        out[*out_count].offset    = (uint32_t)((uint64_t)addr - offset);
        out[*out_count].kind      = (uint32_t)mapped;
        out[*out_count].sym_index = (uint32_t)sym_idx;
        out[*out_count].addend    = implicit_addend;
        (*out_count)++;
    }
    return 0;
}

static int coff_emit_stencil_header(const coff_view_t *v, const char *symbol,
                                     uint64_t offset, uint64_t size,
                                     const char *out_path, int append)
{
    enum { MAX_RELOCS = 64, MAX_SYMS = 32 };
    stencil_reloc_t relocs[MAX_RELOCS];
    const char     *syms[MAX_SYMS];
    int             nrelocs = 0, nsyms = 0;
    if (coff_extract_relocs(v, offset, size, relocs, MAX_RELOCS, &nrelocs,
                             syms, &nsyms, MAX_SYMS) != 0) return -1;
    const uint8_t *text = v->blob->data + v->text_section->PointerToRawData;
    const uint8_t *body = text + offset;
    return write_stencil_header(symbol, body, size, relocs, nrelocs,
                                syms, nsyms, out_path, append);
}

/* The format-agnostic write_stencil_header lives in
 * stencil_extract/core.c. Each per-format emit_stencil_header funnels
 * its extracted body+relocs+syms through it. */

/* The Mach-O entry point macho_emit_stencil_header lives in
 * stencil_extract/macho.c. */

/* ELF entry point: parallel shape to macho_emit_stencil_header. The ELF
 * symbol table records the body size directly in st_size; for Mach-O
 * the size was derived by scanning the next symbol's address. */
static int elf_emit_stencil_header(const elf_view_t *v, const char *symbol,
                                   uint64_t offset, uint64_t size,
                                   const char *out_path, int append)
{
    enum { MAX_RELOCS = 64, MAX_SYMS = 32 };
    stencil_reloc_t relocs[MAX_RELOCS];
    const char     *syms[MAX_SYMS];
    int             nrelocs = 0, nsyms = 0;
    if (elf_extract_relocs(v, offset, size, relocs, MAX_RELOCS, &nrelocs,
                           syms, &nsyms, MAX_SYMS) != 0) return -1;
    const uint8_t *text = v->blob->data + v->text_section->sh_offset;
    const uint8_t *body = text + offset;
    return write_stencil_header(symbol, body, size, relocs, nrelocs,
                                syms, nsyms, out_path, append);
}

/* ------------------------------------------------------------------------- */
/* main                                                                      */
/* ------------------------------------------------------------------------- */

static void usage(void)
{
    fprintf(stderr,
            "usage:\n"
            "  stencil_extract --selftest\n"
            "  stencil_extract <obj> --list\n"
            "  stencil_extract [--append] <obj> <symbol> <out-header>\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 2; }
    if (strcmp(argv[1], "--selftest") == 0) return selftest();
    int append = 0;
    int argi = 1;
    if (argc > argi && strcmp(argv[argi], "--append") == 0) {
        append = 1;
        argi++;
    }
    if (argc < argi + 2) { usage(); return 2; }
    mblob_t blob;
    if (read_file(argv[argi], &blob) != 0) return 1;
    /* Sniff the object file format. Mach-O 64 starts with 0xfeedfacf
     * (little-endian magic); ELF starts with 0x7f 'E' 'L' 'F'. COFF
     * amd64 starts with the 2-byte machine ID 0x8664. The Mach-O and
     * ELF parsers are fully wired; the COFF path returns a
     * placeholder error until the Windows platform release lands the
     * parser + VirtualAlloc adapter. */
    int is_elf = (blob.len >= 4
                  && blob.data[0] == ELF_MAGIC_BYTE_0
                  && blob.data[1] == ELF_MAGIC_BYTE_1
                  && blob.data[2] == ELF_MAGIC_BYTE_2
                  && blob.data[3] == ELF_MAGIC_BYTE_3);
    /* COFF amd64: little-endian machine ID is the first 2 bytes. */
    int is_coff = (!is_elf && blob.len >= 2
                   && blob.data[0] == (COFF_MACHINE_AMD64 & 0xff)
                   && blob.data[1] == ((COFF_MACHINE_AMD64 >> 8) & 0xff));
    int rc = 0;
    if (is_elf) {
        elf_view_t v;
        if (elf_open(&blob, &v) != 0) { blob_free(&blob); return 1; }
        if (strcmp(argv[argi + 1], "--list") == 0) {
            printf("symbols in .text (size=%" PRIu64 ", offset=0x%" PRIx64 "):\n",
                   v.text_section->sh_size, (uint64_t)v.text_section->sh_offset);
            rc = elf_list_symbols(&v);
        } else if (argc >= argi + 3) {
            const char *symbol = argv[argi + 1];
            const char *out    = argv[argi + 2];
            /* ELF symtab carries the source-level name verbatim; no
             * leading-underscore stripping needed. */
            uint64_t off, sz;
            if (elf_find_symbol(&v, symbol, &off, &sz) != 0) {
                fprintf(stderr, "stencil_extract: symbol '%s' not found\n",
                        symbol);
                rc = 1;
            } else {
                rc = elf_emit_stencil_header(&v, symbol, off, sz, out, append);
            }
        } else {
            usage();
            rc = 2;
        }
    } else if (is_coff) {
        coff_view_t v;
        if (coff_open(&blob, &v) != 0) { blob_free(&blob); return 1; }
        if (strcmp(argv[argi + 1], "--list") == 0) {
            rc = coff_list_symbols(&v);
        } else if (argc >= argi + 3) {
            const char *symbol = argv[argi + 1];
            const char *out    = argv[argi + 2];
            /* COFF (PE) on x86_64 doesn't prefix C functions with
             * underscore (unlike i386 PE). Pass the source-level
             * name through verbatim. */
            uint64_t off, sz;
            if (coff_find_symbol(&v, symbol, &off, &sz) != 0) {
                fprintf(stderr, "stencil_extract: symbol '%s' not found\n",
                        symbol);
                rc = 1;
            } else {
                rc = coff_emit_stencil_header(&v, symbol, off, sz, out, append);
            }
        } else {
            usage();
            rc = 2;
        }
    } else {
        macho_view_t v;
        if (macho_open(&blob, &v) != 0) { blob_free(&blob); return 1; }
        if (strcmp(argv[argi + 1], "--list") == 0) {
            printf("symbols in __TEXT,__text (size=%" PRIu64 ", offset=0x%" PRIx64 "):\n",
                   v.text_section->size, (uint64_t)v.text_section->offset);
            rc = macho_list_symbols(&v);
        } else if (argc >= argi + 3) {
            const char *symbol = argv[argi + 1];
            const char *out    = argv[argi + 2];
            /* Mach-O linkers prefix C function names with an underscore.
             * Try the literal name first; on miss prepend `_` so users
             * can pass the source-level name. */
            uint64_t off, sz;
            if (macho_find_symbol(&v, symbol, &off, &sz) != 0) {
                char buf[256];
                snprintf(buf, sizeof(buf), "_%s", symbol);
                if (macho_find_symbol(&v, buf, &off, &sz) != 0) {
                    fprintf(stderr, "stencil_extract: symbol '%s' not found\n",
                            symbol);
                    rc = 1;
                } else {
                    rc = macho_emit_stencil_header(&v, symbol, off, sz, out, append);
                }
            } else {
                rc = macho_emit_stencil_header(&v, symbol, off, sz, out, append);
            }
        } else {
            usage();
            rc = 2;
        }
    }
    blob_free(&blob);
    return rc;
}
