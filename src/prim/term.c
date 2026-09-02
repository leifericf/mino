/*
 * term.c -- terminal primitives: tty?, terminal-width,
 * terminal-height, read-password.
 *
 * tty? answers whether one of the standard streams is attached to a
 * terminal. terminal-width / terminal-height answer the size of the
 * controlling terminal: TIOCGWINSZ over stdout, then stderr, then
 * stdin (the shutil.get_terminal_size probe order); when no fd is a
 * terminal, the COLUMNS / ROWS environment variables when set and
 * numeric; else the 80x24 default. Width and height fall back
 * independently, matching python's shutil.
 *
 * read-password reads one line from stdin with terminal echo turned
 * off, so a typed secret never reaches the terminal transcript. The
 * saved terminal mode is restored on every exit path: the normal
 * return restores inline, and a saved-state global plus an atexit
 * restore covers a mid-read exit (a trapped signal's handler calling
 * exit while echo is off). When stdin is not a terminal read-password
 * throws :term/not-a-tty, unless {:allow-pipe true} opts into a plain
 * line read (scripts pipe secrets on purpose). The prim rides
 * MINO_CAP_TERM alongside the other terminal prims.
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
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <unistd.h>
#  include <sys/ioctl.h>
#  include <termios.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>

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

/* ---- read-password ------------------------------------------------ */

/* Saved terminal state for the echo-off window. armed is set while
 * stdin's echo is disabled and cleared once it is restored, so the
 * atexit restore below is a no-op except on a mid-read exit (a trapped
 * signal's handler calling exit while echo is off). Process-global
 * because the restore must run without a mino_state in hand. */
#ifndef _WIN32
static struct termios g_term_saved;
static volatile sig_atomic_t g_term_echo_armed = 0;

/* Restore the saved terminal mode if the echo-off window is still open.
 * Registered with atexit so a process exit mid-read (a signal handler
 * calling exit before read-password returns) never leaves the terminal
 * wedged with echo disabled. */
static void term_restore_echo(void)
{
    if (g_term_echo_armed) {
        g_term_echo_armed = 0;
        (void)tcsetattr(0, TCSAFLUSH, &g_term_saved);
    }
}
#endif

/* Read status for the non-throwing raw line read below. */
typedef enum {
    TERM_LINE_OK = 0,   /* a line was read (possibly empty); *buf/*len set */
    TERM_LINE_EOF,      /* EOF with no bytes; caller returns nil */
    TERM_LINE_OOM       /* allocation or size-overflow; caller throws */
} term_line_status;

/* Read one line from stdin into a growing caller-owned buffer, dropping a
 * single trailing newline. Never throws: it returns a status so a tty
 * caller can restore terminal echo before raising any error, since a
 * longjmp through this function would leave echo disabled. On OK the
 * caller owns *buf and must free it; on EOF/OOM the buffer is already
 * freed. */
static term_line_status term_read_raw_line(char **buf_out, size_t *len_out)
{
    char  *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    char   chunk[256];
    int    saw_any = 0;
    *buf_out = NULL;
    *len_out = 0;
    while (fgets(chunk, sizeof(chunk), stdin) != NULL) {
        size_t cl     = strlen(chunk);
        int    has_nl = cl > 0 && chunk[cl - 1] == '\n';
        saw_any = 1;
        if (has_nl) cl -= 1;
        if (len > SIZE_MAX - cl - 1) {
            free(buf);
            return TERM_LINE_OOM;
        }
        if (len + cl + 1 > cap) {
            size_t nc = cap == 0 ? 256 : cap;
            char  *nb;
            while (nc < len + cl + 1) {
                if (nc > SIZE_MAX / 2) {
                    free(buf);
                    return TERM_LINE_OOM;
                }
                nc *= 2;
            }
            nb = (char *)realloc(buf, nc);
            if (nb == NULL) {
                free(buf);
                return TERM_LINE_OOM;
            }
            buf = nb;
            cap = nc;
        }
        if (cl > 0) memcpy(buf + len, chunk, cl);
        len += cl;
        if (has_nl) break;
    }
    if (!saw_any) {
        free(buf);
        return TERM_LINE_EOF;
    }
    *buf_out = buf;
    *len_out = len;
    return TERM_LINE_OK;
}

/* Read one line from stdin as a GC-owned string, or nil on EOF. Used on
 * the non-tty path, where a throw is safe (no terminal mode to restore). */
static mino_val *term_read_stdin_line(mino_state *S)
{
    char            *buf;
    size_t           len;
    term_line_status st = term_read_raw_line(&buf, &len);
    if (st == TERM_LINE_OOM) {
        return prim_throw_classified(S, "internal", "MIN001",
                                     "read-password: line too long or out "
                                     "of memory");
    }
    if (st == TERM_LINE_EOF) {
        return mino_nil(S);
    }
    {
        mino_val *result = mino_string_n(S, buf, len);
        free(buf);
        return result;
    }
}

/* Read a boolean opts key from a (possibly nil/absent) opts map. Absent
 * or nil is def; a present non-boolean is a contract error (returns -1
 * with the throw already raised). */
