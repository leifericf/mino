/*
 * src/eval/bc/jit/region.c -- JIT memory region and slab-pool management.
 *
 * Factored out of emit.c to keep each TU under the 1100-line limit.
 * Owns the OS memory abstraction layer (mmap / VirtualAlloc wrappers),
 * the bump-pointer slab pool for small functions, and the region
 * tracking list used by mino_jit_free_all.
 *
 * All functions with external linkage are prefixed `mino_jit_` and
 * declared in internal.h.  The platform wrappers (jit_region_alloc,
 * jit_region_free, jit_region_make_rx, jit_region_page_size) and the
 * slab helpers (jit_slab_acquire, jit_slab_make_rw, jit_slab_make_rx,
 * jit_compile_cleanup, region_track) were originally static in emit.c;
 * they now carry external linkage because emit.c's compile pipeline
 * calls them across the TU boundary.
 */

#include "internal.h"
#include "../jit.h"

#ifdef MINO_CPJIT_HOST

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* Host RWX region API. POSIX uses mmap / mprotect / munmap; Windows
 * uses VirtualAlloc / VirtualProtect / VirtualFree. The wrappers
 * below give the rest of this module a uniform allocate/protect/free
 * surface and a uniform `NULL` sentinel so the size-pass + commit-pass
 * logic doesn't fork on host. */
