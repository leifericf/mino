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

/* ------------------------------------------------------------------------- */
/* Mach-O constants                                                          */
/* ------------------------------------------------------------------------- */

#define MH_MAGIC_64  0xfeedfacfu
#define MH_CIGAM_64  0xcffaedfeu

#define LC_SEGMENT_64 0x19u
#define LC_SYMTAB     0x02u

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

/* N_TYPE mask bits in nlist_64.n_type. Only N_EXT and N_SECT matter for
 * stencil symbol lookup: the linker emits the stencil function as an
 * extern N_SECT-bound symbol, and the relocations reference imm
 * external symbols by index. */
#define N_TYPE_MASK 0x0eu
#define N_SECT      0x0eu  /* defined in section pointed to by n_sect */
#define N_EXT       0x01u

typedef struct {
    uint32_t magic;
    uint32_t cputype;
    uint32_t cpusubtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
    uint32_t reserved;
} macho_header_64_t;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
} macho_load_command_t;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    char     segname[16];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    int32_t  maxprot;
    int32_t  initprot;
    uint32_t nsects;
    uint32_t flags;
} macho_segment_64_t;

typedef struct {
    char     sectname[16];
    char     segname[16];
    uint64_t addr;
    uint64_t size;
    uint32_t offset;
    uint32_t align;
    uint32_t reloff;
    uint32_t nreloc;
    uint32_t flags;
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
} macho_section_64_t;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t symoff;
    uint32_t nsyms;
    uint32_t stroff;
    uint32_t strsize;
} macho_symtab_t;

typedef struct {
    uint32_t n_strx;
    uint8_t  n_type;
    uint8_t  n_sect;
    uint16_t n_desc;
    uint64_t n_value;
} macho_nlist_64_t;

/* relocation_info layout when the high bit (r_scattered) is 0, packed
 * as four bytes for r_address followed by a 32-bit field encoding
 * r_symbolnum (24), r_pcrel (1), r_length (2), r_extern (1), r_type (4). */
typedef struct {
    int32_t  r_address;
    uint32_t r_info;
} macho_reloc_info_t;

/* ARM64 Mach-O reloc kinds the JIT patcher recognises. The numeric
 * values come from the Mach-O ABI documentation; the same constants
 * appear in the Apple cctools `<mach-o/arm64/reloc.h>` header. The
 * subset listed here is the only one the stencils generate today; new
 * kinds get added as new stencil shapes surface. */
#define ARM64_RELOC_UNSIGNED         0u  /* absolute 8 / 4 / 2-byte slot   */
#define ARM64_RELOC_BRANCH26         2u  /* b / bl with 26-bit imm26       */
#define ARM64_RELOC_PAGE21           3u  /* adrp page21 immediate          */
#define ARM64_RELOC_PAGEOFF12        4u  /* add / ldr / str page12 imm     */
#define ARM64_RELOC_GOT_LOAD_PAGE21    5u  /* adrp for GOT-indirect access */
#define ARM64_RELOC_GOT_LOAD_PAGEOFF12 6u  /* ldr  for GOT-indirect access */

/* x86_64 Mach-O reloc kinds. clang emits BRANCH for `call rel32` /
 * `jmp rel32` (the musttail chain marker shows up as a BRANCH);
 * GOT_LOAD for `mov rax, [rip+got]` style extern reads (every
 * MINO_STENCIL_IMM_* read lands here); GOT for the rarer direct GOT
 * accesses; UNSIGNED for any 8-byte absolute the build path might
 * emit. SIGNED_X variants encode addends -1 / -2 / -4 in the kind;
 * stencils never need addends other than -4 for SIGNED but we map
 * the full set so the build fails loudly if clang ever produces a
 * surprising kind. */
#define X86_64_RELOC_UNSIGNED  0u
#define X86_64_RELOC_SIGNED    1u
#define X86_64_RELOC_BRANCH    2u
#define X86_64_RELOC_GOT_LOAD  3u
#define X86_64_RELOC_GOT       4u
#define X86_64_RELOC_SIGNED_1  6u
#define X86_64_RELOC_SIGNED_2  7u
#define X86_64_RELOC_SIGNED_4  8u

