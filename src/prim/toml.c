/*
 * toml.c -- native TOML 1.0 reader.
 *
 * Single-pass byte-cursor parser backing mino.toml/parse-string; the
 * facade (argument validation, the ex-info error shape) stays
 * Clojure. Semantics pin to the python3 tomllib oracle (see
 * docs/adr/25-toml-reader-in-c.md): CRLF normalized up front, tables
 * and arrays of tables with last-element navigation, dotted and
 * quoted keys, immutable inline tables, the four string kinds with
 * the first-terminator plus two-quote fold and the line-ending
 * backslash trim, radix and underscore integers checked to signed
 * 64-bit, floats including inf/nan, and RFC 3339 values kept as the
 * raw source text.
 *
 * The prim returns the parsed map, or an error descriptor
 * [:toml/error "reason" line col "text"] that the facade converts to
 * ex-info. Rooting follows json.c/csv.c: in-flight values live in C
 * locals the conservative stack scan sees, and the one raw-pointer
 * window (string decode buffers) is GC_T_RAW memory held under a
 * gc_depth guard.
 */

#include "prim/internal.h"
#include "mino.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TP_MAX_DEPTH  256
#define TP_MAX_PARTS  128
#define TP_NUM_BUF    600

/* Flag bits for the path bookkeeping map. */
#define TP_DECLARED 1
#define TP_AOT      2
#define TP_DOTTED   4
#define TP_INLINE   8

typedef struct {
    mino_state           *S;
    mino_env             *env;
    const unsigned char  *p;
    const unsigned char  *end;
    const unsigned char  *line_start;
    size_t                line;      /* 1-based */
    mino_val             *pv;        /* :parse-values fn or NULL */
    mino_val             *root;      /* persistent result map */
    mino_val             *flags;     /* persistent path-vector -> bits */
    mino_val             *cur[TP_MAX_PARTS]; /* current table path */
    int                   cur_n;
    size_t                stmt_line; /* bookkeeping errors land here */
    const unsigned char  *stmt_start; /* first line of the statement */
    /* Error capture: set once, then NULL propagates. */
    const char           *err_reason;
    size_t                err_line;
    size_t                err_col;
    const unsigned char  *err_from;  /* statement line span for :text */
    const unsigned char  *err_to;
    int                   depth;
} tp_t;

static mino_val *tp_value(tp_t *t);

/* ---- errors ---- */

/* Record the failure at (line, col) with the statement's first line
 * as :text. */
static void tp_fail(tp_t *t, const char *reason, size_t line, size_t col)
{
    if (t->err_reason == NULL) {
        const unsigned char *ls = t->stmt_start;
        const unsigned char *le;
        if (ls == NULL) ls = t->line_start;
        le = ls;
        t->err_reason = reason;
        t->err_line = line;
        t->err_col = col;
        while (le < t->end && *le != '\n') {
            le++;
        }
        t->err_from = ls;
        t->err_to = le;
    }
}

static mino_val *tp_fail_here(tp_t *t, const char *reason)
{
    tp_fail(t, reason, t->line, (size_t)(t->p - t->line_start) + 1);
    return NULL;
}

static mino_val *tp_fail_stmt(tp_t *t, const char *reason)
{
    tp_fail(t, reason, t->stmt_line, 1);
    return NULL;
}

/* ---- cursor helpers ---- */

static int tp_at_end(tp_t *t)
{
    return t->p >= t->end;
}

static unsigned char tp_peek(tp_t *t)
{
    return *t->p;
}

static void tp_adv(tp_t *t)
{
    if (*t->p == '\n') {
        t->line++;
        t->p++;
        t->line_start = t->p;
    } else {
        t->p++;
    }
}

static int tp_eat(tp_t *t, unsigned char c)
{
    if (!tp_at_end(t) && tp_peek(t) == c) {
        tp_adv(t);
        return 1;
    }
    return 0;
}

static void tp_skip_inline_ws(tp_t *t)
{
    while (!tp_at_end(t) && (tp_peek(t) == ' ' || tp_peek(t) == '\t')) {
        t->p++;
    }
}

/* Whitespace, newlines, and whole comment lines (array gaps). */
static void tp_skip_array_ws(tp_t *t)
{
    for (;;) {
        while (!tp_at_end(t)
               && (tp_peek(t) == ' ' || tp_peek(t) == '\t'
                   || tp_peek(t) == '\n')) {
            tp_adv(t);
        }
        if (!tp_at_end(t) && tp_peek(t) == '#') {
            while (!tp_at_end(t) && tp_peek(t) != '\n') {
                t->p++;
            }
            continue;
        }
        return;
    }
}

/* After a statement's value: [ \t]* (#...)? then EOL or EOF. */
static int tp_expect_eol(tp_t *t)
{
    tp_skip_inline_ws(t);
    if (!tp_at_end(t) && tp_peek(t) == '#') {
        while (!tp_at_end(t) && tp_peek(t) != '\n') {
            t->p++;
        }
    }
    if (tp_at_end(t)) return 0;
    if (tp_peek(t) == '\n') {
        tp_adv(t);
        return 0;
    }
    tp_fail_here(t, "unexpected-text");
    return -1;
}

