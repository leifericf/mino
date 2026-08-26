/*
 * zip.c -- zip container primitives (reader half, ADR 29).
 *
 * zip-entries lists an archive's central directory in archive order
 * (MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY, D8); zip-read returns
 * one entry's bytes by decoded name, the FIRST central-directory
 * match. Both run over the vendored miniz memory reader
 * (mz_zip_reader_init_mem / mz_zip_reader_file_stat /
 * mz_zip_reader_extract_to_heap); the writer half joins this TU in
 * p4.
 *
 * Two decodings are mino-owned, never vendor localtime paths (D5,
 * D6): the CDH time/date words are read straight from the archive
 * bytes and converted to epoch seconds through UTC civil arithmetic
 * (:mtime nil at the DOS minimum 1980-01-01 and for a zero date
 * word), and entry names decode as UTF-8 when the entry sets the
 * language-encoding flag (bit 11), else as CP437 through the
 * generated zip_cp437.h table (the python zipfile behavior; bit-11
 * names pass through byte-identical, claimed-UTF-8). zip-read's
 * lookup decodes every candidate through the same rule, so the name
 * found is the name zip-entries listed (R5 coherence).
 *
 * Zip read IS untrusted input. The central-directory declared size
 * is checked against :max-bytes BEFORE any inflation or allocation
 * (the bomb cap; a lying-small declared size cannot inflate past the
 * buffer the vendor sized from it and fails classified), the EOCD
 * and CDH walks are bounded by the input length, and zip-read writes
 * nothing to any filesystem: "../" and absolute names round trip
 * verbatim, so traversal is inert by construction. A future fs layer
 * must re-litigate that inertness before ever materializing names.
 *
 * Discipline: nothing throws while the vendor reader is open (a
 * longjmp past mz_zip_reader_end leaks the vendor's directory copy;
 * fuzz lanes would grow it without bound). Every path captures what
 * it needs, ends the reader and frees its scratch, and only then
 * classifies and throws.
 *
 * Errors: the gzip read side's one :codec family, extended with zip
 * semantics -- :codec/truncated MGC001, :codec/magic MGC002 (no
 * EOCD: not a zip), :codec/crc MGC003, :codec/corrupt MGC004
 * (LOC/CDH disagreement, malformed streams), :codec/limit MGC005
 * (:max-bytes, zip64 ceilings), :codec/missing MGC006 (name not
 * found), :codec/unsupported MGC007 (encrypted entries, methods
 * other than 0 and 8). Nothing escapes :internal except OOM.
 */

#include "prim/internal.h"
#include "mino.h"
/* Trim define: without it miniz.h declares static zlib-name wrappers
 * (crc32, compress, ...) that this TU never calls. */
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES 1
#include "miniz.h"
#include "zip_cp437.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ZIP_DEFAULT_MAX (64u * 1024u * 1024u)
#define ZIP_EOCD_SIG_LEN 4u
#define ZIP_EOCD_REC_LEN 22u
#define ZIP_EOCD_WINDOW (22u + 0xFFFFu)
#define ZIP_CDH_FIXED_LEN 46u

/* Error codes, contiguous with gzip.c's MGC001-005. */
#define ZIP_MGC_TRUNCATED "MGC001"
#define ZIP_MGC_MAGIC "MGC002"
#define ZIP_MGC_CRC "MGC003"
#define ZIP_MGC_CORRUPT "MGC004"
#define ZIP_MGC_LIMIT "MGC005"
#define ZIP_MGC_MISSING "MGC006"
#define ZIP_MGC_UNSUPPORTED "MGC007"

static const unsigned char ZIP_LOC_SIG[4] = { 0x50, 0x4b, 0x03, 0x04 };
static const unsigned char ZIP_EOCD_SIG[4] = { 0x50, 0x4b, 0x05, 0x06 };

/* ---- small readers (little-endian, C99-portable) ---- */

static unsigned zip_le16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static mz_uint64 zip_le32(const unsigned char *p)
{
    return (mz_uint64)p[0] | ((mz_uint64)p[1] << 8)
         | ((mz_uint64)p[2] << 16) | ((mz_uint64)p[3] << 24);
}

