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

/* The dual-mapped slab path (Linux) uses memfd_create / shm_open /
 * ftruncate, which the C library hides behind feature-test macros. Ask
 * for the GNU set on Linux before any header is pulled in; it implies
 * POSIX 2008 (ftruncate, shm_open) and exposes memfd_create and
 * MAP_ANONYMOUS. Other hosts don't take this path and are unaffected. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include "internal.h"
#include "../jit.h"

#ifdef MINO_CPJIT_HOST

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* Slab write-protection strategy. A JIT slab packs several compiled
 * functions onto one host page; a fresh compile onto a page that
 * already holds a live function must never revoke the page's execute
 * permission as seen by a thread running one of those older functions.
 * Three mechanisms deliver that, picked by host at build time:
 *
 *   MINO_JIT_SLAB_DUAL    -- POSIX with memfd/shm: back the page with a
 *                            shared memory object and keep two permanent
 *                            mappings, an executable one that is never
 *                            made writable and a writable alias the
 *                            compiler copies through. The executable
 *                            mapping's permissions never change, so a
 *                            sibling thread executing the page is never
 *                            disturbed.
 *   MINO_JIT_SLAB_MAPJIT  -- Apple: a single MAP_JIT mapping toggled
 *                            between writable and executable with the
 *                            per-thread pthread_jit_write_protect_np.
 *                            The toggle changes only the calling
 *                            thread's view, so sibling threads keep RX.
 *   (fallback)            -- neither available (e.g. Windows): the page
 *                            is flipped with mprotect / VirtualProtect.
 *                            This retains the W^X page-flip race under
 *                            true multithreaded JIT and is only safe
 *                            where the JIT is single-mutator. See BUGS.
 */
#if defined(__APPLE__)
#define MINO_JIT_SLAB_MAPJIT 1
#elif defined(__linux__)
#define MINO_JIT_SLAB_DUAL 1
#endif

/* Host RWX region API. POSIX uses mmap / mprotect / munmap; Windows
 * uses VirtualAlloc / VirtualProtect / VirtualFree. The wrappers
 * below give the rest of this module a uniform allocate/protect/free
 * surface and a uniform `NULL` sentinel so the size-pass + commit-pass
 * logic doesn't fork on host. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
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
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#if defined(MINO_JIT_SLAB_MAPJIT)
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#ifndef MAP_JIT
#define MAP_JIT 0x800
#endif
#endif
#if defined(MINO_JIT_SLAB_DUAL)
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#endif
void *jit_region_alloc(size_t size)
{
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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

/* Sentinel for alloc failure is defined in internal.h as
 * MINO_JIT_REGION_ALLOC_FAILED; no local redefinition needed. */

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

/* Slab-pool constants (MINO_JIT_SLAB_CUTOFF and MINO_JIT_SLAB_SLOT_ALIGN)
 * are defined in internal.h; no local redefinitions needed here. */

/* Back a slab of `page` bytes. Fills exec_out (the mapping code runs
 * and is addressed through), write_out (the mapping the compiler copies
 * through), and fd_out (the shared backing fd, or -1). Returns 0 on
 * success, -1 on failure with nothing left mapped. The exec mapping is
 * the one whose permissions are never disturbed after creation. */
