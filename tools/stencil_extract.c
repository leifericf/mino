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

#include "stencil_extract/coff.h"
#include "stencil_extract/core.h"
#include "stencil_extract/elf.h"
#include "stencil_extract/macho.h"
#include "stencil_extract/selftest.h"

/* ------------------------------------------------------------------------- */
/* Format-magic constants                                                    */
/* ------------------------------------------------------------------------- */

/* Each per-format parser lives in its own module:
 *   - stencil_extract/macho.{h,c}  Mach-O 64 (Darwin ARM64 + x86_64)
 *   - stencil_extract/elf.{h,c}    ELF64 (Linux ARM64 + x86_64)
 *   - stencil_extract/coff.{h,c}   PE/COFF amd64 (Windows x86_64)
 * The MINO_STENCIL_RELOC_* host enum, stencil_reloc_t, and
 * write_stencil_header live in stencil_extract/core.{h,c}. */

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
    if (elf_reloc_arm64_kind_map(R_AARCH64_ABS64) !=
        (int)MINO_STENCIL_RELOC_ABS64) {
        fprintf(stderr, "selftest: elf kind_map ABS64 wrong\n"); failed++;
    }
    if (elf_reloc_arm64_kind_map(R_AARCH64_CALL26) !=
        (int)MINO_STENCIL_RELOC_ARM64_BRANCH26) {
        fprintf(stderr, "selftest: elf kind_map CALL26 wrong\n"); failed++;
    }
    if (elf_reloc_arm64_kind_map(R_AARCH64_JUMP26) !=
        (int)MINO_STENCIL_RELOC_ARM64_BRANCH26) {
        fprintf(stderr, "selftest: elf kind_map JUMP26 wrong\n"); failed++;
    }
    if (elf_reloc_arm64_kind_map(R_AARCH64_ADR_PREL_PG_HI21) !=
        (int)MINO_STENCIL_RELOC_ARM64_PAGE21) {
        fprintf(stderr, "selftest: elf kind_map ADR_PREL_PG_HI21 wrong\n"); failed++;
    }
    if (elf_reloc_arm64_kind_map(R_AARCH64_ADD_ABS_LO12_NC) !=
        (int)MINO_STENCIL_RELOC_ARM64_PAGEOFF12) {
        fprintf(stderr, "selftest: elf kind_map ADD_ABS_LO12_NC wrong\n"); failed++;
    }
    if (elf_reloc_arm64_kind_map(R_AARCH64_LDST64_ABS_LO12_NC) !=
        (int)MINO_STENCIL_RELOC_ARM64_PAGEOFF12) {
        fprintf(stderr, "selftest: elf kind_map LDST64_ABS_LO12_NC wrong\n"); failed++;
    }
    if (elf_reloc_arm64_kind_map(R_AARCH64_ADR_GOT_PAGE) !=
        (int)MINO_STENCIL_RELOC_ARM64_GOT_LOAD_PAGE21) {
        fprintf(stderr, "selftest: elf kind_map ADR_GOT_PAGE wrong\n"); failed++;
    }
    if (elf_reloc_arm64_kind_map(R_AARCH64_LD64_GOT_LO12_NC) !=
        (int)MINO_STENCIL_RELOC_ARM64_GOT_LOAD_PAGEOFF12) {
        fprintf(stderr, "selftest: elf kind_map LD64_GOT_LO12_NC wrong\n"); failed++;
    }
    if (elf_reloc_arm64_kind_map(0xffffffffu) != -1) {
        fprintf(stderr, "selftest: elf kind_map should reject unknown\n"); failed++;
    }
    /* x86_64 ELF reloc-kind map: ABS64, PC32 (covers PLT32),
     * GOTPCREL (covers GOTPCRELX and REX_GOTPCRELX), unknown rejects. */
    if (elf_reloc_x86_64_kind_map(R_X86_64_64) !=
        (int)MINO_STENCIL_RELOC_X86_64_ABS64) {
        fprintf(stderr, "selftest: x86_64 kind_map ABS64 wrong\n"); failed++;
    }
    if (elf_reloc_x86_64_kind_map(R_X86_64_PC32) !=
        (int)MINO_STENCIL_RELOC_X86_64_PC32) {
        fprintf(stderr, "selftest: x86_64 kind_map PC32 wrong\n"); failed++;
    }
    if (elf_reloc_x86_64_kind_map(R_X86_64_PLT32) !=
        (int)MINO_STENCIL_RELOC_X86_64_PC32) {
        fprintf(stderr, "selftest: x86_64 kind_map PLT32 wrong\n"); failed++;
    }
    if (elf_reloc_x86_64_kind_map(R_X86_64_GOTPCREL) !=
        (int)MINO_STENCIL_RELOC_X86_64_GOTPCREL) {
        fprintf(stderr, "selftest: x86_64 kind_map GOTPCREL wrong\n"); failed++;
    }
    if (elf_reloc_x86_64_kind_map(R_X86_64_GOTPCRELX) !=
        (int)MINO_STENCIL_RELOC_X86_64_GOTPCREL) {
        fprintf(stderr, "selftest: x86_64 kind_map GOTPCRELX wrong\n"); failed++;
    }
    if (elf_reloc_x86_64_kind_map(R_X86_64_REX_GOTPCRELX) !=
        (int)MINO_STENCIL_RELOC_X86_64_GOTPCREL) {
        fprintf(stderr, "selftest: x86_64 kind_map REX_GOTPCRELX wrong\n"); failed++;
    }
    if (elf_reloc_x86_64_kind_map(0xffffffffu) != -1) {
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
    if (coff_reloc_x86_64_kind_map(IMAGE_REL_AMD64_REL32, &coff_addend) !=
        (int)MINO_STENCIL_RELOC_X86_64_PC32 || coff_addend != -4) {
        fprintf(stderr, "selftest: coff REL32 wrong\n"); failed++;
    }
    if (coff_reloc_x86_64_kind_map(IMAGE_REL_AMD64_ADDR64, &coff_addend) !=
        (int)MINO_STENCIL_RELOC_X86_64_ABS64 || coff_addend != 0) {
        fprintf(stderr, "selftest: coff ADDR64 wrong\n"); failed++;
    }
    if (coff_reloc_x86_64_kind_map(IMAGE_REL_AMD64_REL32_1, &coff_addend) !=
        (int)MINO_STENCIL_RELOC_X86_64_PC32 || coff_addend != -5) {
        fprintf(stderr, "selftest: coff REL32_1 wrong\n"); failed++;
    }
    if (coff_reloc_x86_64_kind_map(0xfffu, &coff_addend) != -1) {
        fprintf(stderr, "selftest: coff kind_map should reject unknown\n");
        failed++;
    }
    /* Per-format synthetic-blob unit tests. Each builds a tiny
     * in-memory object file (Mach-O / ELF / COFF) with a known body,
     * symbol table, and reloc table, runs the parser, and asserts the
     * extracted records match expected values. Catches parser
     * regressions independent of compiling the project's own .c files
     * into .o. */
    failed += selftest_macho_synthetic();
    failed += selftest_elf_synthetic();
    failed += selftest_coff_synthetic();
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



/* The format-agnostic write_stencil_header lives in
 * stencil_extract/core.c. Each per-format emit_stencil_header funnels
 * its extracted body+relocs+syms through it. */

/* The Mach-O entry point macho_emit_stencil_header lives in
 * stencil_extract/macho.c. */

/* The ELF entry point elf_emit_stencil_header lives in
 * stencil_extract/elf.c. */

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
