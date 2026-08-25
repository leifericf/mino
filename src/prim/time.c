/*
 * time.c -- wall-clock reads, epoch-ms <-> broken-down-map conversion,
 * and the civil calendar core they run on.
 *
 * Vocabulary (ADR 21): an instant is epoch milliseconds since
 * 1970-01-01T00:00:00Z as a 64-bit integer; broken-down time is a
 * plain map {:year :month :day :hour :min :sec :ms :wday :offset-min}
 * with 1-based months and a computed :wday (0 = Sunday). The
 * representable range is years 1..9999; anything outside throws
 * :time/range. Offsets are fixed minutes east of UTC, data in the
 * map, applied by the converters. There is no named-zone database
 * and no locale anywhere.
 *
 * The civil math is the pure-integer core proven by the time-date
 * spike: Howard Hinnant's published algorithms (chrono-Compatible
 * Low-Level Date Algorithms) implemented from the papers, with
 * musl's __secs_to_tm / __tm_to_secs read as a cross-check
 * reference. No third-party code is copied, so no license entries
 * change.
 *
 * Clocks: `now` / `now-s` are the wall clock (gettimeofday /
 * GetSystemTimeAsFileTime); `cpu-ms` is process CPU time (clock() /
 * GetProcessTimes). The monotonic clock is NOT here: `nano-time`
 * (io.c) already owns it, and conflating wall and monotonic reads
 * is the classic os.clock footgun this module exists to avoid.
 *
 * Conversion validation is strict (ADR 21): unknown map keys throw,
 * field ranges throw, a supplied :wday that contradicts the date
 * throws, out-of-range results throw. Malformed input never
 * normalizes silently the way C mktime does.
 *
 * Prims are ungated: reading a clock is info-only and cheap, the
 * same stance as Janet's os/time. The sandbox preset loses nothing
 * that mutates or exfiltrates.
 */

#define _POSIX_C_SOURCE 200809L

#include "prim/internal.h"
#include "mino.h"
#include "tzdata_blob.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <sys/time.h>
#  include <time.h>
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- civil core (spike-proven; keep the math identical) ------------- */

/* days since 1970-01-01 from proleptic Gregorian y-m-d */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    int64_t yy = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    unsigned mp = (5u * doy + 2u) / 153u;
    *d = doy - (153u * mp + 2u) / 5u + 1u;
    *m = mp + (mp < 10u ? 3u : (unsigned)-9);
    *y = yy + (*m <= 2u);
}

static unsigned weekday_from_days(int64_t z)
{
    return (unsigned)(((z % 7) + 11) % 7); /* 0=Sunday */
}

static int is_leap(int64_t y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static const unsigned char k_dim[12] =
    {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

static unsigned days_in_month(int64_t y, unsigned m)
{
    if (m == 2 && is_leap(y)) return 29;
    return k_dim[m - 1];
}

/* broken-down fields (1-based, 0=Sunday weekday) at UTC */
typedef struct {
    int64_t year;
    unsigned month, day, hour, min, sec, wday;
} civil_tm;

static void broken_from_secs(int64_t t, civil_tm *out)
{
    int64_t days = t / 86400;
    int64_t rem = t % 86400;
    if (rem < 0) { rem += 86400; days -= 1; }
    civil_from_days(days, &out->year, &out->month, &out->day);
    out->hour = (unsigned)(rem / 3600);
    out->min = (unsigned)(rem / 60 % 60);
    out->sec = (unsigned)(rem % 60);
    out->wday = weekday_from_days(days);
}

static int64_t secs_from_broken(const civil_tm *tm)
{
    return 86400LL * days_from_civil(tm->year, tm->month, tm->day)
         + 3600LL * tm->hour + 60LL * tm->min + tm->sec;
}

/* representable range: years 1..9999 in epoch-ms */
#define TIME_MS_MIN (-62135596800000LL)  /* 0001-01-01T00:00:00.000Z */
#define TIME_MS_MAX (253402300799999LL)  /* 9999-12-31T23:59:59.999Z */
#define TIME_OFF_MIN (-1439)             /* minutes: -23:59 */
#define TIME_OFF_MAX (1439)

/* ---- argument helpers ----------------------------------------------- */

static mino_val *time_throw(mino_state *S, const char *kind, const char *code,
                            const char *msg)
{
    return prim_throw_classified(S, kind, code, msg);
}

/* strict integer extraction: ints only, floats and bignums reject */
static int time_arg_ll(mino_state *S, const mino_val *v, const char *who,
                       const char *field, long long *out)
{
    if (v == NULL || mino_type_of((mino_val *)v) != MINO_INT
        || !as_long((mino_val *)v, out)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s: %s must be an integer", who, field);
        time_throw(S, "eval/type", "MTY001", msg);
        return 0;
    }
    return 1;
}

/* ---- timezone database (ADR 27) -------------------------------------- */

static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q -= 1;
    return q;
}

/* The blob layout is fixed by src/vendor/tzdata/tools/gen_tzdata.clj:
 * u32le magic, u32le n_zones, u32le n_streams; zone table (name-sorted)
 * of {u32 name_off, u32 stream_idx}; stream table of {u32 types_off,
 * u32 trans_off, u32 footer_off, u16 n_types, u16 n_trans, u8
 * init_type, u8 pad} (18 bytes per entry); then NUL-terminated names, NUL-terminated POSIX footer
 * strings, then per stream i32le offset tables and sign-extended
 * 40-bit little-endian absolute transition seconds with a parallel
 * u8 type-index array. All lookups are length-bounded; names are
 * NUL-terminated inside the blob, so bounded memcmp never reads
 * past mino_tzdata_blob_size. */

static uint32_t tz_u32le(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t tz_u16le(const unsigned char *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int32_t tz_i32le(const unsigned char *p)
{
    return (int32_t)tz_u32le(p);
}

static int64_t tz_i40le(const unsigned char *p)
{
    uint64_t v = (uint64_t)p[0] | ((uint64_t)p[1] << 8)
               | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24)
               | ((uint64_t)p[4] << 32);
    if (v & 0x8000000000ULL) v |= 0xFFFFFF0000000000ULL;
    return (int64_t)v;
}

typedef struct {
    const unsigned char *types;   /* i32le gmtoff seconds */
    const unsigned char *trans;   /* i40le UTC seconds */
    const unsigned char *tidx;    /* u8 type indices */
    const char *footer;           /* POSIX TZ string, "" when none */
    uint32_t n_types, n_trans;
    uint8_t init_type;
} tz_zone;

static uint32_t tz_zone_count(void)
{
    return tz_u32le(mino_tzdata_blob + 4);
}

static const char *tz_name_at(uint32_t off, size_t *len)
{
    const unsigned char *end = mino_tzdata_blob + mino_tzdata_blob_size;
    const unsigned char *p = mino_tzdata_blob + off;
    const unsigned char *q = p;
    while (q < end && *q != 0) q++;
    *len = (size_t)(q - p);
    return (const char *)p;
}

static int tz_name_cmp(const char *q, size_t qlen, uint32_t off)
{
    size_t nlen;
    const char *nm = tz_name_at(off, &nlen);
    size_t m = qlen < nlen ? qlen : nlen;
    int c = memcmp(q, nm, m);
    if (c != 0) return c;
    if (qlen < nlen) return -1;
    if (qlen > nlen) return 1;
    return 0;
}

/* bounded name lookup: binary search over the sorted zone table.
 * Returns 0 and fills z, or -1 when the name is unknown. */
static int tz_find(const char *q, size_t qlen, tz_zone *z)
{
    uint32_t lo = 0, hi = tz_zone_count();
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const unsigned char *ze = mino_tzdata_blob + 12 + (size_t)mid * 8;
        int c = tz_name_cmp(q, qlen, tz_u32le(ze));
        if (c == 0) {
            uint32_t sid = tz_u32le(ze + 4);
            const unsigned char *se = mino_tzdata_blob + 12
                + (size_t)tz_zone_count() * 8 + (size_t)sid * 18;
            size_t flen;
            z->types = mino_tzdata_blob + tz_u32le(se);
            z->trans = mino_tzdata_blob + tz_u32le(se + 4);
            z->n_types = tz_u16le(se + 12);
            z->n_trans = tz_u16le(se + 14);
            z->init_type = se[16];
            z->tidx = z->trans + (size_t)z->n_trans * 5;
            z->footer = tz_name_at(tz_u32le(se + 8), &flen);
            return 0;
        }
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return -1;
}

static int32_t tz_type_off(const tz_zone *z, unsigned i)
{
    return tz_i32le(z->types + (size_t)i * 4);
}

static int64_t tz_trans_at(const tz_zone *z, unsigned i)
{
    return tz_i40le(z->trans + (size_t)i * 5);
}

static uint8_t tz_trans_type(const tz_zone *z, unsigned i)
{
    return z->tidx[i];
}

/* ---- POSIX TZ footer evaluation -------------------------------------- */

typedef struct {
    int32_t std_off, dst_off;    /* UTC seconds (sign corrected) */
    int has_dst;
    char kind[2];                /* 'M', 'J', or 'D' (day count) */
    int m[2], w[2], d[2], n[2];  /* M rules: m w d; J/D rules: n */
    int32_t time[2];             /* seconds within the frame day */
    char frame[2];               /* 'w' all, 's' std, 'u' utc */
} tz_footer;

/* [+-]hh[:mm[:ss]]; POSIX sign is inverted (EST5 is UTC-5). */
static int tz_parse_tz_off(const char **p, int32_t *out)
{
    const char *s = *p;
    int neg = 0, any = 0;
    int32_t v = 0;
    if (*s == '+' || *s == '-') { neg = *s == '-'; s++; }
    for (int part = 0; part < 3; part++) {
        int32_t n = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            n = n * 10 + (*s - '0');
            s++;
            digits++;
            if (digits > 3) return -1;
        }
        if (digits == 0) return part == 0 ? -1 : 0;
        any = 1;
        v += part == 0 ? n * 3600 : (part == 1 ? n * 60 : n);
        if (*s != ':') break;
        s++;
    }
    if (!any) return -1;
    *out = neg ? v : -v;
    *p = s;
    return 0;
}

/* rule time: [+-]hh[:mm[:ss]] with an optional w/s/u/g/z frame
 * suffix; default 02:00:00 wall. Sign is literal (not the POSIX
 * inverted offset convention); hours may exceed 24 (up to 167). */
static int tz_parse_rule_time(const char **p, int32_t *out, char *frame)
{
    const char *s = *p;
    int neg = 0, any = 0;
    int32_t v = 0;
    *frame = 'w';
    if (*s != '/') {
        *out = 2 * 3600;
        return 0;
    }
    s++;
    if (*s == '+' || *s == '-') {
        neg = *s == '-';
        s++;
    }
    for (int part = 0; part < 3; part++) {
        int32_t n = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            n = n * 10 + (*s - '0');
            s++;
            if (++digits > 3) return -1;
        }
        if (digits == 0) return part == 0 ? -1 : 0;
        any = 1;
        v += part == 0 ? n * 3600 : (part == 1 ? n * 60 : n);
        if (*s != ':') break;
        s++;
    }
    if (!any) return -1;
    if (*s == 'w' || *s == 's' || *s == 'u' || *s == 'g' || *s == 'z') {
        *frame = (*s == 'g' || *s == 'z') ? 'u' : *s;
        s++;
    }
    *out = neg ? -v : v;
    *p = s;
    return 0;
}

