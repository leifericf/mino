/*
 * fs.c -- filesystem primitives: file-exists?, directory?, mkdir-p,
 *         rm-rf, stat, file-size, chmod, symlink, copy, and copy-tree.
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
#endif
#if !defined(_WIN32)
#  include <sys/file.h>   /* flock(2), LOCK_EX/LOCK_SH/LOCK_NB */
#endif
#if defined(_MSC_VER)
#  include "win_dirent.h"
#endif
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* Read a boolean opts key from a (possibly nil/absent) opts map. Absent
 * or nil is `def`; a present non-boolean is a contract error. */
static int fs_opt_bool(mino_state *S, const mino_val *opts, const char *key,
                       int def, int *out)
{
    const mino_val *v;
    *out = def;
    if (opts == NULL || mino_type_of(opts) != MINO_MAP) return 0;
    v = map_get_val(opts, mino_keyword(S, key));
    if (v == NULL || mino_type_of(v) == MINO_NIL) return 0;
    if (mino_type_of(v) != MINO_BOOL) {
        char msg[128];
        snprintf(msg, sizeof(msg), "fs: opts key :%s must be a boolean", key);
        prim_throw_classified(S, "eval/contract", "MCT001", msg);
        return -1;
    }
    *out = mino_val_bool_get(v);
    return 0;
}

/* Modification time of a stat buffer in milliseconds. Mirrors
 * file-mtime's cross-platform sub-second handling. */
static long long stat_mtime_ms(const struct stat *st)
{
#if defined(__APPLE__)
    return (long long)st->st_mtimespec.tv_sec  * 1000LL
         + (long long)st->st_mtimespec.tv_nsec / 1000000LL;
#elif defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200809L && !defined(_WIN32)
    return (long long)st->st_mtim.tv_sec  * 1000LL
         + (long long)st->st_mtim.tv_nsec / 1000000LL;
#else
    return (long long)st->st_mtime * 1000LL;
#endif
}

/* Build the {:type :size :mode :mtime :symlink?} map. link_st is the
 * lstat of the path (used only for :symlink?); info_st is the buffer the
 * :type/:size/:mode/:mtime come from (the target's when following). */
static mino_val *stat_to_map(mino_state *S, const struct stat *info_st,
                             int is_symlink)
{
    mino_val *keys[5];
    mino_val *vals[5];
    const char *type;

    if (S_ISDIR(info_st->st_mode))       type = "dir";
    else if (S_ISREG(info_st->st_mode))  type = "file";
#if !defined(_WIN32)
    else if (S_ISLNK(info_st->st_mode))  type = "symlink";
#endif
    else                                 type = "other";

    keys[0] = mino_keyword(S, "type");
    vals[0] = mino_keyword(S, type);
    keys[1] = mino_keyword(S, "size");
    vals[1] = mino_int(S, (long long)info_st->st_size);
    keys[2] = mino_keyword(S, "mode");
    vals[2] = mino_int(S, (long long)(info_st->st_mode & 07777));
    keys[3] = mino_keyword(S, "mtime");
    vals[3] = mino_int(S, stat_mtime_ms(info_st));
    keys[4] = mino_keyword(S, "symlink?");
    vals[4] = is_symlink ? mino_true(S) : mino_false(S);
    return mino_map(S, keys, vals, 5);
}

/* (stat path) / (stat path {:follow-links? bool}) -- return a metadata
 * map, or nil when the path does not exist. Default lstat semantics
 * (:follow-links? false): the map's :type is :symlink for a symlink and
 * :symlink? reflects the path itself. With :follow-links? true the
 * type/size/mode/mtime come from the target while :symlink? still names
 * the path. */
static mino_val *prim_stat(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *path_val;
    mino_val *opts = NULL;
    struct stat link_st;
    struct stat info_st;
    int follow = 0;
    int is_symlink;
    (void)env;

    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "stat requires a path argument");
    }
    path_val = args->as.cons.car;
    if (mino_is_cons(args->as.cons.cdr)) {
        opts = args->as.cons.cdr->as.cons.car;
        if (mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                                         "stat takes a path and optional opts");
        }
    }
    if (path_val == NULL || mino_type_of(path_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "stat: path must be a string");
    }
    if (fs_opt_bool(S, opts, "follow-links?", 0, &follow) != 0)
        return NULL;