/* ---- argument handling ---- */

/* Container prims accept bytes or string data (a string contributes
 * its UTF-8 bytes, the digest.c rule; the stream prims stay
 * bytes-strict, the gzip.c symmetry). Returns 0 with data/len
 * filled, or a thrown error via -1. */
static int zip_data_arg(mino_state *S, mino_val *v, const char *who,
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
    /* Empty bytes values carry a NULL data pointer; normalize so the
     * buffer arithmetic below stays defined. */
    if (*data == NULL) *data = (const unsigned char *)"";
    return 0;
}

/* Read :max-bytes from opts (non-negative integer; default 64 MiB).
 * Same contract as the compress prims' reader. */
static int zip_max_bytes_opt(mino_state *S, mino_val *opts, const char *who,
                             size_t *max_out)
{
    mino_val *mv;
    long long mb;
    char msg[112];

    *max_out = ZIP_DEFAULT_MAX;
    if (opts == NULL || mino_type_of(opts) != MINO_MAP) return 0;
    mv = map_get_val(opts, mino_keyword(S, "max-bytes"));
    if (mv != NULL && mino_type_of(mv) != MINO_NIL) {
        if (!as_long(mv, &mb) || mb < 0
            || (unsigned long long)mb > SIZE_MAX / 2) {
            snprintf(msg, sizeof(msg),
                     "%s: :max-bytes must be a non-negative integer", who);
            prim_throw_classified(S, "eval/contract", "MCT001", msg);
            return -1;
        }
        *max_out = (size_t)mb;
    }
    return 0;
}

/* ---- EOCD scan and init classification ---- */

/* Bounded backward scan for the EOCD signature inside the classic
 * 22 + 65535 byte comment window, partial records included (the
 * signature may sit closer to the end than a full record). The LAST
 * match wins, matching the vendor's locator. Returns 1 with *ofs
 * filled, else 0. Pure byte scan, no allocation. */
static int zip_find_eocd(const unsigned char *data, size_t len, size_t *ofs)
{
    size_t window, last, i;

    if (len < ZIP_EOCD_SIG_LEN) return 0;
    window = len < ZIP_EOCD_WINDOW ? len : ZIP_EOCD_WINDOW;
    last = len - ZIP_EOCD_SIG_LEN;
    for (i = last;; i--) {
        if (memcmp(data + i, ZIP_EOCD_SIG, ZIP_EOCD_SIG_LEN) == 0) {
            *ofs = i;
            return 1;
        }
        if (i + window == len) break;
    }
    return 0;
}

/* Classify a failed mz_zip_reader_init_mem. The truncated-vs-magic
 * split D9 draws is not recoverable from the vendor error code alone
 * (a tail cut and garbage bytes both report a missing EOCD), so the
 * EOCD scan above supplies the signal: an EOCD signature present but
 * structurally failing is truncation or corruption, an EOCD absent
 * from a stream that starts like a zip is tail truncation, and an
 * EOCD absent from anything else is :codec/magic. Returns NULL after
 * throwing. */
