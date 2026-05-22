/*
 * main.c — standalone entry point and interactive REPL.
 *
 * Reads forms from stdin one at a time, evaluates each in a persistent
 * environment, and prints the result. Multi-line forms are supported by
 * accumulating input until the reader produces a complete form. The prompt
 * is written to stderr so piped output on stdout stays clean.
 */

#define _POSIX_C_SOURCE 200809L
/* Darwin's <sys/sysctl.h> needs the BSD names (u_int, etc.) which
 * strict _POSIX_C_SOURCE hides; _DARWIN_C_SOURCE re-exposes them. */
#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE
#endif

#include "runtime/internal.h"
#include "path_buf.h"
#include "crash_backtrace.h"  /* portable _Unwind_Backtrace, replaces <execinfo.h> */
#include "eval/bc/jit.h"  /* MINO_CPJIT_HOST_DETECTED for --help/--version */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef _WIN32
#include <strings.h>  /* POSIX strcasecmp on Linux + macOS */
#define mino_strcasecmp strcasecmp
#else
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  define mino_strcasecmp _stricmp
#endif

#if defined(__APPLE__)
#  include <sys/sysctl.h>
#  include <sys/types.h>
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
    static const char *exts[] = {".cljc", ".clj", ".cljs"};
    size_t i;
    for (i = 0; i < 3; i++) {
        snprintf(buf, bufsz, "%s%s", name, exts[i]);
        if (file_exists(buf)) return 1;
    }
    /* Try lib/ prefix. */
    for (i = 0; i < 3; i++) {
        snprintf(buf, bufsz, "lib/%s%s", name, exts[i]);
        if (file_exists(buf)) return 1;
    }
    /* Try initial dir's lib/ as fallback. */
    if (initial_dir[0] != '\0') {
        size_t dlen = strlen(initial_dir);
        if (dlen + nlen + 11 < bufsz) {
            for (i = 0; i < 3; i++) {
                snprintf(buf, bufsz, "%s/lib/%s%s", initial_dir, name, exts[i]);
                if (file_exists(buf)) return 1;
            }
        }
    }
    /* Try binary dir's lib/ as fallback. */
    if (binary_dir[0] != '\0') {
        size_t dlen = strlen(binary_dir);
        if (dlen + nlen + 11 < bufsz) {
            for (i = 0; i < 3; i++) {
                snprintf(buf, bufsz, "%s/lib/%s%s", binary_dir, name, exts[i]);
                if (file_exists(buf)) return 1;
            }
        }
    }
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
    if ((nlen >= 5 && (strcmp(name + nlen - 5, ".cljc") == 0
                    || strcmp(name + nlen - 5, ".cljs") == 0))
     || (nlen >= 4 && strcmp(name + nlen - 4, ".clj") == 0)) {
        memcpy(path_buf, name, nlen + 1);
        if (file_exists(path_buf)) return path_buf;
        snprintf(path_buf, sizeof(path_buf), "lib/%s", name);
        if (file_exists(path_buf)) return path_buf;
        if (initial_dir[0] != '\0') {
            int w = snprintf(path_buf, sizeof(path_buf), "%s/lib/%s",
                             initial_dir, name);
            if (w > 0 && (size_t)w < sizeof(path_buf)
                && file_exists(path_buf)) return path_buf;
        }
        if (binary_dir[0] != '\0') {
            int w = snprintf(path_buf, sizeof(path_buf), "%s/lib/%s",
                             binary_dir, name);
            if (w > 0 && (size_t)w < sizeof(path_buf)
                && file_exists(path_buf)) return path_buf;
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
    static const char *exts[] = {".cljc", ".clj", ".cljs"};
    size_t i;
    (void)nlen;
    for (i = 0; i < 3; i++) {
        snprintf(buf, bufsz, "%s/%s%s", dir, name, exts[i]);
        if (file_exists(buf)) return 1;
    }
    return 0;
}

/* Resolver that checks project paths first, then runtime-registered
 * extra paths (from `(add-load-path! ...)`), then falls through to
 * cwd. The state pointer is passed in via ctx so we can read
 * S->module.extra_load_paths without a global. */
static const char *project_resolve(const char *name, void *ctx)
{
    static char pbuf[PATH_BUF_CAP];
    size_t i, nlen;
    mino_state *S = (mino_state *)ctx;
    if (name == NULL) return NULL;
    nlen = strlen(name);
    if (nlen + 10 >= sizeof(pbuf)) return NULL;

    for (i = 0; i < project_path_count; i++) {
        if (try_resolve_in(project_paths[i], name, nlen,
                           pbuf, sizeof(pbuf)))
            return pbuf;
    }
    if (S != NULL) {
        for (i = 0; i < S->module.extra_load_paths_len; i++) {
            if (try_resolve_in(S->module.extra_load_paths[i], name, nlen,
                               pbuf, sizeof(pbuf)))
                return pbuf;
        }
    }
    return cwd_resolve(name, NULL);
}

/* Resolver for the no-mino.edn path: skip project_paths but still
 * consult runtime-registered extra paths (so add-load-path! works
 * regardless of whether the binary loaded a project manifest). */
static const char *runtime_paths_resolve(const char *name, void *ctx)
{
    static char pbuf[PATH_BUF_CAP];
    size_t i, nlen;
    mino_state *S = (mino_state *)ctx;
    const char *cwd_result;
    if (name == NULL) return NULL;
    nlen = strlen(name);
    if (nlen + 10 >= sizeof(pbuf)) return NULL;
    if (S != NULL) {
        for (i = 0; i < S->module.extra_load_paths_len; i++) {
            if (try_resolve_in(S->module.extra_load_paths[i], name, nlen,
                               pbuf, sizeof(pbuf)))
                return pbuf;
        }
    }
    cwd_result = cwd_resolve(name, NULL);
    return cwd_result;
}

/* Read mino.edn and configure project paths + deps.
 * Called before any script execution if mino.edn exists. */
static void setup_project(mino_state *S, mino_env *env)
{
    mino_val *result;
    mino_ref *ref;

    if (!file_exists("mino.edn")) return;

    /* Load the deps module and resolve paths. */
    result = mino_eval_string(S,
        "(require '[mino.deps :as deps])"
        "(deps/resolve-paths (deps/load-manifest \"mino.edn\"))",
        env);

    if (result == NULL || result->type != MINO_VECTOR) return;

    /* Root the result so GC cannot collect it while we extract paths. */
    ref = mino_ref_new(S, result);
    {
        size_t i;
        size_t count = result->as.vec.len;
        if (count > MAX_PROJECT_PATHS) count = MAX_PROJECT_PATHS;
        for (i = 0; i < count; i++) {
            mino_val *p = vec_nth(result, i);
            if (p != NULL && p->type == MINO_STRING) {
                project_paths[project_path_count] = strdup(p->as.s.data);
                if (project_paths[project_path_count] != NULL)
                    project_path_count++;
            }
        }
    }
    mino_unref(S, ref);

    if (project_path_count > 0)
        mino_set_resolver(S, project_resolve, S);
}

/* Report an evaluation error to stderr. */
static void report_eval_error(mino_state *S)
{
    const mino_diag *d = mino_last_diag(S);
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
static int run_task(mino_state *S, mino_env *env, const char *task_name)
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
static int run_task_list(mino_state *S, mino_env *env)
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
static int run_deps(mino_state *S, mino_env *env)
{
    mino_val *result;

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
#if MINO_CPJIT_HOST_DETECTED
    fprintf(out, "mino %s\n", mino_version_string());
#else
    /* mino-lean (or any host where the JIT module was compiled out)
     * advertises a distinct build tag so install audits and bug
     * reports can tell which binary the user is running. */
    fprintf(out, "mino-lean %s (no-jit)\n", mino_version_string());
#endif
}

static void print_usage(FILE *out)
{
    fputs(
        "mino - tiny embeddable Lisp\n"
        "\n"
        "USAGE:\n"
        "    mino [OPTIONS] [FILE]\n"
        "    mino [OPTIONS] EXPR             # EXPR starts with ( [ { # @ '\n"
        "    mino [OPTIONS] -e EXPR\n"
        "    mino <SUBCOMMAND> [ARGS...]\n"
        "\n"
        "OPTIONS:\n"
        "    -e, --eval EXPR     Evaluate EXPR and print the result\n"
        "    -h, --help          Show this help and exit\n"
        "    -V, --version       Show version and exit\n"
#if MINO_CPJIT_HOST_DETECTED
        "    --jit=auto|off|on   JIT mode (default auto; overrides MINO_JIT env)\n"
        "    --jit-threshold=N   Hot-call count before AUTO compiles (default 100)\n"
#else
        "    --jit=auto|off|on   Accepted for parity; this build has the JIT compiled out\n"
        "    --jit-threshold=N   Accepted for parity; this build has the JIT compiled out\n"
#endif
        "    --                  End of options; treat the rest as FILE\n"
        "\n"
        "SUBCOMMANDS:\n"
        "    repl                Start the interactive REPL (default with no FILE)\n"
        "    task [NAME]         Run a task from mino.edn, or list tasks\n"
        "    deps                Resolve and fetch mino.edn dependencies\n"
        "    nrepl [ARGS...]     Discover and exec 'mino-nrepl' from PATH\n"
        "    lsp   [ARGS...]     Discover and exec 'mino-lsp' from PATH\n"
        "\n"
        "If neither FILE, -e, nor a subcommand is given, the REPL is started.\n",
        out);
}

/* Short on-screen help shown when the user types `:help` at the REPL. */
static void print_repl_help(FILE *out)
{
    fputs(
        "REPL meta-commands:\n"
        "  :help          Show this help\n"
        "  :quit          Exit the REPL (or press Ctrl-D)\n"
        "  :capabilities  Show installed vs. available capabilities (alias :caps)\n"
        "\n"
        "Type a form and press Enter to evaluate it. Multi-line forms are\n"
        "gathered until the parens balance.\n",
        out);
}

/* Two-column capability listing for the REPL `:capabilities` command.
 * The full registry is enumerated; each entry is printed under
 * INSTALLED or AVAILABLE depending on the runtime's bitmask. */
static void print_repl_capabilities(FILE *out, mino_state *S)
{
    const mino_capability_info *p;
    int total = 0, installed = 0, longest = 0;

    for (p = mino_capability_list(); p->name != NULL; p++) {
        int n = (int)strlen(p->name);
        if (n > longest) longest = n;
        total++;
        if (mino_capability_installed(S, p->bit)) installed++;
    }

    fprintf(out, "Capabilities: %d of %d installed\n\n", installed, total);
    fprintf(out, "  %-*s  %s\n", longest, "name", "description");
    fprintf(out, "  %-*s  %s\n", longest, "----", "-----------");
    for (p = mino_capability_list(); p->name != NULL; p++) {
        const char *mark = mino_capability_installed(S, p->bit)
                          ? "  [x]" : "  [ ]";
        fprintf(out, "%s %-*s  %s\n",
                mark, longest, p->name,
                p->summary != NULL ? p->summary : "");
    }
    fputc('\n', out);
    fputs("  [x] installed   [ ] available -- the host can enable any\n",
          out);
    fputs("      bit with mino_install(S, env, MINO_CAP_<NAME>) from C.\n", out);
    fputs("      mino-installed? exposes the same bits to running code.\n", out);
}

/* Exec a companion binary ("mino-nrepl" / "mino-lsp") from PATH, passing
 * remaining argv through. Replaces argv[first] with the binary name so the
 * companion observes its own argv[0]. Only returns on failure. */
static int exec_companion(const char *bin, char **rest)
{
    rest[0] = (char *)bin;
    execvp(bin, rest);
    if (errno == ENOENT) {
        fprintf(stderr,
                "mino: '%s' subcommand requires '%s' on PATH\n",
                bin + 5 /* skip "mino-" */, bin);
        return 127;
    }
    fprintf(stderr, "mino: failed to exec %s: %s\n", bin, strerror(errno));
    return 1;
}

/* Run `mino -e EXPR`: evaluate one expression and print its result. */
static int run_eval_expr(mino_state *S, mino_env *env, const char *expr)
{
    mino_val *result = mino_eval_string(S, expr, env);
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
                           int *out_dash_dash,
                           const char **out_jit_mode,
                           const char **out_jit_threshold)
{
    int i = 1;
    *out_eval = NULL;
    *out_dash_dash = 0;
    *out_jit_mode = NULL;
    *out_jit_threshold = NULL;
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
        if (strncmp(a, "--jit=", 6) == 0) {
            *out_jit_mode = a + 6;
            i++;
            continue;
        }
        if (strncmp(a, "--jit-threshold=", 16) == 0) {
            *out_jit_threshold = a + 16;
            i++;
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

/* ---- REPL specials (`*1` / `*2` / `*3` / `*e` / `*command-line-args*` /
 * `*file*`) ----
 *
 * Interned in clojure.core from main.c rather than mino_install_core so that
 * embedders without a REPL or CLI front-end pay nothing. The vars are set up
 * before file/eval/REPL dispatch so a script reading *command-line-args* or
 * *file* sees them too. The history vars (star1, star2, star3, stare) are
 * only mutated by the REPL loop, but interning them up-front keeps
 * unqualified lookup honest in either mode. */
typedef struct {
    mino_val *star1;
    mino_val *star2;
    mino_val *star3;
    mino_val *stare;
    mino_val *cmdargs;
    mino_val *file;
    mino_env *core_env;
} repl_specials_t;

static void repl_set_special(mino_state *S, mino_env *core_env,
                             mino_val *var, const char *name,
                             mino_val *val)
{
    if (val == NULL) val = mino_nil(S);
    if (var != NULL) {
        var_set_root(S, var, val);
    }
    if (core_env != NULL) {
        mino_env_set(S, core_env, name, val);
    }
}

static mino_val *repl_intern_special(mino_state *S, mino_env *core_env,
                                       const char *name)
{
    mino_val *var = var_intern(S, "clojure.core", name);
    if (var != NULL) {
        var->as.var.dynamic = 1;
    }
    repl_set_special(S, core_env, var, name, mino_nil(S));
    return var;
}

/* Build a cons-list of remaining argv entries as mino strings. Returns nil
 * (the canon empty value for *command-line-args*) when start >= argc. */
static mino_val *build_cmd_args(mino_state *S, int argc, char **argv,
                                  int start)
{
    mino_val *head = mino_nil(S);
    int i;
    for (i = argc - 1; i >= start; i--) {
        mino_val *str = mino_string(S, argv[i]);
        head = mino_cons(S, str, head);
    }
    return head;
}

static void repl_specials_init(mino_state *S, repl_specials_t *r,
                               int argc, char **argv, int first)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    r->core_env = core_env;
    r->star1   = repl_intern_special(S, core_env, "*1");
    r->star2   = repl_intern_special(S, core_env, "*2");
    r->star3   = repl_intern_special(S, core_env, "*3");
    r->stare   = repl_intern_special(S, core_env, "*e");
    r->cmdargs = repl_intern_special(S, core_env, "*command-line-args*");
    r->file    = repl_intern_special(S, core_env, "*file*");

    /* *command-line-args*: skip past the script path / -e expression so the
     * first entry the script sees is the first argument *after* itself. In
     * REPL mode (no file, no -e) this is the empty list. */
    {
        int args_start = (first < argc) ? first + 1 : argc;
        repl_set_special(S, core_env, r->cmdargs, "*command-line-args*",
                         build_cmd_args(S, argc, argv, args_start));
    }
    /* *file*: filled in later when entering file mode; stays
     * "NO_SOURCE_PATH" for the REPL and -e modes, matching the canon
     * convention. */
    repl_set_special(S, core_env, r->file, "*file*",
                     mino_string(S, "NO_SOURCE_PATH"));
}

/* Rotate *1 → *2 → *3 and install `result` as the new *1. Called after each
 * successful REPL eval, including when result is nil. */
static void repl_specials_rotate(mino_state *S, repl_specials_t *r,
                                 mino_val *result)
{
    mino_val *prev1 = (r->star1 != NULL) ? r->star1->as.var.root
                                           : mino_nil(S);
    mino_val *prev2 = (r->star2 != NULL) ? r->star2->as.var.root
                                           : mino_nil(S);
    repl_set_special(S, r->core_env, r->star3, "*3", prev2);
    repl_set_special(S, r->core_env, r->star2, "*2", prev1);
    repl_set_special(S, r->core_env, r->star1, "*1", result);
}

/* Capture the current diagnostic into *e as a structured map.
 * mino_last_error_map() materializes the map representation of the
 * pending diagnostic, which becomes *e for the next user-side query. */
static void repl_specials_capture_error(mino_state *S, repl_specials_t *r)
{
    mino_val *err = mino_last_error_map(S);
    repl_set_special(S, r->core_env, r->stare, "*e", err);
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
 *      out of mino_state; if the state is torn mid-mutation those
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

static mino_state *crash_handler_state = NULL;

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
        mino_gc_stats_out st;
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

    {
        /* One portable path for every target. mino_capture_backtrace
         * walks CFI via the compiler unwinder (_Unwind_Backtrace),
         * which glibc, musl, macOS, and mingw all provide -- so this
         * works where the old glibc-only backtrace()/<execinfo.h> did
         * not (notably a fully static musl build). The capture is
         * address-only and allocation-free; resolve the frames with
         * addr2line (atos on macOS). We skip 0 frames -- matching the
         * old backtrace() which also showed the handler frames -- so a
         * couple of crash_handler_* entries lead the trace. Skipping
         * them is tempting but unsafe: when the signal arrives
         * asynchronously (delivered while blocked in a syscall) some
         * unwinders, e.g. LLVM libunwind in a static musl build, cannot
         * walk past the kernel signal trampoline and capture only the
         * handler frames -- skipping those would drop the whole trace. */
        void *frames[64];
        int   nframes = mino_capture_backtrace(
                            frames, (int)(sizeof(frames) / sizeof(frames[0])), 0);
        if (nframes > 0) {
            crash_handler_write_line(
                "[mino] backtrace (addresses; resolve with addr2line):\n");
            for (int i = 0; i < nframes; i++) {
                n = snprintf(buf, sizeof(buf), "  #%-2d %p\n", i, frames[i]);
                if (n > 0) crash_handler_write_line(buf);
            }
        } else if (nframes < 0) {
            crash_handler_write_line(
                "[mino] backtrace: unavailable on this toolchain\n");
        }
    }
}

static void crash_handler(int sig)
{
    crash_handler_report(sig);
    /* Restore the default disposition and re-raise so the OS delivers
     * the intended fate (core dump for SEGV, termination for ABRT). */
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Weak hook called by libasan before main() runs. Disables fake-stack
 * (use-after-return detection) so conservative stack scanning works.
 * ASan's fake-stack feature relocates each function's address-taken
 * locals into a separately allocated region; the address returned by
 * &local for one call lives in a different region than another call's
 * &local. gc_scan_stack uses &probe at scan time and gc_note_host_frame
 * recorded another &probe at state init -- under fake-stack those two
 * pointers can sit in different fake-stack regions with unmapped memory
 * between them, so walking word-by-word from one to the other SEGVs.
 * The other use-after-return-style coverage we'd lose is non-essential
 * for catching the heap-corruption / red-zone class of bugs that
 * release-gate's ASan run is actually meant to find. The detect_leaks
 * setting stays on. */
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define MINO_BUILT_WITH_ASAN 1
#  endif
#elif defined(__SANITIZE_ADDRESS__)
#  define MINO_BUILT_WITH_ASAN 1
#endif
#ifdef MINO_BUILT_WITH_ASAN
const char *__asan_default_options(void);
const char *__asan_default_options(void)
{
    return "detect_stack_use_after_return=0";
}
#endif

static void install_crash_handler(mino_state *S)
{
    const char *disabled = getenv("MINO_NO_CRASH_HANDLER");
    if (disabled != NULL && disabled[0] != '\0' && disabled[0] != '0') {
        return;
    }
    /* Skip when running under a sanitizer: libasan / libtsan / libubsan
     * install their own SIGSEGV / SIGABRT handlers and produce far more
     * detailed reports (red-zone diagnosis, allocation backtraces, leak
     * graphs). Mino's handler intercepting SIGSEGV first means the
     * sanitizer's report path never runs -- on ubuntu-24.04 x86_64 a
     * conservative-stack-scan false positive surfaced as an opaque
     * "[mino] fatal SIGSEGV" line with no ASan preamble, masking the
     * actual diagnostic. Detect clang via __has_feature and gcc via
     * __SANITIZE_*__ predefined macros (same dual-path used in
     * gc/internal.h's MINO_GC_PIN_LOUD_ASSERT). */
#if defined(__has_feature)
#  if __has_feature(address_sanitizer) \
      || __has_feature(thread_sanitizer) \
      || __has_feature(undefined_behavior_sanitizer)
    return;
#  endif
#elif defined(__SANITIZE_ADDRESS__) \
    || defined(__SANITIZE_THREAD__) \
    || defined(__SANITIZE_UNDEFINED__)
    return;
#endif
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

/* Interactive REPL loop. Runs until stdin reaches EOF, the user types
 * :quit, or an unrecoverable allocation failure aborts. `specials`
 * carries the `*1 *2 *3 *e` bookkeeping inherited from main; the env
 * is mino's default user-environment seeded with the standard
 * library. */
static int run_repl(mino_state *S, mino_env *env,
                    repl_specials_t *specials)
{
    char  *buf       = NULL;
    size_t cap       = 0;
    size_t len       = 0;
    /* Session-wide history of REPL input. The parse buffer (buf/len)
     * is truncated each time a form is consumed; the history buffer
     * (hist_buf/hist_len) is append-only so source_cache_store can
     * publish every line the user has typed for snippet rendering.
     * Without it, errors that name a line number past the first form
     * land in a cache that no longer contains the relevant line. */
    char  *hist_buf  = NULL;
    size_t hist_cap  = 0;
    size_t hist_len  = 0;
    int    awaiting_continuation = 0;
    int    exit_code = 0;

    {
        const mino_capability_info *p;
        int total = 0, installed = 0;
        for (p = mino_capability_list(); p->name != NULL; p++) {
            total++;
            if (mino_capability_installed(S, p->bit)) installed++;
        }
        if (installed < total) {
            fprintf(stderr,
                "mino %s (embedded, %d of %d capabilities installed)\n",
                mino_version_string(), installed, total);
            fputs("Type :capabilities to see the full list, "
                  ":help for help, :quit to exit\n", stderr);
        } else {
            fprintf(stderr, "mino %s\n", mino_version_string());
            fputs("Type :help for help, :quit to exit\n", stderr);
        }
    }
    fputs("mino=> ", stderr);
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

            /* Mirror the line into the append-only history buffer
             * used for source-snippet rendering. Errors reference
             * reader_line, which counts every line typed since
             * session start, so the cache needs the full history,
             * not just the bytes still pending parse. */
            if (hist_len + add + 1 > hist_cap) {
                size_t new_cap = hist_cap == 0 ? 256 : hist_cap;
                while (new_cap < hist_len + add + 1) {
                    new_cap *= 2;
                }
                hist_buf = (char *)realloc(hist_buf, new_cap);
                if (hist_buf == NULL) {
                    fputs("mino: out of memory\n", stderr);
                    exit_code = 1;
                    goto cleanup;
                }
                hist_cap = new_cap;
            }
            memcpy(hist_buf + hist_len, line, add + 1);
            hist_len += add;
        }

        /* Drain as many complete forms as we have. */
        for (;;) {
            const char *cursor = buf;
            const char *end    = buf;
            mino_val *form;
            mino_val *result;

            if (has_only_whitespace(buf)) {
                /* Don't discard trailing whitespace: newlines in it feed
                 * the reader's line counter on the next call. */
                awaiting_continuation = 0;
                break;
            }

            /* Cache REPL input for diagnostic source snippets. Publish
             * the full session history, not just the parse buffer: the
             * parse buffer is truncated whenever a form is consumed,
             * but reader_line keeps accumulating across forms. */
            source_cache_store(S, S->reader.reader_file, hist_buf, hist_len);

            form = mino_read(S, cursor, &end);
            if (form != NULL && form->type == MINO_KEYWORD) {
                const char *name = form->as.s.data;
                if (strcmp(name, "quit") == 0) {
                    fputc('\n', stderr);
                    exit_code = 0;
                    goto cleanup;
                }
                if (strcmp(name, "help") == 0) {
                    print_repl_help(stderr);
                    {
                        size_t consumed  = (size_t)(end - buf);
                        size_t remaining = len - consumed;
                        memmove(buf, end, remaining + 1);
                        len = remaining;
                    }
                    awaiting_continuation = 0;
                    continue;
                }
                if (strcmp(name, "capabilities") == 0
                    || strcmp(name, "caps") == 0) {
                    print_repl_capabilities(stderr, S);
                    {
                        size_t consumed  = (size_t)(end - buf);
                        size_t remaining = len - consumed;
                        memmove(buf, end, remaining + 1);
                        len = remaining;
                    }
                    awaiting_continuation = 0;
                    continue;
                }
            }
            if (form == NULL) {
                const char *err = mino_last_error(S);
                if (is_unterminated_error(err)) {
                    awaiting_continuation = 1;
                    break;
                }
                {
                    const mino_diag *d = mino_last_diag(S);
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
                const mino_diag *d = mino_last_diag(S);
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
                repl_specials_capture_error(S, specials);
            } else {
                repl_specials_rotate(S, specials, result);
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

        fputs(awaiting_continuation ? "  #_=> " : "mino=> ", stderr);
        fflush(stderr);
    }

    fputc('\n', stderr);

cleanup:
    free(buf);
    free(hist_buf);
    return exit_code;
}

int main(int argc, char **argv)
{
    mino_state *S;
    int           first       = 1;
    const char   *eval_expr   = NULL;
    int           dash_dash   = 0;
    int           parse_state;

#ifdef _WIN32
    /* On Windows, MSVCRT's stdout is fully buffered when stdout is not
     * a tty (e.g., when launched through Scoop's shim under
     * PowerShell). The shim's child-process plumbing doesn't always
     * propagate the buffered tail when mino.exe exits, so users see
     * `mino --version` and the REPL banner produce no output. Force
     * line-buffered stdout and unbuffered stderr at program start so
     * output is visible regardless of how the binary is invoked. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
#endif

    const char *cli_jit_mode      = NULL;
    const char *cli_jit_threshold = NULL;
    parse_state = parse_cli_flags(argc, argv, &first, &eval_expr, &dash_dash,
                                  &cli_jit_mode, &cli_jit_threshold);
    if (parse_state == 1) return 0;
    if (parse_state == 2) return 2;
    /* --jit=auto|off|on rejects anything else loudly. Matches the
     * MINO_JIT env var's case-insensitive parsing in state.c, so
     * --jit=ON and MINO_JIT=ON don't diverge in behavior. */
    mino_jit_mode cli_jit = MINO_JIT_MODE_AUTO;
    int cli_jit_set = 0;
    if (cli_jit_mode != NULL) {
        if (mino_strcasecmp(cli_jit_mode, "auto") == 0) {
            cli_jit = MINO_JIT_MODE_AUTO; cli_jit_set = 1;
        } else if (mino_strcasecmp(cli_jit_mode, "off") == 0) {
            cli_jit = MINO_JIT_MODE_OFF;  cli_jit_set = 1;
        } else if (mino_strcasecmp(cli_jit_mode, "on") == 0) {
            cli_jit = MINO_JIT_MODE_ON;   cli_jit_set = 1;
        } else {
            fprintf(stderr,
                    "mino: --jit value must be auto, off, or on (got '%s')\n",
                    cli_jit_mode);
            return 2;
        }
    }
    unsigned cli_threshold = 0;
    int cli_threshold_set = 0;
    if (cli_jit_threshold != NULL) {
        char *endp = NULL;
        unsigned long v = strtoul(cli_jit_threshold, &endp, 10);
        if (endp == NULL || *endp != '\0' || v < 1ul || v > 0xfffffffful) {
            fprintf(stderr,
                    "mino: --jit-threshold must be a positive integer "
                    "(got '%s')\n", cli_jit_threshold);
            return 2;
        }
        cli_threshold     = (unsigned)v;
        cli_threshold_set = 1;
    }

    /* Companion-binary subcommands. Exec before runtime init so we don't
     * pay for an interpreter we're about to discard. */
    if (!dash_dash && eval_expr == NULL && first < argc) {
        if (strcmp(argv[first], "nrepl") == 0)
            return exec_companion("mino-nrepl", argv + first);
        if (strcmp(argv[first], "lsp") == 0)
            return exec_companion("mino-lsp", argv + first);
    }

    /* Best-effort: clear the buffer if getcwd fails so any subsequent
     * use of initial_dir reads as the empty path rather than as
     * uninitialised stack. Ubuntu glibc declares getcwd with the
     * `warn_unused_result` attribute and the bootstrap CFLAGS treat
     * unused-result as an error. */
    if (getcwd(initial_dir, sizeof(initial_dir)) == NULL) {
        initial_dir[0] = '\0';
    }

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
    if (cli_jit_set)       mino_state_set_jit_mode(S, cli_jit);
    if (cli_threshold_set) mino_state_set_jit_hot_threshold(S, cli_threshold);

    /* If the user explicitly requested JIT=ON (CLI or env) but this
     * build has the JIT compiled out or the host isn't supported,
     * announce that the request is being honored as a no-op. Silent
     * dropping of user intent is the worst-of-both for the dual-binary
     * design -- the interpreter still runs, but the user thinks they
     * got JIT. AUTO and OFF don't trigger the warning because they're
     * compatible with a no-JIT build. */
    {
        mino_jit_capability cap = mino_state_jit_capability(S);
        if (cap.available == 0) {
            int explicit_on = (cli_jit_set && cli_jit == MINO_JIT_MODE_ON);
            if (!explicit_on) {
                const char *env_jit = getenv("MINO_JIT");
                if (env_jit != NULL && mino_strcasecmp(env_jit, "on") == 0) {
                    explicit_on = 1;
                }
            }
            if (explicit_on) {
                fprintf(stderr,
                    "mino: note: this build has the JIT compiled out; "
                    "--jit=on / MINO_JIT=on has no effect, "
                    "interpreter will run\n");
            }
        }
    }

    install_crash_handler(S);
    mino_env *env = mino_env_new(S);
    int exit_code   = 0;

    /* Development knobs. These override the collector defaults before
     * any allocation happens. Range errors silently fall back to the
     * default because a CLI user fat-fingering an env var should not
     * block the interpreter from starting. */
    {
        struct { const char *name; mino_gc_param p; } knobs[] = {
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

    mino_install_all(S, env);
    mino_set_resolver(S, runtime_paths_resolve, S);

    /* CLI / REPL prints qualified-key maps as `#:ns{:k v}`. The
     * var declaration is `false` at the language level (matching JVM
     * Clojure canon), but JVM Clojure's REPL and Babashka both alter
     * the root to true on startup so users see collapsed prints; mino
     * does the same here. Embedded users get the false default
     * because they don't go through main() at all. Failure of this
     * call would mean the runtime is in a bad enough state that the
     * subsequent user load will also fail clearly; treat it as
     * non-fatal so a build that disables print-namespace-maps still
     * boots. */
    (void)mino_eval_string(S,
        "(alter-var-root (var *print-namespace-maps*) (constantly true))",
        env);

    /* Bytecode require knob. With MINO_BC_REQUIRE=1 set, the bc layer
     * aborts on any tree-walker fallback so VM regressions surface
     * during development. Default (unset / "0") leaves the silent
     * fallback path in place. */
    {
        extern int mino_bc_require_flag;
        const char *raw = getenv("MINO_BC_REQUIRE");
        if (raw != NULL && raw[0] != '\0' && raw[0] != '0') {
            mino_bc_require_flag = 1;
        }
    }

    /* Standalone mode: grant the runtime permission to spawn cpu_count
     * host threads. Embedded users start at thread_limit=1 and opt in
     * via mino_set_thread_limit. The actual host-thread implementation
     * lands across upcoming versions; until it does, future/promise/
     * thread/blocking core.async ops throw :mino/unsupported with a
     * message naming the cycle.
     *
     * Use sysctlbyname on Darwin (the sysconf names are hidden under
     * strict _POSIX_C_SOURCE there), GetSystemInfo on Windows, and
     * sysconf on other POSIX. _SC_NPROCESSORS_ONLN is an enum in the
     * Linux/glibc and musl unistd.h regardless of feature-test macros,
     * so the call resolves; an earlier preprocessor #if guard was dead
     * code (the enum is not a #define). */
    {
        int n = 1;
#if defined(_WIN32)
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        if (si.dwNumberOfProcessors > 0) {
            n = (int)si.dwNumberOfProcessors;
        }
#elif defined(__APPLE__)
        int    hw = 0;
        size_t sz = sizeof(hw);
        if (sysctlbyname("hw.activecpu", &hw, &sz, NULL, 0) == 0 && hw > 0) {
            n = hw;
        } else if (sysctlbyname("hw.ncpu", &hw, &sz, NULL, 0) == 0 && hw > 0) {
            n = hw;
        }
#else
        long sc = sysconf(_SC_NPROCESSORS_ONLN);
        if (sc > 0) { n = (int)sc; }
#endif
        if (n < 1) { n = 1; }
        mino_set_thread_limit(S, n);
    }

    repl_specials_t specials;
    repl_specials_init(S, &specials, argc, argv, first);

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

    /* `mino repl` is an explicit alias for bare `mino`: skip file mode and
     * fall through to the REPL block. Extra positional args are ignored. */
    if (!dash_dash && first < argc && strcmp(argv[first], "repl") == 0) {
        first = argc;
    }

    /* When the first positional argument starts with a character that
     * unambiguously begins a Lisp form, evaluate it as an expression
     * instead of treating it as a file path. Matches the convenience
     * shape other Lisp CLIs offer for `mino "(+ 1 2)"`. The `--`
     * separator forces file-or-task interpretation; the explicit
     * `-e EXPR` flag still works either way. */
    if (!dash_dash && first < argc) {
        char c = argv[first][0];
        if (c == '(' || c == '[' || c == '{'
            || c == '#' || c == '@' || c == '\'') {
            exit_code = run_eval_expr(S, env, argv[first]);
            mino_env_free(S, env);
            mino_state_free(S);
            return exit_code;
        }
    }

    /* File mode: evaluate a script and exit. */
    if (first < argc) {
        mino_val *result;
        repl_set_special(S, specials.core_env, specials.file, "*file*",
                         mino_string(S, argv[first]));
        result = mino_load_file(S, argv[first], env);
        if (result == NULL) {
            const mino_diag *d = mino_last_diag(S);
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

    exit_code = run_repl(S, env, &specials);
    mino_env_free(S, env);
    mino_state_free(S);
    return exit_code;
}
