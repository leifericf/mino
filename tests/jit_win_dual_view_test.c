/*
 * jit_win_dual_view_test.c -- the JIT packs several compiled functions
 * onto one host page. Compiling a new function onto a page that already
 * holds a live function must never revoke the execute permission of the
 * page as seen by a thread currently running the older function. The
 * structural guarantee that closes that race on every threaded host is a
 * dual view: the executable mapping the code runs through is a distinct
 * mapping from the writable one the compiler copies through, so opening
 * the write window for a fresh compile never disturbs the executing
 * mapping. Windows worker threads (host_threads.c) make Windows a
 * threaded JIT host, so the Windows slab must be dual-viewed too.
 *
 * The contract this test pins, and which the page-flip fallback lacks:
 *   - the slab's executable base and writable base are separate
 *     mappings of the same bytes (exec_base != write_base);
 *   - a slot committed through the writable view runs through the
 *     executable view;
 *   - jit_slab_make_rw / jit_slab_make_rx never change the executable
 *     view's protection: a slot committed before a second write window
 *     is opened keeps running while that window is open.
 *
 * This is a single-threaded structural check so it compiles and runs on
 * every JIT host, including the cross-compiled Windows binary, without a
 * host threading primitive.
 *
 * Build (from repo root):
 *   cc -std=c99 -Wall -Wextra -Wpedantic -O2 -DMINO_CPJIT=1 -Isrc \
 *       -o jit_win_dual_view_test tests/jit_win_dual_view_test.c \
 *       src/SRC.c -lm
 * Run: ./jit_win_dual_view_test
 */

#include "mino.h"
#include "runtime/internal.h"
#include "eval/bc/internal.h"
#include "eval/bc/jit.h"
#include "eval/bc/jit/internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
 * the JIT targets. */
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
 * view, seal the slab executable, flush the I-cache for the slot, and
 * return the executable entry address. Advances the bump cursor. */
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
#elif defined(_WIN32)
    FlushInstructionCache(GetCurrentProcess(), entry, sizeof(RET0));
#endif
    slab->bump_offset = slot_off + aligned;
    return (thunk_fn)(void *)entry;
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
            REQUIRE(first != NULL, "first slot committed");

            /* On every dual-view host the executable and writable bases
             * are distinct mappings of the same memory. The page-flip
             * fallback aliases them, so this is the check that distinguishes
             * a true dual view from the fallback. Apple's MAP_JIT design is
             * a single mapping toggled per-thread, so it is exempt. */
#if defined(__linux__) || defined(_WIN32)
            REQUIRE(mino_jit_slab_exec_base(slab)
                    != mino_jit_slab_write_base(slab),
                    "exec and write views are separate mappings");
#endif

            if (first != NULL) {
                /* Open a second write window on the shared page (a fresh
                 * compile onto the same slab) and commit a second slot. */
                size_t   second_off = slab->bump_offset;
                thunk_fn second;

                REQUIRE(first() == 0, "first thunk runs after its compile");

                REQUIRE(jit_slab_make_rw(slab) == 0, "reopen write window");
#if defined(__linux__) || defined(_WIN32)
                /* On a true dual view the executable mapping is a separate,
                 * never-re-protected mapping, so the first slot keeps
                 * executing even while this thread holds the write window
                 * open on the shared page. On a page-flip design the whole
                 * page is non-executable for the duration and this faults.
                 * (Apple's single-mapping MAP_JIT toggles execute per
                 * thread, so its exec/write windows are exclusive on one
                 * thread; that guarantee is proven across threads by
                 * jit_dual_view_test and is exempt here.) */
                REQUIRE(first() == 0,
                        "first slot still executable while a write window "
                        "is open on the shared page");
#endif
                memcpy(mino_jit_slab_write_base(slab) + second_off,
                       RET0, sizeof(RET0));
                REQUIRE(jit_slab_make_rx(slab) == 0, "reseal write window");
                second = (thunk_fn)(void *)
                         (mino_jit_slab_exec_base(slab) + second_off);
#if defined(__GNUC__) || defined(__clang__)
                __builtin___clear_cache(
                    (char *)second, (char *)second + sizeof(RET0));
#elif defined(_WIN32)
                FlushInstructionCache(GetCurrentProcess(), second,
                                      sizeof(RET0));
#endif
                REQUIRE(second() == 0, "second thunk runs");
                REQUIRE(first() == 0,
                        "first slot still runs after the second compile");
            }
        }
    }
#endif

    if (S != NULL) mino_state_free(S);

    if (failures == 0) {
        printf("jit_win_dual_view_test: all checks passed\n");
        return 0;
    }
    return 1;
}