static mino_val *zip_init_error(mino_state *S, mz_zip_archive *zip,
                                const unsigned char *data, size_t len,
                                const char *who)
{
    mz_zip_error err = mz_zip_get_last_error(zip);
    mz_uint64 cd_ofs, cd_size;
    size_t eocd_ofs;
    char msg[128];

    switch (err) {
    case MZ_ZIP_ALLOC_FAILED:
        snprintf(msg, sizeof(msg), "%s: out of memory", who);
        return prim_throw_classified(S, "internal", "MIN001", msg);
    case MZ_ZIP_TOO_MANY_FILES:
    case MZ_ZIP_FILE_TOO_LARGE:
    case MZ_ZIP_UNSUPPORTED_CDIR_SIZE:
        /* The zip64 entry-count and central-directory ceilings throw
         * limit, never truncate (D7). */
        snprintf(msg, sizeof(msg),
                 "%s: archive exceeds the entry or directory-size ceiling",
                 who);
        return prim_throw_classified(S, "codec/limit", ZIP_MGC_LIMIT, msg);
    case MZ_ZIP_UNSUPPORTED_MULTIDISK:
    case MZ_ZIP_UNSUPPORTED_FEATURE:
        snprintf(msg, sizeof(msg), "%s: unsupported archive feature", who);
        return prim_throw_classified(S, "codec/unsupported",
                                     ZIP_MGC_UNSUPPORTED, msg);
    default:
        break;
    }

    if (!zip_find_eocd(data, len, &eocd_ofs)) {
        if (len >= 4 && memcmp(data, ZIP_LOC_SIG, 4) == 0) {
            /* Starts like a zip, ends without an EOCD: the tail,
             * central directory included, was cut away. */
            snprintf(msg, sizeof(msg), "%s: archive is truncated (no "
                     "end-of-central-directory record)", who);
            return prim_throw_classified(S, "codec/truncated",
                                         ZIP_MGC_TRUNCATED, msg);
        }
        snprintf(msg, sizeof(msg),
                 "%s: not a zip (no end-of-central-directory signature)",
                 who);
        return prim_throw_classified(S, "codec/magic", ZIP_MGC_MAGIC, msg);
    }

    if (eocd_ofs + ZIP_EOCD_REC_LEN > len) {
        /* The signature sits too close to the end: the record itself
         * is cut. */
        snprintf(msg, sizeof(msg), "%s: end-of-central-directory record "
                 "is truncated", who);
        return prim_throw_classified(S, "codec/truncated",
                                     ZIP_MGC_TRUNCATED, msg);
    }
    if (err == MZ_ZIP_INVALID_HEADER_OR_CORRUPTED) {
        cd_size = zip_le32(data + eocd_ofs + 12);
        cd_ofs = zip_le32(data + eocd_ofs + 16);
        if (cd_ofs > len || cd_size > len - cd_ofs) {
            snprintf(msg, sizeof(msg),
                     "%s: central directory is truncated", who);
            return prim_throw_classified(S, "codec/truncated",
                                         ZIP_MGC_TRUNCATED, msg);
        }
    }
    snprintf(msg, sizeof(msg), "%s: archive is corrupt", who);
    return prim_throw_classified(S, "codec/corrupt", ZIP_MGC_CORRUPT, msg);
}

/* Init the vendor memory reader in archive order; on failure throws
 * the classified init error and returns -1. */
static int zip_reader_open(mino_state *S, mz_zip_archive *zip,
                           const unsigned char *data, size_t len,
                           const char *who)
{
    memset(zip, 0, sizeof(*zip));
    if (!mz_zip_reader_init_mem(zip, data, len,
                                MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY)) {
        zip_init_error(S, zip, data, len, who);
        return -1;
    }
    return 0;
}

/* ---- central directory walk (D5, D6) ---- */

/* Walks the central directory at zip->m_central_directory_file_ofs
 * in archive order, one CDH per call, handing back the raw time,
 * date, and bit-flag words and the raw name bytes the vendor stat
 * struct truncates and localtime-converts away. Pure: no throws, no
 * allocation (the caller owns cleanup and classification). The
 * vendor has already validated the whole directory before the first
 * call, so every check here is defense in depth for mutated bytes. */
typedef struct {
    const unsigned char *cd;  /* directory start inside the archive */
    size_t cap;               /* bytes from cd to the archive end */
    size_t ofs;               /* running offset of the next CDH */
} zip_cdh_walk;

static int zip_cdh_walk_init(const mz_zip_archive *zip, zip_cdh_walk *w,
                             const unsigned char *data, size_t len)
{
    mz_uint64 cd_ofs = zip->m_central_directory_file_ofs;
    if (cd_ofs > len) return -1;
    w->cd = data + (size_t)cd_ofs;
    w->cap = len - (size_t)cd_ofs;
    w->ofs = 0;
    return 0;
}