#if !defined(_WIN32)
    if (lstat(path_val->as.s.data, &link_st) != 0)
        return mino_nil(S);
    is_symlink = S_ISLNK(link_st.st_mode) ? 1 : 0;
    if (follow && is_symlink) {
        if (stat(path_val->as.s.data, &info_st) != 0)
            return mino_nil(S);
    } else {
        info_st = link_st;
    }
#else
    /* Windows has no lstat; reparse-point handling differs from POSIX
     * symlinks and the POSIX-only test tier does not exercise it, so a
     * plain stat serves both modes and :symlink? is always false. */
    (void)follow;
    if (stat(path_val->as.s.data, &link_st) != 0)
        return mino_nil(S);
    info_st = link_st;
    is_symlink = 0;
#endif
    return stat_to_map(S, &info_st, is_symlink);
}

/* (file-size path) -- byte count of the file at path, or nil when the
 * path does not exist (file-mtime's shape). */
static mino_val *prim_file_size(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *path_val;
    struct stat st;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "file-size requires one argument");
    }
    path_val = args->as.cons.car;
    if (path_val == NULL || mino_type_of(path_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "file-size: argument must be a string");
    }
    if (stat(path_val->as.s.data, &st) != 0)
        return mino_nil(S);
    return mino_int(S, (long long)st.st_size);
}

/* (chmod path mode) -- set permission bits (an int); returns nil. */
static mino_val *prim_chmod(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *path_val;
    mino_val *mode_val;
    long long mode;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "chmod requires a path and a mode");
    }
    path_val = args->as.cons.car;
    mode_val = args->as.cons.cdr->as.cons.car;
    if (path_val == NULL || mino_type_of(path_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "chmod: path must be a string");
    }
    if (!as_long(mode_val, &mode) || mode < 0 || mode > 07777) {
        return prim_throw_classified(S, "eval/contract", "MCT001",
                                     "chmod: mode must be an int in 0..07777");
    }
#if defined(_MSC_VER)
    /* MSVC's CRT spells it _chmod and honours only the write bit. */
    if (_chmod(path_val->as.s.data, (int)mode) != 0) {
#else
    if (chmod(path_val->as.s.data, (mode_t)mode) != 0) {
#endif
        char msg[300];
        snprintf(msg, sizeof(msg), "chmod: cannot set mode on: %.200s",
                 path_val->as.s.data);
        return prim_throw_classified(S, "io", "MIO001", msg);
    }
    return mino_nil(S);
}

#if !defined(_WIN32)
/* (symlink target link-path) -- create a symlink at link-path pointing at
 * target; returns nil. */
static mino_val *prim_symlink(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *target_val;
    mino_val *link_val;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "symlink requires a target and a link");
    }
    target_val = args->as.cons.car;
    link_val = args->as.cons.cdr->as.cons.car;
    if (target_val == NULL || mino_type_of(target_val) != MINO_STRING ||
        link_val == NULL || mino_type_of(link_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "symlink: target and link must be strings");
    }
    if (symlink(target_val->as.s.data, link_val->as.s.data) != 0) {
        char msg[300];
        snprintf(msg, sizeof(msg), "symlink: cannot create link: %.180s",
                 link_val->as.s.data);
        return prim_throw_classified(S, "io", "MIO001", msg);
    }
    return mino_nil(S);
}

