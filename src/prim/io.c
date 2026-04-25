/*
 * io.c -- I/O primitives: println, prn, slurp, spit, exit, time-ms,
 *              nano-time, file-seq, print_str_to helper.
 */

#include "prim/internal.h"
#include "path_buf.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#elif defined(CLOCK_MONOTONIC)
#  include <time.h>
#endif

void print_str_to(mino_state_t *S, FILE *out, const mino_val_t *v)
{
    if (v != NULL && v->type == MINO_STRING) {
        fwrite(v->as.s.data, 1, v->as.s.len, out);
    } else {
        mino_print_to(S, out, v);
    }
}

mino_val_t *prim_println(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    int first = 1;
    (void)env;
    while (mino_is_cons(args)) {
        if (!first) fputc(' ', stdout);
        print_str_to(S, stdout, args->as.cons.car);
        first = 0;
        args = args->as.cons.cdr;
    }
    fputc('\n', stdout);
    fflush(stdout);
    return mino_nil(S);
}

/* Dispatch one value through the print-method hook if installed, or the
 * built-in C formatter otherwise. The hook is a mino fn taking one value
 * and side-effecting to stdout; we ignore its return value.
 *
 * The fallback path stays permanently safe: during boot (before
 * core.mino installs the hook), pr / prn still work on every built-in
 * type. Cortex Q5 invariant. */
static void pr_dispatch_one(mino_state_t *S, mino_val_t *v, mino_env_t *env)
{
    if (S->print_method_fn != NULL) {
        mino_val_t *call_args = mino_cons(S, v, mino_nil(S));
        (void)mino_call(S, S->print_method_fn, call_args, env);
        return;
    }
    mino_print(S, v);
}

mino_val_t *prim_prn(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    int first = 1;
    while (mino_is_cons(args)) {
        if (!first) fputc(' ', stdout);
        pr_dispatch_one(S, args->as.cons.car, env);
        first = 0;
        args = args->as.cons.cdr;
    }
    fputc('\n', stdout);
    fflush(stdout);
    return mino_nil(S);
}

/* (print x ...) writes args space-separated, human-readable, without a
 * trailing newline. Companion to println. */
mino_val_t *prim_print(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    int first = 1;
    (void)env;
    while (mino_is_cons(args)) {
        if (!first) fputc(' ', stdout);
        print_str_to(S, stdout, args->as.cons.car);
        first = 0;
        args = args->as.cons.cdr;
    }
    fflush(stdout);
    return mino_nil(S);
}

/* (pr x ...) writes args space-separated, readably (strings quoted, chars
 * escaped), without a trailing newline. Companion to prn.
 *
 * Routes each argument through print-method when the hook is installed
 * (v0.52.0), otherwise uses the built-in C formatter. */
mino_val_t *prim_pr(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    int first = 1;
    while (mino_is_cons(args)) {
        if (!first) fputc(' ', stdout);
        pr_dispatch_one(S, args->as.cons.car, env);
        first = 0;
        args = args->as.cons.cdr;
    }
    fflush(stdout);
    return mino_nil(S);
}

/* (pr-builtin x) writes one value via the built-in C formatter, bypassing
 * the print-method hook. Used by print-method's :default method so the
 * default path does not recurse into itself. */
mino_val_t *prim_pr_builtin(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "pr-builtin requires one argument");
    }
    mino_print(S, args->as.cons.car);
    fflush(stdout);
    return mino_nil(S);
}

/* (set-print-method! fn) — install a late-binding hook for pr / prn.
 * Calling with nil removes the hook. The hook must be a fn that prints
 * its one argument to stdout. */
mino_val_t *prim_set_print_method_bang(mino_state_t *S, mino_val_t *args,
                                       mino_env_t *env)
{
    mino_val_t *fn;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "set-print-method! requires one argument");
    }
    fn = args->as.cons.car;
    if (fn == NULL || fn->type == MINO_NIL) {
        S->print_method_fn = NULL;
        return mino_nil(S);
    }
    if (fn->type != MINO_FN && fn->type != MINO_PRIM) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "set-print-method! argument must be a fn");
    }
    S->print_method_fn = fn;
    return mino_nil(S);
}