static int zip_cdh_next(zip_cdh_walk *w, unsigned *dos_time,
                        unsigned *dos_date, unsigned *bit_flag,
                        const unsigned char **name, size_t *name_len)
{
    size_t nl, xl, cl;
    const unsigned char *p;

    if (w->ofs + ZIP_CDH_FIXED_LEN > w->cap) return -1;
    p = w->cd + w->ofs;
    if (p[0] != 0x50 || p[1] != 0x4b || p[2] != 0x01 || p[3] != 0x02)
        return -1;
    nl = zip_le16(p + 28);
    xl = zip_le16(p + 30);
    cl = zip_le16(p + 32);
    if (w->ofs + ZIP_CDH_FIXED_LEN + nl + xl + cl > w->cap) return -1;
    *dos_time = zip_le16(p + 12);
    *dos_date = zip_le16(p + 14);
    *bit_flag = zip_le16(p + 8);
    *name = p + ZIP_CDH_FIXED_LEN;
    *name_len = nl;
    w->ofs += ZIP_CDH_FIXED_LEN + nl + xl + cl;
    return 0;
}

/* ---- D6 name decoding ---- */

/* Decode one raw name into dst (capacity >= 3 * name_len): bit 11
 * set passes the claimed-UTF-8 bytes through unchanged (mino
 * strings are byte strings; invalid sequences stay byte-identical),
 * else each byte expands through the CP437 table, ASCII inline.
 * Returns the decoded length. */
static size_t zip_decode_name(unsigned char *dst, const unsigned char *name,
                              size_t name_len, unsigned bit_flag)
{
    size_t i, n = 0;

    if (bit_flag & 0x800) {
        memcpy(dst, name, name_len);
        return name_len;
    }
    for (i = 0; i < name_len; i++) {
        unsigned char b = name[i];
        if (b < 0x80) {
            dst[n++] = b;
        } else {
            const char *u = k_zip_cp437_high[b - 0x80];
            while (*u != '\0') dst[n++] = (unsigned char)*u++;
        }
    }
    return n;
}

/* ---- D5 timestamp decoding ---- */

/* Days from 1970-01-01 for a proleptic Gregorian civil date (the
 * Hinnant era formula), pure integer arithmetic, no localtime. */
static long long zip_days_from_civil(long long y, unsigned m, unsigned d)
{
    long long era;
    unsigned yoe, doy, doe;

    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned)(y - era * 400);
    doy = (153u * (m + (m > 2 ? (unsigned)-3 : 9u)) + 2u) / 5u + d - 1u;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

/* CDH time/date words to epoch seconds (UTC civil fields), nil at
 * the DOS minimum 1980-01-01 00:00 and for a zero date word. */
static mino_val *zip_mtime(mino_state *S, unsigned dos_time,
                           unsigned dos_date)
{
    long long days;

    if (dos_date == 0) return mino_nil(S);
    if (dos_date == 0x0021 && dos_time == 0) return mino_nil(S);
    days = zip_days_from_civil(1980 + (long long)(dos_date >> 9),
                               (dos_date >> 5) & 0xF, dos_date & 0x1F);
    return mino_int(S, days * 86400
                          + (long long)(dos_time >> 11) * 3600
                          + (long long)((dos_time >> 5) & 0x3F) * 60
                          + (long long)((dos_time & 0x1F) * 2));
}

/* ---- the read-side entry map ---- */

/* Build the eight-key map from a vendor stat plus the decoded name
 * and raw flag words. Only called with the reader already closed,
 * so throwing is safe. */
