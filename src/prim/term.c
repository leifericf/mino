/*
 * term.c -- terminal info primitives: tty?, terminal-width,
 * terminal-height.
 *
 * tty? answers whether one of the standard streams is attached to a
 * terminal. terminal-width / terminal-height answer the size of the
 * controlling terminal: TIOCGWINSZ over stdout, then stderr, then
 * stdin (the shutil.get_terminal_size probe order); when no fd is a
 * terminal, the COLUMNS / ROWS environment variables when set and
 * numeric; else the 80x24 default. Width and height fall back
 * independently, matching python's shutil.
 *
 * Env parsing is bounded and total (check-security stance): a value
 * counts only when it is 1..6 plain ASCII digits in 1..10000. No
 * strtol, no sign, no whitespace, no allocation from env content, so
 * no untrusted env string can overflow, underflow, or hang anything.
 *
 * Prims are ungated: reading terminal metadata is info-only, the
 * same stance as the time prims (ADR 21 lineage). Nothing here
 * writes to the terminal or mutates state; the style/progress layer
 * over these lives in the bundled mino.term lib.
 *
 * Windows portability: _isatty answers tty? on the win_console path.
 * The size prims are a deliberate stub there: TIOCGWINSZ does not
 * exist and GetConsoleScreenBufferInfo is the future native
 * implementation, but this codebase's native target here is unix
 * (see time.c / net.c for the same #ifdef _WIN32 guard idiom), so
 * the win path runs the env fallback and the 80x24 default and the
 * console API lands when a windows-native size consumer asks for it.
 */

#define _POSIX_C_SOURCE 200809L

#include "prim/internal.h"
#include "mino.h"

#ifdef _WIN32
#  include <io.h> /* _isatty */
#else
#  include <unistd.h>
#  include <sys/ioctl.h>
#endif

#include <stdlib.h>
#include <string.h>

/* ---- fd selection ------------------------------------------------ */

/* Map the :stdout / :stderr / :stdin keyword to its fd. Returns 0
 * with the classified throw already raised on any other value. */
static int term_stream_fd(mino_state *S, mino_val *v, const char *who,
                          int *fd)
{
    const char *name;
    if (v == NULL || mino_type_of(v) != MINO_KEYWORD) {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "%s: stream must be the keyword :stdout, :stderr, or "
                 ":stdin", who);
        prim_throw_classified(S, "eval/type", "MTY001", msg);
        return 0;
    }
    name = v->as.s.data;
    if (strcmp(name, "stdout") == 0) {
        *fd = 1;
    } else if (strcmp(name, "stderr") == 0) {
        *fd = 2;
    } else if (strcmp(name, "stdin") == 0) {
        *fd = 0;
    } else {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "%s: unknown stream :%s; expected :stdout, :stderr, or "
                 ":stdin", who, name);
        prim_throw_classified(S, "eval/type", "MTY001", msg);
        return 0;
    }
    return 1;
}

/* ---- size probes -------------------------------------------------- */

/* Bounded, total env dimension parse: 1..6 plain digits, value in
 * 1..10000. Returns 0 for anything else (unset, empty, signed,
 * decimal, padded, overlong, out of range). Max 6 digits cannot
 * overflow a long; the range cap keeps nonsense env from producing
 * absurd layout numbers. */
static int term_env_dim(const char *name, long *out)
{
    const char *v = getenv(name);
    size_t i, n;
    long val = 0;
    if (v == NULL) return 0;
    n = strlen(v);
    if (n < 1 || n > 6) return 0;
    for (i = 0; i < n; i++) {
        if (v[i] < '0' || v[i] > '9') return 0;
        val = val * 10 + (long)(v[i] - '0');
    }
    if (val < 1 || val > 10000) return 0;
    *out = val;
    return 1;
}

#ifdef _WIN32

/* win_console size stub: no TIOCGWINSZ on Windows. The env fallback
 * and the 80x24 default below carry the contract; the native
 * GetConsoleScreenBufferInfo probe lands here when a windows-native
 * size consumer asks for it (portability note in the file header). */
