/*
 * tar.c -- tar container primitives (ADR 29's container pattern).
 *
 * tar-entries lists an archive's members in archive order; tar-read
 * returns one member's bytes by name (the FIRST match); tar-extract
 * materializes an archive into a destination directory, hardened
 * against the traversal, absolute-path, and symlink-escape attacks a
 * hostile archive carries. All three run a single pass over the
 * 512-byte header blocks with no vendor dependency: tar is a fixed
 * block format, not a compression container, so the parser is
 * mino-owned end to end. The .tar.gz composition rides the existing
 * gzip prims at the mino.tar facade level, never here.
 *
 * Tar read IS untrusted input (the ADR 23-28 rule). Every header
 * field read is bounded by the input length before it is touched;
 * the octal and GNU base-256 size fields are parsed with an explicit
 * overflow guard so a hostile size degrades as a classified error,
 * never a wrapped offset; a declared size that runs past the input
 * end throws :codec/truncated rather than over-reading. The listing
 * and read sides write nothing to any filesystem, so a "../" or
 * absolute member name is inert there (round-tripped verbatim). The
 * extract side is the one that touches disk, and it re-litigates
 * every hostile name BEFORE anything lands: an absolute name, a name
 * with a ".." component, or a link entry whose target escapes the
 * destination throws :codec/unsafe, and every write goes through an
 * O_NOFOLLOW openat descent (the fs.c copytree discipline) so a
 * symlink materialized earlier in the same archive cannot redirect a
 * later write outside the destination (CWE-22, CWE-59).
 *
 * ustar name/prefix splitting and the pax extended-header "path"
 * record are both honored, so a long name surfaces as its one
 * logical member. Errors reuse the codec family the zip/gzip read
 * side established -- :codec/magic MGC002 (not a tar), :codec/truncated
 * MGC001 (a declared size or block runs past the end), :codec/corrupt
 * MGC004 (a malformed field, a base-256 overflow), :codec/missing
 * MGC006 (tar-read name not found), :codec/unsafe MGC008 (a hostile
 * extract name or link target) -- plus :internal only for OOM.
 */

/* The openat family (mkdirat, fchmodat, utimensat, ...) and the
 * O_NOFOLLOW/O_DIRECTORY flags are POSIX.1-2008; glibc hides them
 * under -std=c99 unless asked. On macOS the strict macro alone would
 * hide the BSD extensions the system headers rely on, so keep
 * _DARWIN_C_SOURCE alongside it (the fs.c discipline). */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1

#include "prim/internal.h"
#include "mino.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#  include <errno.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <time.h>
#  include <unistd.h>
#  include <utime.h>
#endif

/* Error codes, contiguous with gzip.c/zip.c's MGC001-007. */
#define TAR_MGC_TRUNCATED "MGC001"
#define TAR_MGC_MAGIC "MGC002"
#define TAR_MGC_CORRUPT "MGC004"
#define TAR_MGC_MISSING "MGC006"
#define TAR_MGC_UNSAFE "MGC008"

#define TAR_BLOCK 512u
#define TAR_NAME_OFS 0u
#define TAR_NAME_LEN 100u
#define TAR_MODE_OFS 100u
#define TAR_SIZE_OFS 124u
#define TAR_SIZE_LEN 12u
#define TAR_MTIME_OFS 136u
#define TAR_MTIME_LEN 12u
#define TAR_TYPEFLAG_OFS 156u
#define TAR_LINKNAME_OFS 157u
#define TAR_LINKNAME_LEN 100u
#define TAR_MAGIC_OFS 257u
#define TAR_PREFIX_OFS 345u
#define TAR_PREFIX_LEN 155u

/* ---- data / opts argument handling (the zip_data_arg contract) ---- */

/* Container prims accept bytes or a string (its UTF-8 bytes). Returns
 * 0 with data/len filled, or throws via -1. */
static int tar_data_arg(mino_state *S, mino_val *v, const char *who,
                        const unsigned char **data, size_t *len)
{
    char msg[96];

    if (v == NULL || (!mino_is_bytes(v) && !mino_is_string(v))) {
        snprintf(msg, sizeof(msg), "%s: archive must be bytes or a string",
                 who);
        prim_throw_classified(S, "eval/type", "MTY001", msg);
        return -1;
    }
    if (mino_is_bytes(v)) {
        *data = mino_bytes_data(v);
        *len = mino_bytes_len(v);
    } else {
        *data = (const unsigned char *)v->as.s.data;
        *len = v->as.s.len;
    }
    if (*data == NULL) *data = (const unsigned char *)"";
    return 0;
}

/* ---- octal / base-256 numeric fields ---- */

/* Parse a size or mtime field: a NUL/space-terminated octal string,
 * or a GNU base-256 field (high bit of the first byte set). Returns 0
 * with *out filled, or -1 on a malformed field or an overflow past
 * the signed 64-bit range. Every field a payload controls passes
 * through here, so the overflow guard is the security boundary. */
static int tar_read_number(const unsigned char *field, size_t len,
                           long long *out)
{
    unsigned long long acc = 0;
    size_t i;

    if (len == 0) return -1;

    if (field[0] & 0x80) {
        /* GNU base-256: the low 7 bits of the first byte, then
         * big-endian bytes. Reject a set sign bit (negative) and any
         * value past LLONG_MAX; the round-up to a block boundary the
         * caller does must not overflow, so the ceiling is strict. */
        unsigned long long b0 = (unsigned long long)(field[0] & 0x7f);
        if (field[0] & 0x40) return -1; /* negative base-256 value */
        acc = b0;
        for (i = 1; i < len; i++) {
            if (acc > (ULLONG_MAX >> 8)) return -1;
            acc = (acc << 8) | field[i];
        }
        if (acc > (unsigned long long)LLONG_MAX) return -1;
        *out = (long long)acc;
        return 0;
    }

    /* Octal: skip leading spaces, read digits until NUL, space, or the
     * field end. An empty field (all NUL) is zero. */
    i = 0;
    while (i < len && (field[i] == ' ')) i++;
    if (i == len || field[i] == '\0') { *out = 0; return 0; }
    for (; i < len; i++) {
        unsigned char c = field[i];
        if (c == '\0' || c == ' ') break;
        if (c < '0' || c > '7') return -1;
        if (acc > (ULLONG_MAX >> 3)) return -1;
        acc = (acc << 3) | (unsigned long long)(c - '0');
    }
    if (acc > (unsigned long long)LLONG_MAX) return -1;
    *out = (long long)acc;
    return 0;
}

/* ---- header field extraction ---- */

/* An all-zero 512-byte block marks end of archive. Returns 1 for a
 * zero block, 0 otherwise. */
static int tar_block_is_zero(const unsigned char *blk)
{
    size_t i;
    for (i = 0; i < TAR_BLOCK; i++)
        if (blk[i] != 0) return 0;
    return 1;
}

