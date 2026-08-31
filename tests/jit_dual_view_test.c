/*
 * jit_dual_view_test.c -- the JIT packs several compiled functions onto
 * one host page. Compiling a new function onto a page that already holds
 * a live function must never revoke the execute permission of the page
 * as seen by a thread currently running the older function. On a page
 * that is flipped between writable and executable, a sibling worker
 * paused at a safepoint with a return address into the page resumes into
 * a non-executable page and faults -- a W^X violation on a live page.
 *
 * The contract this test pins: after jit_slab_make_rw prepares a slab
 * for a second compile, code already committed to that slab through its
 * executable base still runs. The executable mapping is never the one
 * that is made writable.
 *
 * Build (from repo root):
 *   cc -std=c99 -Wall -Wextra -Wpedantic -O2 -DMINO_CPJIT=1 -Isrc \
 *       -o jit_dual_view_test tests/jit_dual_view_test.c src/SRC.c -lm
 * Run: ./jit_dual_view_test
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

/* A minimal native function: return 0. The host encodings are the two
 * the JIT targets. Each writes a single scalar-return thunk. */
#if defined(MINO_CPJIT_HOST_ARM64)
/* mov w0, #0 ; ret */
static const unsigned char RET0[] = { 0x00, 0x00, 0x80, 0x52,
                                      0xc0, 0x03, 0x5f, 0xd6 };
#elif defined(MINO_CPJIT_HOST_X86_64)
/* xor eax, eax ; ret */
static const unsigned char RET0[] = { 0x31, 0xc0, 0xc3 };
#endif

typedef int (*thunk_fn)(void);

/* Copy a RET0 thunk into slab's current bump slot through the writable
 * view, seal the slab executable, and return the executable entry
 * address of the slot. Advances the bump cursor past the slot. */
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

/* A sibling worker: run the committed thunk in a tight loop while the
 * main thread compiles a second slot onto the shared slab page. On a
 * page-flip design the thunk's page loses execute permission for the
 * duration of the compile and this thread faults. The dual-mapped and
 * per-thread-toggle designs keep this thread's view executable. */
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

#endif /* MINO_CPJIT_HOST */

int main(void)
{
    mino_state *S = mino_state_new();
    REQUIRE(S != NULL, "state creation");

#ifdef MINO_CPJIT_HOST
    if (S != NULL) {
        struct mino_jit_slab *slab = jit_slab_acquire(S, sizeof(RET0));
        thunk_fn              first;
        REQUIRE(slab != NULL, "slab acquired");
        if (slab != NULL) {
            first = commit_ret0_slot(slab);
            REQUIRE(first != NULL, "first slot committed");
            if (first != NULL) {
                volatile int      run = 1, ran = 0;
                struct worker_arg wa = { first, &run, &ran };
                pthread_t         th;
                int               joined = 0;

                REQUIRE(first() == 0, "first thunk runs after its compile");

                /* On the dual-mapped path the executable and writable
                 * views are distinct mappings of the same memory, so
                 * opening the write window never touches the executing
                 * mapping. This is the structural guarantee the page-flip
                 * design lacks. */
#if defined(__linux__)
                REQUIRE(mino_jit_slab_exec_base(slab)
                        != mino_jit_slab_write_base(slab),
                        "exec and write views are separate mappings");
#endif

                if (pthread_create(&th, NULL, worker_main, &wa) == 0) {
                    /* Let the worker enter its execute loop, then open a
                     * sustained write window on the shared page while the
                     * worker keeps executing the first slot. Under a
                     * page-flip design the page is non-executable for the
                     * whole window and the worker faults with near
                     * certainty; the dual-mapped and per-thread-toggle
                     * designs keep the worker's view executable. The first
                     * slot at offset 0 is never overwritten. */
                    size_t second_off = slab->bump_offset;
                    nap();
                    for (int i = 0; i < 32; i++) {
                        thunk_fn second;
                        slab->bump_offset = second_off;
                        if (jit_slab_make_rw(slab) != 0) break;
                        memcpy(mino_jit_slab_write_base(slab) + second_off,
                               RET0, sizeof(RET0));
                        nap();  /* hold the window open while the worker runs */
                        (void)jit_slab_make_rx(slab);
                        second = (thunk_fn)(void *)
                                 (mino_jit_slab_exec_base(slab) + second_off);
                        REQUIRE(second() == 0, "second thunk runs");
                    }
                    run = 0;
                    pthread_join(th, NULL);
                    joined = 1;
                    REQUIRE(ran,
                            "sibling stayed executable across concurrent "
                            "compiles onto the shared page");
                }
                (void)joined;
            }
        }
    }
#endif

    if (S != NULL) mino_state_free(S);

    if (failures == 0) {
        printf("jit_dual_view_test: all checks passed\n");
        return 0;
    }
    return 1;
}
