/*
 * stencil_extract/elf.h -- ELF64 (AArch64 + x86_64) parser. Owns
 * the ELF64 type definitions, accessors, AArch64 + x86_64 reloc
 * constants, reloc-kind maps, and the format-specific parser API.
 */

#ifndef STENCIL_EXTRACT_ELF_H
#define STENCIL_EXTRACT_ELF_H

#include "core.h"

/* ------------------------------------------------------------------------- */
/* ELF64 constants                                                           */
/* ------------------------------------------------------------------------- */

/* ELF64 magic: ELF (0x7f) "E" "L" "F" in the first four bytes. */
#define ELF_MAGIC_BYTE_0  0x7fu
#define ELF_MAGIC_BYTE_1  'E'
#define ELF_MAGIC_BYTE_2  'L'
#define ELF_MAGIC_BYTE_3  'F'

/* ARM64 ELF reloc kinds the JIT patcher consumes. The numeric values
 * come from `<elf.h>` and the AArch64 ELF ABI. */
#define R_AARCH64_ABS64               257u
#define R_AARCH64_CALL26              283u
#define R_AARCH64_JUMP26              282u
#define R_AARCH64_ADR_PREL_PG_HI21    275u
#define R_AARCH64_ADD_ABS_LO12_NC     277u
#define R_AARCH64_LDST64_ABS_LO12_NC  286u
#define R_AARCH64_ADR_GOT_PAGE        311u
#define R_AARCH64_LD64_GOT_LO12_NC    312u

/* x86_64 ELF reloc kinds. */
#define R_X86_64_64           1u   /* direct 64-bit                  */
#define R_X86_64_PC32         2u   /* 32-bit pc-relative             */
#define R_X86_64_PLT32        4u   /* 32-bit PLT call                */
#define R_X86_64_GOTPCREL     9u   /* GOT entry, RIP-relative        */
#define R_X86_64_GOTPCRELX    41u  /* relaxed GOT lookup, RIP-rel    */
#define R_X86_64_REX_GOTPCRELX 42u /* relaxed GOT lookup w/ REX, RIP-rel */

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

/* Symbol info packing -- low nibble is type, high nibble is binding. */
#define ELF64_ST_TYPE(info) ((unsigned)((info) & 0xfu))
#define ELF64_ST_BIND(info) ((unsigned)(((info) >> 4) & 0xfu))
#define STT_FUNC 2u
#define STB_GLOBAL 1u

/* ------------------------------------------------------------------------- */
/* ELF64 type definitions                                                    */
/* ------------------------------------------------------------------------- */

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

/* ------------------------------------------------------------------------- */
/* Reloc info bit-field accessors                                            */
/* ------------------------------------------------------------------------- */

/* Reloc info packing -- top 32 bits hold the symbol index, low 32 bits
 * hold the type. */
static inline uint32_t elf64_r_sym(uint64_t info)  {
    return (uint32_t)(info >> 32);
}
static inline uint32_t elf64_r_type(uint64_t info) {
    return (uint32_t)(info & 0xffffffffu);
}

/* ------------------------------------------------------------------------- */
/* Reloc-kind maps                                                           */
/* ------------------------------------------------------------------------- */

int elf_reloc_arm64_kind_map(uint32_t r_type);
int elf_reloc_x86_64_kind_map(uint32_t r_type);

/* ------------------------------------------------------------------------- */
/* Parser API                                                                */
/* ------------------------------------------------------------------------- */

int elf_open(const mblob_t *blob, elf_view_t *out);
int elf_list_symbols(const elf_view_t *v);
int elf_find_symbol(const elf_view_t *v, const char *name,
                    uint64_t *out_offset, uint64_t *out_size);
int elf_emit_stencil_header(const elf_view_t *v, const char *symbol,
                             uint64_t offset, uint64_t size,
                             const char *out_path, int append);

#endif /* STENCIL_EXTRACT_ELF_H */