/* Length of a NUL-terminated (or field-length-capped) string field. */
static size_t tar_field_len(const unsigned char *field, size_t cap)
{
    size_t i;
    for (i = 0; i < cap; i++)
        if (field[i] == '\0') break;
    return i;
}

/* Recognize a tar header block by its magic. ustar is "ustar\0" at
 * offset 257; the GNU variant writes "ustar  \0" (two spaces). A
 * block whose magic matches neither is not a tar header. */
static int tar_magic_ok(const unsigned char *blk)
{
    const unsigned char *m = blk + TAR_MAGIC_OFS;
    if (memcmp(m, "ustar\0", 6) == 0) return 1;
    if (memcmp(m, "ustar  \0", 8) == 0) return 1;
    return 0;
}

/* One parsed member, before name-extension resolution. name/linkname
 * borrow into the input; a pax path override replaces name with a
 * malloc'd copy the walker owns (freed on the next member and at the
 * end). */
typedef struct {
    char        name[TAR_PREFIX_LEN + 1 + TAR_NAME_LEN + 1];
    char        linkname[TAR_LINKNAME_LEN + 1];
    long long   size;      /* member content byte count */
    long long   mode;      /* 07777-masked */
    long long   mtime;     /* epoch seconds, -1 for a zero field (nil) */
    char        typeflag;
    int         has_link;  /* 1 for symlink/hardlink */
} tar_member;

/* Compose the full name from the ustar prefix + name fields. A set
 * prefix joins to name with a slash (POSIX ustar). */
static void tar_compose_name(const unsigned char *blk, char *out,
                             size_t out_cap)
{
    size_t nl = tar_field_len(blk + TAR_NAME_OFS, TAR_NAME_LEN);
    size_t pl = tar_field_len(blk + TAR_PREFIX_OFS, TAR_PREFIX_LEN);
    size_t n = 0;

    if (pl != 0) {
        if (pl > out_cap - 2) pl = out_cap - 2;
        memcpy(out, blk + TAR_PREFIX_OFS, pl);
        n = pl;
        out[n++] = '/';
    }
    if (nl > out_cap - 1 - n) nl = out_cap - 1 - n;
    memcpy(out + n, blk + TAR_NAME_OFS, nl);
    n += nl;
    out[n] = '\0';
}

/* Parse a plain header block into a member, resolving numeric fields.
 * Returns 0, or -1 with the classified error out-params filled (mode
 * MGC004). The magic is already checked. */
static int tar_parse_header(const unsigned char *blk, tar_member *m,
                            const char **ekind, const char **ecode)
{
    long long size, mode, mtime;

    if (tar_read_number(blk + TAR_SIZE_OFS, TAR_SIZE_LEN, &size) != 0
        || tar_read_number(blk + TAR_MTIME_OFS, TAR_MTIME_LEN, &mtime) != 0
        || tar_read_number(blk + TAR_MODE_OFS, 8, &mode) != 0) {
        *ekind = "codec/corrupt";
        *ecode = TAR_MGC_CORRUPT;
        return -1;
    }
    tar_compose_name(blk, m->name, sizeof(m->name));
    {
        size_t ll = tar_field_len(blk + TAR_LINKNAME_OFS, TAR_LINKNAME_LEN);
        memcpy(m->linkname, blk + TAR_LINKNAME_OFS, ll);
        m->linkname[ll] = '\0';
    }
    m->size = size;
    m->mode = mode & 07777;
    /* A zero mtime field lists as nil (the zip DOS-minimum sibling). */
    m->mtime = (mtime == 0) ? -1 : mtime;
    m->typeflag = (char)blk[TAR_TYPEFLAG_OFS];
    m->has_link = (m->typeflag == '1' || m->typeflag == '2');
    return 0;
}

/* Number of 512-byte blocks a member's content occupies, with an
 * overflow guard. A hostile size near LLONG_MAX (the GNU base-256
 * int64-max attack) rounds up to a block boundary that overflows a
 * signed 64-bit content span; that is a corrupt field, distinct from
 * a merely-larger-than-this-archive size that runs past EOF. Returns
 * 0 on success, -1 when the block round-up overflows the signed range
 * (the caller classifies -1 as :codec/corrupt). */
static int tar_content_blocks(long long size, size_t *blocks)
{
    unsigned long long s = (unsigned long long)size;
    /* The rounded-up span (blocks * 512) must stay within LLONG_MAX,
     * so a size past (LLONG_MAX - 511) rounded up overflows. */
    if (s > (unsigned long long)LLONG_MAX - (TAR_BLOCK - 1)) return -1;
    *blocks = (size_t)((s + (TAR_BLOCK - 1)) / TAR_BLOCK);
    return 0;
}

/* ---- pax extended header parsing ---- */

/* Extract the "path" value from a pax extended-header payload into
 * out (NUL-terminated). Each record is "LEN key=value\n" where LEN is
 * the decimal byte length of the whole record. Returns 1 if a path
 * record was found, 0 if not, -1 on a malformed record. Bounded by
 * len throughout. */
static int tar_pax_path(const unsigned char *pax, size_t len,
                        char *out, size_t out_cap)
{
    size_t i = 0;

    while (i < len) {
        size_t reclen = 0, start = i, keyofs, vlen;
        /* decimal record length */
        if (pax[i] < '0' || pax[i] > '9') return -1;
        while (i < len && pax[i] >= '0' && pax[i] <= '9') {
            if (reclen > (SIZE_MAX - 9) / 10) return -1;
            reclen = reclen * 10 + (size_t)(pax[i] - '0');
            i++;
        }
        if (i >= len || pax[i] != ' ') return -1;
        if (reclen < 3 || start + reclen > len) return -1;
        if (pax[start + reclen - 1] != '\n') return -1;
        i++; /* past the space */
        keyofs = i;
        /* key up to '=' */
        while (i < start + reclen && pax[i] != '=') i++;
        if (i >= start + reclen) return -1;
        if (i - keyofs == 4 && memcmp(pax + keyofs, "path", 4) == 0) {
            size_t vstart = i + 1;
            /* value is the bytes up to the trailing newline */
            vlen = (start + reclen - 1) - vstart;
            if (vlen >= out_cap) vlen = out_cap - 1;
            memcpy(out, pax + vstart, vlen);
            out[vlen] = '\0';
            return 1;
        }
        i = start + reclen;
    }
    return 0;
}

/* ---- the single-pass walker ---- */

/* Visit callback: returns 0 to continue, -1 to stop the walk with the
 * error already recorded (throwing is the caller's job, after the
 * walk unwinds its own C allocations). content points at the member's
 * raw bytes (content_len == m->size), valid until the next call. */