static int tp_is_bare_key_byte(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
           || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static int tp_is_digit(unsigned char c)
{
    return c >= '0' && c <= '9';
}

static int tp_hex_val(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ---- value constructors ---- */

static mino_val *tp_empty_map(tp_t *t)
{
    return mino_map(t->S, NULL, NULL, 0);
}

static mino_val *tp_map_assoc(tp_t *t, mino_val *m, mino_val *k, mino_val *v)
{
    mino_state *S = t->S;
    mino_val *tr = mino_transient(S, m);
    mino_val *tr2;
    if (tr == NULL) return NULL;
    tr2 = mino_assoc_bang(S, tr, k, v);
    if (tr2 == NULL) return NULL;
    return mino_persistent(S, tr2);
}

static long long tp_flags_get(tp_t *t, mino_val **parts, int n)
{
    mino_val *path = mino_vector(t->S, parts, (size_t)n);
    mino_val *cur;
    if (path == NULL) return 0;
    cur = map_get_val(t->flags, path);
    if (cur != NULL && mino_type_of(cur) == MINO_INT) {
        return mino_val_int_get(cur);
    }
    return 0;
}

static int tp_flags_set(tp_t *t, mino_val **parts, int n, int bits)
{
    mino_state *S = t->S;
    mino_val *path = mino_vector(S, parts, (size_t)n);
    mino_val *cur;
    long long mask = 0;
    mino_val *tr, *tr2;
    if (path == NULL) return -1;
    cur = map_get_val(t->flags, path);
    if (cur != NULL && mino_type_of(cur) == MINO_INT) {
        mask = mino_val_int_get(cur);
    }
    if ((mask & bits) == bits) return 0;
    tr = mino_transient(S, t->flags);
    if (tr == NULL) return -1;
    tr2 = mino_assoc_bang(S, tr, path, mino_int(S, mask | bits));
    if (tr2 == NULL) return -1;
    t->flags = mino_persistent(S, tr2);
    return 0;
}

/* Mark every map path inside an inline-table value. Vectors break
 * key reachability, so only map children recurse. */
static int tp_mark_inline(tp_t *t, mino_val *v, mino_val **parts, int n)
{
    if (v != NULL && mino_type_of(v) == MINO_MAP) {
        size_t len = v->as.map.len;
        size_t i;
        if (tp_flags_set(t, parts, n, TP_INLINE) != 0) return -1;
        for (i = 0; i < len; i++) {
            mino_val *k = vec_nth(v->as.map.key_order, i);
            mino_val *child = map_get_val(v, k);
            if (n + 1 >= TP_MAX_PARTS) return -1;
            parts[n] = k;
            if (tp_mark_inline(t, child, parts, n + 1) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

/* ---- strings ---- */

static size_t tp_utf8(int cp, unsigned char *out)
{
    if (cp < 0x80) {
        out[0] = (unsigned char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (unsigned char)(0xC0 | (cp >> 6));
        out[1] = (unsigned char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (unsigned char)(0xE0 | (cp >> 12));
        out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (unsigned char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (unsigned char)(0xF0 | (cp >> 18));
    out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (unsigned char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Decode one escape at the cursor (on the backslash), writing into
 * buf at w. Errors locate the string value start (vline, vcol).
 * Returns the new write index or -1. */
static long tp_decode_escape(tp_t *t, size_t vline, size_t vcol,
                             unsigned char *buf, long w)
{
    const unsigned char *q = t->p + 1;
    unsigned char c;
    if (q >= t->end) {
        tp_fail(t, "invalid-escape", vline, vcol);
        return -1;
    }
    c = *q;
    switch (c) {
    case 'b': buf[w] = 8;    t->p = q + 1; return w + 1;
    case 't': buf[w] = '\t'; t->p = q + 1; return w + 1;
    case 'n': buf[w] = '\n'; t->p = q + 1; return w + 1;
    case 'f': buf[w] = 12;   t->p = q + 1; return w + 1;
    case 'r': buf[w] = '\r'; t->p = q + 1; return w + 1;
    case '"': buf[w] = '"';  t->p = q + 1; return w + 1;
    case '\\': buf[w] = '\\'; t->p = q + 1; return w + 1;
    case 'u':
    case 'U': {
        int want = (c == 'u') ? 4 : 8;
        int cp = 0;
        int i;
        for (i = 0; i < want; i++) {
            int d;
            if (q + 1 + i >= t->end) {
                tp_fail(t, "invalid-escape", vline, vcol);
                return -1;
            }
            d = tp_hex_val(q[1 + i]);
            if (d < 0) {
                tp_fail(t, "invalid-escape", vline, vcol);
                return -1;
            }
            cp = cp * 16 + d;
        }
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            tp_fail(t, "bad-codepoint", vline, vcol);
            return -1;
        }
        t->p = q + 1 + want;
        return w + (long)tp_utf8(cp, buf + w);
    }
    case ' ':
    case '\t':
    case '\n': {
        /* Line-ending backslash: only legal where newlines exist
         * (multiline strings); the caller passes ml so single-line
         * strings reject every whitespace trim shape. */
        const unsigned char *r = q;
        while (r < t->end && (*r == ' ' || *r == '\t')) r++;
        if (r >= t->end || *r != '\n') {
            tp_fail(t, "invalid-escape", vline, vcol);
            return -1;
        }
        r++;
        while (r < t->end && (*r == ' ' || *r == '\t' || *r == '\n')) r++;
        t->p = r;
        return w;
    }
    default:
        tp_fail(t, "invalid-escape", vline, vcol);
        return -1;
    }
}

/* Basic string. The cursor sits on the opening quote; ml selects the
 * triple-quoted form. Errors anchor to the value start. */
static mino_val *tp_basic_string(tp_t *t, int ml, size_t vline, size_t vcol)
{
    mino_state *S = t->S;
    size_t cap;
    const unsigned char *limit;
    unsigned char *buf;
    long w = 0;
    int saved_gc_depth = mino_current_ctx(S)->gc_depth;
    mino_val *out;
    /* Single-line strings cannot cross the next newline, so bound the
     * scratch buffer by it; only multiline spans reach end of input. */
    limit = t->p;
    if (!ml) {
        while (limit < t->end && *limit != '\n') {
            limit++;
        }
    } else {
        limit = t->end;
    }
    cap = (size_t)(limit - t->p) + 1;
    mino_current_ctx(S)->gc_depth = saved_gc_depth + 1;
    buf = (unsigned char *)gc_alloc_typed_inner(S, GC_T_RAW, cap);
    if (buf == NULL) {
        mino_current_ctx(S)->gc_depth = saved_gc_depth;
        return NULL;
    }
    if (ml) {
        t->p += 3;
        if (!tp_at_end(t) && tp_peek(t) == '\n') {
            tp_adv(t);
        }
    } else {
        t->p += 1;
    }
    for (;;) {
        unsigned char c;
        if (tp_at_end(t)) {
            goto unterminated;
        }
        c = tp_peek(t);
        if (c == '"') {
            if (!ml) {
                t->p++;
                break;
            }
            if (t->end - t->p >= 3 && t->p[1] == '"' && t->p[2] == '"') {
                int fold = 0;
                t->p += 3;
                while (!tp_at_end(t) && tp_peek(t) == '"' && fold < 2) {
                    buf[w++] = '"';
                    t->p++;
                    fold++;
                }
                break;
            }
            buf[w++] = '"';
            t->p++;
            continue;
        }
        if (c == '\\') {
            long nw = tp_decode_escape(t, vline, vcol, buf, w);
            if (nw < 0) {
                mino_current_ctx(S)->gc_depth = saved_gc_depth;
                return NULL;
            }
            w = nw;
            continue;
        }
        if (c == '\n' && !ml) {
            goto unterminated;
        }
        buf[w++] = c;
        tp_adv(t);
    }
    out = mino_string_n(S, (const char *)buf, (size_t)w);
    mino_current_ctx(S)->gc_depth = saved_gc_depth;
    return out;

unterminated:
    mino_current_ctx(S)->gc_depth = saved_gc_depth;
    tp_fail(t, "unterminated-string", vline, vcol);
    return NULL;
}

/* Literal string; cursor on the opening quote. */
static mino_val *tp_literal_string(tp_t *t, int ml, size_t vline,
                                   size_t vcol)
{
    mino_state *S = t->S;
    const unsigned char *content;
    if (ml) {
        t->p += 3;
        if (!tp_at_end(t) && tp_peek(t) == '\n') {
            tp_adv(t);
        }
    } else {
        t->p += 1;
    }
    content = t->p;
    for (;;) {
        if (tp_at_end(t)) {
            tp_fail(t, "unterminated-string", vline, vcol);
            return NULL;
        }
        if (tp_peek(t) == '\n' && !ml) {
            tp_fail(t, "unterminated-string", vline, vcol);
            return NULL;
        }
        if (tp_peek(t) == '\'') {
            if (ml) {
                if (t->end - t->p >= 3 && t->p[1] == '\''
                    && t->p[2] == '\'') {
                    const unsigned char *term = t->p;
                    int fold = 0;
                    t->p += 3;
                    while (!tp_at_end(t) && tp_peek(t) == '\'' && fold < 2) {
                        t->p++;
                        fold++;
                    }
                    return mino_string_n(S, (const char *)content,
                                         (size_t)(term - content)
                                         + (size_t)fold);
                }
                tp_adv(t);
                continue;
            }
            {
                mino_val *out = mino_string_n(S, (const char *)content,
                                              (size_t)(t->p - content));
                t->p++;
                return out;
            }
        }
        tp_adv(t);
    }
}

/* ---- scalars ---- */

static mino_val *tp_hook(tp_t *t, mino_val *v)
{
    if (t->pv == NULL || v == NULL) return v;
    {
        mino_val *args = mino_cons(t->S, v, mino_nil(t->S));
        if (args == NULL) return NULL;
        return mino_call(t->S, t->pv, args, t->env);
    }
}

/* Scan digits and underscores into buf at w (underscores dropped).
 * Stops at the first byte that is not part of the run; a misplaced
 * underscore also stops the scan, leaving the cursor on it so the
 * trailer check reports the leftover text. Returns the new w. */
static int tp_scan_digits(tp_t *t, char *buf, int w)
{
    while (!tp_at_end(t)) {
        unsigned char c = tp_peek(t);
        if (tp_is_digit(c)) {
            if (w < TP_NUM_BUF - 8) {
                buf[w++] = (char)c;
            }
            t->p++;
        } else if (c == '_') {
            if (t->p + 1 >= t->end || !tp_is_digit(t->p[1])) {
                break;
            }
            t->p++;
        } else {
            break;
        }
    }
    return w;
}

static int tp_digit_run_len(tp_t *t)
{
    const unsigned char *q = t->p;
    while (q < t->end && tp_is_digit(*q)) q++;
    return (int)(q - t->p);
}

/* Radix integer after the 0x/0o/0b prefix; the caller guaranteed a
 * valid first digit. */
static mino_val *tp_radix_int(tp_t *t, int base, size_t vline, size_t vcol)
{
    unsigned long long acc = 0;
    while (!tp_at_end(t)) {
        unsigned char c = tp_peek(t);
        int d;
        if (c == '_') {
            if (t->p + 1 >= t->end) break;
            d = tp_hex_val(t->p[1]);
            if (d < 0 || d >= base) break;
            t->p++;
            continue;
        }
        d = tp_hex_val(c);
        if (d < 0 || d >= base) break;
        acc = acc * (unsigned long long)base + (unsigned long long)d;
        if (acc > 0x7FFFFFFFFFFFFFFFULL) {
            tp_fail(t, "int-overflow", vline, vcol);
            return NULL;
        }
        t->p++;
    }
    return mino_int(t->S, (long long)acc);
}

/* Date, time, or datetime: shape-check and keep the raw span. The
 * cursor is on the first digit; returns nil-when-not-a-date via the
 * out parameter to distinguish "not a date" from error. */
static int tp_try_date_time(tp_t *t, mino_val **out)
{
    const unsigned char *q = t->p;
    const unsigned char *scan;
    int n = tp_digit_run_len(t);
    *out = NULL;
    if (n == 4 && t->end - q >= 10 && q[4] == '-' && q[7] == '-'
        && tp_is_digit(q[5]) && tp_is_digit(q[6])
        && tp_is_digit(q[8]) && tp_is_digit(q[9])) {
        scan = q + 10;
        if (scan < t->end && (*scan == 'T' || *scan == 't'
                              || *scan == ' ')) {
            const unsigned char *ts = scan + 1;
            if (t->end - ts >= 8
                && tp_is_digit(ts[0]) && tp_is_digit(ts[1]) && ts[2] == ':'
                && tp_is_digit(ts[3]) && tp_is_digit(ts[4]) && ts[5] == ':'
                && tp_is_digit(ts[6]) && tp_is_digit(ts[7])) {
                scan = ts + 8;
                if (scan < t->end && *scan == '.') {
                    const unsigned char *f = scan + 1;
                    while (f < t->end && tp_is_digit(*f)) f++;
                    if (f == scan + 1) return 0;
                    scan = f;
                }
                if (scan < t->end && (*scan == 'Z' || *scan == 'z')) {
                    scan++;
                } else if (scan + 6 <= t->end && (*scan == '+'
                           || *scan == '-')
                           && tp_is_digit(scan[1]) && tp_is_digit(scan[2])
                           && scan[3] == ':'
                           && tp_is_digit(scan[4]) && tp_is_digit(scan[5])) {
                    scan += 6;
                }
            }
        }
        *out = mino_string_n(t->S, (const char *)q,
                             (size_t)(scan - q));
        t->p = scan;
        return 1;
    }
    if (n == 2 && t->end - q >= 8 && q[2] == ':' && q[5] == ':'
        && tp_is_digit(q[0]) && tp_is_digit(q[1])
        && tp_is_digit(q[3]) && tp_is_digit(q[4])
        && tp_is_digit(q[6]) && tp_is_digit(q[7])) {
        const unsigned char *scan = q + 8;
        if (scan < t->end && *scan == '.') {
            const unsigned char *f = scan + 1;
            while (f < t->end && tp_is_digit(*f)) f++;
            if (f == scan + 1) return 0;
            scan = f;
        }
        *out = mino_string_n(t->S, (const char *)q,
                             (size_t)(scan - q));
        t->p = scan;
        return 1;
    }
    return 0;
}

/* Number or date/time scalar. Cursor on a digit, sign, i, or n. */
static mino_val *tp_scalar(tp_t *t, size_t vline, size_t vcol)
{
    mino_state *S = t->S;
    int neg = 0;
    int had_sign = 0;
    unsigned char c0 = tp_peek(t);
    if (c0 == '-' || c0 == '+') {
        neg = (c0 == '-');
        had_sign = 1;
        t->p++;
        if (tp_at_end(t)) {
            return tp_fail_here(t, "invalid-value");
        }
        c0 = tp_peek(t);
    }
    if (c0 == 'i' || c0 == 'n') {
        if (t->end - t->p >= 3 && memcmp(t->p, "inf", 3) == 0) {
            t->p += 3;
            return mino_float(S, neg ? -INFINITY : INFINITY);
        }
        if (t->end - t->p >= 3 && memcmp(t->p, "nan", 3) == 0) {
            t->p += 3;
            return mino_float(S, (double)NAN);
        }
        return tp_fail_here(t, "invalid-value");
    }
    if (!tp_is_digit(c0)) {
        return tp_fail_here(t, "invalid-value");
    }
    if (!had_sign) {
        mino_val *dt = NULL;
        int got = tp_try_date_time(t, &dt);
        if (got) {
            return dt;
        }
    }
    if (c0 == '0' && !had_sign && t->p + 1 < t->end) {
        unsigned char r = t->p[1];
        int base = -1;
        if (r == 'x' || r == 'X') base = 16;
        if (r == 'o' || r == 'O') base = 8;
        if (r == 'b' || r == 'B') base = 2;
        if (base > 0 && t->p + 2 < t->end) {
            int d = tp_hex_val(t->p[2]);
            if (d >= 0 && d < base) {
                t->p += 2;
                return tp_radix_int(t, base, vline, vcol);
            }
        }
    }
    {
        char buf[TP_NUM_BUF];
        int w = 0;
        int is_float = 0;
        unsigned long long acc = 0;
        int i;
        if (neg) buf[w++] = '-';
        if (c0 == '0') {
            /* Leading zero: the integer part is exactly "0" and any
             * further digit or underscore is leftover for the
             * trailer check (matches the oracle's 01 error). */
            buf[w++] = '0';
            t->p++;
            if (tp_at_end(t) || (tp_peek(t) != '.' && tp_peek(t) != 'e'
                                 && tp_peek(t) != 'E')) {
                return mino_int(S, 0);
            }
        } else {
            w = tp_scan_digits(t, buf, w);
        }
        for (i = neg ? 1 : 0; i < w; i++) {
            acc = acc * 10 + (unsigned long long)(buf[i] - '0');
            if (acc > 0x7FFFFFFFFFFFFFFFULL
                         + (unsigned long long)(neg ? 1 : 0)) {
                tp_fail(t, "int-overflow", vline, vcol);
                return NULL;
            }
        }
        if (!tp_at_end(t) && tp_peek(t) == '.'
            && t->p + 1 < t->end && tp_is_digit(t->p[1])) {
            is_float = 1;
            buf[w++] = '.';
            t->p++;
            w = tp_scan_digits(t, buf, w);
        }
        if (!tp_at_end(t) && (tp_peek(t) == 'e' || tp_peek(t) == 'E')) {
            const unsigned char *q = t->p + 1;
            int ok = 0;
            if (q < t->end && tp_is_digit(*q)) ok = 1;
            else if (q < t->end && (*q == '+' || *q == '-')
                     && q + 1 < t->end && tp_is_digit(q[1])) ok = 1;
            if (ok) {
                is_float = 1;
                buf[w++] = 'e';
                t->p++;
                if (!tp_at_end(t)
                    && (tp_peek(t) == '+' || tp_peek(t) == '-')) {
                    buf[w++] = (char)tp_peek(t);
                    t->p++;
                }
                w = tp_scan_digits(t, buf, w);
            }
        }
        if (is_float) {
            buf[w] = '\0';
            return mino_float(S, strtod(buf, NULL));
        }
        return mino_int(S, neg ? -(long long)acc : (long long)acc);
    }
}

/* ---- collections ---- */

static mino_val *tp_array(tp_t *t, size_t vline, size_t vcol)
{
    mino_state *S = t->S;
    mino_val *cur;
    if (++t->depth > TP_MAX_DEPTH) {
        t->depth--;
        tp_fail(t, "invalid-value", vline, vcol);
        return NULL;
    }
    cur = mino_transient(S, mino_vector(S, NULL, 0));
    if (cur == NULL) { t->depth--; return NULL; }
    t->p++;
    for (;;) {
        mino_val *v;
        mino_val *next;
        tp_skip_array_ws(t);
        if (tp_at_end(t)) {
            t->depth--;
            tp_fail(t, "unterminated-array", vline, vcol);
            return NULL;
        }
        if (tp_peek(t) == ']') {
            t->p++;
            t->depth--;
            return mino_persistent(S, cur);
        }
        v = tp_value(t);
        if (v == NULL) { t->depth--; return NULL; }
        next = mino_conj_bang(S, cur, v);
        if (next == NULL) { t->depth--; return NULL; }
        cur = next;
        tp_skip_array_ws(t);
        if (tp_at_end(t)) {
            t->depth--;
            tp_fail(t, "unterminated-array", vline, vcol);
            return NULL;
        }
        if (tp_peek(t) == ',') {
            t->p++;
            continue;
        }
        if (tp_peek(t) == ']') {
            t->p++;
            t->depth--;
            return mino_persistent(S, cur);
        }
        t->depth--;
        return tp_fail_here(t, "expected-separator");
    }
}

/* Assoc v at the dotted key parts inside inline map m. */
static mino_val *tp_inline_assoc(tp_t *t, mino_val *m, mino_val **parts,
                                 int idx, int n, mino_val *v,
                                 size_t kline, size_t kcol)
{
    mino_val *k = parts[idx];
    mino_val *child;
    if (idx == n - 1) {
        if (map_get_val(m, k) != NULL) {
            tp_fail(t, "duplicate-key", kline, kcol);
            return NULL;
        }
        return tp_map_assoc(t, m, k, v);
    }
    child = map_get_val(m, k);
    if (child != NULL && mino_type_of(child) == MINO_MAP) {
        mino_val *child2 = tp_inline_assoc(t, child, parts, idx + 1, n, v,
                                           kline, kcol);
        if (child2 == NULL) return NULL;
        return tp_map_assoc(t, m, k, child2);
    }
    if (child != NULL) {
        tp_fail(t, "overwrite-value", kline, kcol);
        return NULL;
    }
    {
        mino_val *empty = tp_empty_map(t);
        mino_val *child2;
        if (empty == NULL) return NULL;
        child2 = tp_inline_assoc(t, empty, parts, idx + 1, n, v,
                                 kline, kcol);
        if (child2 == NULL) return NULL;
        return tp_map_assoc(t, m, k, child2);
    }
}

/* One dotted key = value pair inside an inline table. */
static mino_val *tp_inline_pair(tp_t *t, mino_val *m)
{
    mino_val *parts[TP_MAX_PARTS];
    int n = 0;
    size_t kline = t->line;
    size_t kcol = (size_t)(t->p - t->line_start) + 1;
    mino_val *v;
    for (;;) {
        mino_val *part;
        tp_skip_inline_ws(t);
        if (tp_at_end(t)) {
            return tp_fail_here(t, "unterminated-inline");
        }
        if (tp_peek(t) == '"') {
            size_t vl = t->line;
            size_t vc = (size_t)(t->p - t->line_start) + 1;
            part = tp_basic_string(t, 0, vl, vc);
        } else if (tp_peek(t) == '\'') {
            size_t vl = t->line;
            size_t vc = (size_t)(t->p - t->line_start) + 1;
            part = tp_literal_string(t, 0, vl, vc);
        } else if (tp_is_bare_key_byte(tp_peek(t))) {
            const unsigned char *s = t->p;
            while (!tp_at_end(t) && tp_is_bare_key_byte(tp_peek(t))) {
                t->p++;
            }
            part = mino_string_n(t->S, (const char *)s,
                                 (size_t)(t->p - s));
        } else {
            return tp_fail_here(t, "invalid-inline");
        }
        if (part == NULL) return NULL;
        {
            mino_val *kw = mino_keyword_n(t->S, part->as.s.data,
                                          part->as.s.len);
            if (kw == NULL) return NULL;
            parts[n++] = kw;
        }
        if (n >= TP_MAX_PARTS) {
            return tp_fail_here(t, "invalid-inline");
        }
        tp_skip_inline_ws(t);
        if (!tp_at_end(t) && tp_peek(t) == '.') {
            t->p++;
            continue;
        }
        break;
    }
    if (!tp_eat(t, '=')) {
        return tp_fail_here(t, "invalid-inline");
    }
    tp_skip_inline_ws(t);
    v = tp_value(t);
    if (v == NULL) return NULL;
    return tp_inline_assoc(t, m, parts, 0, n, v, kline, kcol);
}

static mino_val *tp_inline_table(tp_t *t, size_t vline, size_t vcol)
{
    mino_val *m = tp_empty_map(t);
    if (m == NULL) return NULL;
    if (++t->depth > TP_MAX_DEPTH) {
        t->depth--;
        tp_fail(t, "invalid-value", vline, vcol);
        return NULL;
    }
    t->p++;
    tp_skip_inline_ws(t);
    if (tp_at_end(t)) {
        t->depth--;
        tp_fail(t, "unterminated-inline", vline, vcol);
        return NULL;
    }
    if (tp_peek(t) == '\n') {
        /* A newline with nothing but blanks after it is an
         * unterminated table; content after it is the newline
         * error. */
        const unsigned char *q = t->p + 1;
        while (q < t->end && (*q == ' ' || *q == '\t' || *q == '\n')) {
            q++;
        }
        if (q >= t->end) {
            t->depth--;
            tp_fail(t, "unterminated-inline", vline, vcol);
            return NULL;
        }
        t->depth--;
        return tp_fail_here(t, "newline-in-inline");
    }
    if (tp_peek(t) == '}') {
        t->p++;
        t->depth--;
        return m;
    }
    for (;;) {
        mino_val *m2 = tp_inline_pair(t, m);
        if (m2 == NULL) { t->depth--; return NULL; }
        m = m2;
        tp_skip_inline_ws(t);
        if (tp_at_end(t)) {
            t->depth--;
            tp_fail(t, "unterminated-inline", vline, vcol);
            return NULL;
        }
        if (tp_peek(t) == ',') {
            t->p++;
            tp_skip_inline_ws(t);
            if (tp_at_end(t)) {
                t->depth--;
                tp_fail(t, "unterminated-inline", vline, vcol);
                return NULL;
            }
            if (tp_peek(t) == '\n') {
                t->depth--;
                return tp_fail_here(t, "newline-in-inline");
            }
            if (tp_peek(t) == '}') {
                t->depth--;
                return tp_fail_here(t, "trailing-comma");
            }
            continue;
        }
        if (tp_peek(t) == '}') {
            t->p++;
            t->depth--;
            return m;
        }
        if (tp_peek(t) == '\n') {
            t->depth--;
            return tp_fail_here(t, "newline-in-inline");
        }
        t->depth--;
        return tp_fail_here(t, "trailing-comma");
    }
}

/* ---- values ---- */

static mino_val *tp_value(tp_t *t)
{
    size_t vline = t->line;
    size_t vcol = (size_t)(t->p - t->line_start) + 1;
    unsigned char c;
    mino_val *v;
    if (tp_at_end(t)) {
        return tp_fail_here(t, "invalid-value");
    }
    c = tp_peek(t);
    if (c == '"') {
        int ml = (t->end - t->p >= 3 && t->p[1] == '"' && t->p[2] == '"');
        v = tp_basic_string(t, ml, vline, vcol);
    } else if (c == '\'') {
        int ml = (t->end - t->p >= 3 && t->p[1] == '\'' && t->p[2] == '\'');
        v = tp_literal_string(t, ml, vline, vcol);
    } else if (c == '[') {
        return tp_array(t, vline, vcol);
    } else if (c == '{') {
        return tp_inline_table(t, vline, vcol);
    } else if (c == 't') {
        if (t->end - t->p >= 4 && memcmp(t->p, "true", 4) == 0) {
            t->p += 4;
            v = mino_true(t->S);
        } else {
            return tp_fail_here(t, "invalid-value");
        }
    } else if (c == 'f') {
        if (t->end - t->p >= 5 && memcmp(t->p, "false", 5) == 0) {
            t->p += 5;
            v = mino_false(t->S);
        } else {
            return tp_fail_here(t, "invalid-value");
        }
    } else {
        v = tp_scalar(t, vline, vcol);
        if (v == NULL) return NULL;
        return tp_hook(t, v);
    }
    if (v == NULL) return NULL;
    return tp_hook(t, v);
}

/* ---- table navigation ---- */

static mino_val *tp_deep_set(tp_t *t, mino_val *m, mino_val **parts,
                             int idx, int n, int keylen, mino_val *v)
{
    mino_val *k = parts[idx];
    mino_val *child;
    int in_key = (idx >= n - keylen);
    if (idx == n - 1) {
        if (map_get_val(m, k) != NULL) {
            return tp_fail_stmt(t, "duplicate-key");
        }
        return tp_map_assoc(t, m, k, v);
    }
    child = map_get_val(m, k);
    if (child != NULL && mino_type_of(child) == MINO_MAP) {
        long long fl = tp_flags_get(t, parts, idx + 1);
        mino_val *child2;
        if (in_key && (fl & TP_INLINE)) {
            return tp_fail_stmt(t, "redefine-inline");
        }
        if (in_key && (fl & TP_DECLARED)) {
            return tp_fail_stmt(t, "redeclare");
        }
        child2 = tp_deep_set(t, child, parts, idx + 1, n, keylen, v);
        if (child2 == NULL) return NULL;
        return tp_map_assoc(t, m, k, child2);
    }
    if (child != NULL && mino_type_of(child) == MINO_VECTOR) {
        long long fl = tp_flags_get(t, parts, idx + 1);
        mino_val *last;
        mino_val *last2;
        mino_val *newv;
        if (!(fl & TP_AOT) || child->as.vec.len == 0) {
            return tp_fail_stmt(t, "overwrite-value");
        }
        last = vec_nth(child, child->as.vec.len - 1);
        last2 = tp_deep_set(t, last, parts, idx + 1, n, keylen, v);
        if (last2 == NULL) return NULL;
        newv = vec_assoc1(t->S, child, child->as.vec.len - 1, last2);
        if (newv == NULL) return NULL;
        return tp_map_assoc(t, m, k, newv);
    }
    if (child != NULL) {
        return tp_fail_stmt(t, "overwrite-value");
    }
    {
        mino_val *empty = tp_empty_map(t);
        mino_val *child2;
        if (empty == NULL) return NULL;
        child2 = tp_deep_set(t, empty, parts, idx + 1, n, keylen, v);
        if (child2 == NULL) return NULL;
        return tp_map_assoc(t, m, k, child2);
    }
}

static mino_val *tp_nav_table(tp_t *t, mino_val *m, mino_val **parts,
                              int idx, int n)
{
    mino_val *k = parts[idx];
    mino_val *child = map_get_val(m, k);
    if (idx == n - 1) {
        if (child == NULL) {
            return tp_map_assoc(t, m, k, tp_empty_map(t));
        }
        if (mino_type_of(child) == MINO_MAP) return m;
        return tp_fail_stmt(t, "overwrite-value");
    }
    if (child != NULL && mino_type_of(child) == MINO_MAP) {
        mino_val *child2 = tp_nav_table(t, child, parts, idx + 1, n);
        if (child2 == NULL) return NULL;
        return tp_map_assoc(t, m, k, child2);
    }
    if (child != NULL && mino_type_of(child) == MINO_VECTOR) {
        long long fl = tp_flags_get(t, parts, idx + 1);
        mino_val *last, *last2, *newv;
        if (!(fl & TP_AOT) || child->as.vec.len == 0) {
            return tp_fail_stmt(t, "redeclare");
        }
        last = vec_nth(child, child->as.vec.len - 1);
        last2 = tp_nav_table(t, last, parts, idx + 1, n);
        if (last2 == NULL) return NULL;
        newv = vec_assoc1(t->S, child, child->as.vec.len - 1, last2);
        if (newv == NULL) return NULL;
        return tp_map_assoc(t, m, k, newv);
    }
    if (child != NULL) {
        return tp_fail_stmt(t, "overwrite-value");
    }
    {
        mino_val *empty = tp_empty_map(t);
        mino_val *child2;
        if (empty == NULL) return NULL;
        child2 = tp_nav_table(t, empty, parts, idx + 1, n);
        if (child2 == NULL) return NULL;
        return tp_map_assoc(t, m, k, child2);
    }
}

static mino_val *tp_nav_array(tp_t *t, mino_val *m, mino_val **parts,
                              int idx, int n)
{
    mino_val *k = parts[idx];
    mino_val *child = map_get_val(m, k);
    if (idx == n - 1) {
        if (child == NULL) {
            mino_val *first = tp_empty_map(t);
            mino_val *vec0 = mino_vector(t->S, &first, 1);
            if (vec0 == NULL) return NULL;
            return tp_map_assoc(t, m, k, vec0);
        }
        if (mino_type_of(child) == MINO_VECTOR) {
            mino_val *grown = vec_conj1(t->S, child, tp_empty_map(t));
            if (grown == NULL) return NULL;
            return tp_map_assoc(t, m, k, grown);
        }
        return tp_fail_stmt(t, "overwrite-value");
    }
    if (child != NULL && mino_type_of(child) == MINO_MAP) {
        mino_val *child2 = tp_nav_array(t, child, parts, idx + 1, n);
        if (child2 == NULL) return NULL;
        return tp_map_assoc(t, m, k, child2);
    }
    if (child != NULL && mino_type_of(child) == MINO_VECTOR) {
        long long fl = tp_flags_get(t, parts, idx + 1);
        mino_val *last, *last2, *newv;
        if (!(fl & TP_AOT) || child->as.vec.len == 0) {
            return tp_fail_stmt(t, "redeclare");
        }
        last = vec_nth(child, child->as.vec.len - 1);
        last2 = tp_nav_array(t, last, parts, idx + 1, n);
        if (last2 == NULL) return NULL;
        newv = vec_assoc1(t->S, child, child->as.vec.len - 1, last2);
        if (newv == NULL) return NULL;
        return tp_map_assoc(t, m, k, newv);
    }
    if (child != NULL) {
        return tp_fail_stmt(t, "overwrite-value");
    }
    {
        mino_val *empty = tp_empty_map(t);
        mino_val *child2;
        if (empty == NULL) return NULL;
        child2 = tp_nav_array(t, empty, parts, idx + 1, n);
        if (child2 == NULL) return NULL;
        return tp_map_assoc(t, m, k, child2);
    }
}

/* ---- statements ---- */

/* Parse a keypath into parts; returns the part count or -1. */
static int tp_keypath(tp_t *t, mino_val **parts)
{
    int n = 0;
    for (;;) {
        mino_val *part;
        tp_skip_inline_ws(t);
        if (tp_at_end(t)) return -1;
        if (tp_peek(t) == '"') {
            size_t vl = t->line;
            size_t vc = (size_t)(t->p - t->line_start) + 1;
            part = tp_basic_string(t, 0, vl, vc);
        } else if (tp_peek(t) == '\'') {
            size_t vl = t->line;
            size_t vc = (size_t)(t->p - t->line_start) + 1;
            part = tp_literal_string(t, 0, vl, vc);
        } else if (tp_is_bare_key_byte(tp_peek(t))) {
            const unsigned char *s = t->p;
            while (!tp_at_end(t) && tp_is_bare_key_byte(tp_peek(t))) {
                t->p++;
            }
            part = mino_string_n(t->S, (const char *)s,
                                 (size_t)(t->p - s));
        } else {
            return -1;
        }
        if (part == NULL) return -1;
        {
            mino_val *kw = mino_keyword_n(t->S, part->as.s.data,
                                          part->as.s.len);
            if (kw == NULL) return -1;
            parts[n++] = kw;
        }
        if (n >= TP_MAX_PARTS) return -1;
        tp_skip_inline_ws(t);
        if (!tp_at_end(t) && tp_peek(t) == '.') {
            t->p++;
            continue;
        }
        return n;
    }
}

/* Header statement; cursor on '['. */
static int tp_header(tp_t *t)
{
    mino_val *parts[TP_MAX_PARTS];
    int n;
    int aot = 0;
    long long fl;
    int i;
    t->p++;
    if (!tp_at_end(t) && tp_peek(t) == '[') {
        aot = 1;
        t->p++;
    }
    n = tp_keypath(t, parts);
    if (n < 0) {
        tp_fail_stmt(t, "invalid-statement");
        return -1;
    }
    tp_skip_inline_ws(t);
    if (!tp_eat(t, ']')) {
        tp_fail_stmt(t, "invalid-statement");
        return -1;
    }
    if (aot && !tp_eat(t, ']')) {
        tp_fail_stmt(t, "invalid-statement");
        return -1;
    }
    if (tp_expect_eol(t) != 0) {
        return -1;
    }
    fl = tp_flags_get(t, parts, n);
    if (!aot) {
        if (fl & (TP_DECLARED | TP_AOT | TP_DOTTED)) {
            tp_fail_stmt(t, "redeclare");
            return -1;
        }
    } else {
        if (fl & (TP_DECLARED | TP_DOTTED)) {
            tp_fail_stmt(t, "redeclare");
            return -1;
        }
    }
    for (i = 0; i < n; i++) {
        if (tp_flags_get(t, parts, i + 1) & TP_INLINE) {
            tp_fail_stmt(t, "redeclare");
            return -1;
        }
    }
    if (aot) {
        mino_val *m = tp_nav_array(t, t->root, parts, 0, n);
        if (m == NULL) return -1;
        t->root = m;
        if (tp_flags_set(t, parts, n, TP_AOT) != 0) return -1;
    } else {
        mino_val *m = tp_nav_table(t, t->root, parts, 0, n);
        if (m == NULL) return -1;
        t->root = m;
        if (tp_flags_set(t, parts, n, TP_DECLARED) != 0) return -1;
    }
    for (i = 0; i < n; i++) {
        t->cur[i] = parts[i];
    }
    t->cur_n = n;
    return 0;
}

/* Key/value statement; cursor on the key start. */
static int tp_keyval(tp_t *t)
{
    mino_val *parts[TP_MAX_PARTS];
    mino_val *full[TP_MAX_PARTS];
    int n, i, total;
    mino_val *v;
    n = tp_keypath(t, parts);
    if (n < 0) {
        tp_fail_stmt(t, "invalid-statement");
        return -1;
    }
    if (!tp_eat(t, '=')) {
        tp_fail_stmt(t, "missing-equals");
        return -1;
    }
    tp_skip_inline_ws(t);
    v = tp_value(t);
    if (v == NULL) return -1;
    if (tp_expect_eol(t) != 0) {
        return -1;
    }
    total = t->cur_n + n;
    if (total >= TP_MAX_PARTS) {
        tp_fail_stmt(t, "invalid-statement");
        return -1;
    }
    for (i = 0; i < t->cur_n; i++) {
        full[i] = t->cur[i];
    }
    for (i = 0; i < n; i++) {
        full[t->cur_n + i] = parts[i];
    }
    {
        mino_val *m = tp_deep_set(t, t->root, full, 0, total, n, v);
        if (m == NULL) return -1;
        t->root = m;
    }
    for (i = t->cur_n + 1; i <= total; i++) {
        if (tp_flags_set(t, full, i, TP_DOTTED) != 0) return -1;
    }
    if (mino_type_of(v) == MINO_MAP) {
        if (tp_mark_inline(t, v, full, total) != 0) return -1;
    }
    return 0;
}

/* ---- prim ---- */

static mino_val *prim_toml_parse(mino_state *S, mino_val *args,
                                 mino_env *env)
{
    mino_val *s_val;
    mino_val *pv_val;
    tp_t t;
    const unsigned char *data;
    size_t len;
    size_t i;
    (void)env;
    if (!mino_is_cons(args)
        || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "toml-parse requires two arguments");
    }
    s_val = args->as.cons.car;
    pv_val = args->as.cons.cdr->as.cons.car;
    if (s_val == NULL || mino_type_of(s_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "toml-parse: first argument must be a string");
    }
    if (pv_val == NULL || mino_type_of(pv_val) == MINO_NIL) {
        pv_val = NULL;
    }
    data = (const unsigned char *)s_val->as.s.data;
    len = s_val->as.s.len;
    for (i = 0; i < len; i++) {
        if (data[i] == '\r') break;
    }
    if (i < len) {
        /* Normalize CRLF to LF; a lone CR is an error. */
        unsigned char *buf;
        size_t w = 0;
        int saved = mino_current_ctx(S)->gc_depth;
        mino_current_ctx(S)->gc_depth = saved + 1;
        buf = (unsigned char *)gc_alloc_typed_inner(S, GC_T_RAW, len + 1);
        mino_current_ctx(S)->gc_depth = saved;
        if (buf == NULL) return NULL;
        for (i = 0; i < len; i++) {
            if (data[i] == '\r') {
                if (i + 1 < len && data[i + 1] == '\n') {
                    buf[w++] = '\n';
                    i++;
                } else {
                    mino_val *items[5];
                    size_t ln = 1;
                    size_t j = i;
                    size_t col;
                    while (j > 0 && data[j - 1] != '\n') j--;
                    ln = 1;
                    {
                        size_t k;
                        for (k = 0; k < j; k++) {
                            if (data[k] == '\n') ln++;
                        }
                    }
                    col = i - j + 1;
                    items[0] = mino_keyword(S, "toml/error");
                    items[1] = mino_string(S, "invalid-character");
                    items[2] = mino_int(S, (long long)ln);
                    items[3] = mino_int(S, (long long)col);
                    items[4] = mino_string_n(S, (const char *)"", 0);
                    return mino_vector(S, items, 5);
                }
            } else {
                buf[w++] = data[i];
            }
        }
        data = buf;
        len = w;
    }
    memset(&t, 0, sizeof(t));
    t.S = S;
    t.env = env;
    t.p = data;
    t.end = data + len;
    t.line_start = data;
    t.line = 1;
    t.pv = pv_val;
    t.stmt_start = data;
    t.root = mino_map(S, NULL, NULL, 0);
    t.flags = mino_map(S, NULL, NULL, 0);
    if (t.root == NULL || t.flags == NULL) return NULL;

    while (t.p < t.end) {
        t.stmt_line = t.line;
        t.stmt_start = t.line_start;
        tp_skip_inline_ws(&t);
        if (t.p >= t.end) break;
        if (tp_peek(&t) == '\n') {
            tp_adv(&t);
            continue;
        }
        if (tp_peek(&t) == '#') {
            while (t.p < t.end && tp_peek(&t) != '\n') {
                t.p++;
            }
            continue;
        }
        if (tp_peek(&t) == '[') {
            if (tp_header(&t) != 0) goto fail;
            continue;
        }
        if (tp_keyval(&t) != 0) goto fail;
    }
    return t.root;

fail:
    {
        mino_val *items[5];
        items[0] = mino_keyword(S, "toml/error");
        items[1] = mino_string(S, t.err_reason ? t.err_reason
                                               : "invalid-value");
        items[2] = mino_int(S, (long long)t.err_line);
        items[3] = mino_int(S, (long long)t.err_col);
        items[4] = mino_string_n(S,
                                 t.err_from ? (const char *)t.err_from : "",
                                 t.err_from ? (size_t)(t.err_to - t.err_from)
                                            : (size_t)0);
        return mino_vector(S, items, 5);
    }
}

const mino_prim_def k_prims_toml[] = {
    {"toml-parse", prim_toml_parse,
     "Parses TOML text into plain nested maps with keyword keys. "
     "Second argument is an optional leaf-value transform (the "
     "mino.toml {:parse-values f} hook) or nil. Returns the parsed "
     "map, or an error descriptor vector the mino.toml facade "
     "converts to ex-info."},
};

const size_t k_prims_toml_count =
    sizeof(k_prims_toml) / sizeof(k_prims_toml[0]);