#if defined(MINO_JIT_SLAB_DUAL)
static int jit_slab_backing_open(size_t page)
{
    int fd = -1;
#if defined(SYS_memfd_create)
    /* An anonymous file that lives only as long as its descriptors,
     * created through the raw syscall so glibc-vs-musl memfd_create
     * declaration differences never matter. */
    fd = (int)syscall(SYS_memfd_create, "mino-jit", 0u);
#endif
    if (fd < 0) {
        /* Fallback: an immediately-unlinked POSIX shared memory object. */
        char            name[64];
        static unsigned counter = 0;
        snprintf(name, sizeof(name), "/mino-jit-%d-%u",
                 (int)getpid(), counter++);
        fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) shm_unlink(name);
    }
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)page) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int jit_slab_backing_alloc(size_t page, void **exec_out,
                                  void **write_out, int *fd_out)
{
    int   fd = jit_slab_backing_open(page);
    void *rx, *rw;
    if (fd < 0) return -1;
    rx = mmap(NULL, page, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
    if (rx == MAP_FAILED) { close(fd); return -1; }
    rw = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (rw == MAP_FAILED) { munmap(rx, page); close(fd); return -1; }
    *exec_out  = rx;
    *write_out = rw;
    *fd_out    = fd;
    return 0;
}
#elif defined(MINO_JIT_SLAB_MAPJIT)
static int jit_slab_backing_alloc(size_t page, void **exec_out,
                                  void **write_out, int *fd_out)
{
    void *p = mmap(NULL, page, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
    if (p == MAP_FAILED) return -1;
    *exec_out  = p;   /* one mapping; per-thread write-protect toggles it */
    *write_out = p;
    *fd_out    = -1;
    return 0;
}
#else
static int jit_slab_backing_alloc(size_t page, void **exec_out,
                                  void **write_out, int *fd_out)
{
    void *p = jit_region_alloc(page);   /* single RW/RX-flipped mapping */
    if (p == MINO_JIT_REGION_ALLOC_FAILED) return -1;
    *exec_out  = p;
    *write_out = p;
    *fd_out    = -1;
    return 0;
}
#endif

static void jit_slab_backing_free(void *exec_page, void *write_page,
                                  size_t page, int fd)
{
#if defined(MINO_JIT_SLAB_DUAL)
    if (write_page != NULL && write_page != exec_page)
        munmap(write_page, page);
    if (exec_page != NULL) munmap(exec_page, page);
    if (fd >= 0) close(fd);
#else
    (void)write_page;
    (void)fd;
    if (exec_page != NULL) jit_region_free(exec_page, page);
#endif
}

static struct mino_jit_slab *jit_slab_alloc_new(mino_state *S, size_t need)
{
    struct mino_jit_slab *slab;
    long                  page_l;
    size_t                page;
    void                 *exec_p = NULL;
    void                 *write_p = NULL;
    int                   fd = -1;
    page_l = jit_region_page_size();
    if (page_l <= 0) return NULL;
    page = (size_t)page_l;
    /* For requests larger than one host page, span enough pages. */
    if (need > page) {
        page = (need + page - 1) & ~(page - 1);
    }
    if (jit_slab_backing_alloc(page, &exec_p, &write_p, &fd) != 0)
        return NULL;
    slab = (struct mino_jit_slab *)malloc(sizeof(*slab));
    if (slab == NULL) {
        jit_slab_backing_free(exec_p, write_p, page, fd);
        return NULL;
    }
    slab->page        = exec_p;
    slab->write_page  = write_p;
    slab->page_size   = page;
    slab->bump_offset = 0;
    slab->live_slots  = 0;
    slab->backing_fd  = fd;
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

unsigned char *mino_jit_slab_exec_base(struct mino_jit_slab *slab)
{
    return (unsigned char *)slab->page;
}

unsigned char *mino_jit_slab_write_base(struct mino_jit_slab *slab)
{
    return (unsigned char *)slab->write_page;
}

int jit_slab_make_rw(struct mino_jit_slab *slab)
{
#if defined(MINO_JIT_SLAB_DUAL)
    /* The writable alias is a permanent, separate mapping; no
     * permission change touches the executable mapping. */
    (void)slab;
    return 0;
#elif defined(MINO_JIT_SLAB_MAPJIT)
    /* Per-thread: only the compiling thread's view of the MAP_JIT
     * region becomes writable; siblings keep executing it as RX. */
    (void)slab;
    pthread_jit_write_protect_np(0);
    return 0;
#elif defined(_WIN32)
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
#if defined(MINO_JIT_SLAB_DUAL)
    (void)slab;
    return 0;
#elif defined(MINO_JIT_SLAB_MAPJIT)
    pthread_jit_write_protect_np(1);
    /* The writable window is closed for this thread; flush the I-cache
     * for the slab so freshly written instructions are seen. */
    sys_icache_invalidate(slab->page, slab->page_size);
    return 0;
#elif defined(_WIN32)
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
    jit_slab_backing_free(slab->page, slab->write_page, slab->page_size,
                          slab->backing_fd);
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
            jit_slab_backing_free(slab->page, slab->write_page,
                                  slab->page_size, slab->backing_fd);
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