static mino_val *zip_entry_map(mino_state *S,
                               const mz_zip_archive_file_stat *st,
                               const unsigned char *name, size_t name_len,
                               unsigned bit_flag, unsigned dos_time,
                               unsigned dos_date)
{
    mino_val *ks[8], *vs[8];
    unsigned char *scratch;
    size_t clen, dn;
    char msg[128];

    /* A declared size above LLONG_MAX can never be materialized on
     * this platform; the ceiling throws limit rather than wraps
     * (D7). */
    if (st->m_uncomp_size > (mz_uint64)LLONG_MAX
        || st->m_comp_size > (mz_uint64)LLONG_MAX) {
        snprintf(msg, sizeof(msg),
                 "zip-entries: declared size exceeds the addressable range");
        return prim_throw_classified(S, "codec/limit", ZIP_MGC_LIMIT, msg);
    }

    ks[0] = mino_keyword(S, "name");
    vs[0] = mino_string_n(S, (const char *)name, name_len);
    ks[1] = mino_keyword(S, "size");
    vs[1] = mino_int(S, (long long)st->m_uncomp_size);
    ks[2] = mino_keyword(S, "compressed-size");
    vs[2] = mino_int(S, (long long)st->m_comp_size);
    ks[3] = mino_keyword(S, "crc32");
    vs[3] = mino_int(S, (long long)st->m_crc32);
    ks[4] = mino_keyword(S, "method");
    if (st->m_method == 0) vs[4] = mino_keyword(S, "store");
    else if (st->m_method == 8) vs[4] = mino_keyword(S, "deflate");
    else vs[4] = mino_int(S, (long long)st->m_method);
    ks[5] = mino_keyword(S, "mtime");
    vs[5] = zip_mtime(S, dos_time, dos_date);
    ks[6] = mino_keyword(S, "directory?");
    vs[6] = st->m_is_directory ? mino_true(S) : mino_false(S);
    ks[7] = mino_keyword(S, "comment");
    {
        /* The vendor stat copies the comment zero-terminated (capped
         * at 511 bytes); decode through the same rule as the name. */
        clen = strlen(st->m_comment);
        scratch = (unsigned char *)malloc(clen * 3 + 1);
        if (scratch == NULL)
            return prim_throw_classified(S, "internal", "MIN001",
                                         "zip-entries: out of memory");
        dn = zip_decode_name(scratch, (const unsigned char *)st->m_comment,
                             clen, bit_flag);
        vs[7] = mino_string_n(S, (const char *)scratch, dn);
        free(scratch);
    }
    return mino_map(S, ks, vs, 8);
}

/* Map a captured vendor extract error onto the family. The vendor's
 * memory path reports CRC mismatches as CRC_CHECK_FAILED; everything
 * unclassified is corrupt, never :internal. */
static mino_val *zip_extract_error(mino_state *S, mz_zip_error err,
                                   const char *who)
{
    char msg[128];

    switch (err) {
    case MZ_ZIP_ALLOC_FAILED:
        snprintf(msg, sizeof(msg), "%s: out of memory", who);
        return prim_throw_classified(S, "internal", "MIN001", msg);
    case MZ_ZIP_UNSUPPORTED_METHOD:
    case MZ_ZIP_UNSUPPORTED_ENCRYPTION:
    case MZ_ZIP_UNSUPPORTED_FEATURE:
        snprintf(msg, sizeof(msg), "%s: entry uses an unsupported feature",
                 who);
        return prim_throw_classified(S, "codec/unsupported",
                                     ZIP_MGC_UNSUPPORTED, msg);
    case MZ_ZIP_CRC_CHECK_FAILED:
        snprintf(msg, sizeof(msg), "%s: CRC-32 mismatch", who);
        return prim_throw_classified(S, "codec/crc", ZIP_MGC_CRC, msg);
    default:
        snprintf(msg, sizeof(msg), "%s: entry is corrupt", who);
        return prim_throw_classified(S, "codec/corrupt", ZIP_MGC_CORRUPT,
                                     msg);
    }
}

/* ---- the prims ---- */

/* (zip-entries data) -- vector of read-side entry maps in archive
 * order, from the central directory. data is bytes or a string (its
 * UTF-8 bytes). Pass one collects stats and CDH words with the
 * reader open but never throws; pass two builds the maps after the
 * reader is closed. */
