/*
 * main.c — standalone entry point and interactive REPL.
 *
 * Reads forms from stdin one at a time, evaluates each in a persistent
 * environment, and prints the result. Multi-line forms are supported by
 * accumulating input until the reader produces a complete form. The prompt
 * is written to stderr so piped output on stdout stays clean.
 */

#define _POSIX_C_SOURCE 200809L

#include "runtime/internal.h"
#include "path_buf.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef _WIN32
#include <execinfo.h>
#endif

#define MINO_LINE_MAX PATH_BUF_CAP

/* ---- CWD-relative module resolver ---- */

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return 0;
    fclose(f);
    return 1;
}

/* Directory where the mino binary was invoked from. Used as fallback
 * for resolving lib/ modules even after chdir. */
static char initial_dir[PATH_BUF_CAP] = "";

/* Directory containing the mino binary itself. Used to find bundled
 * lib/ modules when running from a different working directory. */
static char binary_dir[PATH_BUF_CAP] = "";

static int try_resolve(const char *name, size_t nlen, char *buf, size_t bufsz)
{
    static const char *exts[] = {".mino", ".cljc", ".clj", ".cljs"};
    size_t i;
    for (i = 0; i < 4; i++) {
        snprintf(buf, bufsz, "%s%s", name, exts[i]);
        if (file_exists(buf)) return 1;
    }
    /* Try lib/ prefix. */
    for (i = 0; i < 4; i++) {
        snprintf(buf, bufsz, "lib/%s%s", name, exts[i]);
        if (file_exists(buf)) return 1;
    }
    /* Try initial dir's lib/ as fallback. */
    if (initial_dir[0] != '\0') {
        for (i = 0; i < 4; i++) {
            snprintf(buf, bufsz, "%s/lib/%s%s", initial_dir, name, exts[i]);
            if (file_exists(buf)) return 1;
        }
    }
    /* Try binary dir's lib/ as fallback. */
    if (binary_dir[0] != '\0') {
        for (i = 0; i < 4; i++) {
            snprintf(buf, bufsz, "%s/lib/%s%s", binary_dir, name, exts[i]);
            if (file_exists(buf)) return 1;
        }
    }
    (void)nlen;
    return 0;
}

static const char *cwd_resolve(const char *name, void *ctx)
{
    static char path_buf[PATH_BUF_CAP];
    size_t nlen;
    (void)ctx;
    if (name == NULL) return NULL;
    nlen = strlen(name);
    if (nlen + 10 >= sizeof(path_buf)) return NULL;

    /* If name already has an extension, use as-is. */
    if ((nlen >= 5 && (strcmp(name + nlen - 5, ".mino") == 0
                    || strcmp(name + nlen - 5, ".cljc") == 0
                    || strcmp(name + nlen - 5, ".cljs") == 0))
     || (nlen >= 4 && strcmp(name + nlen - 4, ".clj") == 0)) {
        memcpy(path_buf, name, nlen + 1);
        if (file_exists(path_buf)) return path_buf;
        snprintf(path_buf, sizeof(path_buf), "lib/%s", name);
        if (file_exists(path_buf)) return path_buf;
        if (initial_dir[0] != '\0') {
            snprintf(path_buf, sizeof(path_buf), "%s/lib/%s", initial_dir, name);
            if (file_exists(path_buf)) return path_buf;
        }
        if (binary_dir[0] != '\0') {
            snprintf(path_buf, sizeof(path_buf), "%s/lib/%s", binary_dir, name);
            if (file_exists(path_buf)) return path_buf;
        }
        return NULL;
    }

    if (try_resolve(name, nlen, path_buf, sizeof(path_buf)))
        return path_buf;

    return NULL;
}

/* ---- project-aware module resolver ---- */

#define MAX_PROJECT_PATHS 64

static char *project_paths[MAX_PROJECT_PATHS];
static size_t project_path_count = 0;