/* (read-symlink path) -- return the target string a symlink points at. */
static mino_val *prim_read_symlink(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *path_val;
    char buf[PATH_BUF_CAP];
    ssize_t n;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "read-symlink requires one argument");
    }
    path_val = args->as.cons.car;
    if (path_val == NULL || mino_type_of(path_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "read-symlink: argument must be a string");
    }
    /* readlink does not NUL-terminate; a full buffer means the target
     * was too long to represent, which is an error rather than a
     * silently truncated path. */
    n = readlink(path_val->as.s.data, buf, sizeof(buf));
    if (n < 0) {
        char msg[300];
        snprintf(msg, sizeof(msg), "read-symlink: cannot read link: %.180s",
                 path_val->as.s.data);
        return prim_throw_classified(S, "io", "MIO001", msg);
    }
    if ((size_t)n >= sizeof(buf)) {
        return prim_throw_classified(S, "io", "MIO001",
                                     "read-symlink: link target too long");
    }
    return mino_string_n(S, buf, (size_t)n);
}
#endif /* !_WIN32 */

/* ---- copy / copy-tree ---- */

#if !defined(_WIN32)
/* Copy the bytes of an already-open source descriptor into a freshly
 * created destination, preserving the source mode. src_fd is positioned
 * at offset 0 and stat'd by the caller. Returns 0, or -1 with errno set.
 * excl selects O_EXCL (refuse an existing dst) vs O_TRUNC (replace). */
static int copy_fd_to_path(int src_fd, const struct stat *src_st,
                           const char *dst, int excl)
{
    int   flags = O_WRONLY | O_CREAT | O_NOFOLLOW | (excl ? O_EXCL : O_TRUNC);
    int   dst_fd = open(dst, flags, src_st->st_mode & 07777);
    char  buf[65536];
    if (dst_fd < 0) return -1;
    for (;;) {
        ssize_t r = read(src_fd, buf, sizeof(buf));
        ssize_t off = 0;
        if (r < 0) {
            if (errno == EINTR) continue;
            close(dst_fd);
            return -1;
        }
        if (r == 0) break;
        while (off < r) {
            ssize_t w = write(dst_fd, buf + off, (size_t)(r - off));
            if (w < 0) {
                if (errno == EINTR) continue;
                close(dst_fd);
                return -1;
            }
            off += w;
        }
    }
    /* Mode is set from the open() creation mask above, but a restrictive
     * umask can clear bits; fchmod pins the source mode exactly. */
    if (fchmod(dst_fd, src_st->st_mode & 07777) != 0) {
        close(dst_fd);
        return -1;
    }
    return close(dst_fd);
}

/* Copy the regular file at src to dst. Refuses a symlink source: the
 * source is opened O_NOFOLLOW and its type checked, so a symlink planted
 * at src cannot redirect the read into an unintended target (CWE-59). */
static int copy_file(const char *src, const char *dst, int replace,
                     int *was_symlink, int *dst_existed)
{
    struct stat src_st;
    int         src_fd;
    int         rc;
    *was_symlink = 0;
    *dst_existed = 0;
    src_fd = open(src, O_RDONLY | O_NOFOLLOW);
    if (src_fd < 0) {
        /* ELOOP is the O_NOFOLLOW refusal of a symlink source. */
        if (errno == ELOOP) *was_symlink = 1;
        return -1;
    }
    if (fstat(src_fd, &src_st) != 0) { close(src_fd); return -1; }
    if (!S_ISREG(src_st.st_mode)) {
        close(src_fd);
        errno = EINVAL;
        return -1;
    }
    rc = copy_fd_to_path(src_fd, &src_st, dst, !replace);
    if (rc != 0 && errno == EEXIST) *dst_existed = 1;
    close(src_fd);
    return rc;
}

/* Recursively copy the directory tree rooted at src_fd/name (a real
 * subdirectory) into dst. Mirrors rmrf_at: each level is pinned by its
 * open descriptor and every child is stat'd with AT_SYMLINK_NOFOLLOW, so
 * a symlink swapped in mid-walk cannot redirect the descent (CWE-59). A
 * symlink entry is recreated as a symlink to the same target, never
 * dereferenced into a file copy. */
static int copytree_dir(int src_dfd, const char *dst);