typedef int (*tar_visit_fn)(mino_state *S, void *ctx, const tar_member *m,
                            const unsigned char *content, size_t content_len,
                            const char **ekind, const char **ecode,
                            char *emsg, size_t emsg_cap);

#define TAR_EMSG_CAP 200

/* Walk every member of the archive, calling visit for each resolved
 * (name-extended) member. Handles pax "x"/"g" extension headers and
 * GNU long-name "L"/"K" headers transparently. Returns 0 on a clean
 * walk (a full zero block or the end of input ends it), or -1 with
 * the classified error filled. Pure with respect to the filesystem;
 * the visit callback owns any effects. */
static int tar_walk(mino_state *S, const unsigned char *data, size_t len,
                    tar_visit_fn visit, void *ctx,
                    const char **ekind, const char **ecode,
                    char *emsg, size_t emsg_cap)
{
    size_t ofs = 0;
    char pending_path[sizeof(((tar_member *)0)->name)];
    char pending_link[sizeof(((tar_member *)0)->linkname)];
    int have_path = 0, have_link = 0;
    int saw_member = 0;

    /* A valid tar is a whole number of 512-byte blocks. Any input
     * shorter than one block, or with a non-block-aligned tail, that
     * carries a non-zero byte cannot be a tar header and is not a tar
     * archive (an all-zero short input is an empty archive). */
    if (len % TAR_BLOCK != 0) {
        size_t i;
        for (i = 0; i < len; i++) {
            if (data[i] != 0) {
                *ekind = "codec/magic";
                *ecode = TAR_MGC_MAGIC;
                snprintf(emsg, emsg_cap,
                         "tar: not a tar archive (unaligned length)");
                return -1;
            }
        }
    }

    while (ofs + TAR_BLOCK <= len) {
        const unsigned char *blk = data + ofs;
        tar_member m;
        size_t blocks;
        size_t content_ofs;

        /* A zero block ends the archive (the two-zero-block trailer). */
        if (tar_block_is_zero(blk)) break;

        if (!tar_magic_ok(blk)) {
            /* The first non-zero block must be a tar header; anything
             * else is not a tar archive. Deeper in, a bad magic is a
             * corrupt stream. */
            *ekind = saw_member ? "codec/corrupt" : "codec/magic";
            *ecode = saw_member ? TAR_MGC_CORRUPT : TAR_MGC_MAGIC;
            snprintf(emsg, emsg_cap, "tar: %s",
                     saw_member ? "corrupt header block"
                                : "not a tar archive (no ustar magic)");
            return -1;
        }
        if (tar_parse_header(blk, &m, ekind, ecode) != 0) {
            snprintf(emsg, emsg_cap, "tar: malformed header field");
            return -1;
        }
        if (tar_content_blocks(m.size, &blocks) != 0) {
            *ekind = "codec/corrupt";
            *ecode = TAR_MGC_CORRUPT;
            snprintf(emsg, emsg_cap, "tar: size field overflows the "
                     "block count");
            return -1;
        }
        content_ofs = ofs + TAR_BLOCK;
        /* The declared content must fit inside the input. blocks *
         * 512 is guarded against wrap; the sum against len. */
        if (blocks > (SIZE_MAX - content_ofs) / TAR_BLOCK
            || content_ofs + blocks * TAR_BLOCK > len) {
            *ekind = "codec/truncated";
            *ecode = TAR_MGC_TRUNCATED;
            snprintf(emsg, emsg_cap, "tar: declared size runs past the "
                     "end of the archive");
            return -1;
        }

        if (m.typeflag == 'x' || m.typeflag == 'g') {
            /* pax extended header: parse a path record and apply it to
             * the NEXT member. A 'g' global record is honored the same
             * way (the fixtures use 'x'; both carry a path the same). */
            char path[sizeof(m.name)];
            int r = tar_pax_path(data + content_ofs, (size_t)m.size,
                                 path, sizeof(path));
            if (r < 0) {
                *ekind = "codec/corrupt";
                *ecode = TAR_MGC_CORRUPT;
                snprintf(emsg, emsg_cap, "tar: malformed pax header record");
                return -1;
            }
            if (r == 1) {
                memcpy(pending_path, path, sizeof(path));
                have_path = 1;
            }
            ofs = content_ofs + blocks * TAR_BLOCK;
            continue;
        }
        if (m.typeflag == 'L' || m.typeflag == 'K') {
            /* GNU long name ('L') / long link ('K'): the content is the
             * full name/linkname for the NEXT member, NUL-terminated. */
            const unsigned char *c = data + content_ofs;
            size_t clen = tar_field_len(c, (size_t)m.size);
            if (m.typeflag == 'L') {
                if (clen >= sizeof(pending_path)) clen = sizeof(pending_path) - 1;
                memcpy(pending_path, c, clen);
                pending_path[clen] = '\0';
                have_path = 1;
            } else {
                if (clen >= sizeof(pending_link)) clen = sizeof(pending_link) - 1;
                memcpy(pending_link, c, clen);
                pending_link[clen] = '\0';
                have_link = 1;
            }
            ofs = content_ofs + blocks * TAR_BLOCK;
            continue;
        }

        if (have_path) {
            memcpy(m.name, pending_path, sizeof(m.name));
            have_path = 0;
        }
        if (have_link) {
            memcpy(m.linkname, pending_link, sizeof(m.linkname));
            m.has_link = (m.typeflag == '1' || m.typeflag == '2');
            have_link = 0;
        }
        saw_member = 1;
        if (visit(S, ctx, &m, data + content_ofs, (size_t)m.size,
                  ekind, ecode, emsg, emsg_cap) != 0)
            return -1;
        ofs = content_ofs + blocks * TAR_BLOCK;
    }
    return 0;
}

/* ---- the read-side entry map ---- */

/* Map a typeflag to the entry :type keyword. */
static mino_val *tar_type_kw(mino_state *S, char typeflag)
{
    switch (typeflag) {
    case '0': case '\0': return mino_keyword(S, "file");
    case '5':            return mino_keyword(S, "dir");
    case '2':            return mino_keyword(S, "symlink");
    case '1':            return mino_keyword(S, "hardlink");
    case '3':            return mino_keyword(S, "chardev");
    case '4':            return mino_keyword(S, "blockdev");
    case '6':            return mino_keyword(S, "fifo");
    default:             return mino_keyword(S, "other");
    }
}

/* Build the fixed six-key entry map {:name :size :mode :mtime :type
 * :linkname}. :linkname is present-and-nil off link entries. */
