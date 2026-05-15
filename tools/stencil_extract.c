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
 * This first cut handles 64-bit Mach-O (Darwin arm64 and Darwin
 * x86_64) and emits the stencil header in a format the JIT can
 * #include directly. ELF and COFF parsers land in the platform
 * releases.
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
    if (failed > 0) return 1;
    printf("stencil_extract selftest: OK\n");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Header emission                                                           */
/* ------------------------------------------------------------------------- */

static int emit_stencil_header(const macho_view_t *v, const char *symbol,
                               uint64_t offset, uint64_t size,
                               const char *out_path)
{
    FILE *out = (out_path != NULL && strcmp(out_path, "-") != 0)
        ? fopen(out_path, "w") : stdout;
    if (out == NULL) {
        fprintf(stderr, "stencil_extract: cannot open '%s' for write\n",
                out_path);
        return -1;
    }
    const uint8_t *text = v->blob->data + v->text_section->offset;
    const uint8_t *body = text + offset;
    fprintf(out, "/* Generated by tools/stencil_extract. Do not edit. */\n\n");
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
    /* Reloc emission lands in v0.183.0 where we have a stencil source
     * with extern immediates the compiler emits relocations against. */
    if (out != stdout) fclose(out);
    return 0;
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
            "  stencil_extract <obj> <symbol> <out-header>\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 2; }
    if (strcmp(argv[1], "--selftest") == 0) return selftest();
    if (argc < 3) { usage(); return 2; }
    mblob_t blob;
    if (read_file(argv[1], &blob) != 0) return 1;
    macho_view_t v;
    if (macho_open(&blob, &v) != 0) { blob_free(&blob); return 1; }
    int rc = 0;
    if (strcmp(argv[2], "--list") == 0) {
        printf("symbols in __TEXT,__text (size=%" PRIu64 ", offset=0x%" PRIx64 "):\n",
               v.text_section->size, (uint64_t)v.text_section->offset);
        rc = macho_list_symbols(&v);
    } else if (argc >= 4) {
        const char *symbol = argv[2];
        const char *out    = argv[3];
        /* Mach-O linkers prefix C function names with an underscore.
         * Try the literal name first; on miss prepend `_` so users can
         * pass the source-level name. */
        uint64_t off, sz;
        if (macho_find_symbol(&v, symbol, &off, &sz) != 0) {
            char buf[256];
            snprintf(buf, sizeof(buf), "_%s", symbol);
            if (macho_find_symbol(&v, buf, &off, &sz) != 0) {
                fprintf(stderr, "stencil_extract: symbol '%s' not found\n",
                        symbol);
                rc = 1;
            } else {
                rc = emit_stencil_header(&v, symbol, off, sz, out);
            }
        } else {
            rc = emit_stencil_header(&v, symbol, off, sz, out);
        }
    } else {
        usage();
        rc = 2;
    }
    blob_free(&blob);
    return rc;
}