/* (newline) writes a single line separator. Returns nil. */
mino_val_t *prim_newline(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "newline takes no arguments");
    }
    fputc('\n', stdout);
    fflush(stdout);
    return mino_nil(S);
}

/* (slurp path) — read a file's entire contents as a string. I/O capability;
 * only installed by mino_install_io, not mino_install_core. */
mino_val_t *prim_slurp(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *path_val;
    const char *path;
    FILE       *f;
    long        sz;
    size_t      rd;
    char       *buf;
    mino_val_t *result;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "slurp requires one argument");
    }
    path_val = args->as.cons.car;
    if (path_val == NULL || path_val->type != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "slurp: argument must be a string");
    }
    path = path_val->as.s.data;
    f = fopen(path, "rb");
    if (f == NULL) {
        char msg[300];
        snprintf(msg, sizeof(msg), "slurp: cannot open file: %s", path);
        return prim_throw_classified(S, "host", "MHO001", msg);
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return prim_throw_classified(S, "host", "MHO001", "slurp: cannot determine file size");
    }
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(f);
        return prim_throw_classified(S, "host", "MHO001", "slurp: out of memory");
    }
    rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    result = mino_string_n(S, buf, rd);
    free(buf);
    return result;
}

mino_val_t *prim_spit(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *path_val;
    mino_val_t *content;
    const char *path;
    FILE       *f;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "spit requires two arguments");
    }
    path_val = args->as.cons.car;
    content  = args->as.cons.cdr->as.cons.car;
    if (path_val == NULL || path_val->type != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "spit: first argument must be a string path");
    }
    path = path_val->as.s.data;
    f = fopen(path, "wb");
    if (f == NULL) {
        char msg[300];
        snprintf(msg, sizeof(msg), "spit: cannot open file: %s", path);
        return prim_throw_classified(S, "host", "MHO001", msg);
    }
    if (content != NULL && content->type == MINO_STRING) {
        fwrite(content->as.s.data, 1, content->as.s.len, f);
    } else {
        mino_print_to(S, f, content);
    }
    fclose(f);
    return mino_nil(S);
}

/* (exit code) — terminate the process with the given exit code.
 * Defaults to 0 if no argument is given. */
mino_val_t *prim_exit(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    int code = 0;
    (void)env;
    if (mino_is_cons(args)) {
        mino_val_t *v = args->as.cons.car;
        if (v != NULL && v->type == MINO_INT) {
            code = (int)v->as.i;
        } else if (v != NULL && v->type == MINO_FLOAT) {
            code = (int)v->as.f;
        }
    }
    exit(code);
    return mino_nil(S); /* unreachable */
}

/* (time-ms) — return process time in milliseconds as a float.
 * Uses ANSI C clock() for portability across all C99 platforms. */
mino_val_t *prim_time_ms(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)args;
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "time-ms takes no arguments");
    }
    return mino_float(S, (double)clock() / (double)CLOCKS_PER_SEC * 1000.0);
}

/* (nano-time) — return monotonic wall-clock time in nanoseconds as an integer. */
mino_val_t *prim_nano_time(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "nano-time takes no arguments");
    }
    return mino_int(S, mino_monotonic_ns());
}

/* (getcwd) -- return the current working directory as a string. */
mino_val_t *prim_getcwd(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    char buf[PATH_BUF_CAP];
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "getcwd takes no arguments");
    }
    if (getcwd(buf, sizeof(buf)) == NULL) {
        return prim_throw_classified(S, "io", "MIO001",
                                     "getcwd: failed to get working directory");
    }
    return mino_string(S, buf);
}

/* (chdir path) -- change current working directory. Returns nil. */
mino_val_t *prim_chdir(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *path_val;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "chdir requires one argument");
    }
    path_val = args->as.cons.car;
    if (path_val == NULL || path_val->type != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "chdir: argument must be a string");
    }
    if (chdir(path_val->as.s.data) != 0) {
        return prim_throw_classified(S, "io", "MIO001",
                                     "chdir: directory not found");
    }
    return mino_nil(S);
}