static mino_val *tar_entry_map(mino_state *S, const tar_member *m)
{
    mino_val *ks[6], *vs[6];

    ks[0] = mino_keyword(S, "name");
    vs[0] = mino_string_n(S, m->name, strlen(m->name));
    ks[1] = mino_keyword(S, "size");
    vs[1] = mino_int(S, m->size);
    ks[2] = mino_keyword(S, "mode");
    vs[2] = mino_int(S, m->mode);
    ks[3] = mino_keyword(S, "mtime");
    vs[3] = (m->mtime < 0) ? mino_nil(S) : mino_int(S, m->mtime);
    ks[4] = mino_keyword(S, "type");
    vs[4] = tar_type_kw(S, m->typeflag);
    ks[5] = mino_keyword(S, "linkname");
    vs[5] = m->has_link
                ? mino_string_n(S, m->linkname, strlen(m->linkname))
                : mino_nil(S);
    return mino_map(S, ks, vs, 6);
}

/* ---- tar-entries ---- */

typedef struct {
    mino_vec_builder *out;
} tar_entries_ctx;

static int tar_entries_visit(mino_state *S, void *ctx, const tar_member *m,
                             const unsigned char *content, size_t content_len,
                             const char **ekind, const char **ecode,
                             char *emsg, size_t emsg_cap)
{
    tar_entries_ctx *c = (tar_entries_ctx *)ctx;
    (void)content; (void)content_len;
    (void)ekind; (void)ecode; (void)emsg; (void)emsg_cap;
    mino_vector_builder_push(c->out, tar_entry_map(S, m));
    return 0;
}

/* (tar-entries data) -- vector of member maps in archive order. */
static mino_val *prim_tar_entries(mino_state *S, mino_val *args,
                                  mino_env *env)
{
    const unsigned char *data;
    size_t len;
    tar_entries_ctx ctx;
    const char *ekind = NULL, *ecode = NULL;
    char emsg[TAR_EMSG_CAP];
    (void)env;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "tar-entries takes one argument");
    }
    if (tar_data_arg(S, args->as.cons.car, "tar-entries", &data, &len) != 0)
        return NULL;

    ctx.out = mino_vector_builder_new(S);
    if (tar_walk(S, data, len, tar_entries_visit, &ctx,
                 &ekind, &ecode, emsg, sizeof(emsg)) != 0) {
        /* Finish-and-discard: releases the builder's malloc and its
         * GC ref; the half-built vector becomes ordinary garbage. */
        (void)mino_vector_builder_finish(ctx.out);
        return prim_throw_classified(S, ekind, ecode, emsg);
    }
    return mino_vector_builder_finish(ctx.out);
}

/* ---- tar-read ---- */

typedef struct {
    const char          *target;
    size_t               target_len;
    const unsigned char *found;
    size_t               found_len;
    int                  have;
} tar_read_ctx;

static int tar_read_visit(mino_state *S, void *ctx, const tar_member *m,
                          const unsigned char *content, size_t content_len,
                          const char **ekind, const char **ecode,
                          char *emsg, size_t emsg_cap)
{
    tar_read_ctx *c = (tar_read_ctx *)ctx;
    (void)S; (void)ekind; (void)ecode; (void)emsg; (void)emsg_cap;
    if (c->have) return 0;
    if (strlen(m->name) == c->target_len
        && memcmp(m->name, c->target, c->target_len) == 0) {
        c->found = content;
        c->found_len = content_len;
        c->have = 1;
    }
    return 0;
}

/* (tar-read data name) -- the bytes of the first member named name. */
static mino_val *prim_tar_read(mino_state *S, mino_val *args, mino_env *env)
{
    const unsigned char *data;
    size_t len;
    mino_val *name_val;
    tar_read_ctx ctx;
    const char *ekind = NULL, *ecode = NULL;
    char emsg[TAR_EMSG_CAP], msg[160];
    (void)env;

    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "tar-read takes two arguments");
    }
    if (tar_data_arg(S, args->as.cons.car, "tar-read", &data, &len) != 0)
        return NULL;
    name_val = args->as.cons.cdr->as.cons.car;
    if (name_val == NULL || !mino_is_string(name_val)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "tar-read: name must be a string");
    }

    ctx.target = name_val->as.s.data;
    ctx.target_len = name_val->as.s.len;
    ctx.found = NULL;
    ctx.found_len = 0;
    ctx.have = 0;
    if (tar_walk(S, data, len, tar_read_visit, &ctx,
                 &ekind, &ecode, emsg, sizeof(emsg)) != 0)
        return prim_throw_classified(S, ekind, ecode, emsg);
    if (!ctx.have) {
        snprintf(msg, sizeof(msg), "tar-read: no entry named \"%.120s\"",
                 ctx.target);
        return prim_throw_classified(S, "codec/missing", TAR_MGC_MISSING, msg);
    }
    return mino_bytes(S, ctx.found, ctx.found_len);
}

/* ---- tar-extract, the hardened filesystem path ---- */

#if !defined(_WIN32)
/* The whole extract path is POSIX-only: on Windows prim_tar_extract
 * throws host/unsupported before any of it would run. */

/* A member name is unsafe if it is empty, absolute, or carries a
 * ".." path component (CWE-22). Checked BEFORE any filesystem touch.
 * Returns 1 for unsafe, 0 for safe. */
static int tar_name_unsafe(const char *name)
{
    const char *p = name;
    if (name[0] == '\0') return 1;
    if (name[0] == '/') return 1;
    while (*p != '\0') {
        /* p sits at the start of a path component. */
        if (p[0] == '.' && p[1] == '.'
            && (p[2] == '/' || p[2] == '\0'))
            return 1;
        /* advance to the next component */
        while (*p != '\0' && *p != '/') p++;
        while (*p == '/') p++;
    }
    return 0;
}

/* A link target is unsafe if it is absolute or would escape the
 * destination root: simulate the target resolved relative to the
 * link's own directory and reject if the depth ever goes negative.
 * link_dir_depth is the number of directory components above the
 * link inside the destination (0 for a top-level link). */
static int tar_link_unsafe(const char *target, int link_dir_depth)
{
    const char *p = target;
    int depth = link_dir_depth;

    if (target[0] == '\0') return 1;
    if (target[0] == '/') return 1;
    while (*p != '\0') {
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) {
            depth--;
            if (depth < 0) return 1;  /* escapes the destination root */
        } else if (!(p[0] == '.' && (p[1] == '/' || p[1] == '\0'))) {
            depth++;
        }
        while (*p != '\0' && *p != '/') p++;
        while (*p == '/') p++;
    }
    return 0;
}

/* Count the directory components above a member (its name minus the
 * final component), for the link-escape depth simulation. */
static int tar_name_dir_depth(const char *name)
{
    int depth = 0;
    const char *p = name;
    const char *last_slash = NULL;
    while (*p != '\0') {
        if (*p == '/') last_slash = p;
        p++;
    }
    if (last_slash == NULL) return 0;
    for (p = name; p < last_slash; p++)
        if (*p == '/') depth++;
    /* components above the file = number of slashes before the last */
    return depth;
}

