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
#  include <fcntl.h>      /* openat, O_NOFOLLOW, AT_SYMLINK_NOFOLLOW, unlinkat */
#else
#  include "win_dirent.h"
#endif
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

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

#if !defined(_WIN32)
/* POSIX race-free recursive removal using the *at family.
 *
 * The previous lstat(path)-then-opendir(path) walk had a TOCTOU window:
 * between seeing a directory and opening it, an attacker with write
 * access to the parent could swap the entry for a symlink, redirecting
 * the recursion into the symlink's target tree (CWE-59). The *at walk
 * pins each level by its open directory descriptor and stat's every
 * child with AT_SYMLINK_NOFOLLOW relative to that descriptor, so a path
 * swapped mid-walk cannot redirect the descent. A symlink planted as a
 * direct entry is unlinked, never descended.
 *
 * Names are snapshotted before unlinking so a directory mutation during
 * readdir cannot skip or double-visit an entry. */

static int rmrf_at(int dirfd, const char *name);

static int rmrf_children(int dfd)
{
    DIR           *d;
    struct dirent *ent;
    char         **names = NULL;
    size_t          n = 0, cap = 0, i;
    int             rc = 0;
    /* fdopendir takes ownership of dfd (closedir closes it). Keep a
     * second descriptor on the same directory so unlinkat after the
     * close still resolves relative to the pinned directory. */
    int dfd2 = dup(dfd);
    if (dfd2 < 0) return -1;
    d = fdopendir(dfd);
    if (d == NULL) { close(dfd2); return -1; }
    while ((ent = readdir(d)) != NULL) {
        char **nn;
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            nn = (char **)realloc(names, cap * sizeof(*names));
            if (nn == NULL) { rc = -1; break; }
            names = nn;
        }
        names[n] = strdup(ent->d_name);
        if (names[n] == NULL) { rc = -1; break; }
        n++;
    }
    closedir(d);  /* closes the original dfd; dfd2 stays open */
    for (i = 0; i < n && rc == 0; i++)
        rc = rmrf_at(dfd2, names[i]);
    for (i = 0; i < n; i++) free(names[i]);
    free(names);
    close(dfd2);
    return rc;
}

/* Remove name relative to dirfd, recursing into real subdirectories. */
static int rmrf_at(int dirfd, const char *name)
{
    struct stat st;
    if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
        return (errno == ENOENT) ? 0 : -1;
    if (S_ISDIR(st.st_mode)) {
        int dfd = openat(dirfd, name, O_RDONLY | O_NOFOLLOW | O_DIRECTORY);
        if (dfd < 0) {
            /* Lost the race to a symlink, or the entry flipped type;
             * unlink it as a non-dir and move on. A missing entry by
             * now is not an error (concurrent remover). */
            if (unlinkat(dirfd, name, 0) == 0) return 0;
            return (errno == ENOENT) ? 0 : -1;
        }
        if (rmrf_children(dfd) != 0) { close(dfd); return -1; }
        close(dfd);
        if (unlinkat(dirfd, name, AT_REMOVEDIR) != 0)
            return (errno == ENOENT) ? 0 : -1;
        return 0;
    }
    if (unlinkat(dirfd, name, 0) != 0)
        return (errno == ENOENT) ? 0 : -1;
    return 0;
}

static int rmrf(const char *path)
{
    return rmrf_at(AT_FDCWD, path);
}

#else
/* Windows: no *at / AT_SYMLINK_NOFOLLOW surface. Reparse-point handling
 * differs from POSIX symlinks and the POSIX-only test tier does not
 * exercise this path, so keep the simple recursive stat walk. */
static int rmrf(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;  /* nothing to remove */

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
#endif

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

/* ---- realpath ---- */

/* (realpath path) -- resolve to canonical absolute path, or nil. */
static mino_val *prim_realpath(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *path_val;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "realpath requires one argument");
    }
    path_val = args->as.cons.car;
    if (path_val == NULL || mino_type_of(path_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "realpath: argument must be a string");
    }
#if defined(_WIN32) || defined(_MSC_VER)
    /* Windows: _fullpath is the closest equivalent. */
    {
        char resolved[PATH_BUF_CAP];
        if (_fullpath(resolved, path_val->as.s.data, sizeof(resolved)) == NULL)
            return mino_nil(S);
        return mino_string(S, resolved);
    }
#else
    {
        char resolved[PATH_BUF_CAP];
        if (realpath(path_val->as.s.data, resolved) == NULL)
            return mino_nil(S);
        return mino_string(S, resolved);
    }
#endif
}

/* ---- which ---- */

/* (which cmd) -- search PATH for cmd, return absolute path or nil. */
static mino_val *prim_which(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *cmd_val;
    char *path_env, *dir, *saveptr;
    (void)env;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "which requires one argument");
    }
    cmd_val = args->as.cons.car;
    if (cmd_val == NULL || mino_type_of(cmd_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "which: argument must be a string");
    }

    path_env = getenv("PATH");
    if (path_env == NULL) return mino_nil(S);

    /* strtok_r modifies its input, so we need a copy. */
    {
        char *path_copy = (char *)malloc(strlen(path_env) + 1);
        if (path_copy == NULL) return mino_nil(S);
        strcpy(path_copy, path_env);

#ifdef _WIN32
        /* On Windows, also check common extensions. */
        {
            const char *exts[] = {"", ".exe", ".bat", ".cmd", NULL};
            int ei;
            for (dir = strtok_r(path_copy, ";", &saveptr);
                 dir != NULL;
                 dir = strtok_r(NULL, ";", &saveptr)) {
                for (ei = 0; exts[ei] != NULL; ei++) {
                    char candidate[PATH_BUF_CAP];
                    snprintf(candidate, sizeof(candidate), "%s/%s%s",
                             dir, cmd_val->as.s.data, exts[ei]);
                    if (access(candidate, 0) == 0) {
                        free(path_copy);
                        return mino_string(S, candidate);
                    }
                }
            }
        }
#else
        for (dir = strtok_r(path_copy, ":", &saveptr);
             dir != NULL;
             dir = strtok_r(NULL, ":", &saveptr)) {
            char candidate[PATH_BUF_CAP];
            struct stat st;
            snprintf(candidate, sizeof(candidate), "%s/%s",
                     dir, cmd_val->as.s.data);
            if (stat(candidate, &st) == 0 && S_ISREG(st.st_mode) &&
                access(candidate, X_OK) == 0) {
                free(path_copy);
                return mino_string(S, candidate);
            }
        }
#endif
        free(path_copy);
    }
    return mino_nil(S);
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
    {"realpath",     prim_realpath,
     "Resolves a path to its canonical absolute form, or nil."},
    {"which",        prim_which,
     "Searches PATH for an executable, returns its absolute path or nil."},
};

const size_t k_prims_fs_count =
    sizeof(k_prims_fs) / sizeof(k_prims_fs[0]);

void mino_install_fs(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_fs, k_prims_fs_count, "fs");
    /* The glob walker (path.c) rides the fs capability: it reads
     * directory contents, the same surface file-seq gates under
     * io (ADR 22). */
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_path_fs,
                                       k_prims_path_fs_count, "fs");
    mino_install_image_prims(S, env);
    S->caps_installed |= MINO_CAP_FS;
}
