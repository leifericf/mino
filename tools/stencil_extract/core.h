/*
 * stencil_extract/core.h -- format-agnostic plumbing shared by every
 * extractor parser. Defines the file-buffer type, the normalised
 * reloc-record shape, the small symbol-name intern helper, and the
 * MINO_STENCIL_RELOC_* host enum the per-format mappers feed.
 *
 * The per-format modules (macho, elf, coff) own their parser, their
 * reloc-kind constants, and their reloc-kind map. They funnel the
 * extracted records through write_stencil_header so the on-disk
 * shape stays single-source.
 */

#ifndef STENCIL_EXTRACT_CORE_H
#define STENCIL_EXTRACT_CORE_H

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ------------------------------------------------------------------------- */
/* MINO_STENCIL_RELOC_* host enum                                            */
/* ------------------------------------------------------------------------- */

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

/* ------------------------------------------------------------------------- */
/* File buffer                                                               */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint8_t *data;
    size_t   len;
} mblob_t;

int  read_file(const char *path, mblob_t *out);
void blob_free(mblob_t *b);

/* ------------------------------------------------------------------------- */
/* Reloc record + symbol-table intern                                        */
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

/* Find or insert a symbol-table index. Returns the slot index. The
 * caller pre-allocates an array large enough for the worst case (one
 * symbol per reloc). Returns -1 when the table is full. */
int sym_table_intern(const char *name,
                     const char **table, int *count, int cap);

/* ------------------------------------------------------------------------- */
/* Header emission                                                           */
/* ------------------------------------------------------------------------- */

/* Format-agnostic header writer. Takes already-extracted body bytes +
 * relocs + symbol list and emits the C arrays the JIT runtime consumes.
 * Every per-format emit_stencil_header funnels into this so the on-disk
 * shape stays single-source. */
int write_stencil_header(const char *symbol,
                         const uint8_t *body, uint64_t size,
                         const stencil_reloc_t *relocs, int nrelocs,
                         const char *const *syms, int nsyms,
                         const char *out_path, int append);

#endif /* STENCIL_EXTRACT_CORE_H */