/* Try resolving a module name within a specific directory prefix. */
static int try_resolve_in(const char *dir, const char *name, size_t nlen,
                          char *buf, size_t bufsz)
{
    static const char *exts[] = {".mino", ".cljc", ".clj", ".cljs"};
    size_t i;
    (void)nlen;
    for (i = 0; i < 4; i++) {
        snprintf(buf, bufsz, "%s/%s%s", dir, name, exts[i]);
        if (file_exists(buf)) return 1;
    }
    return 0;
}

/* Resolver that checks project paths first, then falls through to cwd. */
static const char *project_resolve(const char *name, void *ctx)
{
    static char pbuf[PATH_BUF_CAP];
    size_t i, nlen;
    (void)ctx;
    if (name == NULL) return NULL;
    nlen = strlen(name);
    if (nlen + 10 >= sizeof(pbuf)) return NULL;

    for (i = 0; i < project_path_count; i++) {
        if (try_resolve_in(project_paths[i], name, nlen,
                           pbuf, sizeof(pbuf)))
            return pbuf;
    }
    return cwd_resolve(name, NULL);
}

/* Read mino.edn and configure project paths + deps.
 * Called before any script execution if mino.edn exists. */
static void setup_project(mino_state_t *S, mino_env_t *env)
{
    mino_val_t *result;
    mino_ref_t *ref;

    if (!file_exists("mino.edn")) return;

    /* Load the deps module and resolve paths. */
    result = mino_eval_string(S,
        "(require '[mino.deps :as deps])"
        "(deps/resolve-paths (deps/load-manifest \"mino.edn\"))",
        env);

    if (result == NULL || result->type != MINO_VECTOR) return;

    /* Root the result so GC cannot collect it while we extract paths. */
    ref = mino_ref(S, result);
    {
        size_t i;
        size_t count = result->as.vec.len;
        if (count > MAX_PROJECT_PATHS) count = MAX_PROJECT_PATHS;
        for (i = 0; i < count; i++) {
            mino_val_t *p = vec_nth(result, i);
            if (p != NULL && p->type == MINO_STRING) {
                project_paths[project_path_count] = strdup(p->as.s.data);
                if (project_paths[project_path_count] != NULL)
                    project_path_count++;
            }
        }
    }
    mino_unref(S, ref);

    if (project_path_count > 0)
        mino_set_resolver(S, project_resolve, NULL);
}

/* Report an evaluation error to stderr. */
static void report_eval_error(mino_state_t *S)
{
    const mino_diag_t *d = mino_last_diag(S);
    if (d != NULL) {
        char dbuf[2048];
        mino_render_diag(S, d, MINO_DIAG_RENDER_PRETTY,
                         dbuf, sizeof(dbuf));
        fprintf(stderr, "%s", dbuf);
    } else {
        const char *err = mino_last_error(S);
        fprintf(stderr, "mino: %s\n", err ? err : "unknown error");
    }
}

/* Return 1 if name contains only [a-zA-Z0-9_-] and fits in eval_buf. */
static int is_valid_task_name(const char *name)
{
    const char *p;
    if (name[0] == '\0') return 0;
    if (strlen(name) > 400) return 0;
    for (p = name; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return 0;
    }
    return 1;
}

/* Run `mino task <name>` subcommand. */
static int run_task(mino_state_t *S, mino_env_t *env, const char *task_name)
{
    char eval_buf[512];

    if (!file_exists("mino.edn")) {
        fprintf(stderr, "mino: no mino.edn found in current directory\n");
        return 1;
    }
    if (!is_valid_task_name(task_name)) {
        fprintf(stderr, "mino: invalid task name: %s\n", task_name);
        return 2;
    }

    snprintf(eval_buf, sizeof(eval_buf),
        "(require '[mino.tasks :as tasks])"
        "(tasks/run-task! '%s (tasks/load-tasks \"mino.edn\"))",
        task_name);

    if (mino_eval_string(S, eval_buf, env) == NULL) {
        report_eval_error(S);
        return 1;
    }
    return 0;
}