/* Mach-O cputype values (subset). The header's cputype tells the
 * extractor which reloc-kind table to use. CPU_ARCH_ABI64 (the high
 * bit, 0x01000000) is OR'd into both AArch64 and x86_64 cputypes. */
#define CPU_TYPE_X86_64  ((uint32_t)(7 | 0x01000000u))  /*  16777223 */
#define CPU_TYPE_ARM64   ((uint32_t)(12 | 0x01000000u)) /*  16777228 */

/* Host enum encoded as a small integer the runtime can switch on. The
 * generated header references these by name so a runtime header change
 * doesn't require regenerating every stencil. Mach-O ARM64 kinds map
 * 1:1 to MINO_STENCIL_RELOC_ARM64_*; other archs add new entries when
 * their parsers land. */
#define MINO_STENCIL_RELOC_ARM64_PAGE21           0u
#define MINO_STENCIL_RELOC_ARM64_PAGEOFF12        1u
#define MINO_STENCIL_RELOC_ARM64_BRANCH26         2u
#define MINO_STENCIL_RELOC_ABS64                  3u
#define MINO_STENCIL_RELOC_ARM64_GOT_LOAD_PAGE21    4u
#define MINO_STENCIL_RELOC_ARM64_GOT_LOAD_PAGEOFF12 5u
#define MINO_STENCIL_RELOC_X86_64_ABS64             6u
#define MINO_STENCIL_RELOC_X86_64_PC32              7u
#define MINO_STENCIL_RELOC_X86_64_GOTPCREL          8u

static int reloc_arm64_kind_map(uint32_t macho_kind, uint32_t length);
static int reloc_x86_64_macho_kind_map(uint32_t macho_kind, uint32_t length,
                                        int32_t *out_implicit_addend);
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

/* ------------------------------------------------------------------------- */
/* File buffer                                                               */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint8_t *data;
    size_t   len;
} mblob_t;

static int read_file(const char *path, mblob_t *out)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "stencil_extract: cannot open '%s'\n", path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (buf == NULL) { fclose(f); return -1; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) { free(buf); return -1; }
    out->data = buf;
    out->len  = (size_t)sz;
    return 0;
}

static void blob_free(mblob_t *b) { free(b->data); b->data = NULL; b->len = 0; }

/* ------------------------------------------------------------------------- */
/* Mach-O parser                                                             */
/* ------------------------------------------------------------------------- */

typedef struct {
    const mblob_t           *blob;
    const macho_header_64_t *hdr;
    const macho_section_64_t *text_section;  /* __TEXT,__text */
    int                       text_section_index; /* 1-based for nlist.n_sect */
    const macho_symtab_t    *symtab;
} macho_view_t;