static int tz_skip_name(const char **p)
{
    const char *s = *p;
    if (*s == '<') {
        s++;
        while (*s != '\0' && *s != '>') s++;
        if (*s != '>') return -1;
        s++;
    } else {
        while ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) s++;
        if (s == *p) return -1;
    }
    *p = s;
    return 0;
}

/* Parse a POSIX TZ footer (RFC 8536 3.3.1). Returns 0 or -1. */
static int tz_parse_footer(const char *s, tz_footer *f)
{
    f->has_dst = 0;
    f->std_off = 0;
    f->dst_off = 0;
    if (tz_skip_name(&s) != 0) return -1;
    if (tz_parse_tz_off(&s, &f->std_off) != 0) return -1;
    if (*s == '\0' || *s == ',') {
        /* std-only footer (a trailing comma with nothing else is
         * malformed, but tolerate it as std-only) */
        return 0;
    }
    if (tz_skip_name(&s) != 0) return -1;
    f->has_dst = 1;
    f->dst_off = f->std_off + 3600;
    if (*s != ',' && *s != '\0') {
        if (tz_parse_tz_off(&s, &f->dst_off) != 0) return -1;
    }
    for (int r = 0; r < 2; r++) {
        if (*s != ',') return -1;
        s++;
        if (*s == 'M') {
            s++;
            f->kind[r] = 'M';
            /* Mm.w.d: the month may be one or two digits, the week
             * and day are single digits */
            int mo = 0, mdig = 0;
            while (*s >= '0' && *s <= '9') {
                mo = mo * 10 + (*s - '0');
                s++;
                if (++mdig > 2) return -1;
            }
            if (mdig == 0 || *s != '.') return -1;
            s++;
            if (!(*s >= '0' && *s <= '9')) return -1;
            f->w[r] = *s++ - '0';
            if (*s != '.') return -1;
            s++;
            if (!(*s >= '0' && *s <= '9')) return -1;
            f->d[r] = *s++ - '0';
            f->m[r] = mo;
            if (f->m[r] < 1 || f->m[r] > 12 || f->w[r] < 1 || f->w[r] > 5
                || f->d[r] > 6) return -1;
        } else if (*s == 'J') {
            s++;
            f->kind[r] = 'J';
            int n = 0, digits = 0;
            while (*s >= '0' && *s <= '9') {
                n = n * 10 + (*s - '0');
                s++;
                if (++digits > 3) return -1;
            }
            if (digits == 0 || n < 1 || n > 365) return -1;
            f->n[r] = n;
        } else {
            f->kind[r] = 'D';
            int n = 0, digits = 0;
            while (*s >= '0' && *s <= '9') {
                n = n * 10 + (*s - '0');
                s++;
                if (++digits > 3) return -1;
            }
            if (digits == 0 || n > 365) return -1;
            f->n[r] = n;
        }
        if (tz_parse_rule_time(&s, &f->time[r], &f->frame[r]) != 0)
            return -1;
    }
    if (*s != '\0') return -1;
    return 0;
}

/* rule r's instant in year y, as UTC seconds */
static int64_t tz_rule_utc(const tz_footer *f, int r, int64_t y)
{
    int64_t days;
    if (f->kind[r] == 'M') {
        unsigned dim = days_in_month(y, (unsigned)f->m[r]);
        int64_t first = days_from_civil(y, (unsigned)f->m[r], 1);
        unsigned wd1 = weekday_from_days(first);
        unsigned day = 1u + (((unsigned)f->d[r] + 7u - wd1) % 7u);
        if (f->w[r] < 5) {
            day += 7u * (unsigned)(f->w[r] - 1);
        } else {
            while (day + 7u <= dim) day += 7u;
        }
        days = days_from_civil(y, (unsigned)f->m[r], (int)day);
    } else if (f->kind[r] == 'J') {
        int64_t n = f->n[r];
        if (is_leap(y) && n >= 60) n += 1;
        days = days_from_civil(y, 1, 1) + n - 1;
    } else {
        days = days_from_civil(y, 1, 1) + f->n[r];
    }
    int64_t local = days * 86400 + f->time[r];
    int32_t fr;
    if (f->frame[r] == 'u') fr = 0;
    else if (f->frame[r] == 's') fr = f->std_off;
    else fr = (r == 0) ? f->std_off : f->dst_off;
    return local - fr;
}

static int tz_footer_dst_at(const tz_footer *f, int64_t t)
{
    if (!f->has_dst) return 0;
    int64_t y;
    unsigned mm, dd;
    civil_from_days(floor_div(t + f->std_off, 86400), &y, &mm, &dd);
    for (int64_t yy = y - 1; yy <= y + 1; yy++) {
        int64_t s = tz_rule_utc(f, 0, yy);
        int64_t e = tz_rule_utc(f, 1, yy);
        int64_t end = (s < e) ? e : tz_rule_utc(f, 1, yy + 1);
        if (s <= t && t < end) return 1;
    }
    return 0;
}

/* UTC offset (seconds) at UTC instant t */
static int32_t tz_off_at(const tz_zone *z, int64_t t)
{
    if (z->n_trans > 0) {
        if (t < tz_trans_at(z, 0))
            return tz_type_off(z, z->init_type);
        if (t < tz_trans_at(z, z->n_trans - 1)) {
            uint32_t lo = 0, hi = z->n_trans - 1;
            while (lo < hi) {
                uint32_t mid = lo + (hi - lo + 1) / 2;
                if (tz_trans_at(z, mid) <= t) lo = mid; else hi = mid - 1;
            }
            return tz_type_off(z, tz_trans_type(z, lo));
        }
    }
    if (z->footer[0] != '\0') {
        tz_footer f;
        if (tz_parse_footer(z->footer, &f) == 0)
            return tz_footer_dst_at(&f, t) ? f.dst_off : f.std_off;
    }
    if (z->n_trans > 0)
        return tz_type_off(z, tz_trans_type(z, z->n_trans - 1));
    return tz_type_off(z, z->init_type);
}

/* offset seconds -> minutes, rounded to nearest (ADR 27's documented
 * granularity: sub-minute historical LMT offsets approximate) */
static int tz_round_min(int32_t secs)
{
    return secs < 0 ? (int)-(((-secs) + 30) / 60) : (int)((secs + 30) / 60);
}

/* Local wall seconds -> UTC seconds and the offset minutes used,
 * python-zoneinfo fold-0 semantics: a fall-back overlap resolves to
 * the first occurrence (the pre-transition offset), a spring-forward
 * gap maps forward using the pre-transition offset. */
static void tz_local_to_utc(const tz_zone *z, int64_t l, int64_t *e,
                            int *off_min)
{
    int64_t use; /* offset seconds */
    if (z->n_trans == 0) {
        /* no transitions: the footer's fixed std offset */
        use = 0;
        if (z->footer[0] != '\0') {
            tz_footer f;
            if (tz_parse_footer(z->footer, &f) == 0) use = f.std_off;
        }
    } else if (l < tz_trans_at(z, 0) + tz_type_off(z, z->init_type)) {
        use = tz_type_off(z, z->init_type);
    } else if (l >= tz_trans_at(z, z->n_trans - 1)
                     + tz_type_off(z, tz_trans_type(z, z->n_trans - 1))) {
        /* footer era: try the larger offset first so overlaps pick
         * the earlier instant; gaps fall back to std */
        int have = 0;
        use = 0;
        if (z->footer[0] != '\0') {
            tz_footer f;
            if (tz_parse_footer(z->footer, &f) == 0) {
                have = 1;
                use = f.std_off;
                if (f.has_dst) {
                    int32_t c0 = f.dst_off, c1 = f.std_off;
                    if (c0 < c1) {
                        int32_t sw = c0;
                        c0 = c1;
                        c1 = sw;
                    }
                    for (int i = 0; i < 2; i++) {
                        int32_t ci = (i == 0) ? c0 : c1;
                        if (tz_off_at(z, l - ci) == ci) {
                            use = ci;
                            break;
                        }
                    }
                }
            }
        }
        if (!have)
            use = tz_type_off(z, tz_trans_type(z, z->n_trans - 1));
    } else {
        /* largest k with local_start_k <= l */
        uint32_t lo = 0, hi = z->n_trans - 1;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo + 1) / 2;
            if (tz_trans_at(z, mid)
                + tz_type_off(z, tz_trans_type(z, mid)) <= l)
                lo = mid;
            else
                hi = mid - 1;
        }
        uint32_t k = lo;
        if (k >= 1 && l < tz_trans_at(z, k)
                          + tz_type_off(z, tz_trans_type(z, k - 1))) {
            use = tz_type_off(z, tz_trans_type(z, k - 1)); /* overlap */
        } else {
            use = tz_type_off(z, tz_trans_type(z, k)); /* normal/gap */
        }
    }
    *off_min = tz_round_min((int32_t)use);
    *e = l - (int64_t)*off_min * 60;
}

/* ---- zone arguments and the :zone option ---------------------------- */

/* A zone argument: an integer is a fixed offset in minutes (the ADR
 * 21 arithmetic path); a string or keyword is an IANA name looked up
 * in the blob. Returns 1 (fixed, minutes in *fixed), 0 (named, *z),
 * or throws. */
