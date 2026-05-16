/*
 * stencil_extract/macho.h -- Mach-O 64 parser. Owns the Mach-O
 * header types, the ARM64 + x86_64 reloc-kind maps, and the
 * format-specific extract / emit entry points.
 */

#ifndef STENCIL_EXTRACT_MACHO_H
#define STENCIL_EXTRACT_MACHO_H

#include "core.h"

/* ------------------------------------------------------------------------- */
/* Mach-O 64 constants                                                       */
/* ------------------------------------------------------------------------- */

#define MH_MAGIC_64  0xfeedfacfu
#define MH_CIGAM_64  0xcffaedfeu

#define LC_SEGMENT_64 0x19u
#define LC_SYMTAB     0x02u

/* N_TYPE mask bits in nlist_64.n_type. Only N_EXT and N_SECT matter for
 * stencil symbol lookup: the linker emits the stencil function as an
 * extern N_SECT-bound symbol, and the relocations reference imm
 * external symbols by index. */
#define N_TYPE_MASK 0x0eu
#define N_SECT      0x0eu  /* defined in section pointed to by n_sect */
#define N_EXT       0x01u

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

/* ------------------------------------------------------------------------- */
/* Mach-O 64 type definitions                                                */
/* ------------------------------------------------------------------------- */

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

typedef struct {
    const mblob_t           *blob;
    const macho_header_64_t *hdr;
    const macho_section_64_t *text_section;  /* __TEXT,__text */
    int                       text_section_index; /* 1-based for nlist.n_sect */
    const macho_symtab_t    *symtab;
} macho_view_t;

/* ------------------------------------------------------------------------- */
/* Reloc bit-field accessors                                                 */
/* ------------------------------------------------------------------------- */

/* Reloc accessors: the Mach-O packs r_info as a 32-bit little-endian
 * field with this layout (bit 0 = LSB):
 *   [0..23]  r_symbolnum  (24 bits)
 *   [24]     r_pcrel      (1 bit)
 *   [25..26] r_length     (2 bits)
 *   [27]     r_extern     (1 bit)
 *   [28..31] r_type       (4 bits) */
static inline uint32_t macho_reloc_symbolnum(const macho_reloc_info_t *r) {
    return r->r_info & 0xffffffu;
}
static inline uint32_t macho_reloc_pcrel(const macho_reloc_info_t *r) {
    return (r->r_info >> 24) & 0x1u;
}
static inline uint32_t macho_reloc_length(const macho_reloc_info_t *r) {
    return (r->r_info >> 25) & 0x3u;
}
static inline uint32_t macho_reloc_extern(const macho_reloc_info_t *r) {
    return (r->r_info >> 27) & 0x1u;
}
static inline uint32_t macho_reloc_type(const macho_reloc_info_t *r) {
    return (r->r_info >> 28) & 0xfu;
}

/* ------------------------------------------------------------------------- */
/* Reloc-kind maps                                                           */
/* ------------------------------------------------------------------------- */

int macho_reloc_arm64_kind_map(uint32_t macho_kind, uint32_t length);
int macho_reloc_x86_64_kind_map(uint32_t macho_kind, uint32_t length,
                                 int32_t *out_implicit_addend);

/* ------------------------------------------------------------------------- */
/* Parser API                                                                */
/* ------------------------------------------------------------------------- */

int macho_open(const mblob_t *blob, macho_view_t *out);
int macho_list_symbols(const macho_view_t *v);
int macho_find_symbol(const macho_view_t *v, const char *name,
                      uint64_t *out_offset, uint64_t *out_size);
int macho_emit_stencil_header(const macho_view_t *v, const char *symbol,
                              uint64_t offset, uint64_t size,
                              const char *out_path, int append);

#endif /* STENCIL_EXTRACT_MACHO_H */
