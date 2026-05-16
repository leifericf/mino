/*
 * stencil_extract/macho.c -- Mach-O 64 parser implementation.
 * Walks load commands, locates __TEXT,__text + LC_SYMTAB, walks
 * the relocation table, and maps Mach-O reloc kinds to the
 * runtime-stable MINO_STENCIL_RELOC_* host enum.
 */

#include "macho.h"

#include <stdio.h>
#include <string.h>

int macho_open(const mblob_t *blob, macho_view_t *out)
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

/* List all N_SECT external symbols defined in __text. */
int macho_list_symbols(const macho_view_t *v)
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
int macho_find_symbol(const macho_view_t *v, const char *name,
                      uint64_t *out_offset, uint64_t *out_size)
{
    const char             *strtab = macho_strtab(v);
    const macho_nlist_64_t *nl     = macho_nlist(v);
    if (strtab == NULL || nl == NULL) return -1;
    uint64_t found_off = (uint64_t)-1;
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

/* Map an ARM64 Mach-O reloc kind to a runtime-stable MINO_STENCIL_RELOC_*.
 * Returns -1 when the kind is unsupported -- the build fails loudly
 * rather than silently dropping the reloc, since a missed patch means
 * the JIT'd code reads garbage at runtime. */
int macho_reloc_arm64_kind_map(uint32_t macho_kind, uint32_t length)
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
 * addend.
 *
 * For BRANCH, GOT_LOAD, GOT, SIGNED, SIGNED_4: addend is -4 -- rip
 * points 4 bytes past the rel32 field, so the patcher needs
 * `target - (insn_addr + 4)`. Setting addend = -4 lets emit.c's
 * patch_pc32 / patch_gotpcrel compute `target + addend - insn_addr`
 * uniformly with the ELF path. SIGNED_1, SIGNED_2 use -1, -2.
 *
 * UNSIGNED is a non-pcrel absolute (8 bytes on length=3); addend
 * is 0. */
int macho_reloc_x86_64_kind_map(uint32_t macho_kind, uint32_t length,
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
static int macho_extract_relocs(const macho_view_t *v,
                                 uint64_t offset, uint64_t size,
                                 stencil_reloc_t *out, int out_cap,
                                 int *out_count,
                                 const char **sym_table, int *sym_count,
                                 int sym_cap)
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
        uint32_t kind  = macho_reloc_type(r);
        uint32_t len   = macho_reloc_length(r);
        uint32_t ext   = macho_reloc_extern(r);
        uint32_t snum  = macho_reloc_symbolnum(r);
        int32_t  implicit_addend = 0;
        int mapped;
        if (cputype == CPU_TYPE_ARM64) {
            mapped = macho_reloc_arm64_kind_map(kind, len);
        } else if (cputype == CPU_TYPE_X86_64) {
            mapped = macho_reloc_x86_64_kind_map(kind, len, &implicit_addend);
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

int macho_emit_stencil_header(const macho_view_t *v, const char *symbol,
                               uint64_t offset, uint64_t size,
                               const char *out_path, int append)
{
    enum { MAX_RELOCS = 64, MAX_SYMS = 32 };
    stencil_reloc_t relocs[MAX_RELOCS];
    const char     *syms[MAX_SYMS];
    int             nrelocs = 0, nsyms = 0;
    if (macho_extract_relocs(v, offset, size, relocs, MAX_RELOCS, &nrelocs,
                              syms, &nsyms, MAX_SYMS) != 0) return -1;
    const uint8_t *text = v->blob->data + v->text_section->offset;
    const uint8_t *body = text + offset;
    return write_stencil_header(symbol, body, size, relocs, nrelocs,
                                syms, nsyms, out_path, append);
}
