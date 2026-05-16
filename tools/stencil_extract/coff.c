/*
 * stencil_extract/coff.c -- PE/COFF amd64 parser implementation.
 * Walks sections, locates .text + the byte-packed symtab + the
 * string table, walks the relocation table, and maps COFF reloc
 * kinds to the runtime-stable MINO_STENCIL_RELOC_* host enum.
 */

#include "coff.h"

#include <stdio.h>
#include <string.h>

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

int coff_open(const mblob_t *blob, coff_view_t *out)
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

int coff_list_symbols(const coff_view_t *v)
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

int coff_find_symbol(const coff_view_t *v, const char *name,
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
int coff_reloc_x86_64_kind_map(uint32_t coff_kind,
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
        int mapped = coff_reloc_x86_64_kind_map((uint32_t)kind,
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

int coff_emit_stencil_header(const coff_view_t *v, const char *symbol,
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
