/*
 * fs.c -- filesystem primitives: file-exists?, directory?, mkdir-p, rm-rf.
 *
 * These are in C rather than mino sh! wrappers for two reasons:
 *   - file-exists? and directory? are called on every module resolution
 *     attempt; stat(2) is microseconds vs. milliseconds for fork+exec.
 *   - mkdir -p and rm -rf are Unix shell commands; the C implementations
 *     using POSIX APIs are portable across platforms.
 *
 * Trust model.
 *
 * `mkdir-p` and `rm-rf` take whatever path the script author hands them.
 * The embedder is inside the trust boundary; the script author *is* the
 * trust boundary.  These primitives validate argument *shape* (paths
 * must be strings) but do not police *intent* — no allowlist, no chroot,
 * no sandbox.  An embedder that wants to forbid filesystem mutation
 * should refuse to bind these primitives in the embedder's namespace.
 */

#define _POSIX_C_SOURCE 200809L
/* On macOS, strict _POSIX_C_SOURCE hides the BSD st_mtimespec extension
 * (and Darwin has no st_mtim), so re-enable the Darwin surface to keep
 * sub-second file-mtime precision available. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif

#include "prim/internal.h"
#include "mino.h"
#include "path_buf.h"
#if !defined(_MSC_VER)
#  include <dirent.h>
#  include <sys/stat.h>
#  include <unistd.h>
#else
#  include "win_dirent.h"
#endif
#include <errno.h>
#include <string.h>

/* (file-exists? path) -- return true if path exists (file or directory). */
static mino_val *prim_file_exists_p(mino_state *S, mino_val *args,
                               mino_env *env)
{
    mino_val *path_val;
    struct stat st;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "file-exists? requires one argument");
    }
    path_val = args->as.cons.car;
    if (path_val == NULL || mino_type_of(path_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "file-exists?: argument must be a string");
    }
    return stat(path_val->as.s.data, &st) == 0
        ? mino_true(S)
        : mino_false(S);
}

/* (directory? path) -- return true if path is a directory. */
static mino_val *prim_directory_p(mino_state *S, mino_val *args,
                             mino_env *env)
{
    mino_val *path_val;
    struct stat st;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "directory? requires one argument");
    }
    path_val = args->as.cons.car;
    if (path_val == NULL || mino_type_of(path_val) != MINO_STRING) {
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
static mino_val *prim_mkdir_p(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *path_val;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "mkdir-p requires one argument");
    }
    path_val = args->as.cons.car;
    if (path_val == NULL || mino_type_of(path_val) != MINO_STRING) {
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
#ifdef _WIN32
    if (stat(path, &st) != 0) return 0;  /* lstat unavailable on Windows */
#else
    if (lstat(path, &st) != 0) return 0; /* don't follow symlinks */
#endif

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
static mino_val *prim_rm_rf(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *path_val;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "rm-rf requires one argument");
    }
    path_val = args->as.cons.car;
    if (path_val == NULL || mino_type_of(path_val) != MINO_STRING) {
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
static mino_val *prim_file_mtime(mino_state *S, mino_val *args,
                            mino_env *env)
{
    mino_val *path_val;
    struct stat st;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "file-mtime requires one argument");
    }
    path_val = args->as.cons.car;
    if (path_val == NULL || mino_type_of(path_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "file-mtime: argument must be a string");
    }
    if (stat(path_val->as.s.data, &st) != 0)
        return mino_nil(S);
    /* Use sub-second precision where struct stat carries it. macOS only
     * ever exposes it as st_mtimespec (it has no st_mtim even though this
     * file defines _POSIX_C_SOURCE), so check __APPLE__ first; Linux and
     * other POSIX.1-2008 platforms use st_mtim. Fall back to whole-second
     * st_mtime on Windows and elsewhere. */
#if defined(__APPLE__)
    return mino_int(S, (long long)st.st_mtimespec.tv_sec  * 1000LL
                     + (long long)st.st_mtimespec.tv_nsec / 1000000LL);
#elif defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200809L && !defined(_WIN32)
    return mino_int(S, (long long)st.st_mtim.tv_sec  * 1000LL
                     + (long long)st.st_mtim.tv_nsec / 1000000LL);
#else
    return mino_int(S, (long long)st.st_mtime * 1000LL);
#endif
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

void mino_install_fs(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_fs, k_prims_fs_count, "fs");
    mino_install_image_prims(S, env);
    S->caps_installed |= MINO_CAP_FS;
}
