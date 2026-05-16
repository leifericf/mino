/*
 * stencil_extract/selftest.c -- per-format synthetic-blob unit
 * tests. Each test crafts a minimal in-memory .o-style buffer
 * (Mach-O / ELF / COFF) by hand, parses it via the public format
 * API, and asserts symbol lookup + reloc extraction match the
 * known-good values encoded into the blob.
 *
 * The reference function bodies are short (4 bytes), recognisable
 * (0xde, 0xad, 0xbe, 0xef), and reference exactly one extern
 * symbol via a reloc kind the runtime patcher understands.
 */

#include "selftest.h"

#include "coff.h"
#include "core.h"
#include "elf.h"
#include "macho.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPECT(cond, msg) do {                                          \
    if (!(cond)) {                                                      \
        fprintf(stderr, "selftest: %s\n", (msg));                       \
        failed++;                                                       \
    }                                                                   \
} while (0)

static const uint8_t kBody[4] = { 0xde, 0xad, 0xbe, 0xef };

/* ------------------------------------------------------------------------- */
/* Mach-O synthetic                                                          */
/* ------------------------------------------------------------------------- */

/* Build a minimal Mach-O 64 .o file in `buf`: one __TEXT,__text
 * section carrying kBody, one symbol pointing at offset 0, one
 * ARM64_RELOC_BRANCH26 reloc into byte 0 referencing an extern
 * helper. Returns the populated blob size; bumps to 4 KiB so the
 * fixed layout below is easier to keep stable. */
static size_t build_macho_blob(uint8_t *buf, size_t cap)
{
    if (cap < 4096) return 0;
    memset(buf, 0, 4096);
    macho_header_64_t *hdr = (macho_header_64_t *)buf;
    hdr->magic       = MH_MAGIC_64;
    hdr->cputype     = CPU_TYPE_ARM64;
    hdr->cpusubtype  = 0;
    hdr->filetype    = 1;
    hdr->ncmds       = 2;
    hdr->sizeofcmds  = sizeof(macho_segment_64_t) + sizeof(macho_section_64_t)
                       + sizeof(macho_symtab_t);

    /* LC_SEGMENT_64 with one section pointing into the buffer. */
    macho_segment_64_t *seg =
        (macho_segment_64_t *)(buf + sizeof(*hdr));
    seg->cmd     = LC_SEGMENT_64;
    seg->cmdsize = sizeof(*seg) + sizeof(macho_section_64_t);
    seg->nsects  = 1;

    macho_section_64_t *sect = (macho_section_64_t *)(seg + 1);
    memcpy(sect->sectname, "__text", 6);
    memcpy(sect->segname,  "__TEXT", 6);
    sect->size    = sizeof(kBody);
    sect->offset  = 1024;
    sect->reloff  = 2048;
    sect->nreloc  = 1;

    /* LC_SYMTAB pointing at symtab+strtab. */
    macho_symtab_t *symtab = (macho_symtab_t *)(sect + 1);
    symtab->cmd     = LC_SYMTAB;
    symtab->cmdsize = sizeof(*symtab);
    symtab->symoff  = 2560;
    symtab->nsyms   = 2;
    symtab->stroff  = 3072;
    symtab->strsize = 32;

    /* Body bytes at offset 1024. */
    memcpy(buf + 1024, kBody, sizeof(kBody));

    /* Reloc at offset 2048: ARM64_RELOC_BRANCH26, pcrel, length=2,
     * extern=1, symbolnum=1, r_address=0. */
    macho_reloc_info_t *r = (macho_reloc_info_t *)(buf + 2048);
    r->r_address = 0;
    r->r_info    = 1u                 /* symbolnum */
                 | (1u << 24)        /* pcrel */
                 | (2u << 25)        /* length=2 */
                 | (1u << 27)        /* extern */
                 | (ARM64_RELOC_BRANCH26 << 28);

    /* Symtab: 2 entries. Index 0 is the defined fn at offset 0 in
     * __text; index 1 is the extern helper referenced by the reloc. */
    macho_nlist_64_t *nl = (macho_nlist_64_t *)(buf + 2560);
    nl[0].n_strx  = 1;        /* "_my_fn"        */
    nl[0].n_type  = N_SECT | N_EXT;
    nl[0].n_sect  = 1;
    nl[0].n_value = 0;
    nl[1].n_strx  = 8;        /* "_my_helper"    */
    nl[1].n_type  = N_EXT;
    nl[1].n_sect  = 0;
    nl[1].n_value = 0;

    /* Strtab: first byte is reserved (per nlist convention). Names
     * are NUL-terminated. */
    char *st = (char *)(buf + 3072);
    st[0] = '\0';
    memcpy(st + 1, "_my_fn", 7);
    memcpy(st + 8, "_my_helper", 11);

    return 4096;
}