static int macho_open(const mblob_t *blob, macho_view_t *out)
{
    memset(out, 0, sizeof(*out));
    if (blob->len < sizeof(macho_header_64_t)) {
        fprintf(stderr, "stencil_extract: file too small for Mach-O 64 header\n");
        return -1;
    }
    const macho_header_64_t *hdr = (const macho_header_64_t *)blob->data;
    if (hdr->magic != MH_MAGIC_64) {
        fprintf(stderr,
                "stencil_extract: not a little-endian Mach-O 64 (magic=0x%x)\n",
                hdr->magic);
        return -1;
    }
    out->blob = blob;
    out->hdr  = hdr;

    /* Walk load commands. Pick up __TEXT,__text section + LC_SYMTAB. */
    const uint8_t *p = blob->data + sizeof(macho_header_64_t);
    const uint8_t *end = p + hdr->sizeofcmds;
    if (end > blob->data + blob->len) {
        fprintf(stderr, "stencil_extract: load commands exceed file\n");
        return -1;
    }
    int sect_index_counter = 0;
    for (uint32_t i = 0; i < hdr->ncmds && p < end; i++) {
        const macho_load_command_t *lc = (const macho_load_command_t *)p;
        if (lc->cmdsize == 0 || p + lc->cmdsize > end) {
            fprintf(stderr,
                    "stencil_extract: bad load command size at index %u\n", i);
            return -1;
        }
        if (lc->cmd == LC_SEGMENT_64) {
            const macho_segment_64_t *seg = (const macho_segment_64_t *)p;
            const macho_section_64_t *sects =
                (const macho_section_64_t *)(p + sizeof(*seg));
            for (uint32_t s = 0; s < seg->nsects; s++) {
                sect_index_counter++;
                if (out->text_section == NULL
                    && memcmp(sects[s].sectname, "__text", 7) == 0
                    && memcmp(sects[s].segname,  "__TEXT", 7) == 0) {
                    out->text_section = &sects[s];
                    out->text_section_index = sect_index_counter;
                }
            }
        } else if (lc->cmd == LC_SYMTAB) {
            out->symtab = (const macho_symtab_t *)p;
        }
        p += lc->cmdsize;
    }
    if (out->text_section == NULL) {
        fprintf(stderr, "stencil_extract: no __TEXT,__text section\n");
        return -1;
    }
    if (out->symtab == NULL) {
        fprintf(stderr, "stencil_extract: no symbol table\n");
        return -1;
    }
    return 0;
}

static const char *macho_strtab(const macho_view_t *v)
{
    if (v->blob->len < (size_t)v->symtab->stroff + v->symtab->strsize) return NULL;
    return (const char *)(v->blob->data + v->symtab->stroff);
}

static const macho_nlist_64_t *macho_nlist(const macho_view_t *v)
{
    if (v->blob->len < (size_t)v->symtab->symoff
        + (size_t)v->symtab->nsyms * sizeof(macho_nlist_64_t)) return NULL;
    return (const macho_nlist_64_t *)(v->blob->data + v->symtab->symoff);
}

/* Reloc accessors: the Mach-O packs r_info as a 32-bit little-endian
 * field with this layout (bit 0 = LSB):
 *   [0..23]  r_symbolnum  (24 bits) -- index into the symtab when
 *                                       r_extern is 1, section number
 *                                       otherwise
 *   [24]     r_pcrel      (1 bit)   -- pc-relative addressing
 *   [25..26] r_length     (2 bits)  -- 0=byte 1=word 2=long 3=quad
 *   [27]     r_extern     (1 bit)   -- symbolnum is a symtab index
 *   [28..31] r_type       (4 bits)  -- kind: PAGE21, PAGEOFF12, etc.
 * Helpers extract each field so the emission code reads cleanly. */
static uint32_t reloc_symbolnum(const macho_reloc_info_t *r) {
    return r->r_info & 0xffffffu;
}
static uint32_t reloc_pcrel(const macho_reloc_info_t *r) {
    return (r->r_info >> 24) & 0x1u;
}
static uint32_t reloc_length(const macho_reloc_info_t *r) {
    return (r->r_info >> 25) & 0x3u;
}
static uint32_t reloc_extern(const macho_reloc_info_t *r) {
    return (r->r_info >> 27) & 0x1u;
}
static uint32_t reloc_type(const macho_reloc_info_t *r) {
    return (r->r_info >> 28) & 0xfu;
}

/* List all N_SECT external symbols defined in __text. */
static int macho_list_symbols(const macho_view_t *v)
{
    const char             *strtab = macho_strtab(v);
    const macho_nlist_64_t *nl     = macho_nlist(v);
    if (strtab == NULL || nl == NULL) {
        fprintf(stderr, "stencil_extract: symbol table out of range\n");
        return -1;
    }
    for (uint32_t i = 0; i < v->symtab->nsyms; i++) {
        const macho_nlist_64_t *e = &nl[i];
        if ((e->n_type & N_TYPE_MASK) != N_SECT) continue;
        if (e->n_sect != v->text_section_index) continue;
        const char *name = strtab + e->n_strx;
        printf("  %-32s 0x%08" PRIx64 "  (n_type=0x%02x sect=%u)\n",
               name, e->n_value, e->n_type, e->n_sect);
    }
    return 0;
}