static int time_zone_arg(mino_state *S, mino_val *v, const char *who,
                         tz_zone *z, long long *fixed)
{
    if (v != NULL && mino_type_of(v) == MINO_INT) {
        long long m;
        if (!as_long(v, &m)) return -1;
        if (m < TIME_OFF_MIN || m > TIME_OFF_MAX) {
            char msg[96];
            snprintf(msg, sizeof(msg),
                     "%s: :zone offset %lld exceeds 23:59", who, m);
            time_throw(S, "time/field", "MTF001", msg);
            return -1;
        }
        *fixed = m;
        return 1;
    }
    if (v != NULL && (mino_type_of(v) == MINO_STRING
                      || mino_type_of(v) == MINO_KEYWORD)) {
        const char *nm;
        size_t len;
        if (mino_type_of(v) == MINO_STRING) {
            nm = v->as.s.data;
            len = v->as.s.len;
        } else if (!mino_to_keyword(v, &nm, &len)) {
            return -1;
        }
        if (tz_find(nm, len, z) != 0) {
            char msg[96];
            snprintf(msg, sizeof(msg),
                     "%s: unknown time zone \"%.32s\"", who, nm);
            time_throw(S, "time/zone", "MTZ001", msg);
            return -1;
        }
        return 0;
    }
    time_throw(S, "eval/type", "MTY001",
               ":zone must be an offset in minutes or a zone name");
    return -1;
}

/* Offset in minutes at instant ms for a resolved zone argument. */
static long long time_zone_offset(const tz_zone *z, long long fixed,
                                  int is_fixed, long long ms)
{
    if (is_fixed) return fixed;
    return tz_round_min(tz_off_at(z, floor_div(ms, 1000)));
}

/* Parse an option map for {:zone ...}; only :zone is an option.
 * Returns 1 (zone present), 0 (no zone), or throws (-1). */
static int time_zone_opt(mino_state *S, mino_val *opts, const char *who,
                         tz_zone *z, long long *fixed, int *is_fixed)
{
    size_t n, i;
    if (opts == NULL || mino_type_of(opts) != MINO_MAP) {
        time_throw(S, "eval/type", "MTY001",
                   "options must be a {:zone ...} map");
        return -1;
    }
    n = opts->as.map.len;
    for (i = 0; i < n; i++) {
        mino_val *k = vec_nth(opts->as.map.key_order, i);
        if (k != mino_keyword(S, "zone")) {
            const char *kd;
            size_t klen;
            char msg[96];
            if (k != NULL && mino_to_keyword(k, &kd, &klen)) {
                snprintf(msg, sizeof(msg),
                         "%s: unknown option :%.*s (only :zone)",
                         who, (int)klen, kd);
            } else {
                snprintf(msg, sizeof(msg),
                         "%s: option keys must be keywords", who);
            }
            time_throw(S, "time/field", "MTF001", msg);
            return -1;
        }
    }
    {
        mino_val *v = map_get_val(opts, mino_keyword(S, "zone"));
        if (v == NULL || mino_is_nil(v)) return 0;
        int r = time_zone_arg(S, v, who, z, fixed);
        if (r < 0) return -1;
        *is_fixed = r;
        return 1;
    }
}

/* ---- date parsing (spike-proven) ------------------------------------ */

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int nn(const char **p, int n, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < n; i++) {
        if (!is_digit(**p)) return -1;
        v = v * 10u + (unsigned)(**p - '0');
        (*p)++;
    }
    *out = v;
    return 0;
}

/* every reject site reports the byte it stopped at */
static int perr(const char *s, const char *p, int *errpos)
{
    if (errpos != NULL) *errpos = (int)(p - s);
    return -1;
}