#ifdef _WIN32
#include <windows.h>
void *jit_region_alloc(size_t size)
{
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE,
                        PAGE_READWRITE);
}
int jit_region_make_rx(void *p, size_t size)
{
    DWORD old;
    return VirtualProtect(p, size, PAGE_EXECUTE_READ, &old) ? 0 : -1;
}
void jit_region_free(void *p, size_t size)
{
    (void)size;  /* MEM_RELEASE expects size = 0 paired with original ptr */
    VirtualFree(p, 0, MEM_RELEASE);
}
long jit_region_page_size(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (long)si.dwPageSize;
}
#else
#include <sys/mman.h>
#include <unistd.h>
void *jit_region_alloc(size_t size)
{
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
int jit_region_make_rx(void *p, size_t size)
{
    return mprotect(p, size, PROT_READ | PROT_EXEC);
}
void jit_region_free(void *p, size_t size)
{
    munmap(p, size);
}
long jit_region_page_size(void)
{
    return sysconf(_SC_PAGESIZE);
}
#endif

/* Sentinel value returned by jit_region_alloc on failure. Both the
 * POSIX (mmap returns MAP_FAILED → NULL via the wrapper) and Windows
 * (VirtualAlloc returns NULL) paths map to NULL, so callers only need
 * to test for NULL. */
#define MINO_JIT_REGION_ALLOC_FAILED  NULL

/* ----- region book-keeping ------------------------------------------------ */

int region_track(mino_state *S, void *ptr, size_t size, void *aux_ptr)
{
    struct mino_jit_region *node =
        (struct mino_jit_region *)malloc(sizeof(*node));
    if (node == NULL) return -1;
    node->ptr     = ptr;
    node->size    = size;
    node->aux_ptr = aux_ptr;
    node->next    = S->jit.jit_regions;
    S->jit.jit_regions = node;
    return 0;
}

/* ----- slab pool ----------------------------------------------------------- */

/* Cutoff for slab-pool eligibility. Fns whose pre-page-rounding
 * `need_bytes` fits under this size go through the slab pool; larger
 * fns keep the one-page-per-fn allocator. Sized just below the
 * smallest host page (Linux x86_64 = 4 KB) so the same cutoff is
 * meaningful across all hosts; on macOS arm64 with a 16 KB page,
 * multiple slots fit per slab. */
#define MINO_JIT_SLAB_CUTOFF      ((size_t)4096)

/* Per-slot alignment. Stencil-emitted code is read by the host CPU;
 * keeping slots 16-byte-aligned matches the alignment trampoline /
 * pool layout already assumes inside a single fn. */
#define MINO_JIT_SLAB_SLOT_ALIGN  ((size_t)16)

static struct mino_jit_slab *jit_slab_alloc_new(mino_state *S, size_t need)
{
    struct mino_jit_slab *slab;
    long                  page_l;
    size_t                page;
    void                 *p;
    page_l = jit_region_page_size();
    if (page_l <= 0) return NULL;
    page = (size_t)page_l;
    /* For requests larger than one host page, span enough pages. */
    if (need > page) {
        page = (need + page - 1) & ~(page - 1);
    }
    p = jit_region_alloc(page);
    if (p == MINO_JIT_REGION_ALLOC_FAILED) return NULL;
    slab = (struct mino_jit_slab *)malloc(sizeof(*slab));
    if (slab == NULL) {
        jit_region_free(p, page);
        return NULL;
    }
    slab->page        = p;
    slab->page_size   = page;
    slab->bump_offset = 0;
    slab->live_slots  = 0;
    slab->next        = S->jit_slabs;
    S->jit_slabs      = slab;
    return slab;
}

/* Find a slab with room for `need` aligned bytes; allocate a new
 * slab when no fit. Returns the slab whose `page` + current
 * `bump_offset` is the slot start. Caller is responsible for the
 * RW/RX cycle around the fill. */
struct mino_jit_slab *jit_slab_acquire(mino_state *S, size_t need)
{
    struct mino_jit_slab *slab;
    size_t                aligned;
    aligned = (need + MINO_JIT_SLAB_SLOT_ALIGN - 1)
              & ~(MINO_JIT_SLAB_SLOT_ALIGN - 1);
    for (slab = S->jit_slabs; slab != NULL; slab = slab->next) {
        if (slab->bump_offset + aligned <= slab->page_size) {
            return slab;
        }
    }
    return jit_slab_alloc_new(S, aligned);
}

int jit_slab_make_rw(struct mino_jit_slab *slab)
{
#ifdef _WIN32
    DWORD old;
    return VirtualProtect(slab->page, slab->page_size,
                          PAGE_READWRITE, &old) ? 0 : -1;
#else
    return mprotect(slab->page, slab->page_size,
                    PROT_READ | PROT_WRITE);
#endif
}

int jit_slab_make_rx(struct mino_jit_slab *slab)
{
#ifdef _WIN32
    DWORD old;
    return VirtualProtect(slab->page, slab->page_size,
                          PAGE_EXECUTE_READ, &old) ? 0 : -1;
#else
    return mprotect(slab->page, slab->page_size,
                    PROT_READ | PROT_EXEC);
#endif
}

/* Compile failure cleanup: release the JIT memory acquired for the
 * compile. Slab path: re-seal the page to RX (the bump cursor stays
 * unchanged, so the just-attempted slot bytes are reusable by the
 * next compile). Legacy path: munmap the fn's dedicated page. */
void jit_compile_cleanup(struct mino_jit_slab *slab, void *region,
                          size_t total_size)
{
    if (slab != NULL) {
        (void)jit_slab_make_rx(slab);
    } else {
        jit_region_free(region, total_size);
    }
}

/* Per-fn slot release. Called from mino_jit_invalidate when a bc
 * record gives up its native slot (deopt, IC-gen mismatch, redef).
 * Decrements the owning slab's live_slots refcount; on the last
 * release, unlinks the slab from S->jit_slabs and munmaps the page.
 * The bump cursor inside the slab is never rewound -- slots are
 * append-only within a slab, and reclamation happens at slab
 * granularity, not slot granularity. */
void mino_jit_slab_release(mino_state *S, struct mino_jit_slab *slab)
{
    struct mino_jit_slab **pp;
    if (slab == NULL) return;
    if (slab->live_slots > 0) slab->live_slots--;
    if (slab->live_slots != 0) return;
    for (pp = &S->jit_slabs; *pp != NULL; pp = &(*pp)->next) {
        if (*pp == slab) {
            *pp = slab->next;
            break;
        }
    }
    if (slab->page != NULL) jit_region_free(slab->page, slab->page_size);
    free(slab);
}

void mino_jit_free_all(mino_state *S)
{
    struct mino_jit_region *node = S->jit.jit_regions;
    while (node != NULL) {
        struct mino_jit_region *next = node->next;
        if (node->aux_ptr != NULL) free(node->aux_ptr);
        if (node->ptr     != NULL) jit_region_free(node->ptr, node->size);
        free(node);
        node = next;
    }
    S->jit.jit_regions = NULL;
    {
        struct mino_jit_slab *slab = S->jit_slabs;
        while (slab != NULL) {
            struct mino_jit_slab *next = slab->next;
            if (slab->page != NULL) jit_region_free(slab->page, slab->page_size);
            free(slab);
            slab = next;
        }
        S->jit_slabs = NULL;
    }
}

#endif /* MINO_CPJIT_HOST */

/* Keep this TU non-empty under -Werror=pedantic when MINO_CPJIT_HOST
 * isn't defined for the build target. */
typedef int mino_jit_region_tu_marker;
