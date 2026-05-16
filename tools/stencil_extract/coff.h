/*
 * stencil_extract/coff.h -- PE/COFF amd64 parser. Owns the COFF
 * type definitions, IMAGE_REL_AMD64_* reloc constants, parser
 * API, x86_64 reloc-kind map, and emit entry.
 */

#ifndef STENCIL_EXTRACT_COFF_H
#define STENCIL_EXTRACT_COFF_H

#include "core.h"

/* ------------------------------------------------------------------------- */
/* COFF constants                                                            */
/* ------------------------------------------------------------------------- */

/* COFF object-file magic: amd64 COFF starts with the 2-byte machine
 * ID 0x8664 in little-endian, followed by the section count. */
#define COFF_MACHINE_AMD64  0x8664u

/* x86_64 COFF reloc kinds (`<winnt.h>` enum). */
#define IMAGE_REL_AMD64_ABSOLUTE  0x0000u
#define IMAGE_REL_AMD64_ADDR64    0x0001u
#define IMAGE_REL_AMD64_ADDR32    0x0002u
#define IMAGE_REL_AMD64_REL32     0x0004u
#define IMAGE_REL_AMD64_REL32_1   0x0005u

/* Storage classes (subset). */
#define IMAGE_SYM_CLASS_EXTERNAL  2u
#define IMAGE_SYM_CLASS_STATIC    3u

/* Section number specials. */
#define IMAGE_SYM_UNDEFINED   0
#define IMAGE_SYM_ABSOLUTE   -1
#define IMAGE_SYM_DEBUG      -2

/* IMAGE_SYMBOL packed entry size, 18 bytes. */
enum { COFF_SYM_ENTRY_SIZE = 18 };

/* IMAGE_RELOCATION packed entry size, 10 bytes. */
enum { COFF_RELOC_ENTRY_SIZE = 10 };

/* ------------------------------------------------------------------------- */
/* COFF type definitions                                                     */
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

/* ------------------------------------------------------------------------- */
/* Reloc-kind map                                                            */
/* ------------------------------------------------------------------------- */

int coff_reloc_x86_64_kind_map(uint32_t coff_kind,
                                int32_t *out_implicit_addend);

/* ------------------------------------------------------------------------- */
/* Parser API                                                                */
/* ------------------------------------------------------------------------- */

int coff_open(const mblob_t *blob, coff_view_t *out);
int coff_list_symbols(const coff_view_t *v);
int coff_find_symbol(const coff_view_t *v, const char *name,
                      uint64_t *out_offset, uint64_t *out_size);
int coff_emit_stencil_header(const coff_view_t *v, const char *symbol,
                              uint64_t offset, uint64_t size,
                              const char *out_path, int append);

#endif /* STENCIL_EXTRACT_COFF_H */
