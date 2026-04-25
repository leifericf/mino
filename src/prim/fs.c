/*
 * fs.c -- filesystem primitives: file-exists?, directory?, mkdir-p, rm-rf.
 *
 * These are in C rather than mino sh! wrappers for two reasons:
 *   - file-exists? and directory? are called on every module resolution
 *     attempt; stat(2) is microseconds vs. milliseconds for fork+exec.
 *   - mkdir -p and rm -rf are Unix shell commands; the C implementations
 *     using POSIX APIs are portable across platforms.
 */

#include "prim/internal.h"
#include "path_buf.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

/* (file-exists? path) -- return true if path exists (file or directory). */
mino_val_t *prim_file_exists_p(mino_state_t *S, mino_val_t *args,
                               mino_env_t *env)
{
    mino_val_t *path_val;
    struct stat st;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "file-exists? requires one argument");
    }
    path_val = args->as.cons.car;
    if (path_val == NULL || path_val->type != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "file-exists?: argument must be a string");
    }
    return stat(path_val->as.s.data, &st) == 0
        ? mino_true(S)
        : mino_false(S);
}

/* (directory? path) -- return true if path is a directory. */
mino_val_t *prim_directory_p(mino_state_t *S, mino_val_t *args,
                             mino_env_t *env)
{
    mino_val_t *path_val;
    struct stat st;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "directory? requires one argument");
    }
    path_val = args->as.cons.car;
    if (path_val == NULL || path_val->type != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "directory?: argument must be a string");
    }
    if (stat(path_val->as.s.data, &st) != 0)
        return mino_false(S);
    return S_ISDIR(st.st_mode) ? mino_true(S) : mino_false(S);
}

/* ---- mkdir-p: recursive directory creation ---- */

static int mkdirp(const char *path)
{
    char buf[PATH_BUF_CAP];
    size_t len;
    size_t i;
    struct stat st;

    len = strlen(path);
    if (len == 0 || len >= sizeof(buf)) return -1;
    memcpy(buf, path, len + 1);

    /* Walk path components and create each one. */
    for (i = 1; i <= len; i++) {
        if (buf[i] == '/' || buf[i] == '\0') {
            char saved = buf[i];
            buf[i] = '\0';
            if (stat(buf, &st) != 0) {
#ifdef _WIN32
                if (mkdir(buf) != 0 && errno != EEXIST) return -1;
#else
                if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
#endif
            }
            buf[i] = saved;
        }
    }
    return 0;
}

/* (mkdir-p path) -- create directory and parents. Returns nil. */
mino_val_t *prim_mkdir_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *path_val;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "mkdir-p requires one argument");
    }
    path_val = args->as.cons.car;
    if (path_val == NULL || path_val->type != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "mkdir-p: argument must be a string");
    }
    if (mkdirp(path_val->as.s.data) != 0) {
        char msg[300];
        snprintf(msg, sizeof(msg), "mkdir-p: cannot create directory: %s",
                 path_val->as.s.data);
        return prim_throw_classified(S, "io", "MIO001", msg);
    }
    return mino_nil(S);
}

/* ---- rm-rf: recursive removal ---- */

static int rmrf(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0; /* nothing to remove */

    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *ent;
        if (d == NULL) return -1;
        while ((ent = readdir(d)) != NULL) {
            char child[PATH_BUF_CAP];
            if (strcmp(ent->d_name, ".") == 0 ||
                strcmp(ent->d_name, "..") == 0)
                continue;
            snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
            if (rmrf(child) != 0) {
                closedir(d);
                return -1;
            }
        }
        closedir(d);
        return rmdir(path);
    }
    return unlink(path);
}

/* (rm-rf path) -- recursively remove path. Returns nil. */
mino_val_t *prim_rm_rf(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *path_val;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "rm-rf requires one argument");
    }
    path_val = args->as.cons.car;
    if (path_val == NULL || path_val->type != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "rm-rf: argument must be a string");
    }
    if (rmrf(path_val->as.s.data) != 0) {
        char msg[300];
        snprintf(msg, sizeof(msg), "rm-rf: cannot remove: %s",
                 path_val->as.s.data);
        return prim_throw_classified(S, "io", "MIO001", msg);
    }
    return mino_nil(S);
}

/* (file-mtime path) -- return modification time as milliseconds, or nil. */
mino_val_t *prim_file_mtime(mino_state_t *S, mino_val_t *args,
                            mino_env_t *env)
{
    mino_val_t *path_val;
    struct stat st;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "file-mtime requires one argument");
    }
    path_val = args->as.cons.car;
    if (path_val == NULL || path_val->type != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "file-mtime: argument must be a string");
    }
    if (stat(path_val->as.s.data, &st) != 0)
        return mino_nil(S);
    return mino_int(S, (long long)st.st_mtime * 1000LL);
}

/* ---- install ---- */

const mino_prim_def k_prims_fs[] = {
    {"file-exists?", prim_file_exists_p,
     "Returns true if the path exists (file or directory)."},
    {"directory?",   prim_directory_p,
     "Returns true if the path is a directory."},
    {"mkdir-p",      prim_mkdir_p,
     "Creates a directory and any missing parent directories."},
    {"rm-rf",        prim_rm_rf,
     "Recursively removes a file or directory."},
    {"file-mtime",   prim_file_mtime,
     "Returns the file modification time in milliseconds, or nil."},
};

const size_t k_prims_fs_count =
    sizeof(k_prims_fs) / sizeof(k_prims_fs[0]);

void mino_install_fs(mino_state_t *S, mino_env_t *env)
{
    prim_install_table(S, env, k_prims_fs, k_prims_fs_count);
}