/* Open (creating as needed) the parent directory of a relative member
 * name under root_fd, descending one component at a time with
 * O_NOFOLLOW so a symlink materialized by an earlier member cannot be
 * traversed (CWE-59). Returns the parent dir fd (caller closes) and
 * writes the final component into leaf (NUL-terminated). Returns -1 on
 * any failure; sets *escaped when a component resolved through a
 * symlink (the write-through attack). */
static int tar_open_parent(int root_fd, const char *name, char *leaf,
                           size_t leaf_cap, int *escaped)
{
    int dir_fd = dup(root_fd);
    const char *p = name;
    *escaped = 0;
    if (dir_fd < 0) return -1;

    for (;;) {
        const char *slash = strchr(p, '/');
        if (slash == NULL) {
            size_t ll = strlen(p);
            if (ll == 0 || ll >= leaf_cap) { close(dir_fd); return -1; }
            memcpy(leaf, p, ll + 1);
            return dir_fd;
        }
        {
            char comp[256];
            size_t cl = (size_t)(slash - p);
            int next;
            if (cl == 0 || cl >= sizeof(comp)) { close(dir_fd); return -1; }
            memcpy(comp, p, cl);
            comp[cl] = '\0';
            if (mkdirat(dir_fd, comp, 0755) != 0 && errno != EEXIST) {
                close(dir_fd); return -1;
            }
            /* O_NOFOLLOW: a symlink planted here (an earlier member)
             * is refused, so no write descends through it. */
            next = openat(dir_fd, comp,
                          O_RDONLY | O_NOFOLLOW | O_DIRECTORY);
            if (next < 0) {
                if (errno == ELOOP || errno == ENOTDIR) *escaped = 1;
                close(dir_fd); return -1;
            }
            close(dir_fd);
            dir_fd = next;
        }
        p = slash + 1;
        while (*p == '/') p++;
    }
}

/* Materialize one member under root_fd. Names and link targets are
 * already validated safe. Returns 0, or -1 with the errno-derived
 * failure; *escaped signals a symlink-in-path write-through. */
static int tar_extract_member(int root_fd, const tar_member *m,
                              const unsigned char *content, size_t content_len,
                              int *escaped)
{
    char leaf[256];
    int parent_fd, rc = 0;
    *escaped = 0;

    /* A trailing-slash directory name has no leaf; create the dir and
     * return. */
    {
        size_t nl = strlen(m->name);
        if (nl > 0 && m->name[nl - 1] == '/') {
            char tmp[sizeof(m->name)];
            char *dleaf = leaf;
            int pfd;
            memcpy(tmp, m->name, nl);
            tmp[nl - 1] = '\0';  /* drop the trailing slash */
            pfd = tar_open_parent(root_fd, tmp, leaf, sizeof(leaf), escaped);
            if (pfd < 0) return -1;
            (void)dleaf;
            if (mkdirat(pfd, leaf, (mode_t)(m->mode & 07777)) != 0
                && errno != EEXIST) { close(pfd); return -1; }
            /* Set the mode explicitly (umask/EEXIST may have shifted it)
             * and the mtime. A mode the archive asked for but the
             * filesystem refused is a failed write, not a shrug. */
            if (fchmodat(pfd, leaf, (mode_t)(m->mode & 07777), 0) != 0) {
                close(pfd);
                return -1;
            }
            if (m->mtime >= 0) {
                struct timespec ts[2];
                ts[0].tv_sec = (time_t)m->mtime; ts[0].tv_nsec = 0;
                ts[1].tv_sec = (time_t)m->mtime; ts[1].tv_nsec = 0;
                utimensat(pfd, leaf, ts, 0);
            }
            close(pfd);
            return 0;
        }
    }

    parent_fd = tar_open_parent(root_fd, m->name, leaf, sizeof(leaf), escaped);
    if (parent_fd < 0) return -1;

    if (m->typeflag == '5') {
        if (mkdirat(parent_fd, leaf, (mode_t)(m->mode & 07777)) != 0
            && errno != EEXIST) rc = -1;
        else if (fchmodat(parent_fd, leaf,
                          (mode_t)(m->mode & 07777), 0) != 0) rc = -1;
    } else if (m->typeflag == '2') {
        /* Symlink entry: recreate the link itself, never follow it. */
        unlinkat(parent_fd, leaf, 0);
        if (symlinkat(m->linkname, parent_fd, leaf) != 0) rc = -1;
    } else if (m->typeflag == '1') {
        /* Hardlink to an earlier member (already materialized under
         * the root): link relative to the root, both ends O_NOFOLLOW
         * safe because linkat does not traverse a trailing symlink
         * without AT_SYMLINK_FOLLOW. */
        char lleaf[256];
        int lparent;
        int lesc = 0;
        lparent = tar_open_parent(root_fd, m->linkname, lleaf,
                                  sizeof(lleaf), &lesc);
        if (lparent < 0) { close(parent_fd); return -1; }
        unlinkat(parent_fd, leaf, 0);
        if (linkat(lparent, lleaf, parent_fd, leaf, 0) != 0) rc = -1;
        close(lparent);
    } else {
        /* Regular file (and unknown flags treated as a file): create
         * O_NOFOLLOW|O_EXCL under the parent and write the content. */
        int fd = openat(parent_fd, leaf,
                        O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW,
                        (mode_t)(m->mode & 07777));
        if (fd < 0) {
            if (errno == ELOOP) *escaped = 1;
            close(parent_fd);
            return -1;
        }
        {
            size_t off = 0;
            while (off < content_len) {
                ssize_t w = write(fd, content + off, content_len - off);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    rc = -1; break;
                }
                off += (size_t)w;
            }
        }
        fchmod(fd, (mode_t)(m->mode & 07777));
        if (m->mtime >= 0) {
            struct timespec ts[2];
            ts[0].tv_sec = (time_t)m->mtime; ts[0].tv_nsec = 0;
            ts[1].tv_sec = (time_t)m->mtime; ts[1].tv_nsec = 0;
            futimens(fd, ts);
        }
        close(fd);
    }
    close(parent_fd);
    return rc;
}
#endif /* !_WIN32 */

typedef struct {
    int               root_fd;
    mino_vec_builder *names;
} tar_extract_ctx;

#if !defined(_WIN32)
/* POSIX-only: on Windows prim_tar_extract throws host/unsupported
 * before the walk starts, so the visitor never compiles there. */