/* Run `mino task` with no args: list available tasks. */
static int run_task_list(mino_state_t *S, mino_env_t *env)
{
    if (!file_exists("mino.edn")) {
        fprintf(stderr, "mino: no mino.edn found in current directory\n");
        return 1;
    }

    if (mino_eval_string(S,
            "(require '[mino.tasks :as tasks])"
            "(tasks/list-tasks (tasks/load-tasks \"mino.edn\"))",
            env) == NULL) {
        report_eval_error(S);
        return 1;
    }
    return 0;
}

/* Run `mino deps` subcommand: fetch all dependencies. */
static int run_deps(mino_state_t *S, mino_env_t *env)
{
    mino_val_t *result;

    if (!file_exists("mino.edn")) {
        fprintf(stderr, "mino: no mino.edn found in current directory\n");
        return 1;
    }

    result = mino_eval_string(S,
        "(require '[mino.deps :as deps])"
        "(deps/fetch-all! (deps/load-manifest \"mino.edn\"))",
        env);

    if (result == NULL) {
        report_eval_error(S);
        return 1;
    }
    return 0;
}

/* ---- standalone-mode CLI helpers ---- */

static void print_version(FILE *out)
{
    fprintf(out, "mino %s\n", mino_version_string());
}

static void print_usage(FILE *out)
{
    fputs(
        "mino - tiny embeddable Lisp\n"
        "\n"
        "USAGE:\n"
        "    mino [OPTIONS] [FILE]\n"
        "    mino [OPTIONS] -e EXPR\n"
        "    mino <SUBCOMMAND> [ARGS...]\n"
        "\n"
        "OPTIONS:\n"
        "    -e, --eval EXPR     Evaluate EXPR and print the result\n"
        "    -h, --help          Show this help and exit\n"
        "    -V, --version       Show version and exit\n"
        "    --                  End of options; treat the rest as FILE\n"
        "\n"
        "SUBCOMMANDS:\n"
        "    task [NAME]         Run a task from mino.edn, or list tasks\n"
        "    deps                Resolve and fetch mino.edn dependencies\n"
        "\n"
        "If neither FILE, -e, nor a subcommand is given, the REPL is started.\n",
        out);
}

/* Run `mino -e EXPR`: evaluate one expression and print its result. */
static int run_eval_expr(mino_state_t *S, mino_env_t *env, const char *expr)
{
    mino_val_t *result = mino_eval_string(S, expr, env);
    if (result == NULL) {
        report_eval_error(S);
        return 1;
    }
    mino_println(S, result);
    return 0;
}

/* Parse leading options. On success, sets *out_first to the index of the
 * first non-option argv slot (== argc if none) and may set *out_eval to a
 * pointer into argv for -e/--eval EXPR, *out_dash_dash if `--` was seen.
 *
 * Returns:
 *    0   normal exit from parsing; caller continues with dispatch
 *    1   handled completely (--help / --version printed); caller should exit 0
 *    2   usage error; caller should exit 2
 */
static int parse_cli_flags(int argc, char **argv,
                           int *out_first,
                           const char **out_eval,
                           int *out_dash_dash)
{
    int i = 1;
    *out_eval = NULL;
    *out_dash_dash = 0;
    while (i < argc) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            *out_dash_dash = 1;
            i++;
            break;
        }
        if (a[0] != '-' || a[1] == '\0') break; /* positional or bare "-" */
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            print_usage(stdout);
            return 1;
        }
        if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
            print_version(stdout);
            return 1;
        }
        if (strcmp(a, "-e") == 0 || strcmp(a, "--eval") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr,
                        "mino: %s requires an EXPR argument\n"
                        "Try 'mino --help' for usage.\n",
                        a);
                return 2;
            }
            *out_eval = argv[i + 1];
            i += 2;
            continue;
        }
        fprintf(stderr,
                "mino: unknown option: %s\n"
                "Try 'mino --help' for usage.\n",
                a);
        return 2;
    }
    *out_first = i;
    return 0;
}

/* ---- helpers ---- */