static int copytree_entry(int src_dfd, const char *name, const char *dst_child)
{
    struct stat st;
    if (fstatat(src_dfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
        return -1;
    if (S_ISLNK(st.st_mode)) {
        char    target[PATH_BUF_CAP];
        ssize_t n = readlinkat(src_dfd, name, target, sizeof(target));
        if (n < 0 || (size_t)n >= sizeof(target)) return -1;
        target[n] = '\0';
        return symlink(target, dst_child);
    }
    if (S_ISDIR(st.st_mode)) {
        int cfd;
        if (mkdir(dst_child, st.st_mode & 07777) != 0 && errno != EEXIST)
            return -1;
        cfd = openat(src_dfd, name, O_RDONLY | O_NOFOLLOW | O_DIRECTORY);
        if (cfd < 0) return -1;
        if (copytree_dir(cfd, dst_child) != 0) { close(cfd); return -1; }
        close(cfd);
        return chmod(dst_child, st.st_mode & 07777);
    }
    if (S_ISREG(st.st_mode)) {
        int src_fd = openat(src_dfd, name, O_RDONLY | O_NOFOLLOW);
        int rc;
        if (src_fd < 0) return -1;
        rc = copy_fd_to_path(src_fd, &st, dst_child, 0);
        close(src_fd);
        return rc;
    }
    /* Sockets, fifos, devices: skip rather than fail the whole tree. */
    return 0;
}

static int copytree_dir(int src_dfd, const char *dst)
{
    DIR           *d;
    struct dirent *ent;
    char         **names = NULL;
    size_t          n = 0, cap = 0, i;
    int             rc = 0;
    int dfd2 = dup(src_dfd);
    if (dfd2 < 0) return -1;
    d = fdopendir(src_dfd);
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
    closedir(d);
    for (i = 0; i < n && rc == 0; i++) {
        char child[PATH_BUF_CAP];
        int  wr = snprintf(child, sizeof(child), "%s/%s", dst, names[i]);
        if (wr < 0 || (size_t)wr >= sizeof(child)) { rc = -1; break; }
        rc = copytree_entry(dfd2, names[i], child);
    }
    for (i = 0; i < n; i++) free(names[i]);
    free(names);
    close(dfd2);
    return rc;
}
#endif /* !_WIN32 */

/* (copy src dst) / (copy src dst {:replace true}) -- copy a regular file,
 * preserving its mode. A symlink source is refused (O_NOFOLLOW); an
 * existing destination without :replace is an error. */
static mino_val *prim_copy(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *src_val, *dst_val, *opts = NULL;
    int replace = 0;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "copy requires a source and a destination");
    }
    src_val = args->as.cons.car;
    dst_val = args->as.cons.cdr->as.cons.car;
    if (mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        opts = args->as.cons.cdr->as.cons.cdr->as.cons.car;
        if (mino_is_cons(args->as.cons.cdr->as.cons.cdr->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                                         "copy takes src, dst, and optional opts");
        }
    }
    if (src_val == NULL || mino_type_of(src_val) != MINO_STRING ||
        dst_val == NULL || mino_type_of(dst_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "copy: src and dst must be strings");
    }
    if (fs_opt_bool(S, opts, "replace", 0, &replace) != 0)
        return NULL;
#if defined(_WIN32)
    (void)replace;
    return prim_throw_classified(S, "io", "MIO001",
                                 "copy: not supported on this platform");
#else
    {
        int was_symlink = 0, dst_existed = 0;
        if (copy_file(src_val->as.s.data, dst_val->as.s.data, replace,
                      &was_symlink, &dst_existed) != 0) {
            char msg[300];
            if (was_symlink) {
                return prim_throw_classified(S, "io", "MIO001",
                    "copy: refusing to copy a symlink source; use copy-tree");
            }
            if (dst_existed) {
                snprintf(msg, sizeof(msg),
                         "copy: destination exists (pass {:replace true}): %.160s",
                         dst_val->as.s.data);
                return prim_throw_classified(S, "io", "MIO001", msg);
            }
            snprintf(msg, sizeof(msg), "copy: cannot copy %.120s to %.120s",
                     src_val->as.s.data, dst_val->as.s.data);
            return prim_throw_classified(S, "io", "MIO001", msg);
        }
        return mino_nil(S);
    }
#endif
}