/* (getenv name) -- return environment variable value or nil. */
mino_val_t *prim_getenv(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *name_val;
    const char *val;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "getenv requires one argument");
    }
    name_val = args->as.cons.car;
    if (name_val == NULL || name_val->type != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "getenv: argument must be a string");
    }
    val = getenv(name_val->as.s.data);
    if (val == NULL) return mino_nil(S);
    return mino_string(S, val);
}

/* ---- file-seq: recursive directory listing ---- */

static void file_seq_recurse(mino_state_t *S, const char *dir,
                             mino_val_t ***items, size_t *len, size_t *cap)
{
    DIR *d = opendir(dir);
    struct dirent *ent;
    if (d == NULL) return;
    while ((ent = readdir(d)) != NULL) {
        char path[PATH_BUF_CAP];
        struct stat st;
        if (ent->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            file_seq_recurse(S, path, items, len, cap);
        } else {
            if (*len == *cap) {
                size_t nc = *cap == 0 ? 64 : *cap * 2;
                mino_val_t **nb = (mino_val_t **)realloc(*items,
                                                          nc * sizeof(**items));
                if (nb == NULL) { closedir(d); return; }
                *items = nb;
                *cap = nc;
            }
            (*items)[*len] = mino_string(S, path);
            (*len)++;
        }
    }
    closedir(d);
}

mino_val_t *prim_file_seq(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *dir_val;
    const char *dir;
    mino_val_t **items = NULL;
    size_t len = 0, cap = 0;
    mino_val_t *result;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "file-seq requires one argument");
    }
    dir_val = args->as.cons.car;
    if (dir_val == NULL || dir_val->type != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "file-seq: argument must be a string");
    }
    dir = dir_val->as.s.data;
    file_seq_recurse(S, dir, &items, &len, &cap);
    result = mino_vector(S, items, len);
    free(items);
    return result;
}

/* k_prims_io_core -- printer hooks installed during mino_install_core so
 * core.mino's print-method multimethod can register itself before any
 * code calls pr.  pr / prn / etc. live in k_prims_io and are installed
 * by mino_install_io, which sandboxed embedders may skip. */
const mino_prim_def k_prims_io_core[] = {
    {"pr-builtin",        prim_pr_builtin,
     "Prints a value readably via the built-in C formatter, bypassing print-method."},
    {"set-print-method!", prim_set_print_method_bang,
     "Installs a fn to dispatch pr / prn output; nil removes the hook."},
};

const size_t k_prims_io_core_count =
    sizeof(k_prims_io_core) / sizeof(k_prims_io_core[0]);

const mino_prim_def k_prims_io[] = {
    {"println",  prim_println,
     "Prints the arguments followed by a newline."},
    {"prn",      prim_prn,
     "Prints the arguments readably followed by a newline."},
    {"print",    prim_print,
     "Prints the arguments space-separated, without a trailing newline."},
    {"pr",       prim_pr,
     "Prints the arguments readably, without a trailing newline."},
    {"newline",  prim_newline,
     "Writes a line separator to stdout."},
    {"slurp",    prim_slurp,
     "Reads the entire contents of a file as a string."},
    {"spit",     prim_spit,
     "Writes the string content to a file."},
    {"exit",     prim_exit,
     "Exits the process with the given status code."},
    {"time-ms",  prim_time_ms,
     "Returns the current time in milliseconds."},
    {"nano-time", prim_nano_time,
     "Returns monotonic wall-clock time in nanoseconds."},
    {"file-seq", prim_file_seq,
     "Returns a vector of all file paths under a directory, recursively."},
    {"getenv",   prim_getenv,
     "Returns the value of an environment variable, or nil."},
    {"getcwd",   prim_getcwd,
     "Returns the current working directory."},
    {"chdir",    prim_chdir,
     "Changes the current working directory."},
    {"gc-stats", prim_gc_stats,
     "Returns a map of GC statistics."},
    {"gc!",      prim_gc_bang,
     "Forces a full garbage collection. Returns nil."},
};

const size_t k_prims_io_count =
    sizeof(k_prims_io) / sizeof(k_prims_io[0]);

void mino_install_io(mino_state_t *S, mino_env_t *env)
{
    prim_install_table(S, env, k_prims_io, k_prims_io_count);
}
