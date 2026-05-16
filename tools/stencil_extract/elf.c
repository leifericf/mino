/*
 * stencil_extract/elf.c -- ELF64 (AArch64 + x86_64) parser
 * implementation. Walks sections, locates .text + .symtab +
 * .rela.text, walks relocations, and maps ELF reloc types to the
 * runtime-stable MINO_STENCIL_RELOC_* host enum.
 */

#include "elf.h"

#include <stdio.h>
#include <string.h>

/* Map an AArch64 ELF reloc type to the runtime-stable
 * MINO_STENCIL_RELOC_*. Returns -1 for unsupported kinds so the build
 * fails loudly rather than silently dropping a patch site. */
int elf_reloc_arm64_kind_map(uint32_t r_type)
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
int elf_reloc_x86_64_kind_map(uint32_t r_type)
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

int elf_open(const mblob_t *blob, elf_view_t *out)
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

int elf_list_symbols(const elf_view_t *v)
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

int elf_find_symbol(const elf_view_t *v, const char *name,
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

static int elf_extract_relocs(const elf_view_t *v,
                               uint64_t offset, uint64_t size,
                               stencil_reloc_t *out, int out_cap,
                               int *out_count,
                               const char **sym_table, int *sym_count,
                               int sym_cap)
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
            ? elf_reloc_arm64_kind_map(rtype)
            : elf_reloc_x86_64_kind_map(rtype);
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

int elf_emit_stencil_header(const elf_view_t *v, const char *symbol,
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