/* (copy-tree src dst) -- recursively copy a directory tree, recreating
 * symlink entries as symlinks (never dereferencing them). */
static mino_val *prim_copy_tree(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *src_val, *dst_val;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "copy-tree requires a source and a destination");
    }
    src_val = args->as.cons.car;
    dst_val = args->as.cons.cdr->as.cons.car;
    if (src_val == NULL || mino_type_of(src_val) != MINO_STRING ||
        dst_val == NULL || mino_type_of(dst_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "copy-tree: src and dst must be strings");
    }
#if defined(_WIN32)
    return prim_throw_classified(S, "io", "MIO001",
                                 "copy-tree: not supported on this platform");
#else
    {
        struct stat src_st;
        int src_fd;
        if (lstat(src_val->as.s.data, &src_st) != 0 ||
            !S_ISDIR(src_st.st_mode)) {
            return prim_throw_classified(S, "io", "MIO001",
                                         "copy-tree: source is not a directory");
        }
        if (mkdir(dst_val->as.s.data, src_st.st_mode & 07777) != 0 &&
            errno != EEXIST) {
            return prim_throw_classified(S, "io", "MIO001",
                                         "copy-tree: cannot create destination");
        }
        src_fd = open(src_val->as.s.data,
                      O_RDONLY | O_NOFOLLOW | O_DIRECTORY);
        if (src_fd < 0) {
            return prim_throw_classified(S, "io", "MIO001",
                                         "copy-tree: cannot open source");
        }
        if (copytree_dir(src_fd, dst_val->as.s.data) != 0) {
            close(src_fd);
            return prim_throw_classified(S, "io", "MIO001",
                                         "copy-tree: copy failed");
        }
        close(src_fd);
        return mino_nil(S);
    }
#endif
}

/* ---- mkdtemp / mkstemp ---- */

#if !defined(_WIN32)
/* Build "<tmpdir>/<prefix>XXXXXX" into out. tmpdir comes from $TMPDIR,
 * falling back to /tmp. prefix may be NULL. Returns 0, or -1 when the
 * assembled template would overflow the buffer. */
static int temp_template(char *out, size_t cap, const char *prefix)
{
    const char *tmp = getenv("TMPDIR");
    size_t      base_len;
    int         wr;
    if (tmp == NULL || tmp[0] == '\0') tmp = "/tmp";
    base_len = strlen(tmp);
    /* Drop a trailing slash so the join never doubles it. */
    while (base_len > 1 && tmp[base_len - 1] == '/') base_len--;
    wr = snprintf(out, cap, "%.*s/%sXXXXXX",
                  (int)base_len, tmp, prefix ? prefix : "mino");
    if (wr < 0 || (size_t)wr >= cap) return -1;
    return 0;
}

/* Read an optional single string prefix argument. Returns 0 with *prefix
 * pointing at the string data (or NULL when absent); -1 on a type error
 * (the throw is already fired). */
static int temp_prefix_arg(mino_state *S, mino_val *args, const char **prefix)
{
    *prefix = NULL;
    if (!mino_is_cons(args)) return 0;
    if (mino_is_cons(args->as.cons.cdr)) {
        prim_throw_classified(S, "eval/arity", "MAR001",
                              "temp: takes at most one prefix argument");
        return -1;
    }
    {
        mino_val *p = args->as.cons.car;
        if (p == NULL || mino_type_of(p) != MINO_STRING) {
            prim_throw_classified(S, "eval/type", "MTY001",
                                  "temp: prefix must be a string");
            return -1;
        }
        *prefix = p->as.s.data;
    }
    return 0;
}
#endif

/* (mkdtemp) / (mkdtemp prefix) -- create a private (0700) directory with
 * a unique name under the system temp dir; return its path. */