static int term_opt_bool(mino_state *S, const mino_val *opts, const char *key,
                         int def, int *out)
{
    const mino_val *v;
    *out = def;
    if (opts == NULL || mino_type_of(opts) != MINO_MAP) return 0;
    v = map_get_val(opts, mino_keyword(S, key));
    if (v == NULL || mino_type_of(v) == MINO_NIL) return 0;
    if (mino_type_of(v) != MINO_BOOL) {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "read-password: opts key :%s must be a boolean", key);
        prim_throw_classified(S, "eval/contract", "MCT001", msg);
        return -1;
    }
    *out = mino_val_bool_get(v);
    return 0;
}

/* (read-password) / (read-password {:allow-pipe bool}) -- read one line
 * from stdin with terminal echo off, returning it without the trailing
 * newline. Throws :term/not-a-tty when stdin is not a terminal unless
 * {:allow-pipe true} opts into a plain line read. */
static mino_val *prim_read_password(mino_state *S, mino_val *args,
                                    mino_env *env)
{
    mino_val *opts = NULL;
    int       allow_pipe = 0;
    int       is_tty;
    (void)env;

    if (mino_is_cons(args)) {
        opts = args->as.cons.car;
        if (mino_is_cons(args->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                                         "read-password takes at most one "
                                         "argument: an opts map");
        }
    }
    if (term_opt_bool(S, opts, "allow-pipe", 0, &allow_pipe) != 0) return NULL;

#ifdef _WIN32
    is_tty = _isatty(0) != 0;
#else
    is_tty = isatty(0) != 0;
#endif

    if (!is_tty) {
        if (!allow_pipe) {
            return prim_throw_classified(S, "term/not-a-tty", "MTT001",
                                         "read-password: stdin is not a "
                                         "terminal; pass {:allow-pipe true} "
                                         "to read a secret from a pipe");
        }
        return term_read_stdin_line(S);
    }

#ifdef _WIN32
    {
        /* Windows console: clear ENABLE_ECHO_INPUT for the read window,
         * restore the prior mode before returning or throwing. The raw
         * read never throws, so the console mode is always restored
         * before any error propagates. */
        HANDLE           h = GetStdHandle(STD_INPUT_HANDLE);
        DWORD            mode = 0;
        char            *buf;
        size_t           len;
        term_line_status st;
        int have_mode = (h != INVALID_HANDLE_VALUE) && GetConsoleMode(h, &mode);
        if (have_mode) {
            SetConsoleMode(h, mode & ~(DWORD)ENABLE_ECHO_INPUT);
        }
        st = term_read_raw_line(&buf, &len);
        if (have_mode) {
            SetConsoleMode(h, mode);
        }
        if (st == TERM_LINE_OOM) {
            return prim_throw_classified(S, "internal", "MIN001",
                                         "read-password: line too long or "
                                         "out of memory");
        }
        if (st == TERM_LINE_EOF) {
            return mino_nil(S);
        }
        {
            mino_val *line = mino_string_n(S, buf, len);
            free(buf);
            return line;
        }
    }
#else
    {
        struct termios   raw;
        char            *buf;
        size_t           len;
        term_line_status st;
        static int       atexit_registered = 0;
        if (tcgetattr(0, &g_term_saved) != 0) {
            return prim_throw_classified(S, "host", "MHO001",
                                         "read-password: cannot read the "
                                         "terminal mode");
        }
        if (!atexit_registered) {
            atexit(term_restore_echo);
            atexit_registered = 1;
        }
        raw = g_term_saved;
        raw.c_lflag &= ~(tcflag_t)ECHO;
        /* Arm the saved-state restore before the read: a signal handler
         * that exits mid-read runs atexit with echo still off. */
        g_term_echo_armed = 1;
        if (tcsetattr(0, TCSAFLUSH, &raw) != 0) {
            g_term_echo_armed = 0;
            return prim_throw_classified(S, "host", "MHO001",
                                         "read-password: cannot disable "
                                         "terminal echo");
        }
        /* The raw read never throws, so echo is always restored below
         * before any error propagates; a longjmp through the read would
         * leave the terminal wedged with echo off. */
        st = term_read_raw_line(&buf, &len);
        /* Restore before returning or throwing; the newline the user's
         * Enter did not echo is printed so the cursor leaves the prompt
         * line. */
        tcsetattr(0, TCSAFLUSH, &g_term_saved);
        g_term_echo_armed = 0;
        fputc('\n', stderr);
        if (st == TERM_LINE_OOM) {
            return prim_throw_classified(S, "internal", "MIN001",
                                         "read-password: line too long or "
                                         "out of memory");
        }
        if (st == TERM_LINE_EOF) {
            return mino_nil(S);
        }
        {
            mino_val *line = mino_string_n(S, buf, len);
            free(buf);
            return line;
        }
    }
#endif
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
    {"read-password", prim_read_password,
     "Reads one line from stdin with terminal echo turned off, so a "
     "typed secret is not shown, and returns it without the trailing "
     "newline. The terminal mode is restored on every exit path. When "
     "stdin is not a terminal, throws :mino/kind :term/not-a-tty unless "
     "{:allow-pipe true} opts into a plain line read for a piped secret. "
     "Rides the term capability."},
};

const size_t k_prims_term_count =
    sizeof(k_prims_term) / sizeof(k_prims_term[0]);
