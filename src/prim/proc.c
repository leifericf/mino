/*
 * proc.c -- process execution primitives: sh, sh!.
 *
 * Uses popen(3) to capture stdout from external commands.  popen is
 * POSIX; Windows provides _popen with the same interface.
 *
 * Trust model.
 *
 * `sh` and `sh!` execute whatever the script author asks.  The embedder
 * is inside the trust boundary; the script author *is* the trust
 * boundary.  These primitives validate argument *shape* (each command
 * piece must be a string) and shell-quote inputs to prevent accidental
 * word-splitting, but they do not police *intent*: an embedder that
 * wants to forbid shell-out should refuse to bind these primitives in
 * the embedder's namespace.
 */

#define _POSIX_C_SOURCE 200809L

#include "prim/internal.h"
#include "mino.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <sys/wait.h>
#endif

/* ---- shell-escape and command-string building ---- */

/* Append a shell-escaped token to buf. Returns new length, or 0 on overflow.
 * POSIX: wraps in single quotes, escaping embedded single quotes.
 * Windows: only quotes arguments containing spaces or cmd.exe metacharacters. */
static size_t append_escaped(char *buf, size_t pos, size_t cap,
                             const char *s, size_t slen)
{
    size_t i;
    if (pos + slen * 2 + 4 >= cap) return 0; /* conservative overflow check */
#ifdef _WIN32
    {
        /* Check if quoting is needed (spaces or cmd.exe special chars). */
        int needs_quote = 0;
        for (i = 0; i < slen; i++) {
            if (s[i] == ' ' || s[i] == '"' || s[i] == '&' ||
                s[i] == '|' || s[i] == '<' || s[i] == '>' ||
                s[i] == '^' || s[i] == '%') {
                needs_quote = 1;
                break;
            }
        }
        if (needs_quote) buf[pos++] = '"';
        for (i = 0; i < slen; i++) {
            if (s[i] == '"') {
                if (pos + 2 >= cap) return 0;
                buf[pos++] = '\\';
                buf[pos++] = '"';
            } else {
                buf[pos++] = s[i];
            }
        }
        if (needs_quote) buf[pos++] = '"';
    }
#else
    buf[pos++] = '\'';
    for (i = 0; i < slen; i++) {
        if (s[i] == '\'') {
            /* end quote, escaped quote, restart quote: '\'' */
            if (pos + 4 >= cap) return 0;
            buf[pos++] = '\'';
            buf[pos++] = '\\';
            buf[pos++] = '\'';
            buf[pos++] = '\'';
        } else {
            buf[pos++] = s[i];
        }
    }
    buf[pos++] = '\'';
#endif
    buf[pos] = '\0';
    return pos;
}

/* Build a shell command string from a cons-list of string arguments.
 * Returns a malloc'd string or NULL on failure. */
static char *build_command(mino_state *S, mino_val *args)
{
    char *buf;
    size_t cap = 4096;
    size_t pos = 0;
    int first = 1;

    buf = (char *)malloc(cap);
    if (buf == NULL) return NULL;
    buf[0] = '\0';

    while (mino_is_cons(args)) {
        mino_val *arg = args->as.cons.car;
        size_t new_pos;
        if (arg == NULL || mino_type_of(arg) != MINO_STRING) {
            free(buf);
            prim_throw_classified(S, "eval/type", "MTY001",
                                  "sh: all arguments must be strings");
            return NULL;
        }
        if (!first) {
            if (pos + 1 >= cap) { free(buf); return NULL; }
            buf[pos++] = ' ';
        }
        new_pos = append_escaped(buf, pos, cap, arg->as.s.data, arg->as.s.len);
        if (new_pos == 0) {
            /* Grow buffer and retry. cap*2 plus len*4 must not wrap. */
            size_t arglen = arg->as.s.len;
            if (arglen > SIZE_MAX / 4 || cap > SIZE_MAX / 2
                || cap * 2 > SIZE_MAX - arglen * 4) {
                free(buf); return NULL;
            }
            cap = cap * 2 + arglen * 4;
            {
                char *newbuf = (char *)realloc(buf, cap);
                if (newbuf == NULL) { free(buf); return NULL; }
                buf = newbuf;
            }
            new_pos = append_escaped(buf, pos, cap,
                                     arg->as.s.data, arg->as.s.len);
            if (new_pos == 0) { free(buf); return NULL; }
        }
        pos = new_pos;
        first = 0;
        args = args->as.cons.cdr;
    }
    return buf;
}