static int term_ioctl_dims(unsigned short *cols, unsigned short *rows)
{
    (void)cols;
    (void)rows;
    return 0;
}

#else /* POSIX */

/* TIOCGWINSZ over the standard streams, stdout first (the
 * shutil.get_terminal_size probe order). A zero dimension from the
 * kernel counts as a miss; some pty layers report 0x0 before the
 * first resize. */
static int term_ioctl_dims(unsigned short *cols, unsigned short *rows)
{
    static const int fds[3] = { 1, 2, 0 }; /* stdout, stderr, stdin */
    struct winsize ws;
    size_t i;
    for (i = 0; i < 3; i++) {
        memset(&ws, 0, sizeof ws);
        if (ioctl(fds[i], TIOCGWINSZ, &ws) == 0
            && ws.ws_col > 0 && ws.ws_row > 0) {
            *cols = ws.ws_col;
            *rows = ws.ws_row;
            return 1;
        }
    }
    return 0;
}

#endif /* _WIN32 */

#define TERM_DEFAULT_COLS 80
#define TERM_DEFAULT_ROWS 24

static long term_width(void)
{
    unsigned short c = 0, r = 0;
    long v;
    if (term_ioctl_dims(&c, &r)) return (long)c;
    if (term_env_dim("COLUMNS", &v)) return v;
    return TERM_DEFAULT_COLS;
}

static long term_height(void)
{
    unsigned short c = 0, r = 0;
    long v;
    if (term_ioctl_dims(&c, &r)) return (long)r;
    if (term_env_dim("ROWS", &v)) return v;
    return TERM_DEFAULT_ROWS;
}

/* ---- primitives --------------------------------------------------- */

/* (tty? :stdout | :stderr | :stdin) -- isatty over the named
 * standard stream. */
static mino_val *prim_tty_p(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *v;
    int fd;
    int tty;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "tty? requires one argument");
    }
    v = args->as.cons.car;
    if (!term_stream_fd(S, v, "tty?", &fd)) return NULL;
#ifdef _WIN32
    tty = _isatty(fd) != 0;
#else
    tty = isatty(fd) != 0;
#endif
    return tty ? mino_true(S) : mino_false(S);
}

/* Shared zero-argument arity check for the size prims. */
static int term_no_args(mino_state *S, mino_val *args, const char *who)
{
    if (mino_is_cons(args)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s requires no arguments", who);
        prim_throw_classified(S, "eval/arity", "MAR001", msg);
        return 0;
    }
    return 1;
}

/* (terminal-width) -- columns of the controlling terminal: ioctl,
 * else COLUMNS, else 80. */
static mino_val *prim_terminal_width(mino_state *S, mino_val *args,
                                     mino_env *env)
{
    (void)env;
    if (!term_no_args(S, args, "terminal-width")) return NULL;
    return mino_int(S, (int64_t)term_width());
}

/* (terminal-height) -- rows of the controlling terminal: ioctl,
 * else ROWS, else 24. */
static mino_val *prim_terminal_height(mino_state *S, mino_val *args,
                                      mino_env *env)
{
    (void)env;
    if (!term_no_args(S, args, "terminal-height")) return NULL;
    return mino_int(S, (int64_t)term_height());
}

const mino_prim_def k_prims_term[] = {
    {"tty?", prim_tty_p,
     "Returns true when the given standard stream (:stdout, :stderr, "
     "or :stdin) is attached to a terminal, false for files, pipes, "
     "and other redirects. Info-only floor prim."},
    {"terminal-width", prim_terminal_width,
     "Returns the terminal width in columns: TIOCGWINSZ when a "
     "standard stream is a terminal (stdout first), else the COLUMNS "
     "environment variable when set to a plain numeric value, else "
     "80. Pair with tty? for color=auto gating."},
    {"terminal-height", prim_terminal_height,
     "Returns the terminal height in rows: TIOCGWINSZ when a "
     "standard stream is a terminal (stdout first), else the ROWS "
     "environment variable when set to a plain numeric value, else "
     "24."},
};

const size_t k_prims_term_count =
    sizeof(k_prims_term) / sizeof(k_prims_term[0]);