static int is_unterminated_error(const char *msg)
{
    return msg != NULL && strstr(msg, "unterminated") != NULL;
}

static int has_only_whitespace(const char *s)
{
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != ',') {
            return 0;
        }
    }
    return 1;
}

/* ---- Crash handler ----------------------------------------------------
 *
 * Installs handlers for SIGSEGV / SIGABRT (and SIGBUS where it exists)
 * so a fatal signal prints a short diagnostic line and a best-effort
 * backtrace before the OS terminates the process. Three rules govern
 * what the handler does:
 *
 *   1. No allocation. A crash from inside the allocator or collector
 *      would livelock. The handler uses stack buffers and write(2),
 *      never malloc or free.
 *
 *   2. Best-effort GC stats. mino_gc_stats only copies scalar counters
 *      out of mino_state_t; if the state is torn mid-mutation those
 *      numbers may be a little off, but reading them will not segfault
 *      because every field the accessor touches is a plain integer.
 *
 *   3. Re-raise after the report. The handler restores the default
 *      disposition (SIG_DFL) and re-raises so the OS still produces the
 *      core file / non-zero exit code the caller expects. We never
 *      return from the handler into the interrupted instruction.
 *
 * Set MINO_NO_CRASH_HANDLER=1 to skip installation (useful when running
 * under an external debugger or signal-aware test runner that would
 * rather handle the crash itself).
 */

static mino_state_t *crash_handler_state = NULL;

static const char *signal_name(int sig)
{
    if (sig == SIGSEGV) return "SIGSEGV";
    if (sig == SIGABRT) return "SIGABRT";
#ifdef SIGBUS
    if (sig == SIGBUS)  return "SIGBUS";
#endif
    return "signal";
}

static void crash_handler_write_line(const char *s)
{
#ifdef _WIN32
    fputs(s, stderr);
    fflush(stderr);
#else
    size_t len = strlen(s);
    ssize_t written = write(STDERR_FILENO, s, len);
    (void)written;
#endif
}

static void crash_handler_report(int sig)
{
    char buf[512];
    int  n;
    n = snprintf(buf, sizeof(buf),
                 "\n[mino] fatal %s (signal %d)\n",
                 signal_name(sig), sig);
    if (n > 0) crash_handler_write_line(buf);

    if (crash_handler_state != NULL) {
        mino_gc_stats_t st;
        mino_gc_stats(crash_handler_state, &st);
        n = snprintf(buf, sizeof(buf),
                     "[mino] gc: minor=%zu major=%zu live=%zu alloc=%zu "
                     "freed=%zu phase=%d remset=%zu/%zu\n",
                     st.collections_minor, st.collections_major,
                     st.bytes_live, st.bytes_alloc, st.bytes_freed,
                     st.phase,
                     st.remset_entries, st.remset_cap);
        if (n > 0) crash_handler_write_line(buf);
    }

#ifndef _WIN32
    {
        void *frames[64];
        int   nframes = backtrace(frames, (int)(sizeof(frames) / sizeof(frames[0])));
        if (nframes > 0) {
            const char header[] = "[mino] backtrace (best effort):\n";
            crash_handler_write_line(header);
            backtrace_symbols_fd(frames, nframes, STDERR_FILENO);
        }
    }
#else
    crash_handler_write_line("[mino] backtrace: not implemented on Windows\n");
#endif
}

static void crash_handler(int sig)
{
    crash_handler_report(sig);
    /* Restore the default disposition and re-raise so the OS delivers
     * the intended fate (core dump for SEGV, termination for ABRT). */
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_crash_handler(mino_state_t *S)
{
    const char *disabled = getenv("MINO_NO_CRASH_HANDLER");
    if (disabled != NULL && disabled[0] != '\0' && disabled[0] != '0') {
        return;
    }
    crash_handler_state = S;
#ifndef _WIN32
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = crash_handler;
        sigemptyset(&sa.sa_mask);
        /* SA_RESETHAND would also restore the default after the first
         * hit; we do the same thing manually inside the handler so the
         * policy is visible next to the re-raise. */
        sa.sa_flags = 0;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGABRT, &sa, NULL);
#ifdef SIGBUS
        sigaction(SIGBUS,  &sa, NULL);
#endif
    }