/* case-insensitive fixed-word match; advances p past the word */
static int match_ci(const char **p, const char *word)
{
    size_t n = strlen(word);
    for (size_t i = 0; i < n; i++) {
        char c = (*p)[i];
        char w = word[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (w >= 'A' && w <= 'Z') w = (char)(w - 'A' + 'a');
        if (c != w) return 0;
    }
    *p += n;
    return 1;
}

static const char *const k_wd[] =
    {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
static const char *const k_mo[] =
    {"Jan","Feb","Mar","Apr","May","Jun",
     "Jul","Aug","Sep","Oct","Nov","Dec"};

/* ISO 8601 / RFC 3339:
   YYYY-MM-DD
   YYYY-MM-DD[Tt ]HH:MM[:SS[.frac]][Zz|+HH:MM|+-HHMM]
   Fractional seconds preserved to milliseconds (truncated past 3
   digits). sec 60 (leap second) folds to 59; 61 rejects. Years
   1..9999 only. date_only flags a bare date. has_off flags an
   explicit Z or numeric offset in the input. */
static int parse_iso8601(const char *s, int64_t *ms, int *offset_min,
                          int *date_only, int *has_off, int *errpos)
{
    const char *p = s;
    unsigned y, mo, d, h = 0, mi = 0, sec = 0, msec = 0;
    int off = 0;
    *date_only = 0;
    *has_off = 0;
    if (nn(&p, 4, &y)) return perr(s, p, errpos);
    if (y < 1) return perr(s, p, errpos);
    if (*p != '-') return perr(s, p, errpos);
    p++;
    if (nn(&p, 2, &mo)) return perr(s, p, errpos);
    if (*p != '-') return perr(s, p, errpos);
    p++;
    if (nn(&p, 2, &d)) return perr(s, p, errpos);
    if (*p == 'T' || *p == 't' || *p == ' ') {
        p++;
        if (nn(&p, 2, &h)) return perr(s, p, errpos);
        if (*p != ':') return perr(s, p, errpos);
        p++;
        if (nn(&p, 2, &mi)) return perr(s, p, errpos);
        if (*p == ':') {
            p++;
            if (nn(&p, 2, &sec)) return perr(s, p, errpos);
            if (sec == 60) sec = 59;      /* leap second folds */
            else if (sec > 59) return perr(s, p, errpos);
            if (*p == '.') {              /* fraction: keep 3 digits */
                p++;
                if (!is_digit(*p)) return perr(s, p, errpos);
                int digits = 0;
                unsigned frac = 0;
                while (is_digit(*p)) {
                    if (digits < 3) frac = frac * 10u + (unsigned)(*p - '0');
                    digits++;
                    p++;
                }
                if (digits == 1) msec = frac * 100u;
                else if (digits == 2) msec = frac * 10u;
                else msec = frac;
            }
        }
        if (*p == 'Z' || *p == 'z') {
            p++;
            off = 0;
            *has_off = 1;
        } else if (*p == '+' || *p == '-') {
            int sign = *p == '-' ? -1 : 1;
            p++;
            unsigned oh, om = 0;
            if (nn(&p, 2, &oh)) return perr(s, p, errpos);
            if (*p == ':') {
                p++;
                if (nn(&p, 2, &om)) return perr(s, p, errpos);
            } else if (is_digit(*p)) {
                if (nn(&p, 2, &om)) return perr(s, p, errpos);
            }
            if (oh > 23 || om > 59) return perr(s, p, errpos);
            off = sign * (int)(oh * 60u + om);
            *has_off = 1;
        }
        /* no offset: naive local read as UTC; offset 0 reported */
    } else {
        *date_only = 1;
    }
    if (*p != '\0') return perr(s, p, errpos);
    if (mo < 1 || mo > 12) return perr(s, p, errpos);
    if (d < 1 || d > days_in_month((int64_t)y, mo)) {
        return perr(s, p, errpos);
    }
    if (h > 23 || mi > 59) return perr(s, p, errpos);
    {
        civil_tm tm = {(int64_t)y, mo, d, h, mi, sec, 0};
        int64_t secs = secs_from_broken(&tm) - (int64_t)off * 60;
        int64_t total = secs * 1000 + (int64_t)msec;
        if (total < TIME_MS_MIN || total > TIME_MS_MAX) {
            return perr(s, p, errpos);
        }
        *ms = total;
    }
    *offset_min = off;
    return 0;
}

/* RFC 1123 / RFC 2822 comma form:
   [Dow, ]D[D] Mon YYYY HH:MM[:SS] (GMT|UT|UTC|+-HHMM)
   Month, day, and zone names case-insensitive (RFC 2822 4.3). Day
   1-2 digits; year exactly 4 digits. The day name, when present,
   must match the LOCAL (offset-shifted) date. alphabetic_zone flags
   GMT/UT/UTC so the caller can report :rfc1123 vs :rfc2822. */
static int parse_rfc2822(const char *s, int64_t *ms, int *offset_min,
                         int *alphabetic_zone, int *errpos)
{
    const char *p = s;
    unsigned d, y, h, mi, sec = 0;
    int mo = -1, off = 0, dow = -1;
    *alphabetic_zone = 0;
    for (int i = 0; i < 7; i++) {
        if (match_ci(&p, k_wd[i])) {
            if (p[0] != ',') return perr(s, p, errpos);
            p++;
            while (*p == ' ') p++;
            dow = i;
            break;
        }
    }
    if (!is_digit(*p)) return perr(s, p, errpos);
    d = (unsigned)(*p++ - '0');
    if (is_digit(*p)) d = d * 10u + (unsigned)(*p++ - '0');
    if (*p != ' ') return perr(s, p, errpos);
    p++;
    for (int i = 0; i < 12; i++) {
        if (match_ci(&p, k_mo[i])) { mo = i + 1; break; }
    }
    if (mo < 0) return perr(s, p, errpos);
    if (*p != ' ') return perr(s, p, errpos);
    p++;
    if (nn(&p, 4, &y)) return perr(s, p, errpos);
    if (y < 1) return perr(s, p, errpos);
    if (*p != ' ') return perr(s, p, errpos);
    p++;
    if (nn(&p, 2, &h)) return perr(s, p, errpos);
    if (*p != ':') return perr(s, p, errpos);
    p++;
    if (nn(&p, 2, &mi)) return perr(s, p, errpos);
    if (*p == ':') {
        p++;
        if (nn(&p, 2, &sec)) return perr(s, p, errpos);
        if (sec == 60) sec = 59;
        else if (sec > 59) return perr(s, p, errpos);
    }
    if (*p != ' ') return perr(s, p, errpos);
    p++;
    if (match_ci(&p, "gmt") || match_ci(&p, "utc")
        || match_ci(&p, "ut")) {
        off = 0;
        *alphabetic_zone = 1;
    } else if (*p == '+' || *p == '-') {
        int sign = *p == '-' ? -1 : 1;
        p++;
        unsigned oh, om;
        if (nn(&p, 2, &oh)) return perr(s, p, errpos);
        if (nn(&p, 2, &om)) return perr(s, p, errpos);
        if (oh > 23 || om > 59) return perr(s, p, errpos);
        off = sign * (int)(oh * 60u + om);
    } else {
        return perr(s, p, errpos);
    }
    if (*p != '\0') return perr(s, p, errpos);
    if (h > 23 || mi > 59) return perr(s, p, errpos);
    if (d < 1 || d > days_in_month((int64_t)y, (unsigned)mo)) {
        return perr(s, p, errpos);
    }
    {
        civil_tm tm = {(int64_t)y, (unsigned)mo, d, h, mi, sec, 0};
        int64_t secs = secs_from_broken(&tm) - (int64_t)off * 60;
        if (secs < TIME_MS_MIN / 1000 || secs > TIME_MS_MAX / 1000) {
            return perr(s, p, errpos);
        }
        if (dow >= 0) {
            /* day name describes the LOCAL date, not UTC */
            int64_t local = secs + (int64_t)off * 60;
            int64_t days = local / 86400;
            if (local % 86400 < 0) days -= 1;
            if ((int)weekday_from_days(days) != dow) {
                return perr(s, p, errpos);
            }
        }
        *ms = secs * 1000;
    }
    *offset_min = off;
    return 0;
}

/* ---- date formatting (spike-proven) --------------------------------- */

static char *put_nn(char *o, unsigned v, int n)
{
    for (int i = n - 1; i >= 0; i--) {
        o[i] = (char)('0' + v % 10u);
        v /= 10u;
    }
    return o + n;
}

/* ISO 8601 datetime; .SSS only when the millisecond part is nonzero */
static int format_iso8601(int64_t ms, int offset_min, char *buf,
                          size_t cap)
{
    if (ms < TIME_MS_MIN || ms > TIME_MS_MAX) return -1;
    if (cap < 35) return -1;
    long long local = ms + (long long)offset_min * 60000;
    long long secs = local / 1000;
    int msec = (int)(local % 1000);
    if (msec < 0) { msec += 1000; secs -= 1; }
    civil_tm tm;
    broken_from_secs(secs, &tm);
    /* the offset shift can push the local date outside years 1..9999 */
    if (tm.year < 1 || tm.year > 9999) return -1;
    char *o = buf;
    o = put_nn(o, (unsigned)tm.year, 4);
    *o++ = '-';
    o = put_nn(o, tm.month, 2);
    *o++ = '-';
    o = put_nn(o, tm.day, 2);
    *o++ = 'T';
    o = put_nn(o, tm.hour, 2);
    *o++ = ':';
    o = put_nn(o, tm.min, 2);
    *o++ = ':';
    o = put_nn(o, tm.sec, 2);
    if (msec != 0) {
        *o++ = '.';
        o = put_nn(o, (unsigned)msec, 3);
    }
    if (offset_min == 0) {
        *o++ = 'Z';
    } else {
        int a = offset_min < 0 ? -offset_min : offset_min;
        *o++ = offset_min < 0 ? '-' : '+';
        o = put_nn(o, (unsigned)(a / 60), 2);
        *o++ = ':';
        o = put_nn(o, (unsigned)(a % 60), 2);
    }
    *o = '\0';
    return 0;
}

/* ISO 8601 date-only (UTC unless an offset shifts the local date) */
static int format_iso8601_date(int64_t ms, int offset_min, char *buf,
                               size_t cap)
{
    if (ms < TIME_MS_MIN || ms > TIME_MS_MAX) return -1;
    if (cap < 11) return -1;
    civil_tm tm;
    long long local = ms + (long long)offset_min * 60000;
    long long secs = local / 1000;
    if (local < 0 && local % 1000 != 0) secs -= 1; /* floor div */
    broken_from_secs(secs, &tm);
    if (tm.year < 1 || tm.year > 9999) return -1;
    char *o = buf;
    o = put_nn(o, (unsigned)tm.year, 4);
    *o++ = '-';
    o = put_nn(o, tm.month, 2);
    *o++ = '-';
    o = put_nn(o, tm.day, 2);
    *o = '\0';
    return 0;
}

/* RFC 1123 (HTTP Date): always GMT */
static int format_rfc1123(int64_t ms, char *buf, size_t cap)
{
    if (ms < TIME_MS_MIN || ms > TIME_MS_MAX) return -1;
    if (cap < 30) return -1;
    civil_tm tm;
    broken_from_secs(ms / 1000
                     - (ms < 0 && ms % 1000 != 0 ? 1 : 0), &tm);
    const char *wd = k_wd[tm.wday % 7u];
    const char *mo = k_mo[tm.month - 1u];
    char *o = buf;
    memcpy(o, wd, 3); o += 3;
    *o++ = ','; *o++ = ' ';
    o = put_nn(o, tm.day, 2);
    *o++ = ' ';
    memcpy(o, mo, 3); o += 3;
    *o++ = ' ';
    o = put_nn(o, (unsigned)tm.year, 4);
    *o++ = ' ';
    o = put_nn(o, tm.hour, 2); *o++ = ':';
    o = put_nn(o, tm.min, 2);  *o++ = ':';
    o = put_nn(o, tm.sec, 2);
    *o++ = ' '; *o++ = 'G'; *o++ = 'M'; *o++ = 'T';
    *o = '\0';
    return 0;
}

/* RFC 2822: numeric offset zone */
static int format_rfc2822(int64_t ms, int offset_min, char *buf,
                          size_t cap)
{
    if (ms < TIME_MS_MIN || ms > TIME_MS_MAX) return -1;
    if (cap < 32) return -1;
    long long local = ms + (long long)offset_min * 60000;
    long long secs = local / 1000;
    if (local < 0 && local % 1000 != 0) secs -= 1; /* floor div */
    civil_tm tm;
    broken_from_secs(secs, &tm);
    if (tm.year < 1 || tm.year > 9999) return -1;
    const char *wd = k_wd[tm.wday % 7u];
    const char *mo = k_mo[tm.month - 1u];
    char *o = buf;
    memcpy(o, wd, 3); o += 3;
    *o++ = ','; *o++ = ' ';
    o = put_nn(o, tm.day, 2);
    *o++ = ' ';
    memcpy(o, mo, 3); o += 3;
    *o++ = ' ';
    o = put_nn(o, (unsigned)tm.year, 4);
    *o++ = ' ';
    o = put_nn(o, tm.hour, 2); *o++ = ':';
    o = put_nn(o, tm.min, 2);  *o++ = ':';
    o = put_nn(o, tm.sec, 2);
    *o++ = ' ';
    if (offset_min == 0) {
        *o++ = '+';
        o = put_nn(o, 0, 4);
    } else {
        int a = offset_min < 0 ? -offset_min : offset_min;
        *o++ = offset_min < 0 ? '-' : '+';
        o = put_nn(o, (unsigned)(a / 60), 2);
        o = put_nn(o, (unsigned)(a % 60), 2);
    }
    *o = '\0';
    return 0;
}

/* ---- format-time prim ------------------------------------------------ */

/* (format-time ms fmt? offset-min? | opts?) -> string. fmt: :iso8601
 * (default), :iso8601-date, :rfc1123, :rfc2822. An argument position
 * that would hold the fmt keyword or the offset integer instead
 * accepts an options map {:zone z}: the zone's offset at ms renders
 * the offset-capable forms. */
static mino_val *prim_format_time(mino_state *S, mino_val *args,
                                  mino_env *env)
{
    mino_val *av[3] = {NULL};
    size_t n;
    long long ms, off = 0;
    mino_val *fmt;
    char buf[40];
    int rc;
    char msg[80];
    tz_zone z;
    long long fixed = 0;
    int is_fixed = 0, have_zone = 0;
    (void)env;

    if (!arg_count(S, args, &n) || n < 1 || n > 3) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "format-time takes one to three "
                                     "arguments");
    }
    av[0] = args->as.cons.car;
    if (n >= 2) av[1] = args->as.cons.cdr->as.cons.car;
    if (n == 3) av[2] = args->as.cons.cdr->as.cons.cdr->as.cons.car;
    if (!time_arg_ll(S, av[0], "format-time", "epoch-ms", &ms))
        return NULL;
    fmt = (n >= 2) ? av[1] : NULL;
    if (fmt != NULL && mino_type_of(fmt) == MINO_MAP) {
        int r = time_zone_opt(S, fmt, "format-time", &z, &fixed,
                              &is_fixed);
        if (r < 0) return NULL;
        if (r == 1) {
            have_zone = 1;
            off = time_zone_offset(&z, fixed, is_fixed, ms);
        }
        fmt = NULL;
    }
    if (n == 3) {
        if (av[2] != NULL && mino_type_of(av[2]) == MINO_MAP) {
            int r;
            if (have_zone) {
                return time_throw(S, "time/field", "MTF001",
                                  "format-time: one :zone option only");
            }
            r = time_zone_opt(S, av[2], "format-time", &z, &fixed,
                              &is_fixed);
            if (r < 0) return NULL;
            if (r == 1) {
                have_zone = 1;
                off = time_zone_offset(&z, fixed, is_fixed, ms);
            }
        } else if (!time_arg_ll(S, av[2], "format-time", "offset-min",
                                &off)) {
            return NULL;
        }
    }
    if (ms < TIME_MS_MIN || ms > TIME_MS_MAX) {
        snprintf(msg, sizeof(msg),
                 "format-time: %lld is outside years 1..9999", ms);
        return time_throw(S, "time/range", "MTR001", msg);
    }
    if (off < TIME_OFF_MIN || off > TIME_OFF_MAX) {
        snprintf(msg, sizeof(msg),
                 "format-time: offset %lld exceeds 23:59", off);
        return time_throw(S, "time/field", "MTF001", msg);
    }

    if (fmt == NULL || fmt == mino_keyword(S, "iso8601")) {
        rc = format_iso8601(ms, (int)off, buf, sizeof(buf));
    } else if (fmt == mino_keyword(S, "iso8601-date")) {
        rc = format_iso8601_date(ms, (int)off, buf, sizeof(buf));
    } else if (fmt == mino_keyword(S, "rfc1123")) {
        if (n == 3 || have_zone) {
            return time_throw(S, "time/field", "MTF001",
                              "format-time: :rfc1123 is always GMT; an "
                              "offset or zone argument is not accepted");
        }
        rc = format_rfc1123(ms, buf, sizeof(buf));
    } else if (fmt == mino_keyword(S, "rfc2822")) {
        rc = format_rfc2822(ms, (int)off, buf, sizeof(buf));
    } else {
        return time_throw(S, "time/field", "MTF001",
                          "format-time: fmt must be :iso8601, "
                          ":iso8601-date, :rfc1123, or :rfc2822");
    }
    if (rc != 0) {
        /* the formatters fail only when the offset shift pushes the
         * local date outside years 1..9999 (buffers are sized above
         * the format maxima), so this is a range error, not internal */
        return time_throw(S, "time/range", "MTR001",
                          "format-time: offset shifts the date outside "
                          "years 1..9999");
    }
    return mino_string_n(S, buf, strlen(buf));
}