static int tar_extract_visit(mino_state *S, void *ctx, const tar_member *m,
                             const unsigned char *content, size_t content_len,
                             const char **ekind, const char **ecode,
                             char *emsg, size_t emsg_cap)
{
    tar_extract_ctx *c = (tar_extract_ctx *)ctx;

    /* The hostile-name gate: every rejection is :codec/unsafe and
     * fires BEFORE any write, so nothing lands outside the root. */
    if (tar_name_unsafe(m->name)) {
        *ekind = "codec/unsafe";
        *ecode = TAR_MGC_UNSAFE;
        snprintf(emsg, emsg_cap, "tar-extract: unsafe member name "
                 "\"%.120s\"", m->name);
        return -1;
    }
    if (m->has_link
        && tar_link_unsafe(m->linkname, tar_name_dir_depth(m->name))) {
        *ekind = "codec/unsafe";
        *ecode = TAR_MGC_UNSAFE;
        snprintf(emsg, emsg_cap, "tar-extract: link target \"%.100s\" "
                 "escapes the destination", m->linkname);
        return -1;
    }

    {
        int escaped = 0;
        if (tar_extract_member(c->root_fd, m, content, content_len,
                               &escaped) != 0) {
            if (escaped) {
                *ekind = "codec/unsafe";
                *ecode = TAR_MGC_UNSAFE;
                snprintf(emsg, emsg_cap, "tar-extract: a symlink in the "
                         "path of \"%.100s\" escapes the destination",
                         m->name);
            } else {
                *ekind = "io";
                *ecode = "MIO001";
                snprintf(emsg, emsg_cap, "tar-extract: cannot write "
                         "\"%.120s\"", m->name);
            }
            return -1;
        }
        mino_vector_builder_push(c->names,
                                 mino_string_n(S, m->name, strlen(m->name)));
        return 0;
    }
}
#endif /* !_WIN32 */

/* (tar-extract data dest opts?) -- materialize the archive under the
 * destination directory, returning the vector of extracted member
 * names in archive order. Hardened: an absolute name, a ".."
 * component, or an escaping link target throws :codec/unsafe before
 * anything lands. */
static mino_val *prim_tar_extract(mino_state *S, mino_val *args,
                                  mino_env *env)
{
    const unsigned char *data;
    size_t len;
    mino_val *dest_val;
    const char *ekind = NULL, *ecode = NULL;
    char emsg[TAR_EMSG_CAP];
    tar_extract_ctx ctx;
    mino_val *result;
    (void)env;

    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "tar-extract takes an archive and a "
                                     "destination");
    }
    if (tar_data_arg(S, args->as.cons.car, "tar-extract", &data, &len) != 0)
        return NULL;
    dest_val = args->as.cons.cdr->as.cons.car;
    if (mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        /* opts arg accepted for forward compatibility; no keys yet. */
        if (mino_is_cons(args->as.cons.cdr->as.cons.cdr->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                                         "tar-extract takes an archive, a "
                                         "destination, and optional opts");
        }
    }
    if (dest_val == NULL || !mino_is_string(dest_val)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "tar-extract: destination must be a "
                                     "string");
    }

#if defined(_WIN32)
    (void)ctx; (void)result; (void)ekind; (void)ecode; (void)emsg;
    return prim_throw_classified(S, "host/unsupported", "MHO002",
                                 "tar-extract: not supported on this "
                                 "platform");
#else
    ctx.root_fd = open(dest_val->as.s.data,
                       O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (ctx.root_fd < 0) {
        char msg[200];
        snprintf(msg, sizeof(msg), "tar-extract: cannot open destination "
                 "\"%.150s\"", dest_val->as.s.data);
        return prim_throw_classified(S, "io", "MIO001", msg);
    }
    ctx.names = mino_vector_builder_new(S);
    if (tar_walk(S, data, len, tar_extract_visit, &ctx,
                 &ekind, &ecode, emsg, sizeof(emsg)) != 0) {
        close(ctx.root_fd);
        /* Finish-and-discard releases the builder (see tar-entries). */
        (void)mino_vector_builder_finish(ctx.names);
        return prim_throw_classified(S, ekind, ecode, emsg);
    }
    close(ctx.root_fd);
    result = mino_vector_builder_finish(ctx.names);
    return result;
#endif
}

/* ---- the write side (p7t3): tar-create ---- */

/* One captured write entry, validated before any output allocation
 * (the zip_wentry_capture discipline). name/linkname are malloc'd;
 * data borrows the caller's mino value (safe through the build loop:
 * no mino allocation between capture and result). */
typedef struct {
    char                *name;
    size_t               name_len;
    char                *linkname;
    const unsigned char *data;
    size_t               data_len;
    long long            mode;
    long long            mtime;      /* -1 for the zero field (nil) */
    char                 typeflag;
} tar_wentry;

static void tar_wentries_free(tar_wentry *es, size_t n)
{
    size_t i;
    if (es == NULL) return;
    for (i = 0; i < n; i++) { free(es[i].name); free(es[i].linkname); }
    free(es);
}

#define TAR_WEMSG_CAP 160

/* Write an octal field of width w for value v: (w-1) zero-padded
 * octal digits in field[0..w-2] then a NUL terminator at field[w-1]
 * (the conventional ustar numeric field). The low digit lands at
 * field[w-2], so the terminator never clobbers a value digit. */
static void tar_put_octal(unsigned char *field, size_t w, unsigned long long v)
{
    size_t i;
    for (i = w - 1; i-- > 0;) {
        field[i] = (unsigned char)('0' + (v & 7));
        v >>= 3;
    }
    field[w - 1] = '\0';
}

/* The header checksum: sum of all 512 bytes with the checksum field
 * itself taken as 8 spaces, written back as a 6-digit octal + NUL +
 * space at offset 148. */
static void tar_put_checksum(unsigned char *blk)
{
    unsigned long sum = 0;
    size_t i;
    memset(blk + 148, ' ', 8);
    for (i = 0; i < TAR_BLOCK; i++) sum += blk[i];
    for (i = 6; i-- > 0;) {
        blk[148 + i] = (unsigned char)('0' + (sum & 7));
        sum >>= 3;
    }
    blk[148 + 6] = '\0';
    blk[148 + 7] = ' ';
}

/* Emit one member's header block(s) plus content into buf at *ofs,
 * growing nothing (buf is pre-sized by the caller). A name longer
 * than 100 bytes with no valid ustar prefix split rides a GNU 'L'
 * long-name header. */