#else
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
#endif
}

int main(int argc, char **argv)
{
    mino_state_t *S;
    int           first       = 1;
    const char   *eval_expr   = NULL;
    int           dash_dash   = 0;
    int           parse_state;

    parse_state = parse_cli_flags(argc, argv, &first, &eval_expr, &dash_dash);
    if (parse_state == 1) return 0;
    if (parse_state == 2) return 2;

    getcwd(initial_dir, sizeof(initial_dir));

    /* Compute binary_dir from argv[0] for finding bundled lib/. */
    {
        char resolved[PATH_BUF_CAP];
        const char *slash;
#ifdef _WIN32
        if (_fullpath(resolved, argv[0], sizeof(resolved)) != NULL) {
#else
        if (realpath(argv[0], resolved) != NULL) {
#endif
            slash = strrchr(resolved, '/');
#ifdef _WIN32
            if (slash == NULL) slash = strrchr(resolved, '\\');
#endif
            if (slash != NULL) {
                size_t dlen = (size_t)(slash - resolved);
                if (dlen < sizeof(binary_dir)) {
                    memcpy(binary_dir, resolved, dlen);
                    binary_dir[dlen] = '\0';
                }
            }
        }
    }

    S = mino_state_new();
    install_crash_handler(S);
    mino_env_t   *env = mino_env_new(S);
    char *buf  = NULL;
    size_t cap = 0;
    size_t len = 0;
    int    awaiting_continuation = 0;
    int    exit_code = 0;

    /* Development knobs. These override the collector defaults before
     * any allocation happens. Range errors silently fall back to the
     * default because a CLI user fat-fingering an env var should not
     * block the interpreter from starting. */
    {
        struct { const char *name; mino_gc_param_t p; } knobs[] = {
            { "MINO_NURSERY",          MINO_GC_NURSERY_BYTES       },
            { "MINO_GC_MAJOR_GROWTH",  MINO_GC_MAJOR_GROWTH_TENTHS },
            { "MINO_GC_PROMOTION_AGE", MINO_GC_PROMOTION_AGE       },
            { "MINO_GC_BUDGET",        MINO_GC_INCREMENTAL_BUDGET  },
            { "MINO_GC_QUANTUM",       MINO_GC_STEP_ALLOC_BYTES    }
        };
        size_t ki;
        for (ki = 0; ki < sizeof(knobs) / sizeof(knobs[0]); ki++) {
            const char *raw = getenv(knobs[ki].name);
            if (raw != NULL && raw[0] != '\0') {
                long long v = atoll(raw);
                if (v > 0) {
                    mino_gc_set_param(S, knobs[ki].p, (size_t)v);
                }
            }
        }
    }

    mino_install_core(S, env);
    mino_install_io(S, env);
    mino_install_fs(S, env);
    mino_install_proc(S, env);
    mino_set_resolver(S, cwd_resolve, NULL);

    /* Subcommand: mino deps */
    if (!dash_dash && first < argc && strcmp(argv[first], "deps") == 0) {
        exit_code = run_deps(S, env);
        mino_env_free(S, env);
        mino_state_free(S);
        return exit_code;
    }

    /* Auto-wire project paths from mino.edn if present. */
    setup_project(S, env);

    /* -e / --eval EXPR mode: evaluate one expression and exit. */
    if (eval_expr != NULL) {
        exit_code = run_eval_expr(S, env, eval_expr);
        mino_env_free(S, env);
        mino_state_free(S);
        return exit_code;
    }

    /* Subcommand: mino task [<name>] */
    if (!dash_dash && first < argc && strcmp(argv[first], "task") == 0) {
        if (first + 1 < argc)
            exit_code = run_task(S, env, argv[first + 1]);
        else
            exit_code = run_task_list(S, env);
        mino_env_free(S, env);
        mino_state_free(S);
        return exit_code;
    }

    /* File mode: evaluate a script and exit. */
    if (first < argc) {
        mino_val_t *result = mino_load_file(S, argv[first], env);
        if (result == NULL) {
            const mino_diag_t *d = mino_last_diag(S);
            if (d != NULL) {
                char dbuf[2048];
                mino_render_diag(S, d, MINO_DIAG_RENDER_PRETTY,
                                 dbuf, sizeof(dbuf));
                fprintf(stderr, "%s", dbuf);
            } else {
                const char *err = mino_last_error(S);
                fprintf(stderr, "mino: %s\n", err ? err : "unknown error");
            }
            exit_code = 1;
        }
        mino_env_free(S, env);
        mino_state_free(S);
        return exit_code;
    }

    fprintf(stderr, "mino %s\n", mino_version_string());
    fputs("mino> ", stderr);
    fflush(stderr);

    for (;;) {
        char line[MINO_LINE_MAX];
        if (fgets(line, sizeof(line), stdin) == NULL) {
            break;
        }
        {
            size_t add = strlen(line);
            if (len + add + 1 > cap) {
                size_t new_cap = cap == 0 ? 256 : cap;
                while (new_cap < len + add + 1) {
                    new_cap *= 2;
                }
                buf = (char *)realloc(buf, new_cap);
                if (buf == NULL) {
                    fputs("mino: out of memory\n", stderr);
                    exit_code = 1;
                    goto cleanup;
                }
                cap = new_cap;
            }
            memcpy(buf + len, line, add + 1);
            len += add;
        }

        /* Drain as many complete forms as we have. */
        for (;;) {
            const char *cursor = buf;
            const char *end    = buf;
            mino_val_t *form;
            mino_val_t *result;

            if (has_only_whitespace(buf)) {
                /* Don't discard trailing whitespace: newlines in it feed
                 * the reader's line counter on the next call. */
                awaiting_continuation = 0;
                break;
            }

            /* Cache REPL input for diagnostic source snippets. */
            source_cache_store(S, S->reader_file, buf, len);

            form = mino_read(S, cursor, &end);
            if (form == NULL) {
                const char *err = mino_last_error(S);
                if (is_unterminated_error(err)) {
                    awaiting_continuation = 1;
                    break;
                }
                {
                    const mino_diag_t *d = mino_last_diag(S);
                    if (d != NULL) {
                        char dbuf[2048];
                        mino_render_diag(S, d, MINO_DIAG_RENDER_PRETTY,
                                         dbuf, sizeof(dbuf));
                        fprintf(stderr, "%s", dbuf);
                    } else {
                        fprintf(stderr, "read error: %s\n",
                                err ? err : "unknown");
                    }
                }
                len = 0;
                buf[0] = '\0';
                awaiting_continuation = 0;
                break;
            }

            result = mino_eval(S, form, env);
            if (result == NULL) {
                const mino_diag_t *d = mino_last_diag(S);
                if (d != NULL) {
                    char dbuf[2048];
                    mino_render_diag(S, d, MINO_DIAG_RENDER_PRETTY,
                                     dbuf, sizeof(dbuf));
                    fprintf(stderr, "%s", dbuf);
                } else {
                    const char *err = mino_last_error(S);
                    fprintf(stderr, "eval error: %s\n",
                            err ? err : "unknown");
                }
            } else {
                mino_println(S, result);
            }

            /* Shift unread bytes to the front of the buffer. */
            {
                size_t consumed = (size_t)(end - buf);
                size_t remaining = len - consumed;
                memmove(buf, end, remaining + 1);
                len = remaining;
            }
            awaiting_continuation = 0;
        }

        fputs(awaiting_continuation ? "...   " : "mino> ", stderr);
        fflush(stderr);
    }

    fputc('\n', stderr);

cleanup:
    free(buf);
    mino_env_free(S, env);
    mino_state_free(S);
    return exit_code;
}