/* ---- zone-offset-mins prim ------------------------------------------- */

/* (zone-offset-mins zone ms) -> the zone's UTC offset in minutes at
 * the instant. zone is an IANA name (string or keyword) or a fixed
 * offset in minutes, which passes through unchanged. Unknown names
 * throw :time/zone carrying the name. */
static mino_val *prim_zone_offset_mins(mino_state *S, mino_val *args,
                                       mino_env *env)
{
    mino_val *zv, *mv;
    tz_zone z;
    long long fixed = 0, ms;
    int r;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "zone-offset-mins takes two "
                                     "arguments");
    }
    zv = args->as.cons.car;
    mv = args->as.cons.cdr->as.cons.car;
    if (!time_arg_ll(S, mv, "zone-offset-mins", "epoch-ms", &ms))
        return NULL;
    if (ms < TIME_MS_MIN || ms > TIME_MS_MAX) {
        return time_throw(S, "time/range", "MTR001",
                          "zone-offset-mins: epoch-ms outside years "
                          "1..9999");
    }
    r = time_zone_arg(S, zv, "zone-offset-mins", &z, &fixed);
    if (r < 0) return NULL;
    return mino_int(S, time_zone_offset(&z, fixed, r, ms));
}

/* ---- parse-time prim ------------------------------------------------- */

/* (parse-time s opts?) -> {:epoch-ms :offset-min :format :date-only?}
 * opts: {:zone z} interprets an offset-less input (naive datetime
 * or date-only) as local wall time in z; an input carrying its own
 * offset together with :zone is a :time/field conflict. */
static mino_val *prim_parse_time(mino_state *S, mino_val *args,
                                 mino_env *env)
{
    mino_val *v, *opts = NULL, *keys[4], *vals[4], *m;
    int pinned = 0;
    const char *s;
    size_t len;
    int64_t ms;
    int off, date_only = 0, az = 0, errpos = 0, has_off = 0;
    const char *family;
    char msg[96];
    tz_zone z;
    long long fixed = 0;
    int is_fixed = 0, have_zone = 0;
    (void)env;

    if (!mino_is_cons(args)
        || (mino_is_cons(args->as.cons.cdr)
            && mino_is_cons(args->as.cons.cdr->as.cons.cdr))) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "parse-time requires one or two "
                                     "arguments");
    }
    v = args->as.cons.car;
    if (mino_is_cons(args->as.cons.cdr)) {
        opts = args->as.cons.cdr->as.cons.car;
        have_zone = time_zone_opt(S, opts, "parse-time", &z, &fixed,
                                  &is_fixed);
        if (have_zone < 0) return NULL;
    }
    if (v == NULL || mino_type_of(v) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "parse-time: argument must be a "
                                     "string");
    }
    s = v->as.s.data;
    len = v->as.s.len;
    if (len > 64) {
        return time_throw(S, "time/parse", "MTP001",
                          "parse-time: input longer than 64 characters");
    }
    if (strlen(s) != len) {
        /* embedded NUL would truncate the parse at the wrong place;
         * the string's bytes after the NUL must not be ignored */
        return time_throw(S, "time/parse", "MTP001",
                          "parse-time: input contains a NUL byte");
    }

    /* dispatch: 4 digits then '-' is ISO; a leading alphabetic run is
     * the RFC comma form; anything else tries ISO first for the
     * better error position. */
    if (len >= 5 && is_digit(s[0]) && is_digit(s[1]) && is_digit(s[2])
        && is_digit(s[3]) && s[4] == '-') {
        family = "ISO 8601";
        if (parse_iso8601(s, &ms, &off, &date_only, &has_off, &errpos)) {
            snprintf(msg, sizeof(msg),
                     "parse-time: invalid ISO 8601 date at byte %d",
                     errpos);
            return time_throw(S, "time/parse", "MTP001", msg);
        }
    } else {
        family = "RFC 1123/2822";
        if (parse_rfc2822(s, &ms, &off, &az, &errpos)) {
            int errpos2 = 0;
            if (parse_iso8601(s, &ms, &off, &date_only, &has_off,
                              &errpos2) == 0) {
                family = "ISO 8601";
            } else {
                snprintf(msg, sizeof(msg),
                         "parse-time: invalid RFC 1123/2822 date at "
                         "byte %d", errpos);
                return time_throw(S, "time/parse", "MTP001", msg);
            }
        }
    }

    if (have_zone) {
        if (has_off || family[0] == 'R') {
            return time_throw(S, "time/field", "MTF001",
                              "parse-time: input carries its own "
                              "offset; :zone applies to offset-less "
                              "inputs");
        }
        if (!is_fixed) {
            int64_t local_secs = floor_div(ms, 1000);
            int64_t msec = ms - local_secs * 1000;
            int64_t e;
            int offm;
            tz_local_to_utc(&z, local_secs, &e, &offm);
            ms = e * 1000 + msec;
            off = offm;
        } else {
            off = (int)fixed;
            ms -= (int64_t)fixed * 60000;
        }
        if (ms < TIME_MS_MIN || ms > TIME_MS_MAX) {
            return time_throw(S, "time/range", "MTR001",
                              "parse-time: result outside years "
                              "1..9999");
        }
    }

    keys[0] = mino_keyword(S, "epoch-ms");   gc_pin(keys[0]); pinned++;
    vals[0] = mino_int(S, ms);               gc_pin(vals[0]); pinned++;
    keys[1] = mino_keyword(S, "offset-min"); gc_pin(keys[1]); pinned++;
    vals[1] = mino_int(S, off);              gc_pin(vals[1]); pinned++;
    keys[2] = mino_keyword(S, "format");     gc_pin(keys[2]); pinned++;
    vals[2] = mino_keyword(S, (family[0] == 'I')
                                  ? "iso8601"
                                  : (az ? "rfc1123" : "rfc2822"));
    gc_pin(vals[2]); pinned++;
    keys[3] = mino_keyword(S, "date-only?"); gc_pin(keys[3]); pinned++;
    vals[3] = date_only ? mino_true(S) : mino_false(S);

    m = mino_map(S, keys, vals, 4);
    gc_unpin(pinned);
    return m;
}

/* ---- clocks ---------------------------------------------------------- */

static long long time_wall_ms(void)
{
#ifdef _WIN32
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (long long)(u.QuadPart / 10000) - 11644473600000LL;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

static long long time_cpu_ms(void)
{
#ifdef _WIN32
    FILETIME crea, exit_, kern, user;
    ULARGE_INTEGER uk, uu;
    if (!GetProcessTimes(GetCurrentProcess(), &crea, &exit_, &kern, &user)) {
        return 0;
    }
    uk.LowPart = kern.dwLowDateTime; uk.HighPart = kern.dwHighDateTime;
    uu.LowPart = user.dwLowDateTime; uu.HighPart = user.dwHighDateTime;
    return (long long)((uk.QuadPart + uu.QuadPart) / 10000);
#else
    clock_t t = clock();
    return ((long long)t * 1000) / CLOCKS_PER_SEC;
#endif
}

/* (now) -- wall clock, epoch milliseconds. */
static mino_val *prim_now(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "now takes no arguments");
    }
    return mino_int(S, time_wall_ms());
}

/* (now-s) -- wall clock, epoch seconds. */
static mino_val *prim_now_s(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "now-s takes no arguments");
    }
    return mino_int(S, time_wall_ms() / 1000);
}

/* (cpu-ms) -- process CPU time in milliseconds. */
static mino_val *prim_cpu_ms(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "cpu-ms takes no arguments");
    }
    return mino_int(S, time_cpu_ms());
}

/* ---- epoch-ms <-> time map ------------------------------------------ */

/* (epoch->time-map ms offset-min? | opts?) -> plain map. The map
 * always carries :offset-min so the conversion round-trips as data.
 * opts: {:zone z} renders at the zone's offset at that instant
 * (z may be a fixed offset in minutes or an IANA name). */