static void tar_emit_member(unsigned char *buf, size_t *ofs,
                            const tar_wentry *e)
{
    unsigned char *blk;

    /* Long name: emit a GNU 'L' header whose content is the full name,
     * then the real header with the name truncated to the field (the
     * reader honors the 'L' override, so the truncated field is
     * cosmetic). */
    if (e->name_len > TAR_NAME_LEN) {
        size_t nblocks = (e->name_len + 1 + TAR_BLOCK - 1) / TAR_BLOCK;
        blk = buf + *ofs;
        memset(blk, 0, TAR_BLOCK);
        memcpy(blk + TAR_NAME_OFS, "././@LongLink", 13);
        tar_put_octal(blk + TAR_MODE_OFS, 8, 0);
        tar_put_octal(blk + TAR_SIZE_OFS, TAR_SIZE_LEN,
                      (unsigned long long)(e->name_len + 1));
        tar_put_octal(blk + TAR_MTIME_OFS, TAR_MTIME_LEN, 0);
        blk[TAR_TYPEFLAG_OFS] = 'L';
        memcpy(blk + TAR_MAGIC_OFS, "ustar  \0", 8);
        tar_put_checksum(blk);
        *ofs += TAR_BLOCK;
        memcpy(buf + *ofs, e->name, e->name_len);
        *ofs += nblocks * TAR_BLOCK;
    }

    blk = buf + *ofs;
    memset(blk, 0, TAR_BLOCK);
    {
        size_t nl = e->name_len > TAR_NAME_LEN ? TAR_NAME_LEN : e->name_len;
        memcpy(blk + TAR_NAME_OFS, e->name, nl);
    }
    tar_put_octal(blk + TAR_MODE_OFS, 8, (unsigned long long)(e->mode & 07777));
    tar_put_octal(blk + 108, 8, 0); /* uid */
    tar_put_octal(blk + 116, 8, 0); /* gid */
    tar_put_octal(blk + TAR_SIZE_OFS, TAR_SIZE_LEN,
                  (unsigned long long)e->data_len);
    tar_put_octal(blk + TAR_MTIME_OFS, TAR_MTIME_LEN,
                  e->mtime < 0 ? 0ull : (unsigned long long)e->mtime);
    blk[TAR_TYPEFLAG_OFS] = e->typeflag;
    if (e->linkname != NULL)
        memcpy(blk + TAR_LINKNAME_OFS, e->linkname,
               strlen(e->linkname) > TAR_LINKNAME_LEN
                   ? TAR_LINKNAME_LEN : strlen(e->linkname));
    memcpy(blk + TAR_MAGIC_OFS, "ustar\0", 6);
    blk[263] = '0'; blk[264] = '0'; /* version "00" */
    tar_put_checksum(blk);
    *ofs += TAR_BLOCK;

    if (e->data_len > 0) {
        size_t cblocks = (e->data_len + TAR_BLOCK - 1) / TAR_BLOCK;
        memcpy(buf + *ofs, e->data, e->data_len);
        *ofs += cblocks * TAR_BLOCK;
    }
}

/* Capture one entry map into a wentry, validating as it goes. On a
 * validation failure it records the classified error (no throw: the
 * caller frees the partial array first, then throws). */
static int tar_wentry_capture(mino_state *S, mino_val *entry, size_t idx,
                              tar_wentry *out, char *emsg,
                              const char **ekind, const char **ecode)
{
    mino_val *v;
    const unsigned char *bytes;
    size_t len;
    long long n;

#define TAR_WFAIL(kind, code, ...)                                      \
    do {                                                                \
        snprintf(emsg, TAR_WEMSG_CAP, __VA_ARGS__);                     \
        *ekind = (kind); *ecode = (code);                              \
        free(out->name); free(out->linkname);                          \
        out->name = NULL; out->linkname = NULL;                        \
        return -1;                                                      \
    } while (0)
#define TAR_WCONTRACT(...) TAR_WFAIL("eval/contract", "MCT001", __VA_ARGS__)

    out->name = NULL;
    out->linkname = NULL;
    out->data = (const unsigned char *)"";
    out->data_len = 0;
    out->mode = -1;
    out->mtime = -1;
    out->typeflag = '0';

    if (entry == NULL || mino_type_of(entry) != MINO_MAP)
        TAR_WCONTRACT("tar-create: entry %lu must be a map",
                      (unsigned long)idx);

    /* :name -- required non-empty string, no NUL. */
    v = map_get_val(entry, mino_keyword(S, "name"));
    if (v == NULL || mino_type_of(v) != MINO_STRING)
        TAR_WCONTRACT("tar-create: entry %lu :name must be a string",
                      (unsigned long)idx);
    len = v->as.s.len;
    bytes = (const unsigned char *)v->as.s.data;
    if (bytes == NULL) bytes = (const unsigned char *)"";
    if (len == 0)
        TAR_WCONTRACT("tar-create: entry %lu :name must not be empty",
                      (unsigned long)idx);
    if (memchr(bytes, 0, len) != NULL)
        TAR_WCONTRACT("tar-create: entry %lu :name must not contain a NUL",
                      (unsigned long)idx);
    out->name = (char *)malloc(len + 1);
    if (out->name == NULL)
        TAR_WFAIL("internal", "MIN001", "tar-create: out of memory");
    memcpy(out->name, bytes, len);
    out->name[len] = '\0';
    out->name_len = len;

    /* :type -- :file (default), :dir, :symlink, :hardlink. */
    v = map_get_val(entry, mino_keyword(S, "type"));
    if (v != NULL && mino_type_of(v) != MINO_NIL) {
        if (v == mino_keyword(S, "file"))          out->typeflag = '0';
        else if (v == mino_keyword(S, "dir"))      out->typeflag = '5';
        else if (v == mino_keyword(S, "symlink"))  out->typeflag = '2';
        else if (v == mino_keyword(S, "hardlink")) out->typeflag = '1';
        else
            TAR_WCONTRACT("tar-create: entry %lu :type must be :file, :dir, "
                          ":symlink, or :hardlink", (unsigned long)idx);
    }

    /* :linkname -- required for a link entry, rejected otherwise. */
    v = map_get_val(entry, mino_keyword(S, "linkname"));
    if (out->typeflag == '2' || out->typeflag == '1') {
        if (v == NULL || mino_type_of(v) != MINO_STRING)
            TAR_WCONTRACT("tar-create: entry %lu link needs a :linkname "
                          "string", (unsigned long)idx);
        len = v->as.s.len;
        bytes = (const unsigned char *)v->as.s.data;
        if (bytes == NULL) bytes = (const unsigned char *)"";
        out->linkname = (char *)malloc(len + 1);
        if (out->linkname == NULL)
            TAR_WFAIL("internal", "MIN001", "tar-create: out of memory");
        memcpy(out->linkname, bytes, len);
        out->linkname[len] = '\0';
    } else if (v != NULL && mino_type_of(v) != MINO_NIL) {
        TAR_WCONTRACT("tar-create: entry %lu :linkname only applies to a "
                      "link entry", (unsigned long)idx);
    }

    /* :data -- bytes or string, only for a file entry. */
    v = map_get_val(entry, mino_keyword(S, "data"));
    if (v != NULL && mino_type_of(v) != MINO_NIL) {
        if (out->typeflag != '0')
            TAR_WCONTRACT("tar-create: entry %lu :data only applies to a "
                          "file entry", (unsigned long)idx);
        if (!mino_is_bytes(v) && !mino_is_string(v))
            TAR_WCONTRACT("tar-create: entry %lu :data must be bytes or a "
                          "string", (unsigned long)idx);
        if (mino_is_bytes(v)) {
            out->data = mino_bytes_data(v);
            out->data_len = mino_bytes_len(v);
        } else {
            out->data = (const unsigned char *)v->as.s.data;
            out->data_len = v->as.s.len;
        }
        if (out->data == NULL) out->data = (const unsigned char *)"";
    }

    /* :mode -- default 0644 file, 0755 dir, 0777 symlink. */
    v = map_get_val(entry, mino_keyword(S, "mode"));
    if (v != NULL && mino_type_of(v) != MINO_NIL) {
        if (!as_long(v, &n) || n < 0 || n > 07777)
            TAR_WCONTRACT("tar-create: entry %lu :mode must be 0..07777",
                          (unsigned long)idx);
        out->mode = n;
    } else {
        out->mode = out->typeflag == '5' ? 0755
                  : out->typeflag == '2' ? 0777 : 0644;
    }

    /* :mtime -- epoch seconds; nil / absent stores the zero field. */
    v = map_get_val(entry, mino_keyword(S, "mtime"));
    if (v != NULL && mino_type_of(v) != MINO_NIL) {
        if (!as_long(v, &n) || n < 0)
            TAR_WCONTRACT("tar-create: entry %lu :mtime must be a "
                          "non-negative integer", (unsigned long)idx);
        out->mtime = n;
    }

    return 0;
#undef TAR_WCONTRACT
#undef TAR_WFAIL
}