static mino_val *prim_zip_entries(mino_state *S, mino_val *args,
                                  mino_env *env)
{
    const unsigned char *data;
    size_t len;
    mz_zip_archive zip;
    mz_uint32 i, total;
    mz_zip_archive_file_stat *stats = NULL;
    unsigned *words = NULL;       /* 4 slots per entry: time date flag _ */
    size_t *name_ofs = NULL, *name_len = NULL;
    const unsigned char *name;
    size_t nlen, dn, scratch_cap = 0;
    unsigned dos_time, dos_date, bit_flag;
    unsigned char *scratch = NULL;
    zip_cdh_walk walk;
    mino_vec_builder *out;
    mino_val *result;
    char msg[128];
    (void)env;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        prim_throw_classified(S, "eval/arity", "MAR001",
                              "zip-entries takes one argument");
        return NULL;
    }
    if (zip_data_arg(S, args->as.cons.car, "zip-entries", &data, &len) != 0)
        return NULL;
    if (zip_reader_open(S, &zip, data, len, "zip-entries") != 0) return NULL;

    total = zip.m_total_files;
    if (total != 0) {
        stats = (mz_zip_archive_file_stat *)malloc(
            (size_t)total * sizeof(*stats));
        words = (unsigned *)malloc((size_t)total * 4 * sizeof(unsigned));
        name_ofs = (size_t *)malloc((size_t)total * sizeof(size_t));
        name_len = (size_t *)malloc((size_t)total * sizeof(size_t));
        if (stats == NULL || words == NULL || name_ofs == NULL
            || name_len == NULL) {
            mz_zip_reader_end(&zip);
            free(stats); free(words); free(name_ofs); free(name_len);
            snprintf(msg, sizeof(msg), "zip-entries: out of memory");
            return prim_throw_classified(S, "internal", "MIN001", msg);
        }
    }
    if (zip_cdh_walk_init(&zip, &walk, data, len) != 0) {
        mz_zip_reader_end(&zip);
        free(stats); free(words); free(name_ofs); free(name_len);
        snprintf(msg, sizeof(msg), "zip-entries: archive is corrupt");
        return prim_throw_classified(S, "codec/corrupt", ZIP_MGC_CORRUPT,
                                     msg);
    }

    for (i = 0; i < total; i++) {
        if (!mz_zip_reader_file_stat(&zip, i, &stats[i])) {
            mz_zip_reader_end(&zip);
            free(stats); free(words); free(name_ofs); free(name_len);
            snprintf(msg, sizeof(msg), "zip-entries: archive is corrupt");
            return prim_throw_classified(S, "codec/corrupt",
                                         ZIP_MGC_CORRUPT, msg);
        }
        if (zip_cdh_next(&walk, &dos_time, &dos_date, &bit_flag,
                         &name, &nlen) != 0) {
            mz_zip_reader_end(&zip);
            free(stats); free(words); free(name_ofs); free(name_len);
            snprintf(msg, sizeof(msg), "zip-entries: archive is corrupt");
            return prim_throw_classified(S, "codec/corrupt",
                                         ZIP_MGC_CORRUPT, msg);
        }
        words[i * 4] = dos_time;
        words[i * 4 + 1] = dos_date;
        words[i * 4 + 2] = bit_flag;
        name_ofs[i] = (size_t)(name - data);
        name_len[i] = nlen;
    }
    mz_zip_reader_end(&zip);

    out = mino_vector_builder_new(S);
    /* Init non-NULL: the grow branch below is conditional on the
     * capacity, and the analyzer cannot correlate the two across
     * iterations; a provably live 1-byte buffer keeps every decoded
     * name off a NULL path. */
    scratch = (unsigned char *)malloc(1);
    if (scratch == NULL) {
        free(stats); free(words); free(name_ofs); free(name_len);
        snprintf(msg, sizeof(msg), "zip-entries: out of memory");
        return prim_throw_classified(S, "internal", "MIN001", msg);
    }
    for (i = 0; i < total; i++) {
        mino_val *entry;

        if (name_len[i] * 3 + 1 > scratch_cap) {
            unsigned char *grown =
                (unsigned char *)malloc(name_len[i] * 3 + 1);
            if (grown == NULL) {
                free(scratch);
                free(stats); free(words); free(name_ofs); free(name_len);
                snprintf(msg, sizeof(msg), "zip-entries: out of memory");
                return prim_throw_classified(S, "internal", "MIN001", msg);
            }
            free(scratch);
            scratch = grown;
            scratch_cap = name_len[i] * 3 + 1;
        }
        dn = zip_decode_name(scratch, data + name_ofs[i], name_len[i],
                             words[i * 4 + 2]);
        entry = zip_entry_map(S, &stats[i], scratch, dn, words[i * 4 + 2],
                              words[i * 4], words[i * 4 + 1]);
        if (entry == NULL) {
            free(scratch);
            free(stats); free(words); free(name_ofs); free(name_len);
            return NULL;
        }
        mino_vector_builder_push(out, entry);
    }
    free(scratch);
    free(stats); free(words); free(name_ofs); free(name_len);
    result = mino_vector_builder_finish(out);
    return result;
}

