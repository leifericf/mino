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

/* ---- sha256 ---- */

/* Compact SHA-256 implementation (FIPS 180-4). */

struct sha256_ctx {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  data[64];
    uint32_t datalen;
};

static const uint32_t sha256_k[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,
    0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
    0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,
    0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,
    0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
    0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,
    0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,
    0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
    0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

#define SHA256_ROTR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_transform(struct sha256_ctx *ctx, const uint8_t *p)
{
    uint32_t a,b,c,d,e,f,g,h,t1,t2,m[64];
    int i;
    for (i = 0; i < 16; i++)
        m[i] = ((uint32_t)p[i*4]<<24)|((uint32_t)p[i*4+1]<<16)|
               ((uint32_t)p[i*4+2]<<8)|((uint32_t)p[i*4+3]);
    for (; i < 64; i++) {
        uint32_t s0 = SHA256_ROTR(m[i-15],7)^SHA256_ROTR(m[i-15],18)^(m[i-15]>>3);
        uint32_t s1 = SHA256_ROTR(m[i-2],17)^SHA256_ROTR(m[i-2],19)^(m[i-2]>>10);
        m[i] = m[i-16]+s0+m[i-7]+s1;
    }
    a=ctx->state[0]; b=ctx->state[1]; c=ctx->state[2]; d=ctx->state[3];
    e=ctx->state[4]; f=ctx->state[5]; g=ctx->state[6]; h=ctx->state[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1=SHA256_ROTR(e,6)^SHA256_ROTR(e,11)^SHA256_ROTR(e,25);
        uint32_t ch=(e&f)^(~e&g);
        uint32_t S0=SHA256_ROTR(a,2)^SHA256_ROTR(a,13)^SHA256_ROTR(a,22);
        uint32_t maj=(a&b)^(a&c)^(b&c);
        t1=h+S1+ch+sha256_k[i]+m[i];
        t2=S0+maj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d;
    ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}

static void sha256_init(struct sha256_ctx *ctx)
{
    ctx->bitlen=0; ctx->datalen=0;
    ctx->state[0]=0x6a09e667u; ctx->state[1]=0xbb67ae85u;
    ctx->state[2]=0x3c6ef372u; ctx->state[3]=0xa54ff53au;
    ctx->state[4]=0x510e527fu; ctx->state[5]=0x9b05688cu;
    ctx->state[6]=0x1f83d9abu; ctx->state[7]=0x5be0cd19u;
}

static void sha256_update(struct sha256_ctx *ctx, const uint8_t *data, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void sha256_final(struct sha256_ctx *ctx, uint8_t *hash)
{
    uint32_t i = ctx->datalen;
    ctx->data[i++] = 0x80;
    if (i > 56) {
        while (i < 64) ctx->data[i++] = 0;
        sha256_transform(ctx, ctx->data);
        i = 0;
    }
    while (i < 56) ctx->data[i++] = 0;
    ctx->bitlen += (uint64_t)ctx->datalen * 8;
    {
        int j;
        for (j = 7; j >= 0; j--)
            ctx->data[56 + (7-j)] = (uint8_t)(ctx->bitlen >> (j*8));
    }
    sha256_transform(ctx, ctx->data);
    {
        int j, k;
        for (j = 0; j < 8; j++)
            for (k = 0; k < 4; k++)
                hash[j*4+k] = (uint8_t)((ctx->state[j] >> (24 - k*8)) & 0xff);
    }
}

/* (sha256 string) -- return the hex digest of the SHA-256 hash. */
static mino_val *prim_sha256(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *val;
    struct sha256_ctx ctx;
    uint8_t hash[32];
    char hex[65];
    int i;
    (void)env;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "sha256 requires one argument");
    }
    val = args->as.cons.car;
    if (val == NULL || mino_type_of(val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "sha256: argument must be a string");
    }
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)val->as.s.data, val->as.s.len);
    sha256_final(&ctx, hash);
    for (i = 0; i < 32; i++)
        snprintf(hex + i*2, 3, "%02x", hash[i]);
    hex[64] = '\0';
    return mino_string(S, hex);
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
    {"sha256",       prim_sha256,
     "Returns the hex-encoded SHA-256 digest of a string."},
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
    mino_install_image_prims(S, env);
    S->caps_installed |= MINO_CAP_FS;
}