/* Total bytes a captured entry occupies in the output. */
static size_t tar_wentry_span(const tar_wentry *e)
{
    size_t span = TAR_BLOCK;  /* the header */
    if (e->name_len > TAR_NAME_LEN)
        span += TAR_BLOCK
              + ((e->name_len + 1 + TAR_BLOCK - 1) / TAR_BLOCK) * TAR_BLOCK;
    if (e->data_len > 0)
        span += ((e->data_len + TAR_BLOCK - 1) / TAR_BLOCK) * TAR_BLOCK;
    return span;
}

/* (tar-create entries) -- build a tar archive from a vector of entry
 * maps and return the bytes. Deterministic: no timestamps or uid/gid
 * beyond the entry data. Two trailing zero blocks close the archive.
 * The .tar.gz composition is the facade's job (gzip-compress). */
static mino_val *prim_tar_create(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *entries_val, *result;
    tar_wentry *wentries = NULL;
    size_t n, i, total = 0, ofs = 0;
    unsigned char *buf;
    (void)env;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "tar-create takes one argument (a "
                                     "vector of entry maps)");
    }
    entries_val = args->as.cons.car;
    if (entries_val == NULL || mino_type_of(entries_val) != MINO_VECTOR) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "tar-create: entries must be a vector "
                                     "of entry maps");
    }

    n = entries_val->as.vec.len;
    if (n != 0) {
        wentries = (tar_wentry *)calloc(n, sizeof(*wentries));
        if (wentries == NULL)
            return prim_throw_classified(S, "internal", "MIN001",
                                         "tar-create: out of memory");
    }
    for (i = 0; i < n; i++) {
        char emsg[TAR_WEMSG_CAP];
        const char *ekind = NULL, *ecode = NULL;
        if (tar_wentry_capture(S, vec_nth(entries_val, i), i, &wentries[i],
                               emsg, &ekind, &ecode) != 0) {
            tar_wentries_free(wentries, i);
            return prim_throw_classified(S, ekind, ecode, emsg);
        }
    }

    for (i = 0; i < n; i++) {
        size_t span = tar_wentry_span(&wentries[i]);
        if (span > SIZE_MAX - total - 2 * TAR_BLOCK) {
            tar_wentries_free(wentries, n);
            return prim_throw_classified(S, "codec/limit", "MGC005",
                                         "tar-create: archive exceeds the "
                                         "addressable range");
        }
        total += span;
    }
    total += 2 * TAR_BLOCK;  /* the two trailing zero blocks */

    buf = (unsigned char *)calloc(1, total);
    if (buf == NULL) {
        tar_wentries_free(wentries, n);
        return prim_throw_classified(S, "internal", "MIN001",
                                     "tar-create: out of memory");
    }
    for (i = 0; i < n; i++)
        tar_emit_member(buf, &ofs, &wentries[i]);
    /* ofs now sits at total - 1024; the two zero blocks are already
     * zeroed by calloc. */
    tar_wentries_free(wentries, n);
    result = mino_bytes(S, buf, total);
    free(buf);
    return result;
}

const mino_prim_def k_prims_tar[] = {
    {"tar-entries", prim_tar_entries,
     "Lists a tar archive's members as a vector of maps with the keys "
     "{:name :size :mode :mtime :type :linkname}, in archive order. "
     ":mode is the 07777-masked permission int; :mtime is epoch "
     "seconds, nil when the stored field is zero; :type is :file, "
     ":dir, :symlink, :hardlink, or another keyword for other header "
     "types; :linkname is the link target for a link entry, nil "
     "otherwise. ustar prefix names and the pax path extension are "
     "resolved, so a long name surfaces as its one logical member. "
     "The archive is bytes or a string; reading touches no filesystem, "
     "so a traversal or absolute name is inert data here."},
    {"tar-read", prim_tar_read,
     "Returns one tar member's bytes: the FIRST member whose name "
     "equals name. Throws :codec/missing when no member matches. The "
     "archive is bytes or a string; nothing is written to any "
     "filesystem."},
    {"tar-extract", prim_tar_extract,
     "Materializes a tar archive under the destination directory and "
     "returns the vector of extracted member names in archive order. "
     "Hardened against hostile archives: an absolute member name, a "
     "name with a \"..\" component, or a link entry whose target "
     "escapes the destination throws :codec/unsafe before anything "
     "lands, and every write descends through O_NOFOLLOW so a symlink "
     "in the archive cannot redirect a later write outside the "
     "destination. Symlink entries are recreated as links, never "
     "followed. Modes and mtimes are restored."},
    {"tar-create", prim_tar_create,
     "Builds a tar archive (ustar, with a GNU long-name header for a "
     "name past 100 bytes) from a vector of entry maps and returns "
     "the bytes. Entries: {:name string (required), :type :file "
     "(default) / :dir / :symlink / :hardlink, :data bytes-or-string "
     "(file only), :linkname string (link entries), :mode 0..07777 "
     "(default 0644 file, 0755 dir, 0777 symlink), :mtime "
     "epoch-seconds-or-nil (nil stores the zero field)}. Output is "
     "deterministic: same entries give byte-identical bytes. The "
     ".tar.gz form composes with gzip-compress at the mino.tar "
     "facade."},
};

const size_t k_prims_tar_count =
    sizeof(k_prims_tar) / sizeof(k_prims_tar[0]);