/* (zip-read data name opts?) -- one entry's bytes. name matches the
 * DECODED entry name (the same decoding zip-entries listed, R5);
 * the first central-directory match wins (D8). Opts:
 * {:max-bytes 64 MiB}, checked against the declared size BEFORE any
 * inflation or allocation. */
static mino_val *prim_zip_read(mino_state *S, mino_val *args, mino_env *env)
{
    const unsigned char *data;
    size_t len, max_out, target_len, nlen, scratch_cap = 0, dn, out_len;
    const char *target;
    mino_val *name_val, *opts = NULL, *result;
    mz_zip_archive zip;
    mz_uint32 i, total;
    mz_zip_archive_file_stat st;
    zip_cdh_walk walk;
    unsigned dos_time, dos_date, bit_flag;
    const unsigned char *name;
    unsigned char *scratch = NULL;
    mz_zip_error err;
    void *out;
    int have = 0;
    char msg[160];
    (void)env;

    if (!mino_is_cons(args)) {
        prim_throw_classified(S, "eval/arity", "MAR001",
                              "zip-read takes two or three arguments");
        return NULL;
    }
    if (zip_data_arg(S, args->as.cons.car, "zip-read", &data, &len) != 0)
        return NULL;
    args = args->as.cons.cdr;
    if (!mino_is_cons(args)) {
        prim_throw_classified(S, "eval/arity", "MAR001",
                              "zip-read takes two or three arguments");
        return NULL;
    }
    name_val = args->as.cons.car;
    if (name_val == NULL || !mino_is_string(name_val)) {
        prim_throw_classified(S, "eval/type", "MTY001",
                              "zip-read: name must be a string");
        return NULL;
    }
    target = name_val->as.s.data;
    target_len = name_val->as.s.len;
    args = args->as.cons.cdr;
    if (mino_is_cons(args)) {
        opts = args->as.cons.car;
        if (mino_is_cons(args->as.cons.cdr)) {
            prim_throw_classified(S, "eval/arity", "MAR001",
                                  "zip-read takes two or three arguments");
            return NULL;
        }
        if (opts != NULL && mino_type_of(opts) != MINO_MAP
            && mino_type_of(opts) != MINO_NIL) {
            prim_throw_classified(S, "eval/type", "MTY001",
                                  "zip-read: opts must be a map");
            return NULL;
        }
    }
    if (zip_max_bytes_opt(S, opts, "zip-read", &max_out) != 0) return NULL;

    if (zip_reader_open(S, &zip, data, len, "zip-read") != 0) return NULL;
    total = zip.m_total_files;
    if (zip_cdh_walk_init(&zip, &walk, data, len) != 0) {
        mz_zip_reader_end(&zip);
        snprintf(msg, sizeof(msg), "zip-read: archive is corrupt");
        return prim_throw_classified(S, "codec/corrupt", ZIP_MGC_CORRUPT,
                                     msg);
    }

    /* First-match decoded-name lookup (D8): every candidate name
     * decodes through the same rule the listing used. The scratch
     * starts provably live (1 byte) so no modeled path passes NULL
     * into the decode; the grow branch is conditional on capacity. */
    scratch = (unsigned char *)malloc(1);
    if (scratch == NULL) {
        mz_zip_reader_end(&zip);
        snprintf(msg, sizeof(msg), "zip-read: out of memory");
        return prim_throw_classified(S, "internal", "MIN001", msg);
    }
    for (i = 0; i < total; i++) {
        if (zip_cdh_next(&walk, &dos_time, &dos_date, &bit_flag,
                         &name, &nlen) != 0) {
            mz_zip_reader_end(&zip);
            free(scratch);
            snprintf(msg, sizeof(msg), "zip-read: archive is corrupt");
            return prim_throw_classified(S, "codec/corrupt",
                                         ZIP_MGC_CORRUPT, msg);
        }
        if (nlen * 3 + 1 > scratch_cap) {
            unsigned char *grown = (unsigned char *)malloc(nlen * 3 + 1);
            if (grown == NULL) {
                mz_zip_reader_end(&zip);
                free(scratch);
                snprintf(msg, sizeof(msg), "zip-read: out of memory");
                return prim_throw_classified(S, "internal", "MIN001", msg);
            }
            free(scratch);
            scratch = grown;
            scratch_cap = nlen * 3 + 1;
        }
        dn = zip_decode_name(scratch, name, nlen, bit_flag);
        if (dn == target_len && memcmp(scratch, target, dn) == 0) {
            have = 1;
            break;
        }
    }
    if (!have) {
        mz_zip_reader_end(&zip);
        free(scratch);
        snprintf(msg, sizeof(msg), "zip-read: no entry named \"%s\"",
                 target);
        return prim_throw_classified(S, "codec/missing", ZIP_MGC_MISSING,
                                     msg);
    }

    if (!mz_zip_reader_file_stat(&zip, i, &st)) {
        err = mz_zip_get_last_error(&zip);
        mz_zip_reader_end(&zip);
        free(scratch);
        return zip_extract_error(S, err, "zip-read");
    }
    /* Encrypted entries and methods other than store/deflate are
     * rejected before any allocation (MGC007). */
    if (st.m_is_encrypted) {
        mz_zip_reader_end(&zip);
        free(scratch);
        snprintf(msg, sizeof(msg), "zip-read: entry is encrypted");
        return prim_throw_classified(S, "codec/unsupported",
                                     ZIP_MGC_UNSUPPORTED, msg);
    }
    if (st.m_method != 0 && st.m_method != 8) {
        mz_zip_reader_end(&zip);
        free(scratch);
        snprintf(msg, sizeof(msg),
                 "zip-read: compression method %u is not supported",
                 (unsigned)st.m_method);
        return prim_throw_classified(S, "codec/unsupported",
                                     ZIP_MGC_UNSUPPORTED, msg);
    }
    /* The bomb cap: the central-directory declared size is checked
     * against :max-bytes BEFORE any inflation or allocation. */
    if (st.m_uncomp_size > (mz_uint64)max_out) {
        mz_zip_reader_end(&zip);
        free(scratch);
        snprintf(msg, sizeof(msg),
                 "zip-read: entry declares %llu bytes, over the %lu byte "
                 "cap", (unsigned long long)st.m_uncomp_size,
                 (unsigned long)max_out);
        return prim_throw_classified(S, "codec/limit", ZIP_MGC_LIMIT, msg);
    }

    out = mz_zip_reader_extract_to_heap(&zip, i, &out_len, 0);
    if (out == NULL) {
        err = mz_zip_get_last_error(&zip);
        mz_zip_reader_end(&zip);
        free(scratch);
        return zip_extract_error(S, err, "zip-read");
    }
    mz_zip_reader_end(&zip);
    free(scratch);
    result = mino_bytes(S, (const unsigned char *)out, out_len);
    free(out);
    return result;
}