static mino_val *prim_mkdtemp(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
#if defined(_WIN32)
    (void)args;
    return prim_throw_classified(S, "io", "MIO001",
                                 "mkdtemp: not supported on this platform");
#else
    {
        char        tmpl[PATH_BUF_CAP];
        const char *prefix;
        if (temp_prefix_arg(S, args, &prefix) != 0) return NULL;
        if (temp_template(tmpl, sizeof(tmpl), prefix) != 0) {
            return prim_throw_classified(S, "io", "MIO001",
                                         "mkdtemp: temp path too long");
        }
        if (mkdtemp(tmpl) == NULL) {
            return prim_throw_classified(S, "io", "MIO001",
                                         "mkdtemp: cannot create temp directory");
        }
        /* mkdtemp already creates the directory 0700; make it explicit so
         * the mode is independent of any implementation drift. */
        if (chmod(tmpl, 0700) != 0) {
            return prim_throw_classified(S, "io", "MIO001",
                                         "mkdtemp: cannot set private mode");
        }
        return mino_string(S, tmpl);
    }
#endif
}

/* (mkstemp) / (mkstemp prefix) -- create a private (0600) empty file with
 * a unique name under the system temp dir; return its path. */
static mino_val *prim_mkstemp(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
#if defined(_WIN32)
    (void)args;
    return prim_throw_classified(S, "io", "MIO001",
                                 "mkstemp: not supported on this platform");
#else
    {
        char        tmpl[PATH_BUF_CAP];
        const char *prefix;
        int         fd;
        if (temp_prefix_arg(S, args, &prefix) != 0) return NULL;
        if (temp_template(tmpl, sizeof(tmpl), prefix) != 0) {
            return prim_throw_classified(S, "io", "MIO001",
                                         "mkstemp: temp path too long");
        }
        fd = mkstemp(tmpl);
        if (fd < 0) {
            return prim_throw_classified(S, "io", "MIO001",
                                         "mkstemp: cannot create temp file");
        }
        /* POSIX mkstemp creates 0600 already; pin it explicitly. */
        if (fchmod(fd, 0600) != 0) {
            close(fd);
            return prim_throw_classified(S, "io", "MIO001",
                                         "mkstemp: cannot set private mode");
        }
        close(fd);
        return mino_string(S, tmpl);
    }
#endif
}

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

/* ---- flock: advisory file locks ---- */

#define FLOCK_TAG "mino/file-lock"

#if !defined(_WIN32)
typedef struct {
    int fd;
    int closed;
} fs_lock_t;

static void fs_lock_finalize(void *ptr, const char *tag)
{
    fs_lock_t *lk = (fs_lock_t *)ptr;
    (void)tag;
    if (lk == NULL) return;
    /* Closing the descriptor releases the advisory lock. */
    if (!lk->closed) close(lk->fd);
    free(lk);
}
#endif

/* (flock path) / (flock path {:shared bool :block bool}) -- acquire an
 * advisory lock on a lockfile, returning an opaque handle. Exclusive and
 * blocking by default. With {:block false} a lock held elsewhere yields
 * nil (contention is an expected outcome, not an error), rather than
 * throwing. The single-instance cron guard: acquire a non-blocking lock
 * at startup and exit quietly when it returns nil. */
static mino_val *prim_flock(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *path_val, *opts = NULL;
    int shared = 0, block = 1;
    (void)env;

    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "flock requires a lockfile path");
    }
    path_val = args->as.cons.car;
    if (mino_is_cons(args->as.cons.cdr)) {
        opts = args->as.cons.cdr->as.cons.car;
        if (mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                                         "flock takes a path and optional opts");
        }
    }
    if (path_val == NULL || mino_type_of(path_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "flock: path must be a string");
    }
    if (fs_opt_bool(S, opts, "shared", 0, &shared) != 0) return NULL;
    if (fs_opt_bool(S, opts, "block", 1, &block) != 0) return NULL;
#if defined(_WIN32)
    (void)shared; (void)block;
    return prim_throw_classified(S, "io", "MIO001",
                                 "flock: not supported on this platform");
