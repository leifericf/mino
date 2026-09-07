/* crash_backtrace.h -- portable, async-signal-safe best-effort backtrace.
 *
 * Replaces the glibc-only pair <execinfo.h> backtrace() /
 * backtrace_symbols_fd() that the CLI crash handler used to call. Those
 * symbols do not exist on musl, so a fully static musl build of mino
 * could not link -- which is exactly the standalone Linux artifact the
 * Zig cross-build wants to ship. The compiler unwinder's
 * _Unwind_Backtrace, by contrast, is provided by every GCC-compatible
 * toolchain (gcc, clang, Apple clang, mingw, and zig cc's bundled clang)
 * against glibc, musl, macOS libSystem, and the mingw runtime alike, so
 * one code path now covers every target and the old glibc/Windows
 * #ifdef split collapses.
 *
 * The capture is address-only: resolve the printed frames with
 * addr2line (or atos on macOS). We deliberately do NOT call the
 * symbolizing backtrace_symbols(), which allocates and is not
 * async-signal-safe; the crash handler runs under rule "no allocation".
 *
 * Builds under any C99 compiler. When no unwinder is available (e.g.
 * MSVC, which never compiles mino's CLI) mino_capture_backtrace is a
 * no-op returning -1 so callers can report "unavailable".
 */

#ifndef MINO_CRASH_BACKTRACE_H
#define MINO_CRASH_BACKTRACE_H

#include <stddef.h>

/* GCC-compatible toolchains ship <unwind.h> and the _Unwind_* runtime.
 * This is the same predicate the rest of mino uses to gate GCC/clang
 * builtins, and it is true for gcc, clang, Apple clang, mingw, and the
 * clang bundled in `zig cc`. */
#if defined(__GNUC__)
#  define MINO_HAVE_UNWIND_BACKTRACE 1
#endif

#ifdef MINO_HAVE_UNWIND_BACKTRACE

#include <stdint.h>
#include <unwind.h>

struct mino_bt_cursor {
    void **frames;
    int    count;
    int    cap;
    int    skip;
};

static _Unwind_Reason_Code
mino_bt_trace_cb(struct _Unwind_Context *ctx, void *arg)
{
    struct mino_bt_cursor *cur = (struct mino_bt_cursor *)arg;
    uintptr_t ip = (uintptr_t)_Unwind_GetIP(ctx);
    if (ip == 0) {
        return _URC_END_OF_STACK;
    }
    if (cur->skip > 0) {
        cur->skip--;
        return _URC_NO_REASON;
    }
    if (cur->count >= cur->cap) {
        return _URC_END_OF_STACK;
    }
    cur->frames[cur->count++] = (void *)ip;
    return _URC_NO_REASON;
}

/* Capture up to `cap` instruction pointers into `frames`, skipping the
 * innermost `skip` frames (handler plumbing the caller does not want to
 * show). Returns the number of frames captured. Async-signal-safe: the
 * unwinder walks CFI without allocating, and we only touch the
 * caller-provided buffer. */
static int mino_capture_backtrace(void **frames, int cap, int skip)
{
    struct mino_bt_cursor cur;
    cur.frames = frames;
    cur.count  = 0;
    cur.cap    = cap;
    cur.skip   = skip;
    _Unwind_Backtrace(mino_bt_trace_cb, &cur);
    return cur.count;
}

#else /* !MINO_HAVE_UNWIND_BACKTRACE */

static int mino_capture_backtrace(void **frames, int cap, int skip)
{
    (void)frames;
    (void)cap;
    (void)skip;
    return -1; /* no unwinder on this toolchain */
}

#endif /* MINO_HAVE_UNWIND_BACKTRACE */

#endif /* MINO_CRASH_BACKTRACE_H */