/* Find the section-relative offset of a named symbol in __text. Returns
 * -1 if absent. Sets *out_size to the byte length spanning to the next
 * symbol in the same section (or to __text size if no next). */
static int macho_find_symbol(const macho_view_t *v, const char *name,
                             uint64_t *out_offset, uint64_t *out_size)
{
    const char             *strtab = macho_strtab(v);
    const macho_nlist_64_t *nl     = macho_nlist(v);
    if (strtab == NULL || nl == NULL) return -1;
    uint64_t found_off = (uint64_t)-1;
    /* Compute the end-of-fn cutoff: the next N_SECT symbol in __text
     * with a strictly greater value, or the section size. */
    uint64_t end_off   = v->text_section->size;
    int      found     = 0;
    for (uint32_t i = 0; i < v->symtab->nsyms; i++) {
        const macho_nlist_64_t *e = &nl[i];
        if ((e->n_type & N_TYPE_MASK) != N_SECT) continue;
        if (e->n_sect != v->text_section_index) continue;
        const char *en = strtab + e->n_strx;
        if (strcmp(en, name) == 0) {
            found_off = e->n_value;
            found     = 1;
        }
    }
    if (!found) return -1;
    for (uint32_t i = 0; i < v->symtab->nsyms; i++) {
        const macho_nlist_64_t *e = &nl[i];
        if ((e->n_type & N_TYPE_MASK) != N_SECT) continue;
        if (e->n_sect != v->text_section_index) continue;
        if (e->n_value > found_off && e->n_value < end_off) {
            end_off = e->n_value;
        }
    }
    *out_offset = found_off;
    *out_size   = end_off - found_off;
    return 0;
}

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
    if (reloc_symbolnum(&probe) != 0x123456u) {
        fprintf(stderr, "selftest: reloc_symbolnum decode wrong\n"); failed++;
    }
    if (reloc_pcrel(&probe) != 1u) {
        fprintf(stderr, "selftest: reloc_pcrel decode wrong\n"); failed++;
    }
    if (reloc_length(&probe) != 2u) {
        fprintf(stderr, "selftest: reloc_length decode wrong\n"); failed++;
    }
    if (reloc_extern(&probe) != 1u) {
        fprintf(stderr, "selftest: reloc_extern decode wrong\n"); failed++;
    }
    if (reloc_type(&probe) != ARM64_RELOC_PAGE21) {
        fprintf(stderr, "selftest: reloc_type decode wrong\n"); failed++;
    }
    if (reloc_arm64_kind_map(ARM64_RELOC_PAGE21, 2) !=
        (int)MINO_STENCIL_RELOC_ARM64_PAGE21) {
        fprintf(stderr, "selftest: kind_map PAGE21 wrong\n"); failed++;
    }
    if (reloc_arm64_kind_map(ARM64_RELOC_UNSIGNED, 3) !=
        (int)MINO_STENCIL_RELOC_ABS64) {
        fprintf(stderr, "selftest: kind_map UNSIGNED-quad wrong\n"); failed++;
    }
    if (reloc_arm64_kind_map(0xff, 3) != -1) {
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
    if (reloc_x86_64_macho_kind_map(X86_64_RELOC_UNSIGNED, 3, &imp_addend) !=
        (int)MINO_STENCIL_RELOC_X86_64_ABS64 || imp_addend != 0) {
        fprintf(stderr, "selftest: macho x86_64 kind_map UNSIGNED wrong\n"); failed++;
    }
    if (reloc_x86_64_macho_kind_map(X86_64_RELOC_BRANCH, 2, &imp_addend) !=
        (int)MINO_STENCIL_RELOC_X86_64_PC32 || imp_addend != -4) {
        fprintf(stderr, "selftest: macho x86_64 kind_map BRANCH wrong\n"); failed++;
    }
    if (reloc_x86_64_macho_kind_map(X86_64_RELOC_SIGNED, 2, &imp_addend) !=
        (int)MINO_STENCIL_RELOC_X86_64_PC32 || imp_addend != -4) {
        fprintf(stderr, "selftest: macho x86_64 kind_map SIGNED wrong\n"); failed++;
    }
    if (reloc_x86_64_macho_kind_map(X86_64_RELOC_SIGNED_1, 2, &imp_addend) !=
        (int)MINO_STENCIL_RELOC_X86_64_PC32 || imp_addend != -1) {
        fprintf(stderr, "selftest: macho x86_64 kind_map SIGNED_1 wrong\n"); failed++;
    }
    if (reloc_x86_64_macho_kind_map(X86_64_RELOC_SIGNED_2, 2, &imp_addend) !=
        (int)MINO_STENCIL_RELOC_X86_64_PC32 || imp_addend != -2) {
        fprintf(stderr, "selftest: macho x86_64 kind_map SIGNED_2 wrong\n"); failed++;
    }
    if (reloc_x86_64_macho_kind_map(X86_64_RELOC_SIGNED_4, 2, &imp_addend) !=
        (int)MINO_STENCIL_RELOC_X86_64_PC32 || imp_addend != -4) {
        fprintf(stderr, "selftest: macho x86_64 kind_map SIGNED_4 wrong\n"); failed++;
    }
    if (reloc_x86_64_macho_kind_map(X86_64_RELOC_GOT_LOAD, 2, &imp_addend) !=
        (int)MINO_STENCIL_RELOC_X86_64_GOTPCREL || imp_addend != -4) {
        fprintf(stderr, "selftest: macho x86_64 kind_map GOT_LOAD wrong\n"); failed++;
    }
    if (reloc_x86_64_macho_kind_map(X86_64_RELOC_GOT, 2, &imp_addend) !=
        (int)MINO_STENCIL_RELOC_X86_64_GOTPCREL || imp_addend != -4) {
        fprintf(stderr, "selftest: macho x86_64 kind_map GOT wrong\n"); failed++;
    }
    if (reloc_x86_64_macho_kind_map(0xffu, 2, &imp_addend) != -1) {
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

/* Stencil-extracted reloc, normalised to a host-format the JIT runtime
 * consumes. Per stencil we filter only the relocations that fall inside
 * the function body and emit them keyed by a stable kind enum that the
 * runtime maps to a patcher. */
typedef struct {
    uint32_t offset;       /* byte offset within the stencil body         */
    uint32_t kind;         /* MINO_STENCIL_RELOC_* in the host enum       */
    uint32_t sym_index;    /* index into the per-stencil symbol table     */
    int32_t  addend;       /* added to the symbol value before patching   */
} stencil_reloc_t;

/* Forward declaration: the format-agnostic header writer is defined
 * after the COFF parser block but is called from coff_emit_stencil_header
 * which lives inside that block. Both Mach-O and ELF emitters use it. */
static int write_stencil_header(const char *symbol,
                                const uint8_t *body, uint64_t size,
                                const stencil_reloc_t *relocs, int nrelocs,
                                const char *const *syms, int nsyms,
                                const char *out_path, int append);

/* Find or insert a symbol-table index. Returns the slot index. The
 * caller pre-allocates an array large enough for the worst case (one
 * symbol per reloc). */
static int sym_table_intern(const char *name,
                            const char **table, int *count, int cap)
{
    for (int i = 0; i < *count; i++) {
        if (strcmp(table[i], name) == 0) return i;
    }
    if (*count >= cap) return -1;
    table[*count] = name;
    return (*count)++;
}

/* Map an ARM64 Mach-O reloc kind to a runtime-stable MINO_STENCIL_RELOC_*.
 * Returns -1 when the kind is unsupported -- the build fails loudly
 * rather than silently dropping the reloc, since a missed patch means
 * the JIT'd code reads garbage at runtime. */
static int reloc_arm64_kind_map(uint32_t macho_kind, uint32_t length)
{
    switch (macho_kind) {
    case ARM64_RELOC_PAGE21:
        return (int)MINO_STENCIL_RELOC_ARM64_PAGE21;
    case ARM64_RELOC_PAGEOFF12:
        return (int)MINO_STENCIL_RELOC_ARM64_PAGEOFF12;
    case ARM64_RELOC_BRANCH26:
        return (int)MINO_STENCIL_RELOC_ARM64_BRANCH26;
    case ARM64_RELOC_UNSIGNED:
        if (length == 3u) return (int)MINO_STENCIL_RELOC_ABS64;
        return -1;
    case ARM64_RELOC_GOT_LOAD_PAGE21:
        return (int)MINO_STENCIL_RELOC_ARM64_GOT_LOAD_PAGE21;
    case ARM64_RELOC_GOT_LOAD_PAGEOFF12:
        return (int)MINO_STENCIL_RELOC_ARM64_GOT_LOAD_PAGEOFF12;
    default: return -1;
    }
}

/* Mach-O x86_64 reloc-kind map. Returns the runtime-stable
 * MINO_STENCIL_RELOC_X86_64_* constant; also writes the implicit
 * addend (Mach-O REL relocations carry no r_addend, so the addend
 * is encoded either inline in the relocated bytes -- which the
 * extractor doesn't yet decode -- or in the kind itself for the
 * SIGNED_X family).
 *
 * For the kinds stencils actually emit (BRANCH, GOT_LOAD, GOT,
 * SIGNED), the addend is always -4: rip points 4 bytes past the
 * rel32 field, so the patcher needs `target - (insn_addr + 4)`.
 * Setting addend = -4 lets emit.c's patch_pc32 / patch_gotpcrel
 * compute `target + addend - insn_addr` uniformly with the ELF
 * path. The SIGNED_1 and SIGNED_2 variants use -1 and -2; included
 * here for completeness even though stencils don't generate them.
 *
 * UNSIGNED is a non-pcrel absolute (8 bytes on length=3); addend
 * is the inline value at the reloc site, which the extractor
 * leaves at 0 because stencils never embed UNSIGNED in __text. */
static int reloc_x86_64_macho_kind_map(uint32_t macho_kind, uint32_t length,
                                        int32_t *out_implicit_addend)
{
    *out_implicit_addend = 0;
    switch (macho_kind) {
    case X86_64_RELOC_UNSIGNED:
        if (length == 3u) return (int)MINO_STENCIL_RELOC_X86_64_ABS64;
        return -1;
    case X86_64_RELOC_BRANCH:
    case X86_64_RELOC_SIGNED:
    case X86_64_RELOC_SIGNED_4:
        *out_implicit_addend = -4;
        return (int)MINO_STENCIL_RELOC_X86_64_PC32;
    case X86_64_RELOC_SIGNED_1:
        *out_implicit_addend = -1;
        return (int)MINO_STENCIL_RELOC_X86_64_PC32;
    case X86_64_RELOC_SIGNED_2:
        *out_implicit_addend = -2;
        return (int)MINO_STENCIL_RELOC_X86_64_PC32;
    case X86_64_RELOC_GOT_LOAD:
    case X86_64_RELOC_GOT:
        *out_implicit_addend = -4;
        return (int)MINO_STENCIL_RELOC_X86_64_GOTPCREL;
    default: return -1;
    }
}

/* Extract section relocations that fall inside the function body. The
 * Mach-O relocation table is per-section; for an .o file each reloc's
 * r_address is relative to the section's start. We translate that to
 * an offset relative to the function start by subtracting `offset`. */
static int extract_relocs(const macho_view_t *v, uint64_t offset, uint64_t size,
                          stencil_reloc_t *out, int out_cap, int *out_count,
                          const char **sym_table, int *sym_count, int sym_cap)
{
    *out_count = 0;
    const macho_section_64_t *sect = v->text_section;
    if (sect->nreloc == 0) return 0;
    if (v->blob->len < (size_t)sect->reloff
        + (size_t)sect->nreloc * sizeof(macho_reloc_info_t)) {
        fprintf(stderr, "stencil_extract: reloc table out of range\n");
        return -1;
    }
    const macho_reloc_info_t *relocs =
        (const macho_reloc_info_t *)(v->blob->data + sect->reloff);
    const char             *strtab = macho_strtab(v);
    const macho_nlist_64_t *nl     = macho_nlist(v);
    if (strtab == NULL || nl == NULL) return -1;
    uint32_t cputype = v->hdr->cputype;
    for (uint32_t i = 0; i < sect->nreloc; i++) {
        const macho_reloc_info_t *r = &relocs[i];
        int32_t addr = r->r_address;
        if (addr < (int32_t)offset || addr >= (int32_t)(offset + size)) continue;
        uint32_t kind  = reloc_type(r);
        uint32_t len   = reloc_length(r);
        uint32_t ext   = reloc_extern(r);
        uint32_t snum  = reloc_symbolnum(r);
        int32_t  implicit_addend = 0;
        int mapped;
        if (cputype == CPU_TYPE_ARM64) {
            mapped = reloc_arm64_kind_map(kind, len);
        } else if (cputype == CPU_TYPE_X86_64) {
            mapped = reloc_x86_64_macho_kind_map(kind, len, &implicit_addend);
        } else {
            fprintf(stderr,
                    "stencil_extract: unsupported Mach-O cputype 0x%x\n",
                    (unsigned)cputype);
            return -1;
        }
        if (mapped < 0) {
            fprintf(stderr,
                    "stencil_extract: unsupported Mach-O reloc kind %u (length=%u) "
                    "at offset 0x%x (cputype=0x%x)\n",
                    kind, len, (unsigned)addr, (unsigned)cputype);
            return -1;
        }
        if (!ext) {
            fprintf(stderr,
                    "stencil_extract: non-extern relocation at offset 0x%x "
                    "-- stencils must reference extern symbols only\n",
                    (unsigned)addr);
            return -1;
        }
        if (snum >= v->symtab->nsyms) {
            fprintf(stderr, "stencil_extract: reloc symbol index %u out of range\n",
                    snum);
            return -1;
        }
        const char *name = strtab + nl[snum].n_strx;
        if (*name == '_') name++;  /* strip leader underscore */
        int sym_idx = sym_table_intern(name, sym_table, sym_count, sym_cap);
        if (sym_idx < 0) {
            fprintf(stderr, "stencil_extract: too many distinct symbols\n");
            return -1;
        }
        if (*out_count >= out_cap) {
            fprintf(stderr, "stencil_extract: too many relocations per stencil\n");
            return -1;
        }
        out[*out_count].offset    = (uint32_t)(addr - (int32_t)offset);
        out[*out_count].kind      = (uint32_t)mapped;
        out[*out_count].sym_index = (uint32_t)sym_idx;
        /* ARM64 patchers don't read addend; x86_64 patchers do. The
         * map fn above returns the implicit addend per Mach-O kind
         * (e.g., -4 for BRANCH / SIGNED / GOT_LOAD on x86_64). */
        out[*out_count].addend    = implicit_addend;
        (*out_count)++;
    }
    return 0;
}

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

/* Format-agnostic header writer. Takes already-extracted body bytes +
 * relocs + symbol list and emits the C arrays the JIT runtime consumes.
 * Both the Mach-O and ELF parsers funnel into this so the on-disk shape
 * stays single-source. */
static int write_stencil_header(const char *symbol,
                                const uint8_t *body, uint64_t size,
                                const stencil_reloc_t *relocs, int nrelocs,
                                const char *const *syms, int nsyms,
                                const char *out_path, int append)
{
    FILE *out = (out_path != NULL && strcmp(out_path, "-") != 0)
        ? fopen(out_path, append ? "a" : "w") : stdout;
    if (out == NULL) {
        fprintf(stderr, "stencil_extract: cannot open '%s' for write\n",
                out_path);
        return -1;
    }
    if (!append) {
        fprintf(out, "/* Generated by tools/stencil_extract. Do not edit. */\n\n");
    } else {
        fprintf(out, "\n");
    }
    fprintf(out, "static const unsigned char %s_bytes[%" PRIu64 "] = {\n",
            symbol, size);
    for (uint64_t i = 0; i < size; i++) {
        if ((i & 15) == 0) fprintf(out, "    ");
        fprintf(out, "0x%02x", body[i]);
        if (i + 1 < size) fprintf(out, ",");
        if ((i & 15) == 15) fprintf(out, "\n");
        else                fprintf(out, " ");
    }
    if ((size & 15) != 0) fprintf(out, "\n");
    fprintf(out, "};\n");
    fprintf(out, "static const unsigned long %s_size = %" PRIu64 ";\n",
            symbol, size);
    /* Symbol table for this stencil: each reloc references a symbol by
     * its index in this table. */
    fprintf(out, "static const char *const %s_symbols[%d] = {\n",
            symbol, nsyms == 0 ? 1 : nsyms);
    if (nsyms == 0) {
        fprintf(out, "    0\n");
    } else {
        for (int i = 0; i < nsyms; i++) {
            fprintf(out, "    \"%s\"%s\n", syms[i], i + 1 < nsyms ? "," : "");
        }
    }
    fprintf(out, "};\n");
    fprintf(out, "static const unsigned long %s_nsymbols = %d;\n",
            symbol, nsyms);
    /* Reloc table emitted as quadruples (offset, kind, sym_index,
     * addend). Empty when the stencil takes no immediates. */
    fprintf(out, "static const unsigned int %s_relocs[%d][4] = {\n",
            symbol, nrelocs == 0 ? 1 : nrelocs);
    if (nrelocs == 0) {
        fprintf(out, "    {0, 0, 0, 0}\n");
    } else {
        for (int i = 0; i < nrelocs; i++) {
            fprintf(out, "    {%u, %u, %u, %d}%s\n",
                    relocs[i].offset, relocs[i].kind,
                    relocs[i].sym_index, relocs[i].addend,
                    i + 1 < nrelocs ? "," : "");
        }
    }
    fprintf(out, "};\n");
    fprintf(out, "static const unsigned long %s_nrelocs = %d;\n",
            symbol, nrelocs);
    if (out != stdout) fclose(out);
    return 0;
}

/* Mach-O entry point: extract relocs from the Mach-O view then funnel
 * through the format-agnostic writer. */
static int emit_stencil_header(const macho_view_t *v, const char *symbol,
                               uint64_t offset, uint64_t size,
                               const char *out_path, int append)
{
    enum { MAX_RELOCS = 64, MAX_SYMS = 32 };
    stencil_reloc_t relocs[MAX_RELOCS];
    const char     *syms[MAX_SYMS];
    int             nrelocs = 0, nsyms = 0;
    if (extract_relocs(v, offset, size, relocs, MAX_RELOCS, &nrelocs,
                       syms, &nsyms, MAX_SYMS) != 0) return -1;
    const uint8_t *text = v->blob->data + v->text_section->offset;
    const uint8_t *body = text + offset;
    return write_stencil_header(symbol, body, size, relocs, nrelocs,
                                syms, nsyms, out_path, append);
}

/* ELF entry point: parallel shape to emit_stencil_header. The ELF
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
                    rc = emit_stencil_header(&v, symbol, off, sz, out, append);
                }
            } else {
                rc = emit_stencil_header(&v, symbol, off, sz, out, append);
            }
        } else {
            usage();
            rc = 2;
        }
    }
    blob_free(&blob);
    return rc;
}