#else
    {
        mino_val  *hv;
        fs_lock_t *lk;
        int        fd;
        int        op;
        int        rc;

        fd = open(path_val->as.s.data, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0) {
            return prim_throw_classified(S, "io", "MIO001",
                                         "flock: cannot open lockfile");
        }
        /* Pre-flight the handle: its allocation can throw for OOM, which
         * from here would strand the open descriptor. */
        hv = mino_handle_ex(S, NULL, FLOCK_TAG, fs_lock_finalize);
        gc_pin(hv);

        op = shared ? LOCK_SH : LOCK_EX;
        if (!block) op |= LOCK_NB;
        {
            /* A blocking flock parks the calling thread; release the
             * runtime lock so a collection can run concurrently, matching
             * the net.c GC-safety pattern. */
            int depth = mino_yield_lock(S);
            do {
                rc = flock(fd, op);
            } while (rc < 0 && errno == EINTR);
            mino_resume_lock(S, depth);
        }
        if (rc < 0) {
            gc_unpin(1);
            close(fd);
            if (!block && (errno == EWOULDBLOCK || errno == EAGAIN))
                return mino_nil(S);
            return prim_throw_classified(S, "io", "MIO001",
                                         "flock: cannot acquire lock");
        }
        lk = (fs_lock_t *)malloc(sizeof(*lk));
        if (lk == NULL) {
            gc_unpin(1);
            close(fd);
            return prim_throw_classified(S, "internal", "MIN001",
                                         "flock: out of memory");
        }
        lk->fd = fd;
        lk->closed = 0;
        hv->as.handle.ptr = lk;
        gc_unpin(1);
        return hv;
    }
#endif
}

/* (funlock handle) -- release a lock acquired by flock. Idempotent.
 * Returns nil. */
static mino_val *prim_funlock(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *h;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "funlock requires one argument");
    }
    h = args->as.cons.car;
    if (h == NULL || mino_type_of(h) != MINO_HANDLE ||
        h->as.handle.tag == NULL ||
        strcmp(h->as.handle.tag, FLOCK_TAG) != 0) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "funlock: argument must be a lock handle");
    }
#if !defined(_WIN32)
    {
        fs_lock_t *lk = (fs_lock_t *)h->as.handle.ptr;
        if (lk != NULL && !lk->closed) {
            close(lk->fd);
            lk->closed = 1;
        }
    }
#endif
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
    {"stat",         prim_stat,
     "Returns a {:type :size :mode :mtime :symlink?} map, or nil when "
     "the path is missing. Default lstat semantics; pass {:follow-links? "
     "true} to report the symlink target."},
    {"file-size",    prim_file_size,
     "Returns the file size in bytes, or nil when the path is missing."},
    {"chmod",        prim_chmod,
     "Sets the permission bits (an int) on a path. Returns nil."},
    {"copy",         prim_copy,
     "Copies a regular file, preserving its mode. Refuses a symlink "
     "source; pass {:replace true} to overwrite an existing destination."},
    {"copy-tree",    prim_copy_tree,
     "Recursively copies a directory tree, keeping symlink entries as "
     "symlinks rather than following them."},
    {"mkdtemp",      prim_mkdtemp,
     "Creates a uniquely-named private (0700) directory under the system "
     "temp dir and returns its path. Optional string prefix."},
    {"mkstemp",      prim_mkstemp,
     "Creates a uniquely-named private (0600) empty file under the system "
     "temp dir and returns its path. Optional string prefix."},
    {"flock",        prim_flock,
     "Acquires an advisory lock on a lockfile and returns a handle. "
     "Exclusive and blocking by default; {:shared true} for a shared "
     "lock, {:block false} yields nil when the lock is held elsewhere."},
    {"funlock",      prim_funlock,
     "Releases a lock acquired by flock. Idempotent. Returns nil."},
#if !defined(_WIN32)
    {"symlink",      prim_symlink,
     "Creates a symlink at the link path pointing at the target. Returns nil."},
    {"read-symlink", prim_read_symlink,
     "Returns the target path a symlink points at."},
#endif
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
