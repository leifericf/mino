/*
 * profile.c -- compile-time-gated allocation profiler.
 *
 * Enabled with -DMINO_ALLOC_PROFILE=1 at build time. When the flag is
 * not set, this TU compiles to nothing and gc_alloc_typed is its
 * regular fast self.
 *
 * Records (file, line, tag, size) for every gc_alloc_typed call site
 * via the macro wrapper in gc/internal.h. Counts and bytes accumulate
 * in a fixed-size open-addressing hash so the recording site is
 * lock-free and allocation-free, keeping per-call overhead a few
 * loads, a hash mix, and an integer compare.
 *
 * Dump and reset are exposed via the public API in mino.h:
 *   mino_alloc_profile_dump_top(state, FILE *out, int top_n)
 *   mino_alloc_profile_reset(state)
 *   mino_alloc_profile_enabled()
 *
 * The hash is process-wide rather than per-state because hot-path
 * profiling needs to survive state teardown and re-init in test
 * harnesses, and the data carries no state-specific identity.
 */

#include "runtime/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int mino_alloc_profile_enabled(void)
{
#ifdef MINO_ALLOC_PROFILE
    return 1;
#else
    return 0;
#endif
}

#ifdef MINO_ALLOC_PROFILE

#define ALLOC_PROFILE_CAP 4096u  /* power of two, ~24 KB at 24 B/slot */

typedef struct {
    const char    *file;
    int            line;
    unsigned char  tag;
    unsigned long  count;
    unsigned long  bytes;
} alloc_site_t;

static alloc_site_t g_sites[ALLOC_PROFILE_CAP];

/* Mix file pointer and line. File pointer is interned by the C compiler
 * so identical __FILE__ literals share the same address; that lets us
 * key on pointer identity without strcmp. */
static unsigned alloc_site_hash(const char *file, int line,
                                unsigned char tag)
{
    uintptr_t f = (uintptr_t)file;
    unsigned  h = (unsigned)(f ^ (f >> 16)) * 16777619u;
    h ^= (unsigned)line * 2654435761u;
    h ^= (unsigned)tag;
    return h;
}

void mino_alloc_profile_record(const char *file, int line,
                               unsigned char tag, size_t size)
{
    unsigned start = alloc_site_hash(file, line, tag) & (ALLOC_PROFILE_CAP - 1u);
    unsigned i;
    for (i = 0; i < ALLOC_PROFILE_CAP; i++) {
        unsigned      slot = (start + i) & (ALLOC_PROFILE_CAP - 1u);
        alloc_site_t *s    = &g_sites[slot];
        if (s->file == NULL) {
            s->file  = file;
            s->line  = line;
            s->tag   = tag;
            s->count = 1;
            s->bytes = (unsigned long)size;
            return;
        }
        if (s->file == file && s->line == line && s->tag == tag) {
            s->count++;
            s->bytes += (unsigned long)size;
            return;
        }
    }
    /* Hash full: silently drop. ALLOC_PROFILE_CAP is sized so that all
     * gc_alloc_typed call sites in the codebase fit; if you hit this in
     * practice, raise the cap. */
}

void mino_alloc_profile_reset(mino_state_t *S)
{
    (void)S;
    memset(g_sites, 0, sizeof(g_sites));
}

static int site_cmp_count_desc(const void *a, const void *b)
{
    unsigned long ca = ((const alloc_site_t *)a)->count;
    unsigned long cb = ((const alloc_site_t *)b)->count;
    if (cb > ca) return 1;
    if (cb < ca) return -1;
    return 0;
}

static const char *tag_name(unsigned char tag)
{
    switch (tag) {
    case GC_T_RAW:        return "RAW";
    case GC_T_VAL:        return "VAL";
    case GC_T_ENV:        return "ENV";
    case GC_T_VEC_NODE:   return "VECN";
    case GC_T_HAMT_NODE:  return "HAMT";
    case GC_T_HAMT_ENTRY: return "HAEN";
    case GC_T_PTRARR:     return "PTRA";
    case GC_T_VALARR:     return "VALA";
    case GC_T_RB_NODE:    return "RBN";
    default:              return "??";
    }
}

void mino_alloc_profile_dump_top(mino_state_t *S, FILE *out, int top_n)
{
    static alloc_site_t copy[ALLOC_PROFILE_CAP];
    int            i, kept = 0;
    unsigned long  total_count = 0, total_bytes = 0;
    (void)S;
    if (out == NULL) out = stderr;
    memcpy(copy, g_sites, sizeof(copy));
    for (i = 0; i < (int)ALLOC_PROFILE_CAP; i++) {
        if (copy[i].file != NULL) {
            copy[kept++]  = copy[i];
            total_count  += copy[i].count;
            total_bytes  += copy[i].bytes;
        }
    }
    qsort(copy, (size_t)kept, sizeof(*copy), site_cmp_count_desc);
    if (top_n <= 0 || top_n > kept) top_n = kept;
    fprintf(out, "alloc-profile: %d sites, %lu allocs, %lu bytes\n",
            kept, total_count, total_bytes);
    fprintf(out, "  rank      count        bytes  tag   callsite\n");
    for (i = 0; i < top_n; i++) {
        fprintf(out, "  %4d  %9lu  %11lu  %-4s  %s:%d\n",
                i + 1,
                copy[i].count,
                copy[i].bytes,
                tag_name(copy[i].tag),
                copy[i].file,
                copy[i].line);
    }
}

#else /* !MINO_ALLOC_PROFILE */

void mino_alloc_profile_reset(mino_state_t *S)
{
    (void)S;
}

void mino_alloc_profile_dump_top(mino_state_t *S, FILE *out, int top_n)
{
    (void)S; (void)top_n;
    if (out == NULL) out = stderr;
    fprintf(out,
            "alloc-profile: not built (rebuild with MINO_ALLOC_PROFILE=1)\n");
}

#endif /* MINO_ALLOC_PROFILE */