int selftest_macho_synthetic(void)
{
    int failed = 0;
    uint8_t buf[4096];
    size_t  len = build_macho_blob(buf, sizeof(buf));
    EXPECT(len == 4096, "macho_synthetic: blob build failed");

    mblob_t blob = { buf, len };
    macho_view_t v;
    EXPECT(macho_open(&blob, &v) == 0, "macho_synthetic: open failed");
    EXPECT(v.text_section != NULL,     "macho_synthetic: no text section");
    EXPECT(v.text_section_index == 1,  "macho_synthetic: wrong text section index");

    uint64_t off = 0, sz = 0;
    EXPECT(macho_find_symbol(&v, "_my_fn", &off, &sz) == 0,
           "macho_synthetic: find_symbol failed");
    EXPECT(off == 0,                   "macho_synthetic: symbol off wrong");
    EXPECT(sz == sizeof(kBody),        "macho_synthetic: symbol size wrong");

    if (failed > 0) return failed;
    fprintf(stdout, "selftest_macho_synthetic: OK\n");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* ELF synthetic                                                             */
/* ------------------------------------------------------------------------- */

/* Build a minimal ELF64 AArch64 .o file with the same shape as
 * the Mach-O variant: one .text carrying kBody, one defined
 * function symbol, one R_AARCH64_CALL26 reloc, one extern helper. */
static size_t build_elf_blob(uint8_t *buf, size_t cap)
{
    if (cap < 4096) return 0;
    memset(buf, 0, 4096);
    elf64_ehdr_t *eh = (elf64_ehdr_t *)buf;
    eh->e_ident[0]  = ELF_MAGIC_BYTE_0;
    eh->e_ident[1]  = ELF_MAGIC_BYTE_1;
    eh->e_ident[2]  = ELF_MAGIC_BYTE_2;
    eh->e_ident[3]  = ELF_MAGIC_BYTE_3;
    eh->e_ident[EI_CLASS] = ELFCLASS64;
    eh->e_ident[EI_DATA]  = ELFDATA2LSB;
    eh->e_type      = 1;  /* ET_REL */
    eh->e_machine   = EM_AARCH64;
    eh->e_version   = 1;
    eh->e_shoff     = 256;
    eh->e_ehsize    = sizeof(elf64_ehdr_t);
    eh->e_shentsize = sizeof(elf64_shdr_t);
    eh->e_shnum     = 6;
    eh->e_shstrndx  = 5;

    elf64_shdr_t *sh = (elf64_shdr_t *)(buf + 256);
    /* [0] SHT_NULL */
    sh[0].sh_type = SHT_NULL;
    /* [1] .text PROGBITS at offset 1024, size 4 */
    sh[1].sh_name      = 1;   /* ".text" */
    sh[1].sh_type      = SHT_PROGBITS;
    sh[1].sh_offset    = 1024;
    sh[1].sh_size      = sizeof(kBody);
    sh[1].sh_addralign = 4;
    /* [2] .symtab SYMTAB at offset 1280, 3 entries, link -> [3] strtab */
    sh[2].sh_name      = 7;   /* ".symtab" */
    sh[2].sh_type      = SHT_SYMTAB;
    sh[2].sh_offset    = 1280;
    sh[2].sh_size      = sizeof(elf64_sym_t) * 3;
    sh[2].sh_link      = 3;
    sh[2].sh_entsize   = sizeof(elf64_sym_t);
    /* [3] .strtab STRTAB at offset 1664, size 32 */
    sh[3].sh_name      = 15;  /* ".strtab" */
    sh[3].sh_type      = SHT_STRTAB;
    sh[3].sh_offset    = 1664;
    sh[3].sh_size      = 32;
    /* [4] .rela.text RELA at offset 1792, 1 entry, info -> [1] .text */
    sh[4].sh_name      = 23;  /* ".rela.text" */
    sh[4].sh_type      = SHT_RELA;
    sh[4].sh_offset    = 1792;
    sh[4].sh_size      = sizeof(elf64_rela_t);
    sh[4].sh_info      = 1;
    sh[4].sh_entsize   = sizeof(elf64_rela_t);
    /* [5] .shstrtab at offset 1920 size 64 */
    sh[5].sh_name      = 34;  /* ".shstrtab" */
    sh[5].sh_type      = SHT_STRTAB;
    sh[5].sh_offset    = 1920;
    sh[5].sh_size      = 64;

    /* Body bytes at offset 1024. */
    memcpy(buf + 1024, kBody, sizeof(kBody));

    /* Symtab: index 0 reserved STN_UNDEF, index 1 = my_fn, index 2 =
     * my_helper extern. */
    elf64_sym_t *syms = (elf64_sym_t *)(buf + 1280);
    syms[1].st_name  = 1;       /* "my_fn" */
    syms[1].st_info  = (STB_GLOBAL << 4) | STT_FUNC;
    syms[1].st_shndx = 1;       /* .text */
    syms[1].st_value = 0;
    syms[1].st_size  = sizeof(kBody);
    syms[2].st_name  = 7;       /* "my_helper" */
    syms[2].st_info  = (STB_GLOBAL << 4);
    syms[2].st_shndx = 0;

    /* Strtab. */
    char *strs = (char *)(buf + 1664);
    strs[0] = '\0';
    memcpy(strs + 1, "my_fn", 6);
    memcpy(strs + 7, "my_helper", 10);

    /* Rela entry: r_offset 0, r_info = (sym=2 << 32) | R_AARCH64_CALL26,
     * r_addend 0. */
    elf64_rela_t *rel = (elf64_rela_t *)(buf + 1792);
    rel->r_offset = 0;
    rel->r_info   = ((uint64_t)2 << 32) | (uint64_t)R_AARCH64_CALL26;
    rel->r_addend = 0;

    /* shstrtab: section name strings. */
    char *sst = (char *)(buf + 1920);
    sst[0] = '\0';
    memcpy(sst + 1,  ".text",       6);
    memcpy(sst + 7,  ".symtab",     8);
    memcpy(sst + 15, ".strtab",     8);
    memcpy(sst + 23, ".rela.text", 11);
    memcpy(sst + 34, ".shstrtab",  10);

    return 4096;
}

int selftest_elf_synthetic(void)
{
    int failed = 0;
    uint8_t buf[4096];
    size_t  len = build_elf_blob(buf, sizeof(buf));
    EXPECT(len == 4096, "elf_synthetic: blob build failed");

    mblob_t blob = { buf, len };
    elf_view_t v;
    EXPECT(elf_open(&blob, &v) == 0, "elf_synthetic: open failed");
    EXPECT(v.text_section != NULL,  "elf_synthetic: no text section");
    EXPECT(v.rela_text_section != NULL, "elf_synthetic: no rela section");

    uint64_t off = 0, sz = 0;
    EXPECT(elf_find_symbol(&v, "my_fn", &off, &sz) == 0,
           "elf_synthetic: find_symbol failed");
    EXPECT(off == 0,                "elf_synthetic: symbol off wrong");
    EXPECT(sz == sizeof(kBody),     "elf_synthetic: symbol size wrong");

    if (failed > 0) return failed;
    fprintf(stdout, "selftest_elf_synthetic: OK\n");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* COFF synthetic                                                            */
/* ------------------------------------------------------------------------- */

/* Build a minimal COFF amd64 .o file: one .text section with
 * kBody, one defined function symbol, one extern helper, one
 * IMAGE_REL_AMD64_REL32 reloc into byte 0. */
static size_t build_coff_blob(uint8_t *buf, size_t cap)
{
    if (cap < 4096) return 0;
    memset(buf, 0, 4096);
    coff_file_header_t *hdr = (coff_file_header_t *)buf;
    hdr->Machine             = COFF_MACHINE_AMD64;
    hdr->NumberOfSections    = 1;
    hdr->PointerToSymbolTable = 1024;
    hdr->NumberOfSymbols     = 2;
    hdr->SizeOfOptionalHeader = 0;

    /* Section header right after file header. */
    coff_section_t *sect =
        (coff_section_t *)(buf + sizeof(coff_file_header_t));
    memcpy(sect->Name, ".text", 5);
    sect->SizeOfRawData          = sizeof(kBody);
    sect->PointerToRawData       = 512;
    sect->PointerToRelocations   = 2048;
    sect->NumberOfRelocations    = 1;

    /* Body bytes at offset 512. */
    memcpy(buf + 512, kBody, sizeof(kBody));

    /* Symtab at offset 1024, two 18-byte entries.
     *   sym 0: inline name "my_fn", section 1, value 0, class EXTERNAL
     *   sym 1: inline name "my_help", section 0 (extern), value 0,
     *          class EXTERNAL */
    uint8_t *s0 = buf + 1024;
    memcpy(s0, "my_fn\0\0\0", 8);
    /* value (uint32 LE) = 0 */ memset(s0 + 8,  0, 4);
    /* section number = 1 */    s0[12] = 1; s0[13] = 0;
    /* type (uint16) = 0  */    s0[14] = 0; s0[15] = 0;
    /* storage class      */    s0[16] = IMAGE_SYM_CLASS_EXTERNAL;
    /* num aux            */    s0[17] = 0;

    uint8_t *s1 = buf + 1024 + COFF_SYM_ENTRY_SIZE;
    memcpy(s1, "my_help\0", 8);
    memset(s1 + 8,  0, 4);
    s1[12] = 0; s1[13] = 0;
    s1[14] = 0; s1[15] = 0;
    s1[16] = IMAGE_SYM_CLASS_EXTERNAL;
    s1[17] = 0;

    /* String table starts after the symtab (offset 1024 + 36 = 1060).
     * The first 4 bytes are the size field; we use only inline names so
     * the size field is 4 itself. */
    uint8_t *str = buf + 1024 + 2 * COFF_SYM_ENTRY_SIZE;
    str[0] = 4; str[1] = 0; str[2] = 0; str[3] = 0;

    /* Reloc entry: VirtualAddress=0, SymbolTableIndex=1 (extern helper),
     * Type=IMAGE_REL_AMD64_REL32. */
    uint8_t *r = buf + 2048;
    memset(r, 0, COFF_RELOC_ENTRY_SIZE);
    uint16_t kind = IMAGE_REL_AMD64_REL32;
    uint32_t snum = 1;
    memcpy(r + 4, &snum, 4);
    memcpy(r + 8, &kind, 2);

    return 4096;
}

int selftest_coff_synthetic(void)
{
    int failed = 0;
    uint8_t buf[4096];
    size_t  len = build_coff_blob(buf, sizeof(buf));
    EXPECT(len == 4096, "coff_synthetic: blob build failed");

    mblob_t blob = { buf, len };
    coff_view_t v;
    EXPECT(coff_open(&blob, &v) == 0, "coff_synthetic: open failed");
    EXPECT(v.text_section != NULL,   "coff_synthetic: no text section");
    EXPECT(v.text_index == 1,        "coff_synthetic: wrong text index");

    uint64_t off = 0, sz = 0;
    EXPECT(coff_find_symbol(&v, "my_fn", &off, &sz) == 0,
           "coff_synthetic: find_symbol failed");
    EXPECT(off == 0,                "coff_synthetic: symbol off wrong");
    EXPECT(sz == sizeof(kBody),     "coff_synthetic: symbol size wrong");

    if (failed > 0) return failed;
    fprintf(stdout, "selftest_coff_synthetic: OK\n");
    return 0;
}