static mino_val *prim_epoch_to_time_map(mino_state *S, mino_val *args,
                                        mino_env *env)
{
    mino_val *av[2] = {NULL};
    size_t n;
    long long ms, off = 0;
    civil_tm tm;
    mino_val *keys[9], *vals[9], *m;
    int pinned = 0;
    char msg[80];
    tz_zone z;
    long long fixed = 0;
    int is_fixed = 0;
    (void)env;

    if (!arg_count(S, args, &n) || n < 1 || n > 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "epoch->time-map takes one or two "
                                     "arguments");
    }
    av[0] = args->as.cons.car;
    if (n == 2) av[1] = args->as.cons.cdr->as.cons.car;
    if (!time_arg_ll(S, av[0], "epoch->time-map", "epoch-ms", &ms))
        return NULL;
    if (n == 2) {
        if (av[1] != NULL && mino_type_of(av[1]) == MINO_MAP) {
            int r = time_zone_opt(S, av[1], "epoch->time-map", &z,
                                  &fixed, &is_fixed);
            if (r < 0) return NULL;
            if (r == 1)
                off = time_zone_offset(&z, fixed, is_fixed, ms);
        } else if (!time_arg_ll(S, av[1], "epoch->time-map", "offset-min",
                                &off)) {
            return NULL;
        }
    }
    if (ms < TIME_MS_MIN || ms > TIME_MS_MAX) {
        snprintf(msg, sizeof(msg),
                 "epoch->time-map: %lld is outside years 1..9999",
                 ms);
        return time_throw(S, "time/range", "MTR001", msg);
    }
    if (off < TIME_OFF_MIN || off > TIME_OFF_MAX) {
        snprintf(msg, sizeof(msg),
                 "epoch->time-map: offset %lld exceeds 23:59", off);
        return time_throw(S, "time/field", "MTF001", msg);
    }

    long long local = ms + off * 60000;
    long long secs = local / 1000;
    int msec = (int)(local % 1000);
    if (msec < 0) { msec += 1000; secs -= 1; }
    broken_from_secs(secs, &tm);
    /* the offset shift can push the local date outside years 1..9999 */
    if (tm.year < 1 || tm.year > 9999) {
        snprintf(msg, sizeof(msg),
                 "epoch->time-map: offset %lld shifts the date outside "
                 "years 1..9999", off);
        return time_throw(S, "time/range", "MTR001", msg);
    }

    keys[0] = mino_keyword(S, "year");        gc_pin(keys[0]); pinned++;
    vals[0] = mino_int(S, tm.year);           gc_pin(vals[0]); pinned++;
    keys[1] = mino_keyword(S, "month");       gc_pin(keys[1]); pinned++;
    vals[1] = mino_int(S, tm.month);          gc_pin(vals[1]); pinned++;
    keys[2] = mino_keyword(S, "day");         gc_pin(keys[2]); pinned++;
    vals[2] = mino_int(S, tm.day);            gc_pin(vals[2]); pinned++;
    keys[3] = mino_keyword(S, "hour");        gc_pin(keys[3]); pinned++;
    vals[3] = mino_int(S, tm.hour);           gc_pin(vals[3]); pinned++;
    keys[4] = mino_keyword(S, "min");         gc_pin(keys[4]); pinned++;
    vals[4] = mino_int(S, tm.min);            gc_pin(vals[4]); pinned++;
    keys[5] = mino_keyword(S, "sec");         gc_pin(keys[5]); pinned++;
    vals[5] = mino_int(S, tm.sec);            gc_pin(vals[5]); pinned++;
    keys[6] = mino_keyword(S, "ms");          gc_pin(keys[6]); pinned++;
    vals[6] = mino_int(S, msec);              gc_pin(vals[6]); pinned++;
    keys[7] = mino_keyword(S, "wday");        gc_pin(keys[7]); pinned++;
    vals[7] = mino_int(S, tm.wday);           gc_pin(vals[7]); pinned++;
    keys[8] = mino_keyword(S, "offset-min");  gc_pin(keys[8]); pinned++;
    vals[8] = mino_int(S, off);               gc_pin(vals[8]); pinned++;

    m = mino_map(S, keys, vals, 9);
    gc_unpin(pinned);
    return m;
}

/* time-map->epoch field reader: known keys only, strict validation */
static int tm_field(mino_state *S, const mino_val *m, const char *name,
                    int required, long long *out)
{
    mino_val *v = map_get_val(m, mino_keyword(S, name));
    char msg[96];
    if (v == NULL || mino_is_nil(v)) {
        if (required) {
            snprintf(msg, sizeof(msg),
                     "time-map->epoch: missing required field :%s", name);
            time_throw(S, "time/field", "MTF001", msg);
            return 0;
        }
        *out = 0;
        return 1;
    }
    if (!time_arg_ll(S, v, "time-map->epoch", name, out)) return 0;
    return 1;
}

/* (time-map->epoch m) -> epoch-ms. Strict: required :year :month :day;
 * optional :hour :min :sec :ms :wday :offset-min; unknown keys throw;
 * :wday, when present, must match the computed weekday. */
static mino_val *prim_time_map_to_epoch(mino_state *S, mino_val *args,
                                        mino_env *env)
{
    mino_val *m;
    long long y, mo, d, h, mi, sec, msec, off, wday;
    int has_wday = 0;
    size_t n, i;
    civil_tm tm;
    long long total;
    char msg[112];
    tz_zone z;
    long long fixed = 0;
    int is_fixed = 0, have_zone = 0;
    (void)env;

    if (!mino_is_cons(args) || args->as.cons.car == NULL
        || mino_type_of(args->as.cons.car) != MINO_MAP) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "time-map->epoch requires one map "
                                     "argument");
    }
    m = args->as.cons.car;
    if (mino_is_cons(args->as.cons.cdr)) {
        mino_val *opts;
        if (mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                                         "time-map->epoch takes one or "
                                         "two arguments");
        }
        opts = args->as.cons.cdr->as.cons.car;
        have_zone = time_zone_opt(S, opts, "time-map->epoch", &z, &fixed,
                                  &is_fixed);
        if (have_zone < 0) return NULL;
    }

    /* unknown keys reject: every key must be one of the nine known.
     * Keywords are interned, so identity is the equality. */
    {
        static const char *const names[] = {
            "year", "month", "day", "hour", "min",
            "sec", "ms", "wday", "offset-min"
        };
        n = m->as.map.len;
        for (i = 0; i < n; i++) {
            mino_val *k = vec_nth(m->as.map.key_order, i);
            int ok = 0;
            for (int j = 0; j < 9; j++) {
                if (k == mino_keyword(S, names[j])) { ok = 1; break; }
            }
            if (!ok) {
                const char *kd;
                size_t klen;
                if (k != NULL && mino_to_keyword(k, &kd, &klen)) {
                    snprintf(msg, sizeof(msg),
                             "time-map->epoch: unknown key :%.*s",
                             (int)klen, kd);
                } else {
                    snprintf(msg, sizeof(msg),
                             "time-map->epoch: keys must be keywords");
                }
                return time_throw(S, "time/field", "MTF001", msg);
            }
        }
    }

    if (!tm_field(S, m, "year", 1, &y)) return NULL;
    if (!tm_field(S, m, "month", 1, &mo)) return NULL;
    if (!tm_field(S, m, "day", 1, &d)) return NULL;
    if (!tm_field(S, m, "hour", 0, &h)) return NULL;
    if (!tm_field(S, m, "min", 0, &mi)) return NULL;
    if (!tm_field(S, m, "sec", 0, &sec)) return NULL;
    if (!tm_field(S, m, "ms", 0, &msec)) return NULL;
    if (!tm_field(S, m, "offset-min", 0, &off)) return NULL;
    {
        mino_val *w = map_get_val(m, mino_keyword(S, "wday"));
        has_wday = !(w == NULL || mino_is_nil(w));
        if (has_wday && !time_arg_ll(S, w, "time-map->epoch", "wday", &wday))
            return NULL;
    }

    if (y < 1 || y > 9999) {
        snprintf(msg, sizeof(msg),
                 "time-map->epoch: year %lld outside 1..9999", y);
        return time_throw(S, "time/field", "MTF001", msg);
    }
    if (mo < 1 || mo > 12) {
        snprintf(msg, sizeof(msg),
                 "time-map->epoch: month %lld outside 1..12", mo);
        return time_throw(S, "time/field", "MTF001", msg);
    }
    if (d < 1 || d > (long long)days_in_month(y, (unsigned)mo)) {
        snprintf(msg, sizeof(msg),
                 "time-map->epoch: day %lld invalid for %lld-%02lld", d, y,
                 mo);
        return time_throw(S, "time/field", "MTF001", msg);
    }
    if (h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec > 59
        || msec < 0 || msec > 999) {
        snprintf(msg, sizeof(msg),
                 "time-map->epoch: time-of-day out of range");
        return time_throw(S, "time/field", "MTF001", msg);
    }
    if (off < TIME_OFF_MIN || off > TIME_OFF_MAX) {
        snprintf(msg, sizeof(msg),
                 "time-map->epoch: offset %lld exceeds 23:59", off);
        return time_throw(S, "time/field", "MTF001", msg);
    }
    if (has_wday && (wday < 0 || wday > 6)) {
        snprintf(msg, sizeof(msg),
                 "time-map->epoch: wday %lld outside 0..6", wday);
        return time_throw(S, "time/field", "MTF001", msg);
    }

    tm.year = y;
    tm.month = (unsigned)mo;
    tm.day = (unsigned)d;
    tm.hour = (unsigned)h;
    tm.min = (unsigned)mi;
    tm.sec = (unsigned)sec;
    tm.wday = weekday_from_days(days_from_civil(y, (unsigned)mo, (unsigned)d));
    if (has_wday && (long long)tm.wday != wday) {
        snprintf(msg, sizeof(msg),
                 "time-map->epoch: :wday %lld contradicts the date "
                 "(actual %lld)", wday, (long long)tm.wday);
        return time_throw(S, "time/field", "MTF001", msg);
    }

    if (have_zone) {
        /* the map's fields are local wall time in the zone; :zone
         * overrides the map's own :offset-min (the composition
         * (time-map->epoch (epoch->time-map ms {:zone z}) {:zone z})
         * round-trips). */
        int64_t local_secs = secs_from_broken(&tm);
        int64_t e;
        int offm;
        if (is_fixed) {
            total = (local_secs - fixed * 60) * 1000 + msec;
        } else {
            tz_local_to_utc(&z, local_secs, &e, &offm);
            total = e * 1000 + msec;
        }
    } else {
        total = (secs_from_broken(&tm) - off * 60) * 1000 + msec;
    }
    if (total < TIME_MS_MIN || total > TIME_MS_MAX) {
        snprintf(msg, sizeof(msg),
                 "time-map->epoch: result outside years 1..9999");
        return time_throw(S, "time/range", "MTR001", msg);
    }
    return mino_int(S, total);
}

/* ---- calendar arithmetic and human diff ------------------------------- */

/* add months with day clamping (Jan 31 + 1mo -> Feb 28/29) */
static void add_months_civil(civil_tm *tm, long long delta)
{
    long long total = tm->year * 12 + (long long)(tm->month - 1) + delta;
    long long y = total / 12;
    long long mo = total % 12;
    if (mo < 0) { mo += 12; y -= 1; }
    tm->year = y;
    tm->month = (unsigned)mo + 1u;
    unsigned dim = days_in_month(y, tm->month);
    if (tm->day > dim) tm->day = dim;
}

/* shift an epoch-ms by n calendar months on the UTC civil date,
 * keeping the intra-day milliseconds; 0 ok, -1 out of range */
static int add_months_ms(int64_t ms, long long n, long long *out)
{
    civil_tm tm;
    int64_t base = floor_div(ms, 1000);
    int msec = (int)(ms - base * 1000);
    broken_from_secs(base, &tm);
    add_months_civil(&tm, n);
    if (tm.year < 1 || tm.year > 9999) return -1;
    {
        long long r = secs_from_broken(&tm) * 1000 + msec;
        if (r < TIME_MS_MIN || r > TIME_MS_MAX) return -1;
        *out = r;
    }
    return 0;
}

/* months_between as the exact definition: the largest n with
 * add_months(a, n) <= b (requires a <= b). The field estimate is
 * within one, so the correction loops run at most once each. */
