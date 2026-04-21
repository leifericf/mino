/*
 * prim_io.c -- I/O primitives: println, prn, slurp, spit, exit, time-ms,
 *              nano-time, file-seq, print_str_to helper.
 */

#include "prim_internal.h"
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

mino_val_t *prim_prn(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    int first = 1;
    (void)env;
    while (mino_is_cons(args)) {
        if (!first) fputc(' ', stdout);
        mino_print(S, args->as.cons.car);
        first = 0;
        args = args->as.cons.cdr;
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

/* (nano-time) — return monotonic wall-clock time in nanoseconds as an integer.
 * Uses CLOCK_MONOTONIC on POSIX, QueryPerformanceCounter on Windows,
 * clock() as a coarse fallback. */
mino_val_t *prim_nano_time(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    long long ns;
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "nano-time takes no arguments");
    }
#if defined(_WIN32)
    {
        LARGE_INTEGER freq, count;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&count);
        ns = (long long)(count.QuadPart * 1000000000LL / freq.QuadPart);
    }
#elif defined(CLOCK_MONOTONIC)
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ns = (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
    }
#else
    ns = (long long)((double)clock() / (double)CLOCKS_PER_SEC * 1e9);
#endif
    return mino_int(S, ns);
}

/* (getcwd) -- return the current working directory as a string. */
mino_val_t *prim_getcwd(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    char buf[4096];
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
        char path[4096];
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
