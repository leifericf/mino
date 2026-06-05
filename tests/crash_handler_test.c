/* tests/crash_handler_test.c
 *
 * Validates the portable backtrace primitive (src/runtime/crash_backtrace.h)
 * that the CLI crash handler uses in place of glibc's <execinfo.h>. The
 * spike that introduced it must keep producing a best-effort backtrace on
 * glibc, musl, macOS, and mingw from one code path. Exercising the
 * mechanism directly (rather than the CLI) lets this test run unchanged
 * under a fully static musl binary, where <execinfo.h> does not exist --
 * which is the whole point of the rework.
 *
 * Checks:
 *   1. direct capture from nested frames yields >= 1 non-NULL frame;
 *   2. the cap and skip arguments are honored;
 *   3. (POSIX) capture from inside a real signal handler yields >= 1
 *      frame -- the path the crash handler actually takes on a fatal
 *      signal.
 *
 * On a toolchain with no unwinder (e.g. MSVC, which never compiles the
 * CLI) the primitive is a documented no-op returning -1; the test asserts
 * that contract and passes.
 */

/* sigaction / sigsetjmp / siglongjmp are POSIX, hidden under bare
 * -std=c99; request the POSIX.1-2008 surface before any include. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crash_backtrace.h"

static int failures = 0;

#define CHECK(cond, msg)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            fprintf(stderr, "FAIL: %s\n", (msg));         \
            failures++;                                   \
        } else {                                          \
            fprintf(stderr, "ok: %s\n", (msg));           \
        }                                                 \
    } while (0)

#if defined(__GNUC__)
#  define MINO_NOINLINE __attribute__((noinline))
#else
#  define MINO_NOINLINE
#endif

#ifdef MINO_HAVE_UNWIND_BACKTRACE

static void   *g_frames[64];
static int     g_cap;
static volatile int g_depth_count;

/* noinline + the return-value plumbing keep the compiler from folding
 * these two frames into main, so the unwinder has a real chain to walk. */
MINO_NOINLINE static int level_two(void)
{
    g_depth_count = mino_capture_backtrace(g_frames, g_cap, 0);
    return g_depth_count;
}

MINO_NOINLINE static int level_one(void)
{
    return level_two() + 1;
}

#if !defined(_WIN32)
#include <signal.h>
#include <setjmp.h>

static sigjmp_buf   g_jmp;
static volatile int g_sig_count = -2;
static void        *g_sig_frames[64];

static void seg_handler(int sig)
{
    (void)sig;
    g_sig_count = mino_capture_backtrace(g_sig_frames, 64, 0);
    siglongjmp(g_jmp, 1);
}
#endif /* !_WIN32 */

int main(void)
{
    g_cap = 64;

    /* 1. direct capture from nested frames */
    (void)level_one();
    CHECK(g_depth_count >= 1, "direct capture yields >= 1 frame");
    {
        int all_nonnull = 1;
        for (int i = 0; i < g_depth_count; i++)
            if (g_frames[i] == NULL) all_nonnull = 0;
        CHECK(all_nonnull, "all captured frames are non-NULL");
    }

    /* 2. cap and skip are honored */
    {
        void *f2[64];
        int n0 = mino_capture_backtrace(f2, 64, 0);
        int n1 = mino_capture_backtrace(f2, 64, 1);
        CHECK(n1 <= n0, "skip>0 captures no more frames than skip=0");
        CHECK(n0 - n1 <= 1, "skip=1 drops at most one frame");
    }
    {
        void *f3[2];
        int n = mino_capture_backtrace(f3, 2, 0);
        CHECK(n >= 0 && n <= 2, "capture respects cap");
    }

#if !defined(_WIN32)
    /* 3. capture from inside a real signal handler */
    if (sigsetjmp(g_jmp, 1) == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = seg_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGSEGV, &sa, NULL);
        raise(SIGSEGV);
        CHECK(0, "unreachable: raise(SIGSEGV) returned");
    } else {
        CHECK(g_sig_count >= 1, "capture from signal handler yields >= 1 frame");
    }
#endif /* !_WIN32 */

    if (failures) {
        fprintf(stderr, "crash_handler_test: %d failure(s)\n", failures);
        return 1;
    }
    fprintf(stderr, "crash_handler_test: all checks passed\n");
    return 0;
}

#else /* !MINO_HAVE_UNWIND_BACKTRACE */

int main(void)
{
    void *f[4];
    CHECK(mino_capture_backtrace(f, 4, 0) == -1,
          "no-unwinder build reports backtrace unavailable (-1)");
    return failures ? 1 : 0;
}

#endif /* MINO_HAVE_UNWIND_BACKTRACE */