static long long months_between_calc(int64_t a, int64_t b)
{
    civil_tm ta, tb;
    long long probe;
    broken_from_secs(floor_div(a, 1000), &ta);
    broken_from_secs(floor_div(b, 1000), &tb);
    long long n = (tb.year - ta.year) * 12 + (long long)tb.month
                - (long long)ta.month;
    while (n > 0 && (add_months_ms(a, n, &probe) != 0 || probe > b)) n--;
    while (add_months_ms(a, n + 1, &probe) == 0 && probe <= b) n++;
    return n;
}

/* (leap-year? y) */
static mino_val *prim_leap_year_p(mino_state *S, mino_val *args,
                                  mino_env *env)
{
    long long y;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)
        || !time_arg_ll(S, args->as.cons.car, "leap-year?", "year", &y))
        return NULL;
    return is_leap(y) ? mino_true(S) : mino_false(S);
}

/* (days-in-month y m) */
static mino_val *prim_days_in_month(mino_state *S, mino_val *args,
                                    mino_env *env)
{
    mino_val *av[2] = {NULL};
    long long y, m;
    char msg[72];
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "days-in-month takes two arguments");
    }
    av[0] = args->as.cons.car;
    av[1] = args->as.cons.cdr->as.cons.car;
    if (!time_arg_ll(S, av[0], "days-in-month", "year", &y)) return NULL;
    if (!time_arg_ll(S, av[1], "days-in-month", "month", &m)) return NULL;
    if (m < 1 || m > 12) {
        snprintf(msg, sizeof(msg),
                 "days-in-month: month %lld outside 1..12", m);
        return time_throw(S, "time/field", "MTF001", msg);
    }
    return mino_int(S, days_in_month(y, (unsigned)m));
}

/* (weekday ms-or-map) -> 0..6 (0 = Sunday). For an epoch-ms, the
 * UTC weekday; for a time map, the weekday of the map's own
 * (offset-local) date, agreeing with the map's :wday field. */
static mino_val *prim_weekday(mino_state *S, mino_val *args,
                              mino_env *env)
{
    mino_val *v;
    long long ms;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "weekday takes one argument");
    }
    v = args->as.cons.car;
    if (v != NULL && mino_type_of(v) == MINO_MAP) {
        long long y, mo, d;
        if (!tm_field(S, v, "year", 1, &y)
            || !tm_field(S, v, "month", 1, &mo)
            || !tm_field(S, v, "day", 1, &d)) {
            return NULL;
        }
        if (mo < 1 || mo > 12
            || d < 1 || d > (long long)days_in_month(y, (unsigned)mo)
            || y < 1 || y > 9999) {
            return time_throw(S, "time/field", "MTF001",
                              "weekday: invalid date fields");
        }
        return mino_int(S,
                        weekday_from_days(days_from_civil(
                            y, (unsigned)mo, (unsigned)d)));
    }
    if (!time_arg_ll(S, v, "weekday", "epoch-ms", &ms)) {
        return NULL;
    }
    if (ms < TIME_MS_MIN || ms > TIME_MS_MAX) {
        return time_throw(S, "time/range", "MTR001",
                          "weekday: epoch-ms outside years 1..9999");
    }
    return mino_int(S, weekday_from_days(floor_div(ms, 86400000)));
}

/* (add-days ms n) -> ms; exact 86400000-ms days (the model has no
 * DST, so a day is always exact) */
static mino_val *prim_add_days(mino_state *S, mino_val *args,
                               mino_env *env)
{
    mino_val *av[2] = {NULL};
    long long ms, n;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "add-days takes two arguments");
    }
    av[0] = args->as.cons.car;
    av[1] = args->as.cons.cdr->as.cons.car;
    if (!time_arg_ll(S, av[0], "add-days", "epoch-ms", &ms)) return NULL;
    if (!time_arg_ll(S, av[1], "add-days", "n", &n)) return NULL;
    /* years 1..9999 span under 3.7M days; anything beyond can only
     * leave the range, and 4M days cannot overflow int64 in ms */
    if (n > 4000000 || n < -4000000) {
        return time_throw(S, "time/range", "MTR001",
                          "add-days: result outside years 1..9999");
    }
    {
        int64_t r = (int64_t)ms + (int64_t)n * 86400000;
        if (r < TIME_MS_MIN || r > TIME_MS_MAX) {
            return time_throw(S, "time/range", "MTR001",
                              "add-days: result outside years 1..9999");
        }
        return mino_int(S, r);
    }
}

/* (add-months ms-or-map n) -> same kind as the input. An ms input
 * shifts the UTC civil date; a map input shifts its own fields and
 * keeps hour/min/sec/ms/offset untouched. Day clamps to the target
 * month (Jan 31 + 1mo -> Feb 28/29). */
static mino_val *prim_add_months(mino_state *S, mino_val *args,
                                 mino_env *env)
{
    mino_val *v, *nv;
    long long n;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "add-months takes two arguments");
    }
    v = args->as.cons.car;
    nv = args->as.cons.cdr->as.cons.car;
    if (!time_arg_ll(S, nv, "add-months", "n", &n)) return NULL;

    if (v != NULL && mino_type_of(v) == MINO_MAP) {
        /* validate through the strict converter (one-arg list), then
         * rebuild the map with shifted local fields */
        mino_val *one, *ep, *mv;
        int pinned = 0;
        long long ms;
        one = mino_cons(S, v, mino_nil(S));
        gc_pin(one); pinned++;
        ep = prim_time_map_to_epoch(S, one, env);
        if (ep == NULL) { gc_unpin(pinned); return NULL; }
        gc_pin(ep); pinned++;
        if (!as_long(ep, &ms)) { gc_unpin(pinned); return NULL; }
        {
            mino_val *offv = map_get_val(v, mino_keyword(S, "offset-min"));
            long long off = 0;
            mino_val *msv, *two, *mm;
            if (offv != NULL && !mino_is_nil(offv)
                && !as_long(offv, &off)) {
                gc_unpin(pinned);
                return NULL;
            }
            msv = mino_int(S, ms);
            gc_pin(msv); pinned++;
            two = mino_cons(S, msv, mino_nil(S));
            gc_pin(two); pinned++;
            mm = mino_int(S, off);
            gc_pin(mm); pinned++;
            two = mino_cons(S, msv, mino_cons(S, mm, mino_nil(S)));
            gc_pin(two); pinned++;
            mv = prim_epoch_to_time_map(S, two, env);
            if (mv == NULL) { gc_unpin(pinned); return NULL; }
            gc_pin(mv); pinned++;
            {
                long long y, mo, d, h, mi, sec, msec, off2, wd;
                mino_val *keys[9], *vals[9], *m;
                int p2 = 0;
                civil_tm tm;
                if (!tm_field(S, mv, "year", 1, &y)
                    || !tm_field(S, mv, "month", 1, &mo)
                    || !tm_field(S, mv, "day", 1, &d)
                    || !tm_field(S, mv, "hour", 0, &h)
                    || !tm_field(S, mv, "min", 0, &mi)
                    || !tm_field(S, mv, "sec", 0, &sec)
                    || !tm_field(S, mv, "ms", 0, &msec)
                    || !tm_field(S, mv, "offset-min", 0, &off2)) {
                    gc_unpin(pinned);
                    return NULL;
                }
                tm.year = y;
                tm.month = (unsigned)mo;
                tm.day = (unsigned)d;
                add_months_civil(&tm, n);
                if (tm.year < 1 || tm.year > 9999) {
                    gc_unpin(pinned);
                    return time_throw(S, "time/range", "MTR001",
                                      "add-months: result outside years "
                                      "1..9999");
                }
                wd = weekday_from_days(days_from_civil(tm.year, tm.month,
                                                       tm.day));
                keys[0] = mino_keyword(S, "year");     gc_pin(keys[0]); p2++;
                vals[0] = mino_int(S, tm.year);        gc_pin(vals[0]); p2++;
                keys[1] = mino_keyword(S, "month");    gc_pin(keys[1]); p2++;
                vals[1] = mino_int(S, tm.month);       gc_pin(vals[1]); p2++;
                keys[2] = mino_keyword(S, "day");      gc_pin(keys[2]); p2++;
                vals[2] = mino_int(S, tm.day);         gc_pin(vals[2]); p2++;
                keys[3] = mino_keyword(S, "hour");     gc_pin(keys[3]); p2++;
                vals[3] = mino_int(S, h);              gc_pin(vals[3]); p2++;
                keys[4] = mino_keyword(S, "min");      gc_pin(keys[4]); p2++;
                vals[4] = mino_int(S, mi);             gc_pin(vals[4]); p2++;
                keys[5] = mino_keyword(S, "sec");      gc_pin(keys[5]); p2++;
                vals[5] = mino_int(S, sec);            gc_pin(vals[5]); p2++;
                keys[6] = mino_keyword(S, "ms");       gc_pin(keys[6]); p2++;
                vals[6] = mino_int(S, msec);           gc_pin(vals[6]); p2++;
                keys[7] = mino_keyword(S, "wday");     gc_pin(keys[7]); p2++;
                vals[7] = mino_int(S, wd);             gc_pin(vals[7]); p2++;
                keys[8] = mino_keyword(S, "offset-min");
                gc_pin(keys[8]); p2++;
                vals[8] = mino_int(S, off2);           gc_pin(vals[8]); p2++;
                m = mino_map(S, keys, vals, 9);
                gc_unpin(p2);
                gc_unpin(pinned);
                return m;
            }
        }
    }
    {
        long long ms, r;
        if (!time_arg_ll(S, v, "add-months", "epoch-ms", &ms)) return NULL;
        if (add_months_ms(ms, n, &r) != 0) {
            return time_throw(S, "time/range", "MTR001",
                              "add-months: result outside years 1..9999");
        }
        return mino_int(S, r);
    }
}

/* (days-between a b) -> whole civil days from a to b, floor; the
 * sign follows b - a */
static mino_val *prim_days_between(mino_state *S, mino_val *args,
                                   mino_env *env)
{
    mino_val *av[2] = {NULL};
    long long a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "days-between takes two arguments");
    }
    av[0] = args->as.cons.car;
    av[1] = args->as.cons.cdr->as.cons.car;
    if (!time_arg_ll(S, av[0], "days-between", "a", &a)) return NULL;
    if (!time_arg_ll(S, av[1], "days-between", "b", &b)) return NULL;
    if (a < TIME_MS_MIN || a > TIME_MS_MAX || b < TIME_MS_MIN
        || b > TIME_MS_MAX) {
        return time_throw(S, "time/range", "MTR001",
                          "days-between: epoch-ms outside years 1..9999");
    }
    return mino_int(S, floor_div((int64_t)b - (int64_t)a, 86400000));
}