/* ---- read all stdout from a popen handle ---- */

static char *read_all(FILE *fp, size_t *out_len)
{
    char *buf = NULL;
    size_t len = 0, cap = 0;
    char chunk[4096];
    size_t n;

    while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        /* len + n + 1 must not wrap; subsequent doubling must not either. */
        if (n > SIZE_MAX - len - 1) { free(buf); return NULL; }
        if (len + n + 1 > cap) {
            cap = cap == 0 ? 4096 : cap * 2;
            while (cap < len + n + 1) {
                if (cap > SIZE_MAX / 2) { free(buf); return NULL; }
                cap *= 2;
            }
            {
                char *newbuf = (char *)realloc(buf, cap);
                if (newbuf == NULL) { free(buf); return NULL; }
                buf = newbuf;
            }
        }
        memcpy(buf + len, chunk, n);
        len += n;
    }
    if (buf == NULL) {
        buf = (char *)malloc(1);
        if (buf == NULL) return NULL;
    }
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

/* ---- primitives ---- */

/* (sh cmd & args) -- run command, return {:exit n :out "..."} */
static mino_val *prim_sh(mino_state *S, mino_val *args, mino_env *env)
{
    char *cmd;
    FILE *fp;
    char *out;
    size_t out_len;
    int status;
    mino_val *keys[2], *vals[2];
    mino_val *result;
    (void)env;

    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "sh requires at least one argument");
    }

    cmd = build_command(S, args);
    if (cmd == NULL) return NULL; /* error already thrown */

    /* Redirect stderr to stdout so we capture both.
     * On POSIX this uses the shell (sh -c) redirect syntax.
     * On Windows, _popen passes the command to cmd.exe, which also
     * supports "2>&1" — so the same string is valid on both platforms. */
    {
        size_t clen = strlen(cmd);
        char *cmd2 = (char *)realloc(cmd, clen + 16);
        if (cmd2 == NULL) { free(cmd); return NULL; }
        cmd = cmd2;
        memcpy(cmd + clen, " 2>&1", 6);
    }

#ifdef _WIN32
    fp = _popen(cmd, "r");
#else
    fp = popen(cmd, "r");
#endif
    free(cmd);
    if (fp == NULL) {
        return prim_throw_classified(S, "io", "MIO001",
                                     "sh: failed to execute command");
    }

    out = read_all(fp, &out_len);
#ifdef _WIN32
    status = _pclose(fp);
#else
    status = pclose(fp);
#endif
    /* -1 means pclose / _pclose itself failed (e.g. wait was interrupted
     * or the child was reaped elsewhere). The value is not a wait
     * status, so WIFEXITED on it is undefined and would silently route
     * nonsense back to the caller's :exit. Surface the teardown error
     * instead. */
    if (status == -1) {
        free(out);
        return prim_throw_classified(S, "io", "MIO001",
                                     "sh: pclose failed");
    }
#ifndef _WIN32
    /* POSIX: extract exit code from wait status. */
    if (WIFEXITED(status)) status = WEXITSTATUS(status);
