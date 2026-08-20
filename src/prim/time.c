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
   1..9999 only. date_only flags a bare date. */
static int parse_iso8601(const char *s, int64_t *ms, int *offset_min,
                         int *date_only, int *errpos)
{
    const char *p = s;
    unsigned y, mo, d, h = 0, mi = 0, sec = 0, msec = 0;
    int off = 0;
    *date_only = 0;
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
    if (tm.year < 0 || tm.year > 9999) return -1;
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

/* (format-time ms fmt? offset-min?) -> string. fmt: :iso8601 (default),
 * :iso8601-date, :rfc1123, :rfc2822. */
static mino_val *prim_format_time(mino_state *S, mino_val *args,
                                  mino_env *env)
{
    mino_val *av[3];
    size_t n;
    long long ms, off = 0;
    mino_val *fmt;
    char buf[40];
    int rc;
    char msg[80];
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
    if (n == 3 && !time_arg_ll(S, av[2], "format-time", "offset-min",
                               &off))
        return NULL;
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
        if (n == 3) {
            return time_throw(S, "time/field", "MTF001",
                              "format-time: :rfc1123 is always GMT; an "
                              "offset argument is not accepted");
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
        return time_throw(S, "internal", "MIN001",
                          "format-time: internal format failure");
    }
    return mino_string_n(S, buf, strlen(buf));
}

/* ---- parse-time prim ------------------------------------------------- */

/* (parse-time s) -> {:epoch-ms :offset-min :format :date-only?} */
static mino_val *prim_parse_time(mino_state *S, mino_val *args,
                                 mino_env *env)
{
    mino_val *v, *keys[4], *vals[4], *m;
    int pinned = 0;
    const char *s;
    size_t len;
    int64_t ms;
    int off, date_only = 0, az = 0, errpos = 0;
    const char *family;
    char msg[96];
    (void)env;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "parse-time requires one argument");
    }
    v = args->as.cons.car;
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
        if (parse_iso8601(s, &ms, &off, &date_only, &errpos)) {
            snprintf(msg, sizeof(msg),
                     "parse-time: invalid ISO 8601 date at byte %d",
                     errpos);
            return time_throw(S, "time/parse", "MTP001", msg);
        }
    } else {
        family = "RFC 1123/2822";
        if (parse_rfc2822(s, &ms, &off, &az, &errpos)) {
            int errpos2 = 0;
            if (parse_iso8601(s, &ms, &off, &date_only, &errpos2) == 0) {
                family = "ISO 8601";
            } else {
                snprintf(msg, sizeof(msg),
                         "parse-time: invalid RFC 1123/2822 date at "
                         "byte %d", errpos);
                return time_throw(S, "time/parse", "MTP001", msg);
            }
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

/* (epoch->time-map ms offset-min?) -> plain map. The map always
 * carries :offset-min so the conversion round-trips as data. */
static mino_val *prim_epoch_to_time_map(mino_state *S, mino_val *args,
                                        mino_env *env)
{
    mino_val *av[2];
    size_t n;
    long long ms, off = 0;
    civil_tm tm;
    mino_val *keys[9], *vals[9], *m;
    int pinned = 0;
    char msg[80];
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
    if (n == 2 && !time_arg_ll(S, av[1], "epoch->time-map", "offset-min",
                               &off))
        return NULL;
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
    (void)env;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)
        || args->as.cons.car == NULL
        || mino_type_of(args->as.cons.car) != MINO_MAP) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "time-map->epoch requires one map "
                                     "argument");
    }
    m = args->as.cons.car;

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

    total = (secs_from_broken(&tm) - off * 60) * 1000 + msec;
    if (total < TIME_MS_MIN || total > TIME_MS_MAX) {
        snprintf(msg, sizeof(msg),
                 "time-map->epoch: result outside years 1..9999");
        return time_throw(S, "time/range", "MTR001", msg);
    }
    return mino_int(S, total);
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
      "(the map carries :offset-min so the value round-trips). "
      "Throws :time/range outside years 1..9999."},
    {"time-map->epoch", prim_time_map_to_epoch,
     "Converts a time map back to epoch milliseconds. :year :month "
      ":day are required; :hour :min :sec :ms :offset-min default to "
      "0; :wday is optional but must match the date when present. "
      "Strict: unknown keys, out-of-range fields, and impossible "
      "dates (February 30th) throw :time/field rather than "
      "normalizing silently."},
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
      "zone), or :rfc2822 (numeric zone)."},
    {"format-time", prim_format_time,
     "Formats epoch milliseconds as a string. (format-time ms) is "
      "ISO 8601 UTC with a Z suffix and a .SSS fraction only when "
      "the milliseconds are nonzero. Optional fmt keyword: "
      ":iso8601 (default), :iso8601-date (YYYY-MM-DD), :rfc1123 "
      "(HTTP Date, always GMT, no offset argument), :rfc2822 "
      "(numeric-offset zone). An optional final offset-min argument "
      "renders the offset-capable forms at a fixed offset. No "
      "pattern strings: compose custom formats from the time map "
      "and str. Throws :time/range outside years 1..9999."},
};

const size_t k_prims_time_count =
    sizeof(k_prims_time) / sizeof(k_prims_time[0]);