/* (months-between a b) -> whole calendar months from a to b: the
 * largest n with (add-months a n) <= b; the sign follows b - a */
static mino_val *prim_months_between(mino_state *S, mino_val *args,
                                     mino_env *env)
{
    mino_val *av[2] = {NULL};
    long long a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "months-between takes two arguments");
    }
    av[0] = args->as.cons.car;
    av[1] = args->as.cons.cdr->as.cons.car;
    if (!time_arg_ll(S, av[0], "months-between", "a", &a)) return NULL;
    if (!time_arg_ll(S, av[1], "months-between", "b", &b)) return NULL;
    if (a < TIME_MS_MIN || a > TIME_MS_MAX || b < TIME_MS_MIN
        || b > TIME_MS_MAX) {
        return time_throw(S, "time/range", "MTR001",
                          "months-between: epoch-ms outside years "
                          "1..9999");
    }
    if (a <= b) return mino_int(S, months_between_calc(a, b));
    return mino_int(S, -months_between_calc(b, a));
}

/* (human-diff a b?) -> the largest-unit phrase for b - a.
 * Pinned vocabulary (ADR 21): |d| < 1s "just now" / "in a moment";
 * then N seconds < 60s, minutes < 60m, hours < 24h; calendar months
 * (add-months based, so a Feb-clamped month still counts) before
 * 12, then years; full singular/plural words. */
static mino_val *prim_human_diff(mino_state *S, mino_val *args,
                                 mino_env *env)
{
    mino_val *av[2] = {NULL};
    size_t n;
    long long a, b;
    int64_t diff, ad, earlier;
    long long count, mb;
    const char *unit;
    char buf[64];
    (void)env;

    if (!arg_count(S, args, &n) || n < 1 || n > 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "human-diff takes one or two "
                                     "arguments");
    }
    av[0] = args->as.cons.car;
    if (n == 2) av[1] = args->as.cons.cdr->as.cons.car;
    if (!time_arg_ll(S, av[0], "human-diff", "a", &a)) return NULL;
    if (n == 2) {
        if (!time_arg_ll(S, av[1], "human-diff", "b", &b)) return NULL;
    } else {
        b = time_wall_ms();
    }
    if (a < TIME_MS_MIN || a > TIME_MS_MAX || b < TIME_MS_MIN
        || b > TIME_MS_MAX) {
        return time_throw(S, "time/range", "MTR001",
                          "human-diff: epoch-ms outside years 1..9999");
    }
    diff = (int64_t)b - (int64_t)a;
    ad = diff < 0 ? -diff : diff;
    earlier = diff >= 0 ? (int64_t)a : (int64_t)b;

    if (ad < 1000) {
        return mino_string_n(S, diff >= 0 ? "just now" : "in a moment",
                             diff >= 0 ? 8 : 11);
    }
    if (ad < 60LL * 1000) {
        count = ad / 1000;
        unit = count == 1 ? "second" : "seconds";
    } else if (ad < 60LL * 60 * 1000) {
        count = ad / 60000;
        unit = count == 1 ? "minute" : "minutes";
    } else if (ad < 24LL * 60 * 60 * 1000) {
        count = ad / 3600000;
        unit = count == 1 ? "hour" : "hours";
    } else {
        mb = months_between_calc(earlier, earlier + ad);
        if (mb >= 12) {
            count = mb / 12;
            unit = count == 1 ? "year" : "years";
        } else if (mb >= 1) {
            count = mb;
            unit = count == 1 ? "month" : "months";
        } else {
            count = ad / 86400000;
            unit = count == 1 ? "day" : "days";
        }
    }
    if (diff >= 0) {
        snprintf(buf, sizeof(buf), "%lld %s ago", count, unit);
    } else {
        snprintf(buf, sizeof(buf), "in %lld %s", count, unit);
    }
    return mino_string_n(S, buf, strlen(buf));
}

/* ---- prim table ------------------------------------------------------ */

const mino_prim_def k_prims_time[] = {
    {"now", prim_now,
     "Returns the wall clock as epoch milliseconds since "
      "1970-01-01T00:00:00Z (an integer). For elapsed-time "
      "measurement prefer the monotonic nano-time; for process CPU "
      "time see cpu-ms."},
    {"now-s", prim_now_s,
     "Returns the wall clock as epoch seconds since 1970-01-01T00:00:00Z "
      "(an integer). Equivalent to (quot (now) 1000) but one read."},
    {"cpu-ms", prim_cpu_ms,
     "Returns process CPU time in milliseconds (user plus kernel on "
      "Windows, clock() elsewhere). A work metric, not a clock: two "
      "busy threads consume it twice as fast."},
    {"epoch->time-map", prim_epoch_to_time_map,
     "Converts epoch milliseconds to a plain time map {:year :month "
     ":day :hour :min :sec :ms :wday :offset-min} with 1-based "
     "months and :wday 0=Sunday. Optional second argument renders "
     "the fields shifted by a fixed offset in minutes east of UTC "
     "(the map carries :offset-min so the value round-trips), or an "
     "options map {:zone z} with z a fixed offset in minutes or an "
     "IANA zone name rendered at the zone's offset at that instant "
     "(ADR 27; the offset is minute-granular). Throws :time/range "
     "outside years 1..9999."},
    {"time-map->epoch", prim_time_map_to_epoch,
     "Converts a time map back to epoch milliseconds. :year :month "
     ":day are required; :hour :min :sec :ms :offset-min default to "
     "0; :wday is optional but must match the date when present. "
     "Strict: unknown keys, out-of-range fields, and impossible "
     "dates (February 30th) throw :time/field rather than "
     "normalizing silently. Optional second argument {:zone z} "
     "reads the fields as local wall time in the zone (fold-0: "
     "overlaps take the first occurrence, gaps shift forward); "
     ":zone overrides the map's own :offset-min."},
    {"parse-time", prim_parse_time,
     "Parses a date or datetime string into {:epoch-ms :offset-min "
     ":format :date-only?}. Accepts ISO 8601 / RFC 3339 (date-only or "
     "datetime, T/t/space separator, optional seconds, fractional "
     "seconds kept to milliseconds, Z/z and +HH:MM / +-HHMM offsets; "
     "leap second 60 folds to 59) and the RFC 1123 / 2822 comma form "
     "(optional day name, case-insensitive month and zone names, "
     "zones GMT/UT/UTC or +-HHMM). Strict: impossible dates, "
     "mismatched day names, named non-UTC zones, trailing junk, and "
     "input over 64 characters throw :time/parse with the byte "
     "position. :format reports :iso8601, :rfc1123 (alphabetic "
     "zone), or :rfc2822 (numeric zone). Optional second argument "
     "{:zone z} interprets an offset-less input as local wall time "
     "in the zone (fold-0: overlaps take the first occurrence, "
     "gaps shift forward); an input carrying its own offset plus "
     ":zone throws :time/field."},
    {"format-time", prim_format_time,
     "Formats epoch milliseconds as a string. (format-time ms) is "
     "ISO 8601 UTC with a Z suffix and a .SSS fraction only when "
     "the milliseconds are nonzero. Optional fmt keyword: "
     ":iso8601 (default), :iso8601-date (YYYY-MM-DD), :rfc1123 "
     "(HTTP Date, always GMT, no offset argument), :rfc2822 "
     "(numeric-offset zone). An optional final offset-min argument "
     "renders the offset-capable forms at a fixed offset; an "
     "options map {:zone z} in the fmt or offset position renders "
     "them at the zone's offset at ms (ADR 27). No "
     "pattern strings: compose custom formats from the time map "
     "and str. Throws :time/range outside years 1..9999."},
    {"zone-offset-mins", prim_zone_offset_mins,
     "Returns a zone's UTC offset in minutes at an epoch-ms "
     "instant. The zone is an IANA name (string or keyword) or a "
     "fixed offset in minutes, which passes through unchanged. "
     "Named zones come from the embedded tzdata snapshot (ADR 27): "
     "598 canonical zones, DST via transition table with the POSIX "
     "footer governing past the last stored transition, offsets "
     "minute-granular (sub-minute historical LMT offsets round to "
     "the nearest minute). Unknown names throw :time/zone."},
    {"leap-year?", prim_leap_year_p,
     "True when the year is a Gregorian leap year (divisible by 4, "
      "except centuries not divisible by 400)."},
    {"days-in-month", prim_days_in_month,
     "Returns the number of days in the month (1..12) of the year; "
      "February answers 29 in leap years."},
    {"weekday", prim_weekday,
     "Returns the day of the week as an integer 0..6, 0 = Sunday. "
      "For an epoch-ms, the UTC weekday; for a time map, the "
      "weekday of the map's own (offset-local) date, agreeing with "
      "the map's :wday field."},
    {"add-days", prim_add_days,
     "Adds n exact 86400000-ms days to an epoch-ms (the model has no "
      "DST, so a day is always exact). Result stays inside years "
      "1..9999 or throws :time/range."},
    {"add-months", prim_add_months,
     "Adds n calendar months to an epoch-ms or a time map, returning "
      "the same kind. The day clamps to the target month (January 31 "
      "plus one month is February 28 or 29). A map input shifts its "
      "own fields and keeps hour/min/sec/ms/offset; an ms input "
      "shifts the UTC civil date."},
    {"days-between", prim_days_between,
     "Whole civil days from a to b, floored; the sign follows "
      "b - a. Both arguments are epoch-ms."},
    {"months-between", prim_months_between,
     "Whole calendar months from a to b: the largest n with "
      "(add-months a n) <= b, so a January 31 to February 28 gap "
      "counts as one month. The sign follows b - a."},
    {"human-diff", prim_human_diff,
     "Renders the difference b - a between two epoch-ms as the "
      "largest unit phrase: under a second \"just now\" / \"in a "
      "moment\", then seconds under 60, minutes under 60, hours "
      "under 24, calendar months under 12, then years, with full "
      "singular and plural words (\"3 days ago\", \"in 5 minutes\"). "
      "b defaults to (now)."},
};

const size_t k_prims_time_count =
    sizeof(k_prims_time) / sizeof(k_prims_time[0]);