#endif

    if (out == NULL) {
        return prim_throw_classified(S, "io", "MIO001",
                                     "sh: out of memory reading output");
    }

    {
        mino_val *exit_kw = mino_keyword(S, "exit"); gc_pin(exit_kw);
        mino_val *out_kw  = mino_keyword(S, "out");  gc_pin(out_kw);
        mino_val *exit_v  = mino_int(S, status);     gc_pin(exit_v);
        mino_val *out_v   = mino_string_n(S, out, out_len);
        free(out);
        keys[0] = exit_kw; keys[1] = out_kw;
        vals[0] = exit_v;  vals[1] = out_v;
        gc_unpin(3);
    }

    result = mino_map(S, keys, vals, 2);
    return result;
}

/* (sh! cmd & args) -- run command, return stdout; throw on non-zero exit. */
static mino_val *prim_sh_bang(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *result = prim_sh(S, args, env);
    mino_val *exit_val;
    mino_val *out_val;
    mino_val *exit_key;

    if (result == NULL) return NULL;

    exit_key = mino_keyword(S, "exit");
    exit_val = map_get_val(result, exit_key);

    if (exit_val != NULL && mino_val_int_p(exit_val)
        && mino_val_int_get(exit_val) != 0) {
        out_val = map_get_val(result, mino_keyword(S, "out"));
        {
            char msg[512];
            snprintf(msg, sizeof(msg), "sh!: command exited with code %lld: %s",
                     mino_val_int_get(exit_val),
                     (out_val && mino_type_of(out_val) == MINO_STRING)
                         ? out_val->as.s.data : "");
            return prim_throw_classified(S, "io", "MIO001", msg);
        }
    }

    return map_get_val(result, mino_keyword(S, "out"));
}

/* (thread-sleep ms) -- block the current thread for ms milliseconds.
 * Returns nil. Negative or non-integer ms throws. */
mino_val *prim_thread_sleep(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *ms_val;
    long long   ms;
#ifndef _WIN32
    struct timespec ts;
#endif
    (void)env;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "thread-sleep requires exactly 1 argument");
    }
    ms_val = args->as.cons.car;
    if (!mino_to_int(ms_val, &ms)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "thread-sleep: argument must be an integer");
    }
    if (ms < 0) {
        return prim_throw_classified(S, "eval/contract", "MCT001",
            "thread-sleep: argument must be non-negative");
    }
    /* Yield state_lock for the duration of the sleep so worker
     * threads (futures, agents) can make progress. Without this,
     * a `(thread-sleep 200)` between a `(future ...)` spawn and a
     * subsequent realized? / state observation would starve every
     * worker -- they all need state_lock to call into mino_call,
     * but a sleeping main thread holds it. The polling pattern
     *     (loop [...] (when-not (realized? p) (thread-sleep 50) (recur)))
     * would otherwise spin forever. mino_future_deref already does
     * the same yield; this brings thread-sleep into alignment. */
    {
        int depth = mino_yield_lock(S);
#ifdef _WIN32
        /* Windows has no nanosleep; Sleep() takes milliseconds. */
        Sleep((DWORD)(ms < 0 ? 0 : ms));
#else
        ts.tv_sec  = (time_t)(ms / 1000);
        ts.tv_nsec = (long)((ms % 1000) * 1000000L);
        while (nanosleep(&ts, &ts) == -1) {
            /* Restart on EINTR using the residual time written into ts. */
        }
#endif
        mino_resume_lock(S, depth);
    }
    return mino_nil(S);
}

/* ---- install ---- */

const mino_prim_def k_prims_proc[] = {
    {"sh",  prim_sh,
     "Runs an external command. Returns {:exit n :out \"...\"}."},
    {"sh!", prim_sh_bang,
     "Runs an external command. Returns stdout; throws on non-zero exit."},
    {"thread-sleep", prim_thread_sleep,
     "Blocks the current thread for the given number of milliseconds. Returns nil."},
};

const size_t k_prims_proc_count =
    sizeof(k_prims_proc) / sizeof(k_prims_proc[0]);

void mino_install_proc(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_proc, k_prims_proc_count,
                                       "proc");
    S->caps_installed |= MINO_CAP_PROC;
}