const mino_prim_def k_prims_archive[] = {
    {"zip-entries", prim_zip_entries,
     "Lists a zip archive's entries as a vector of maps with the keys "
     "{:name :size :compressed-size :crc32 :method :mtime :directory? "
     ":comment}, in archive order, from the central directory. Names "
     "decode as UTF-8 when the entry sets the language-encoding flag, "
     "else as CP437; :mtime is epoch seconds, nil at the DOS minimum "
     "1980-01-01; :method is :store, :deflate, or the raw integer "
     "code. The archive is bytes or a string (its UTF-8 bytes). Zip "
     "read is untrusted input: nothing is written to any filesystem."},
    {"zip-read", prim_zip_read,
     "Returns one zip entry's bytes: the FIRST central-directory entry "
     "whose decoded name equals name (the same decoding zip-entries "
     "listed). Throws :codec/missing when no entry matches, "
     ":codec/unsupported for encrypted entries and compression methods "
     "other than store and deflate. Opts: {:max-bytes 67108864}; the "
     "declared entry size is checked against :max-bytes BEFORE any "
     "inflation (:codec/limit), and the CRC-32 is verified "
     "(:codec/crc). The archive is bytes or a string."},
};

const size_t k_prims_archive_count =
    sizeof(k_prims_archive) / sizeof(k_prims_archive[0]);
