/*
 * jit_retire_reclaim_test.c -- a JIT slab page must not be unmapped while
 * any thread can still hold a C-stack return address into it. The runtime
 * drops its per-state lock at one point during execution: the backjump
 * safepoint auto-yield, where a worker parks with a return address into
 * its executing slab page still live on the C stack. If a peer thread
 * redefines a function that shares that page, the invalidation path
 * releases the slab's last slot -- and must not munmap the page while the
 * parked worker can resume and return into it.
 *
 * The contract this test pins: releasing a slab's last slot while a
 * worker is inside native JIT code (jit_invoke_depth > 0) retires the
 * page rather than freeing it, and the page is executable throughout.
 * The page is reclaimed only at a quiescent point where no context is in
 * native JIT code. A sibling thread executes the slab's committed thunk
 * across the whole release-and-reclaim sequence; under a free-on-release
 * design its return address lands in unmapped memory and ASan reports a
 * use-after-free / the process faults. The deferred design keeps the page
 * mapped and executable until quiescence.
 *
 * Build (from repo root):
 *   cc -std=c99 -Wall -Wextra -Wpedantic -O2 -DMINO_CPJIT=1 -Isrc \
 *       -o jit_retire_reclaim_test tests/jit_retire_reclaim_test.c \
 *       src/SRC.c -lm -pthread
 * Run: ./jit_retire_reclaim_test
 */

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* nanosleep */
#endif

#include "mino.h"
#include "runtime/internal.h"
#include "eval/bc/internal.h"
#include "eval/bc/jit.h"
#include "eval/bc/jit/internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <pthread.h>
#include <time.h>
#endif

static int failures = 0;

#define REQUIRE(cond, msg)                                         \
    do {                                                           \
        if (!(cond)) {                                             \
            fprintf(stderr, "FAIL (%s:%d): %s\n",                  \
                    __FILE__, __LINE__, (msg));                    \
            failures++;                                            \
        }                                                          \
    } while (0)

#ifdef MINO_CPJIT_HOST

#if defined(MINO_CPJIT_HOST_ARM64)
/* mov w0, #0 ; ret */
static const unsigned char RET0[] = { 0x00, 0x00, 0x80, 0x52,
                                      0xc0, 0x03, 0x5f, 0xd6 };
#elif defined(MINO_CPJIT_HOST_X86_64)
/* xor eax, eax ; ret */
static const unsigned char RET0[] = { 0x31, 0xc0, 0xc3 };
#endif

typedef int (*thunk_fn)(void);

static thunk_fn commit_ret0_slot(struct mino_jit_slab *slab)
{
    size_t         slot_off = slab->bump_offset;
    size_t         aligned  = (sizeof(RET0) + MINO_JIT_SLAB_SLOT_ALIGN - 1)
                              & ~(MINO_JIT_SLAB_SLOT_ALIGN - 1);
    unsigned char *entry;

    if (jit_slab_make_rw(slab) != 0) return NULL;
    entry = mino_jit_slab_exec_base(slab) + slot_off;
    memcpy(mino_jit_slab_write_base(slab) + slot_off, RET0, sizeof(RET0));
    if (jit_slab_make_rx(slab) != 0) return NULL;
#if defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache((char *)entry, (char *)entry + sizeof(RET0));
#endif
    slab->bump_offset = slot_off + aligned;
    return (thunk_fn)(void *)entry;
}

struct worker_arg {
    thunk_fn      fn;
    volatile int *run;
    volatile int *ran;
};

static void *worker_main(void *p)
{
    struct worker_arg *a = (struct worker_arg *)p;
    while (*a->run) {
        if (a->fn() == 0) *a->ran = 1;
    }
    return NULL;
}

static void nap(void)
{
    struct timespec ts;
    ts.tv_sec  = 0;
    ts.tv_nsec = 1000000; /* 1 ms */
    nanosleep(&ts, NULL);
}

/* Count the slabs currently on the state's live pool. */
static int count_live_slabs(mino_state *S)
{
    int n = 0;
    for (struct mino_jit_slab *s = S->jit_slabs; s != NULL; s = s->next) n++;
    return n;
}

#endif /* MINO_CPJIT_HOST */

int main(void)
{
    mino_state *S = mino_state_new();
    REQUIRE(S != NULL, "state creation");

#ifdef MINO_CPJIT_HOST
    if (S != NULL) {
        struct mino_jit_slab *slab = jit_slab_acquire(S, sizeof(RET0));
        REQUIRE(slab != NULL, "slab acquired");
        if (slab != NULL) {
            thunk_fn first = commit_ret0_slot(slab);
            REQUIRE(first != NULL, "slot committed");
            /* One slot lives on this slab. */
            slab->live_slots = 1;
            REQUIRE(count_live_slabs(S) == 1, "slab on the live pool");

            if (first != NULL) {
                volatile int      run = 1, ran = 0;
                struct worker_arg wa = { first, &run, &ran };
                pthread_t         th;

                REQUIRE(first() == 0, "thunk runs after its compile");

                if (pthread_create(&th, NULL, worker_main, &wa) == 0) {
                    /* Model a peer worker paused at a backjump safepoint
                     * with a live return address into the slab page: mark
                     * the current context as inside native JIT code. */
                    mino_current_ctx(S)->jit_invoke_depth = 1;

                    nap();
                    /* The last slot is released (redefinition path). With
                     * a worker still in native JIT, the page must NOT be
                     * unmapped: it is retired, staying mapped + executable
                     * so the sibling keeps running its thunk. */
                    mino_jit_slab_release(S, slab);
                    REQUIRE(count_live_slabs(S) == 0,
                            "released slab off the live pool");
                    nap();
                    REQUIRE(ran,
                            "sibling kept executing across the release "
                            "while a worker was in native JIT");
                    REQUIRE(S->jit_slabs_retired != NULL,
                            "spent slab retired, not freed, while a worker "
                            "was in native JIT");

                    /* Stop the sibling so no thread can execute the page,
                     * then model the worker leaving native JIT: the
                     * quiescent point. A reclaim now frees the retired
                     * page. */
                    run = 0;
                    pthread_join(th, NULL);
                    mino_current_ctx(S)->jit_invoke_depth = 0;
                    mino_jit_reclaim_retired(S);

                    REQUIRE(S->jit_slabs_retired == NULL,
                            "retired list drained after quiescent reclaim");
                }
            }
        }
    }
#endif

    if (S != NULL) mino_state_free(S);

    if (failures == 0) {
        printf("jit_retire_reclaim_test: all checks passed\n");
        return 0;
    }
    return 1;
}
