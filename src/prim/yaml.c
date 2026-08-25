/*
 * yaml.c -- native YAML 1.2 subset reader (ADR 26).
 *
 * Single-pass byte-cursor parser backing mino.yaml/parse-string and
 * parse-string-all; the facade (argument validation, the ex-info
 * error shape) stays Clojure. Semantics follow the yaml-test-suite
 * examples for the in-subset shapes, ported from the green mino-side
 * reader this prim replaced: block maps and sequences by indentation
 * with the compact forms, flow collections, plain and quoted scalars
 * with line folding, literal and folded block scalars with chomping
 * and indentation indicators, comments, and ---/... documents.
 * Anchors, aliases, tags, complex keys, and directives are rejected
 * with their own reasons. Plain scalars resolve through the 1.2 core
 * schema; keywordization of string keys is driven by the flag.
 *
 * The prim returns a vector of documents, or an error descriptor
 * [:yaml/error "reason" line col "text"] the facade converts to
 * ex-info. Rooting follows toml.c/json.c: in-flight values live in C
 * locals the conservative stack scan sees, and the raw-pointer
 * windows (scalar scratch buffers) are GC_T_RAW memory held under a
 * gc_depth guard.
 */

#include "prim/internal.h"
#include "mino.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define YP_MAX_DEPTH  200
#define YP_NO_POS     ((size_t)-1)

typedef struct {
    mino_state          *S;
    mino_env            *env;
    const unsigned char *data;
    size_t               len;
    int                  keywords;
    int                  depth;
    /* error capture: set once, then NULL propagates */
    const char          *err_reason;
    size_t               err_pos;
} yp_t;

/* forward declarations */
static mino_val *yp_block_node_r(yp_t *t, size_t p, long pi, size_t *resume);
static mino_val *yp_block_map(yp_t *t, size_t p, long I, size_t *resume);
static mino_val *yp_block_seq(yp_t *t, size_t p, long I, size_t *resume);
static mino_val *yp_flow_node(yp_t *t, size_t p, long pi, int key_mode,
                              size_t *out_end);
static mino_val *yp_quoted(yp_t *t, size_t p, long pi, int key_mode,
                           size_t *out_end);
static mino_val *yp_inline_value(yp_t *t, size_t p, long I, size_t *resume);
static mino_val *yp_lookahead_value(yp_t *t, size_t p, long I,
                                    int dash_value, size_t *resume);
static mino_val *yp_marker_line_root(yp_t *t, size_t p, size_t *resume);
static int yp_next_doc(yp_t *t, size_t p, size_t *resume, mino_val **doc,
                       int *emitted);

/* ---- errors ---- */

static void yp_fail(yp_t *t, const char *reason, size_t pos)
{
    if (t->err_reason == NULL) {
        t->err_reason = reason;
        t->err_pos = pos;
    }
}

static void yp_loc(yp_t *t, size_t pos, size_t *line, size_t *col)
{
    size_t ln = 1;
    size_t ls = 0;
    size_t i;
    for (i = 0; i < pos && i < t->len; i++) {
        if (t->data[i] == '\n') {
            ln++;
            ls = i + 1;
        }
    }
    *line = ln;
    *col = pos - ls + 1;
}

/* ---- cursor ---- */

static int yp_is(yp_t *t, size_t p, unsigned char c)
{
    return p < t->len && t->data[p] == c;
}

static unsigned char yp_peek(yp_t *t, size_t p)
{
    return (p < t->len) ? t->data[p] : 0;
}

static int yp_ws_p(unsigned char c)
{
    return c == ' ' || c == '\t';
}

static size_t yp_line_start(yp_t *t, size_t p)
{
    size_t q = (p > t->len) ? t->len : p;
    while (q > 0 && t->data[q - 1] != '\n') q--;
    return q;
}

static long yp_col(yp_t *t, size_t p)
{
    return (long)(p - yp_line_start(t, p));
}

static long yp_line_indent(yp_t *t, size_t p)
{
    size_t ls = yp_line_start(t, p);
    size_t q = ls;
    while (q < t->len && t->data[q] == ' ') q++;
    return (long)(q - ls);
}

static size_t yp_eol(yp_t *t, size_t p)
{
    size_t q = (p > t->len) ? t->len : p;
    while (q < t->len && t->data[q] != '\n') q++;
    return q;
}

static size_t yp_next_line(yp_t *t, size_t p)
{
    size_t q = yp_eol(t, p);
    return (q < t->len) ? q + 1 : q;
}

static size_t yp_first_nonspace(yp_t *t, size_t ls)
{
    size_t q = ls;
    while (q < t->len && t->data[q] == ' ') q++;
    return q;
}

static size_t yp_content_start(yp_t *t, size_t p)
{
    size_t ls = yp_line_start(t, p);
    size_t q = ls;
    for (;;) {
        if (q >= t->len || t->data[q] == '\n') return YP_NO_POS;
        if (t->data[q] == ' ') { q++; continue; }
        if (t->data[q] == '#' && (q == ls || t->data[q - 1] == ' ')) {
            return YP_NO_POS;
        }
        return q;
    }
}

static size_t yp_next_content(yp_t *t, size_t p)
{
    size_t q = yp_line_start(t, p);
    for (;;) {
        if (q >= t->len) return YP_NO_POS;
        {
            size_t eol = yp_eol(t, q);
            size_t cs = yp_content_start(t, q);
            if (cs != YP_NO_POS) return cs;
            if (eol >= t->len) return YP_NO_POS;
            q = eol + 1;
        }
    }
}

static size_t yp_skip_ws(yp_t *t, size_t p)
{
    while (p < t->len && yp_ws_p(t->data[p])) p++;
    return p;
}

static int yp_at_eol(yp_t *t, size_t p)
{
    return p >= t->len || t->data[p] == '\n';
}

static int yp_sentinel_line(yp_t *t, size_t p)
{
    return yp_is(t, p, '\n') && (p + 1 >= t->len);
}

static int yp_dash_entry(yp_t *t, size_t p)
{
    unsigned char d;
    if (!yp_is(t, p, '-')) return 0;
    d = yp_peek(t, p + 1);
    return d == 0 || d == '\n' || yp_ws_p(d);
}

static int yp_doc_marker(yp_t *t, size_t p)
{
    unsigned char c;
    unsigned char d;
    if (p == YP_NO_POS || p >= t->len) return 0;
    if (p != yp_line_start(t, p)) return 0;
    c = t->data[p];
    if (c != '-' && c != '.') return 0;
    if (t->data[p + 1] != c || t->data[p + 2] != c) return 0;
    d = yp_peek(t, p + 3);
    return d == 0 || d == '\n' || yp_ws_p(d);
}

static int yp_line_clear(yp_t *t, size_t p)
{
    size_t q = yp_skip_ws(t, p);
    if (yp_at_eol(t, q)) return 1;
    if (t->data[q] == '#') {
        size_t ls = yp_line_start(t, q);
        if (q == ls || yp_ws_p(t->data[q - 1])) return 1;
    }
    return 0;
}

static void yp_check_line_clear(yp_t *t, size_t p)
{
    size_t q = yp_skip_ws(t, p);
    if (yp_at_eol(t, q)) return;
    if (t->data[q] == '#') {
        size_t ls = yp_line_start(t, q);
        if (q == ls || yp_ws_p(t->data[q - 1])) return;
    }
    yp_fail(t, "unexpected-content", q);
}

/* ---- scratch buffer (GC_T_RAW under a gc_depth guard) ---- */

typedef struct {
    unsigned char *buf;
    size_t         len;
    size_t         cap;
    int            saved_depth;
} yp_buf_t;

static int yp_buf_init(yp_t *t, yp_buf_t *b, size_t cap)
{
    mino_state *S = t->S;
    b->saved_depth = mino_current_ctx(S)->gc_depth;
    mino_current_ctx(S)->gc_depth = b->saved_depth + 1;
    b->cap = cap ? cap : 64;
    b->len = 0;
    b->buf = (unsigned char *)gc_alloc_typed_inner(S, GC_T_RAW, b->cap);
    if (b->buf == NULL) {
        mino_current_ctx(S)->gc_depth = b->saved_depth;
        return -1;
    }
    return 0;
}

static void yp_buf_done(yp_t *t, yp_buf_t *b)
{
    mino_current_ctx(t->S)->gc_depth = b->saved_depth;
}

static int yp_buf_grow(yp_t *t, yp_buf_t *b, size_t need)
{
    mino_state *S = t->S;
    unsigned char *nb;
    size_t cap = b->cap;
    if (b->len + need <= cap) return 0;
    while (cap < b->len + need) cap *= 2;
    nb = (unsigned char *)gc_alloc_typed_inner(S, GC_T_RAW, cap);
    if (nb == NULL) return -1;
    memcpy(nb, b->buf, b->len);
    b->buf = nb;
    b->cap = cap;
    return 0;
}

static int yp_buf_putn(yp_t *t, yp_buf_t *b, const unsigned char *s, size_t n)
{
    if (n == 0) return 0;
    if (yp_buf_grow(t, b, n) != 0) return -1;
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    return 0;
}

static int yp_buf_put1(yp_t *t, yp_buf_t *b, unsigned char c)
{
    if (yp_buf_grow(t, b, 1) != 0) return -1;
    b->buf[b->len++] = c;
    return 0;
}

static int yp_buf_put_rep(yp_t *t, yp_buf_t *b, unsigned char c, long n)
{
    long i;
    for (i = 0; i < n; i++) {
        if (yp_buf_put1(t, b, c) != 0) return -1;
    }
    return 0;
}

static void yp_buf_trim_ws(yp_buf_t *b)
{
    while (b->len > 0 && yp_ws_p(b->buf[b->len - 1])) b->len--;
}

static mino_val *yp_buf_string(yp_t *t, yp_buf_t *b)
{
    return mino_string_n(t->S, (const char *)b->buf, b->len);
}

static void yp_span_trim(yp_t *t, size_t p, size_t q,
                         const unsigned char **out, size_t *out_len)
{
    while (q > p && yp_ws_p(t->data[q - 1])) q--;
    *out = t->data + p;
    *out_len = q - p;
}

/* ---- core schema resolution ---- */

static int yp_digits_int(yp_t *t, size_t p, const unsigned char *s, size_t n,
                         int neg, mino_val **out)
{
    long long acc = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        int d = s[i] - '0';
        if (acc > 922337203685477580LL) {
            yp_fail(t, "int-overflow", p);
            return -1;
        }
        if (acc == 922337203685477580LL) {
            if (d == 8 && neg && i == n - 1) {
                *out = mino_int(t->S, (-9223372036854775807LL) - 1);
                return (*out == NULL) ? -1 : 0;
            }
            if (d > (neg ? 8 : 7)) {
                yp_fail(t, "int-overflow", p);
                return -1;
            }
        }
        acc = acc * 10 + d;
    }
    *out = mino_int(t->S, neg ? -acc : acc);
    return (*out == NULL) ? -1 : 0;
}

static int yp_radix_int(yp_t *t, const unsigned char *s, size_t n,
                        int base, int offset, mino_val **out)
{
    long long acc = 0;
    int i;
    for (i = offset; i < (int)n; i++) {
        unsigned char c = s[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else d = -1;
        if (d < 0 || d >= base) break;
        acc = acc * base + d;
    }
    *out = mino_int(t->S, acc);
    return (*out == NULL) ? -1 : 0;
}

static int yp_streq(const unsigned char *s, size_t n, const char *lit)
{
    size_t ln = strlen(lit);
    return n == ln && memcmp(s, lit, ln) == 0;
}

static mino_val *yp_resolve(yp_t *t, size_t p,
                            const unsigned char *s, size_t n)
{
    mino_state *S = t->S;
    if (n == 0 || yp_streq(s, n, "~") || yp_streq(s, n, "null") ||
        yp_streq(s, n, "Null") || yp_streq(s, n, "NULL")) {
        return mino_nil(S);
    }
    if (yp_streq(s, n, "true") || yp_streq(s, n, "True") ||
        yp_streq(s, n, "TRUE")) {
        return mino_true(S);
    }
    if (yp_streq(s, n, "false") || yp_streq(s, n, "False") ||
        yp_streq(s, n, "FALSE")) {
        return mino_false(S);
    }
    if (yp_streq(s, n, ".inf") || yp_streq(s, n, ".Inf") ||
        yp_streq(s, n, ".INF") || yp_streq(s, n, "+.inf") ||
        yp_streq(s, n, "+.Inf") || yp_streq(s, n, "+.INF")) {
        return mino_float(S, INFINITY);
    }
    if (yp_streq(s, n, "-.inf") || yp_streq(s, n, "-.Inf") ||
        yp_streq(s, n, "-.INF")) {
        return mino_float(S, -INFINITY);
    }
    if (yp_streq(s, n, ".nan") || yp_streq(s, n, ".NaN") ||
        yp_streq(s, n, ".NAN")) {
        return mino_float(S, (double)NAN);
    }
    if (n > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        size_t i;
        mino_val *v;
        for (i = 2; i < n; i++) {
            unsigned char c = s[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F'))) break;
        }
        if (i == n) {
            if (yp_radix_int(t, s, n, 16, 2, &v) != 0) return NULL;
            return v;
        }
    }
    if (n > 2 && s[0] == '0' && s[1] == 'o') {
        size_t i;
        mino_val *v;
        for (i = 2; i < n; i++) {
            if (s[i] < '0' || s[i] > '7') break;
        }
        if (i == n) {
            if (yp_radix_int(t, s, n, 8, 2, &v) != 0) return NULL;
            return v;
        }
    }
    if (n > 0) {
        int neg = 0;
        size_t i = 0;
        if (s[0] == '-' || s[0] == '+') {
            neg = (s[0] == '-');
            i = 1;
        }
        if (i < n) {
            int all_digits = 1;
            size_t j;
            for (j = i; j < n; j++) {
                if (s[j] < '0' || s[j] > '9') { all_digits = 0; break; }
            }
            if (all_digits) {
                mino_val *v;
                if (yp_digits_int(t, p, s + i, n - i, neg, &v) != 0) {
                    return NULL;
                }
                return v;
            }
        }
    }
    /* core float: [-+]?(\.[0-9]+|[0-9]+(\.[0-9]*)?)([eE][-+]?[0-9]+)? */
    if (n > 0) {
        size_t i = 0;
        int ok = 0;
        int is_float = 0;
        if (s[0] == '-' || s[0] == '+') i = 1;
        if (i < n && s[i] >= '0' && s[i] <= '9') {
            while (i < n && s[i] >= '0' && s[i] <= '9') i++;
            ok = 1;
            if (i < n && s[i] == '.') {
                is_float = 1;
                i++;
                while (i < n && s[i] >= '0' && s[i] <= '9') i++;
            }
        } else if (i < n && s[i] == '.') {
            i++;
            if (i < n && s[i] >= '0' && s[i] <= '9') {
                is_float = 1;
                while (i < n && s[i] >= '0' && s[i] <= '9') i++;
                ok = 1;
            }
        }
        if (ok && i < n && (s[i] == 'e' || s[i] == 'E')) {
            size_t j = i + 1;
            if (j < n && (s[j] == '+' || s[j] == '-')) j++;
            if (j < n && s[j] >= '0' && s[j] <= '9') {
                while (j < n && s[j] >= '0' && s[j] <= '9') j++;
                i = j;
                is_float = 1;
            }
        }
        if (ok && i == n && is_float) {
            char buf[400];
            size_t cn = n;
            if (cn > sizeof(buf) - 1) cn = sizeof(buf) - 1;
            memcpy(buf, s, cn);
            buf[cn] = '\0';
            return mino_float(S, strtod(buf, NULL));
        }
    }
    return mino_string_n(S, (const char *)s, n);
}

/* ---- plain runs ---- */

typedef enum {
    YP_RUN_EOL,
    YP_RUN_COLON,
    YP_RUN_COMMENT,
    YP_RUN_FLOW
} yp_run_kind;

static yp_run_kind yp_scan_plain(yp_t *t, size_t p, int flow, size_t *stop)
{
    size_t q = p;
    while (q < t->len) {
        unsigned char c = t->data[q];
        if (c == '\n') { *stop = q; return YP_RUN_EOL; }
        if (flow && (c == ',' || c == '[' || c == ']' ||
                     c == '{' || c == '}')) {
            *stop = q;
            return YP_RUN_FLOW;
        }
        if (c == ':') {
            unsigned char d = (q + 1 < t->len) ? t->data[q + 1] : 0;
            if (d == 0 || d == '\n' || yp_ws_p(d) ||
                (flow && (d == ',' || d == '[' || d == ']' ||
                          d == '{' || d == '}'))) {
                *stop = q;
                return YP_RUN_COLON;
            }
            q++;
            continue;
        }
        if (c == '#' && (q == p || yp_ws_p(t->data[q - 1]))) {
            *stop = q;
            return YP_RUN_COMMENT;
        }
        q++;
    }
    *stop = q;
    return YP_RUN_EOL;
}

static int yp_ws_only_from(yp_t *t, size_t q)
{
    while (q < t->len) {
        unsigned char c = t->data[q];
        if (c == '\n') return 1;
        if (!yp_ws_p(c)) return 0;
        q++;
    }
    return 1;
}

typedef enum {
    YP_PC_CONTENT,
    YP_PC_STOP,
    YP_PC_EOF
} yp_pc_kind;

static yp_pc_kind yp_plain_continuation(yp_t *t, size_t eol, long pi,
                                        size_t *cs, long *blanks)
{
    size_t r = (eol < t->len) ? eol + 1 : eol;
    long n = 0;
    for (;;) {
        size_t fn;
        unsigned char c;
        if (r >= t->len) { *blanks = n; return YP_PC_EOF; }
        fn = yp_first_nonspace(t, r);
        c = yp_peek(t, fn);
        if (c == 0 || c == '\n' || (yp_ws_p(c) && yp_ws_only_from(t, fn))) {
            size_t nl = yp_next_line(t, r);
            if (nl >= t->len || nl == r) { *blanks = n; return YP_PC_EOF; }
            n++;
            r = nl;
            continue;
        }
        if (c == '#') { *blanks = n; return YP_PC_STOP; }
        if (yp_doc_marker(t, fn)) { *blanks = n; return YP_PC_STOP; }
        if (yp_line_indent(t, fn) <= pi) { *blanks = n; return YP_PC_STOP; }
        *cs = fn;
        *blanks = n;
        return YP_PC_CONTENT;
    }
}

/* Multi-line plain scalar in block context. */
static mino_val *yp_plain_block(yp_t *t, size_t p, long pi, size_t *resume)
{
    size_t stop;
    yp_run_kind k = yp_scan_plain(t, p, 0, &stop);
    size_t last_eol;
    const unsigned char *s;
    size_t n;
    yp_buf_t b;
    if (k == YP_RUN_COLON) {
        yp_fail(t, "mapping-in-scalar", stop);
        return NULL;
    }
    if (yp_buf_init(t, &b, 64) != 0) return NULL;
    yp_span_trim(t, p, stop, &s, &n);
    if (yp_buf_putn(t, &b, s, n) != 0) goto pb_oom;
    if (k == YP_RUN_COMMENT) {
        *resume = stop;
        goto pb_done;
    }
    last_eol = stop;
    for (;;) {
        size_t cs;
        long blanks;
        yp_pc_kind pc = yp_plain_continuation(t, last_eol, pi, &cs, &blanks);
        if (pc != YP_PC_CONTENT) {
            *resume = last_eol;
            goto pb_done;
        }
        {
            yp_run_kind k2 = yp_scan_plain(t, cs, 0, &stop);
            if (k2 == YP_RUN_COLON) {
                yp_fail(t, "mapping-in-scalar", stop);
                goto pb_fail;
            }
            if (b.len > 0) {
                if (blanks == 0) {
                    if (yp_buf_put1(t, &b, ' ') != 0) goto pb_oom;
                } else {
                    if (yp_buf_put_rep(t, &b, '\n', blanks) != 0) goto pb_oom;
                }
            }
            yp_span_trim(t, cs, stop, &s, &n);
            if (yp_buf_putn(t, &b, s, n) != 0) goto pb_oom;
            if (k2 == YP_RUN_COMMENT) {
                *resume = stop;
                goto pb_done;
            }
            last_eol = stop;
        }
    }
  pb_done: {
        mino_val *v = yp_resolve(t, p, b.buf, b.len);
        yp_buf_done(t, &b);
        return v;
    }
  pb_fail:
    yp_buf_done(t, &b);
    return NULL;
  pb_oom:
    yp_buf_done(t, &b);
    return NULL;
}

/* ---- quoted scalars ---- */

static size_t yp_utf8(int cp, unsigned char *out)
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

static int yp_hexv(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode the escape at the backslash at q into b. Returns the bytes
 * consumed after the backslash+letter, or -1 failure, -2 escaped
 * break. */
static long yp_decode_escape(yp_t *t, size_t q, yp_buf_t *b,
                             size_t *consumed)
{
    unsigned char c = yp_peek(t, q + 1);
    unsigned char tmp[4];
    size_t n;
    switch (c) {
    case 0: yp_fail(t, "invalid-escape", q); return -1;
    case '0': tmp[0] = 0; n = 1; break;
    case 'a': tmp[0] = 7; n = 1; break;
    case 'b': tmp[0] = 8; n = 1; break;
    case 't': tmp[0] = '\t'; n = 1; break;
    case 'n': tmp[0] = '\n'; n = 1; break;
    case 'v': tmp[0] = 11; n = 1; break;
    case 'f': tmp[0] = 12; n = 1; break;
    case 'r': tmp[0] = '\r'; n = 1; break;
    case 'e': tmp[0] = 27; n = 1; break;
    case ' ': tmp[0] = ' '; n = 1; break;
    case '"': tmp[0] = '"'; n = 1; break;
    case '/': tmp[0] = '/'; n = 1; break;
    case '\\': tmp[0] = '\\'; n = 1; break;
    case 'N': tmp[0] = 0xC2; tmp[1] = 0x85; n = 2; break;
    case '_': tmp[0] = 0xC2; tmp[1] = 0xA0; n = 2; break;
    case 'L': tmp[0] = 0xE2; tmp[1] = 0x80; tmp[2] = 0xA8; n = 3; break;
    case 'P': tmp[0] = 0xE2; tmp[1] = 0x80; tmp[2] = 0xA9; n = 3; break;
    case '\n': return -2;
    case 'x': case 'u': case 'U': {
        int want = (c == 'x') ? 2 : (c == 'u') ? 4 : 8;
        int cp = 0;
        int i;
        for (i = 0; i < want; i++) {
            int d = yp_hexv(yp_peek(t, q + 2 + i));
            if (d < 0) {
                yp_fail(t, "invalid-escape", q);
                return -1;
            }
            cp = cp * 16 + d;
        }
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            yp_fail(t, "bad-codepoint", q);
            return -1;
        }
        n = yp_utf8(cp, tmp);
        if (yp_buf_putn(t, b, tmp, n) != 0) return -1;
        *consumed = (size_t)want + 2;
        return 0;
    }
    default:
        yp_fail(t, "invalid-escape", q);
        return -1;
    }
    if (yp_buf_putn(t, b, tmp, n) != 0) return -1;
    *consumed = 2;
    return 0;
}

static mino_val *yp_quoted(yp_t *t, size_t p, long pi, int key_mode,
                           size_t *out_end)
{
    int dbl = (t->data[p] == '"');
    unsigned char closer = dbl ? '"' : '\'';
    yp_buf_t b;
    size_t q = p + 1;
    size_t seg_start = q;
    int seen_seg = 0;
    int raw_join = 0;
    if (yp_buf_init(t, &b, 64) != 0) return NULL;
    for (;;) {
        unsigned char c;
        if (q >= t->len) {
            yp_fail(t, "unterminated-quote", p);
            goto q_fail;
        }
        c = t->data[q];
        if (c == closer) {
            if (!dbl && yp_is(t, q + 1, '\'')) {
                if (yp_buf_putn(t, &b, t->data + seg_start, q - seg_start)
                    != 0) goto q_oom;
                if (yp_buf_put1(t, &b, '\'') != 0) goto q_oom;
                q += 2;
                seg_start = q;
                continue;
            }
            if (yp_buf_putn(t, &b, t->data + seg_start, q - seg_start) != 0) {
                goto q_oom;
            }
            *out_end = q + 1;
            {
                mino_val *v = yp_buf_string(t, &b);
                yp_buf_done(t, &b);
                return v;
            }
        }
        if (dbl && c == '\\') {
            size_t consumed = 0;
            long r;
            if (yp_buf_putn(t, &b, t->data + seg_start, q - seg_start) != 0) {
                goto q_oom;
            }
            r = yp_decode_escape(t, q, &b, &consumed);
            if (r == -2) {
                /* escaped break: keep bytes to EOL; next line raw */
                size_t eol = yp_eol(t, q);
                size_t nl;
                size_t cs;
                size_t raw;
                if (key_mode) {
                    yp_fail(t, "multiline-key", p);
                    goto q_fail;
                }
                if (yp_buf_putn(t, &b, t->data + q + 1, eol - (q + 1)) != 0) {
                    goto q_oom;
                }
                raw_join = 1;
                nl = yp_next_line(t, eol);
                cs = yp_content_start(t, nl);
                if (cs == YP_NO_POS) {
                    yp_fail(t, "unterminated-quote", p);
                    goto q_fail;
                }
                if (yp_doc_marker(t, cs)) {
                    yp_fail(t, "doc-marker", cs);
                    goto q_fail;
                }
                if (yp_line_indent(t, cs) <= pi) {
                    yp_fail(t, "bad-indentation", yp_line_start(t, cs));
                    goto q_fail;
                }
                raw = yp_line_start(t, cs);
                q = raw;
                seg_start = raw;
                continue;
            }
            if (r != 0) goto q_fail;
            q += consumed;
            seg_start = q;
            continue;
        }
        if (c == '\n') {
            size_t r = q + 1;
            long pend = 0;
            if (yp_buf_putn(t, &b, t->data + seg_start, q - seg_start) != 0) {
                goto q_oom;
            }
            yp_buf_trim_ws(&b);
            if (key_mode) {
                yp_fail(t, "multiline-key", p);
                goto q_fail;
            }
            seen_seg = 1;
            for (;;) {
                size_t ls;
                size_t fn;
                unsigned char fc;
                if (r >= t->len) {
                    yp_fail(t, "unterminated-quote", p);
                    goto q_fail;
                }
                ls = yp_line_start(t, r);
                fn = yp_first_nonspace(t, ls);
                fc = yp_peek(t, fn);
                if (fc == 0 || fc == '\n') {
                    size_t nl;
                    pend++;
                    nl = yp_next_line(t, ls);
                    if (nl >= t->len || nl == ls) {
                        yp_fail(t, "unterminated-quote", p);
                        goto q_fail;
                    }
                    r = nl;
                    continue;
                }
                if (yp_doc_marker(t, fn)) {
                    yp_fail(t, "doc-marker", fn);
                    goto q_fail;
                }
                if (yp_line_indent(t, fn) <= pi) {
                    yp_fail(t, "bad-indentation", yp_line_start(t, fn));
                    goto q_fail;
                }
                {
                    size_t cs2 = yp_skip_ws(t, fn);
                    if (raw_join) {
                        raw_join = 0;
                    } else if (seen_seg) {
                        if (pend == 0) {
                            if (yp_buf_put1(t, &b, ' ') != 0) goto q_oom;
                        } else {
                            if (yp_buf_put_rep(t, &b, '\n', pend) != 0) {
                                goto q_oom;
                            }
                        }
                    }
                    q = cs2;
                    seg_start = cs2;
                }
                break;
            }
            continue;
        }
        q++;
    }
  q_oom:
    yp_buf_done(t, &b);
    return NULL;
  q_fail:
    yp_buf_done(t, &b);
    return NULL;
}

/* ---- block scalars ---- */

typedef enum {
    YP_CH_CLIP,
    YP_CH_STRIP,
    YP_CH_KEEP
} yp_chomp;

typedef struct {
    int                 kind;  /* 0 seg, 1 empty */
    const unsigned char *s;
    size_t              n;
    int                 more;
} yp_item;

static int yp_block_scalar(yp_t *t, size_t p, long pi,
                           size_t *resume, mino_val **out)
{
    mino_state *S = t->S;
    size_t q = p + 1;
    int literal = (t->data[p] == '|');
    int ind = 0;
    yp_chomp ch = YP_CH_CLIP;
    size_t he;
    size_t start;
    long detected = -1;
    yp_item *items = NULL;
    size_t n_items = 0;
    size_t cap_items = 0;
    int saved_depth;
    for (;;) {
        unsigned char c = yp_peek(t, q);
        if (c >= '1' && c <= '9' && ind == 0) {
            ind = c - '0';
            q++;
            continue;
        }
        if ((c == '-' || c == '+') && ch == YP_CH_CLIP) {
            ch = (c == '-') ? YP_CH_STRIP : YP_CH_KEEP;
            q++;
            continue;
        }
        if (yp_ws_p(c)) { q++; continue; }
        if (yp_at_eol(t, q) ||
            (c == '#' && yp_ws_p(t->data[q - 1]))) {
            he = q;
            break;
        }
        yp_fail(t, "block-scalar-header", q);
        return -1;
    }
    start = yp_next_line(t, he);
    if (ind > 0) {
        detected = pi + ind;
    } else {
        size_t r = start;
        for (;;) {
            size_t ls = yp_line_start(t, r);
            size_t fn = yp_first_nonspace(t, ls);
            unsigned char c = yp_peek(t, fn);
            if (r >= t->len || yp_sentinel_line(t, r)) {
                detected = -2;
                break;
            }
            if (c == 0 || c == '\n') {
                size_t nl = yp_next_line(t, r);
                if (nl >= t->len || nl == r) {
                    detected = -2;
                    break;
                }
                r = nl;
                continue;
            }
            {
                long sp = yp_line_indent(t, fn);
                if (sp > pi) {
                    /* leading blank lines longer than the detected
                     * indent are an error */
                    size_t r2 = start;
                    for (;;) {
                        size_t ls2 = yp_line_start(t, r2);
                        size_t fn2 = yp_first_nonspace(t, ls2);
                        unsigned char c2 = yp_peek(t, fn2);
                        if (c2 == 0 || c2 == '\n') {
                            long sp2 = (long)(fn2 - ls2);
                            if (sp2 > sp) {
                                yp_fail(t, "block-scalar-indent", ls2);
                                return -1;
                            }
                            {
                                size_t nl2 = yp_next_line(t, r2);
                                if (nl2 >= t->len || nl2 == r2) break;
                                r2 = nl2;
                            }
                            continue;
                        }
                        break;
                    }
                    detected = sp;
                    break;
                }
                detected = -2;
                break;
            }
        }
    }
    /* collect items */
    saved_depth = mino_current_ctx(S)->gc_depth;
    mino_current_ctx(S)->gc_depth = saved_depth + 1;
    cap_items = 32;
    items = (yp_item *)gc_alloc_typed_inner(S, GC_T_RAW,
                                            cap_items * sizeof(yp_item));
    if (items == NULL) {
        mino_current_ctx(S)->gc_depth = saved_depth;
        return -1;
    }
    if (detected == -2) {
        /* no content: empties only */
        size_t r = start;
        while (r < t->len && !yp_sentinel_line(t, r)) {
            size_t ls = yp_line_start(t, r);
            size_t fn = yp_first_nonspace(t, ls);
            unsigned char c = yp_peek(t, fn);
            if (!(c == 0 || c == '\n')) {
                break;
            }
            if (n_items == cap_items) {
                yp_item *ni = (yp_item *)gc_alloc_typed_inner(
                    S, GC_T_RAW, cap_items * 2 * sizeof(yp_item));
                if (ni == NULL) goto bs_oom;
                memcpy(ni, items, n_items * sizeof(yp_item));
                items = ni;
                cap_items *= 2;
            }
            items[n_items].kind = 1;
            items[n_items].s = t->data + ls;
            items[n_items].n = 0;
            items[n_items].more = 0;
            n_items++;
            {
                size_t nl = yp_next_line(t, r);
                if (nl >= t->len || nl == r) break;
                r = nl;
            }
        }
        mino_current_ctx(S)->gc_depth = saved_depth;
        /* render: literal keeps them, folded keep too, others empty */
        if (ch == YP_CH_KEEP) {
            yp_buf_t b;
            size_t i2;
            if (yp_buf_init(t, &b, 16) != 0) return -1;
            for (i2 = 0; i2 < n_items; i2++) {
                if (yp_buf_put1(t, &b, '\n') != 0) {
                    yp_buf_done(t, &b);
                    return -1;
                }
            }
            *out = yp_buf_string(t, &b);
            yp_buf_done(t, &b);
            if (*out == NULL) return -1;
        } else {
            *out = mino_string_n(S, "", 0);
            if (*out == NULL) return -1;
        }
        *resume = he;
        return 0;
    }
    {
        size_t r = start;
        size_t last_eol = yp_eol(t, he);
        while (r < t->len && !yp_sentinel_line(t, r)) {
            size_t ls = yp_line_start(t, r);
            size_t fn = yp_first_nonspace(t, ls);
            unsigned char c = yp_peek(t, fn);
            int blank = (c == 0 || c == '\n');
            long sp = (long)(fn - ls);
            if (!blank && c != '\t' && sp < detected) break;
            if (n_items == cap_items) {
                yp_item *ni = (yp_item *)gc_alloc_typed_inner(
                    S, GC_T_RAW, cap_items * 2 * sizeof(yp_item));
                if (ni == NULL) goto bs_oom;
                memcpy(ni, items, n_items * sizeof(yp_item));
                items = ni;
                cap_items *= 2;
            }
            if (blank) {
                items[n_items].kind = 1;
                if (sp >= detected) {
                    items[n_items].s = t->data + ls + (size_t)detected;
                    items[n_items].n = fn - (ls + (size_t)detected);
                } else {
                    items[n_items].s = t->data + ls;
                    items[n_items].n = 0;
                }
                items[n_items].more = 0;
            } else {
                size_t cstart = (c == '\t') ? fn : ls + (size_t)detected;
                size_t ce = yp_eol(t, fn);
                items[n_items].kind = 0;
                items[n_items].s = t->data + cstart;
                items[n_items].n = ce - cstart;
                items[n_items].more = yp_ws_p(t->data[cstart]) ? 1 : 0;
            }
            n_items++;
            last_eol = yp_eol(t, r);
            {
                size_t nl = yp_next_line(t, r);
                if (nl >= t->len || nl == r) break;
                r = nl;
            }
        }
        mino_current_ctx(S)->gc_depth = saved_depth;
        /* render */
        {
            yp_buf_t b;
            size_t i2;
            size_t trail_start = n_items;
            if (yp_buf_init(t, &b, 128) != 0) return -1;
            if (literal) {
                for (i2 = 0; i2 < n_items; i2++) {
                    if (yp_buf_putn(t, &b, items[i2].s, items[i2].n) != 0) {
                        goto bs_r_oom;
                    }
                    if (yp_buf_put1(t, &b, '\n') != 0) goto bs_r_oom;
                }
                /* chomp over the joined text */
                if (ch == YP_CH_STRIP) {
                    while (b.len > 0 && b.buf[b.len - 1] == '\n') b.len--;
                } else if (ch == YP_CH_CLIP) {
                    while (b.len > 0 && b.buf[b.len - 1] == '\n') b.len--;
                    if (b.len > 0) {
                        if (yp_buf_put1(t, &b, '\n') != 0) goto bs_r_oom;
                    }
                }
                *out = yp_buf_string(t, &b);
                yp_buf_done(t, &b);
                if (*out == NULL) return -1;
                *resume = last_eol;
                return 0;
            }
            /* folded */
            {
                int seen_seg = 0;
                int prev_more = 0;
                int has_pend = 0;
                size_t pend_first = 0;
                /* trailing empties for keep */
                if (n_items > 0) {
                    size_t e = n_items;
                    while (e > 0 && items[e - 1].kind == 1) e--;
                    trail_start = e;
                }
                for (i2 = 0; i2 < trail_start; i2++) {
                    if (items[i2].kind == 1) {
                        if (!has_pend) {
                            has_pend = 1;
                            pend_first = i2;
                        }
                        continue;
                    }
                    {
                        int more = items[i2].more;
                        if (!seen_seg) {
                            /* leading empties flush */
                            size_t j;
                            for (j = pend_first; j < i2; j++) {
                                if (yp_buf_putn(t, &b, items[j].s,
                                                items[j].n) != 0) {
                                    goto bs_r_oom;
                                }
                                if (yp_buf_put1(t, &b, '\n') != 0) {
                                    goto bs_r_oom;
                                }
                            }
                        } else {
                            if (!has_pend) {
                                if (yp_buf_put1(t, &b, (more || prev_more)
                                                ? '\n' : ' ') != 0) {
                                    goto bs_r_oom;
                                }
                            } else {
                                size_t j;
                                for (j = pend_first; j < i2; j++) {
                                    if (yp_buf_putn(t, &b, items[j].s,
                                                    items[j].n) != 0) {
                                        goto bs_r_oom;
                                    }
                                    if (yp_buf_put1(t, &b, '\n') != 0) {
                                        goto bs_r_oom;
                                    }
                                }
                                if (more || prev_more) {
                                    if (yp_buf_put1(t, &b, '\n') != 0) {
                                        goto bs_r_oom;
                                    }
                                }
                            }
                        }
                        if (yp_buf_putn(t, &b, items[i2].s,
                                        items[i2].n) != 0) goto bs_r_oom;
                        seen_seg = 1;
                        prev_more = more;
                        has_pend = 0;
                    }
                }
                if (ch == YP_CH_KEEP) {
                    if (seen_seg) {
                        if (yp_buf_put1(t, &b, '\n') != 0) goto bs_r_oom;
                    }
                    for (i2 = trail_start; i2 < n_items; i2++) {
                        if (yp_buf_putn(t, &b, items[i2].s,
                                        items[i2].n) != 0) goto bs_r_oom;
                        if (yp_buf_put1(t, &b, '\n') != 0) goto bs_r_oom;
                    }
                } else if (ch == YP_CH_CLIP) {
                    if (seen_seg) {
                        if (yp_buf_put1(t, &b, '\n') != 0) goto bs_r_oom;
                    }
                }
                *out = yp_buf_string(t, &b);
                yp_buf_done(t, &b);
                if (*out == NULL) return -1;
                *resume = last_eol;
                return 0;
            }
          bs_r_oom:
            yp_buf_done(t, &b);
            return -1;
        }
    }
  bs_oom:
    mino_current_ctx(S)->gc_depth = saved_depth;
    return -1;
}

/* ---- flow ---- */

static size_t yp_flow_skip(yp_t *t, size_t p, long pi)
{
    size_t q = p;
    for (;;) {
        unsigned char c;
        if (q >= t->len) return q;
        c = t->data[q];
        if (yp_ws_p(c)) { q++; continue; }
        if (c == '\n') {
            size_t cs = yp_content_start(t, q + 1);
            if (cs == YP_NO_POS) { q++; continue; }
            if (yp_doc_marker(t, cs)) {
                yp_fail(t, "doc-marker", cs);
                return YP_NO_POS;
            }
            if (yp_line_indent(t, cs) <= pi) {
                yp_fail(t, "bad-indentation", yp_line_start(t, cs));
                return YP_NO_POS;
            }
            q = yp_skip_ws(t, cs);
            continue;
        }
        if (c == '#') {
            size_t ls = yp_line_start(t, q);
            if (q == ls || yp_ws_p(t->data[q - 1])) {
                q = yp_next_line(t, q);
                continue;
            }
        }
        return q;
    }
}

static int yp_flow_key_colon(yp_t *t, size_t p, int quoted)
{
    unsigned char d;
    if (!yp_is(t, p, ':')) return 0;
    if (quoted) return 1;
    d = yp_peek(t, p + 1);
    if (d == 0 || d == '\n' || yp_ws_p(d)) return 1;
    return d == ',' || d == '[' || d == ']' || d == '{' || d == '}';
}

static mino_val *yp_flow_plain(yp_t *t, size_t p, long pi, int key_mode,
                               size_t *resume)
{
    size_t stop;
    yp_run_kind k = yp_scan_plain(t, p, 1, &stop);
    const unsigned char *s;
    size_t n;
    yp_buf_t b;
    if (yp_buf_init(t, &b, 64) != 0) return NULL;
    yp_span_trim(t, p, stop, &s, &n);
    if (yp_buf_putn(t, &b, s, n) != 0) goto fp_oom;
    if (k != YP_RUN_EOL) {
        *resume = stop;
        goto fp_done;
    }
    {
        size_t last_eol = stop;
        for (;;) {
            size_t ls = (last_eol < t->len) ? last_eol + 1 : last_eol;
            size_t cs = yp_content_start(t, ls);
            if (cs == YP_NO_POS) { *resume = last_eol; goto fp_done; }
            if (yp_doc_marker(t, cs)) {
                yp_fail(t, "doc-marker", cs);
                goto fp_fail;
            }
            if (yp_line_indent(t, cs) <= pi) {
                *resume = last_eol;
                goto fp_done;
            }
            {
                unsigned char fc = t->data[cs];
                if (fc == ',' || fc == '[' || fc == ']' || fc == '{' ||
                    fc == '}' || fc == '#') {
                    *resume = last_eol;
                    goto fp_done;
                }
            }
            {
                size_t cs2 = yp_skip_ws(t, cs);
                yp_run_kind k2 = yp_scan_plain(t, cs2, 1, &stop);
                yp_span_trim(t, cs2, stop, &s, &n);
                if (k2 == YP_RUN_COLON) {
                    if (key_mode && n > 0) {
                        if (b.len > 0 &&
                            yp_buf_put1(t, &b, ' ') != 0) goto fp_oom;
                        if (yp_buf_putn(t, &b, s, n) != 0) goto fp_oom;
                        *resume = stop;
                        goto fp_done;
                    }
                    *resume = last_eol;
                    goto fp_done;
                }
                if (b.len > 0 && yp_buf_put1(t, &b, ' ') != 0) goto fp_oom;
                if (yp_buf_putn(t, &b, s, n) != 0) goto fp_oom;
                if (k2 != YP_RUN_EOL) {
                    *resume = stop;
                    goto fp_done;
                }
                last_eol = stop;
            }
        }
    }
  fp_done: {
        mino_val *v = yp_resolve(t, p, b.buf, b.len);
        yp_buf_done(t, &b);
        return v;
    }
  fp_fail:
    yp_buf_done(t, &b);
    return NULL;
  fp_oom:
    yp_buf_done(t, &b);
    return NULL;
}

static mino_val *yp_flow_seq(yp_t *t, size_t p, long pi, size_t *out_end);
static mino_val *yp_flow_map(yp_t *t, size_t p, long pi, size_t *out_end);

static mino_val *yp_flow_node(yp_t *t, size_t p, long pi, int key_mode,
                              size_t *out_end)
{
    unsigned char c;
    if (p >= t->len) {
        yp_fail(t, "flow-syntax", p);
        return NULL;
    }
    c = t->data[p];
    if (c == '#') {
        yp_fail(t, "unexpected-content", p);
        return NULL;
    }
    if (c == '[') return yp_flow_seq(t, p, pi, out_end);
    if (c == '{') return yp_flow_map(t, p, pi, out_end);
    if (c == '"' || c == '\'') {
        return yp_quoted(t, p, pi, key_mode, out_end);
    }
    if (c == '&') {
        yp_fail(t, "unsupported-anchor", p);
        return NULL;
    }
    if (c == '*') {
        yp_fail(t, "unsupported-alias", p);
        return NULL;
    }
    if (c == '!') {
        yp_fail(t, "unsupported-tag", p);
        return NULL;
    }
    if (c == '-' || c == '?' || c == ':') {
        unsigned char d = yp_peek(t, p + 1);
        if (d == 0 || d == '\n' || yp_ws_p(d) || d == ',' || d == '[' ||
            d == ']' || d == '{' || d == '}') {
            yp_fail(t, "flow-syntax", p);
            return NULL;
        }
    }
    return yp_flow_plain(t, p, pi, key_mode, out_end);
}

static mino_val *yp_key_of(yp_t *t, mino_val *v)
{
    if (t->keywords && v != NULL && mino_type_of(v) == MINO_STRING) {
        return mino_keyword_n(t->S, v->as.s.data, v->as.s.len);
    }
    return v;
}

static mino_val *yp_assoc(yp_t *t, mino_val *m, mino_val *k, mino_val *v)
{
    mino_state *S = t->S;
    mino_val *tr = mino_transient(S, m);
    mino_val *tr2;
    if (tr == NULL) return NULL;
    tr2 = mino_assoc_bang(S, tr, k, v);
    if (tr2 == NULL) return NULL;
    return mino_persistent(S, tr2);
}

static mino_val *yp_single_pair(yp_t *t, mino_val *k, mino_val *v)
{
    mino_state *S = t->S;
    mino_val *m = mino_map(S, NULL, NULL, 0);
    if (m == NULL) return NULL;
    return yp_assoc(t, m, k, v);
}

static int yp_flow_tail(yp_t *t, size_t q, long pi, size_t *next,
                        int *is_close, unsigned char close_c)
{
    size_t q2 = yp_flow_skip(t, q, pi);
    if (q2 == YP_NO_POS) return -1;
    if (q2 < t->len && t->data[q2] == ',') {
        *next = q2 + 1;
        *is_close = 0;
        return 0;
    }
    if (q2 < t->len && t->data[q2] == close_c) {
        *next = q2;
        *is_close = 1;
        return 0;
    }
    if (q2 >= t->len) {
        return 1; /* unterminated */
    }
    yp_fail(t, "flow-syntax", q2);
    return -1;
}

static mino_val *yp_flow_seq(yp_t *t, size_t p, long pi, size_t *out_end)
{
    mino_state *S = t->S;
    mino_val *cur;
    size_t q;
    if (++t->depth > YP_MAX_DEPTH) {
        t->depth--;
        yp_fail(t, "flow-syntax", p);
        return NULL;
    }
    cur = mino_transient(S, mino_vector(S, NULL, 0));
    if (cur == NULL) { t->depth--; return NULL; }
    q = p + 1;
    for (;;) {
        size_t q2 = yp_flow_skip(t, q, pi);
        mino_val *v;
        size_t q3;
        size_t q4;
        int quoted;
        size_t nx;
        int is_close;
        int tr;
        if (q2 == YP_NO_POS) { t->depth--; return NULL; }
        if (q2 >= t->len) {
            t->depth--;
            yp_fail(t, "unterminated-flow", p);
            return NULL;
        }
        if (t->data[q2] == ']') {
            *out_end = q2 + 1;
            t->depth--;
            return mino_persistent(S, cur);
        }
        if (t->data[q2] == '}' || t->data[q2] == ',') {
            yp_fail(t, "flow-syntax", q2);
            t->depth--;
            return NULL;
        }
        quoted = (t->data[q2] == '"' || t->data[q2] == '\'');
        if (yp_flow_key_colon(t, q2, 0)) {
            /* empty-key single pair */
            size_t qv = yp_flow_skip(t, q2 + 1, pi);
            mino_val *pair;
            if (qv == YP_NO_POS) { t->depth--; return NULL; }
            if (qv < t->len && t->data[qv] != ',' && t->data[qv] != ']') {
                v = yp_flow_node(t, qv, pi, 0, &q4);
                if (v == NULL) { t->depth--; return NULL; }
            } else {
                v = mino_nil(S);
                q4 = qv;
            }
            pair = yp_single_pair(t, mino_nil(S), v);
            if (pair == NULL) { t->depth--; return NULL; }
            {
                mino_val *c2 = mino_conj_bang(S, cur, pair);
                if (c2 == NULL) { t->depth--; return NULL; }
                cur = c2;
            }
            tr = yp_flow_tail(t, q4, pi, &nx, &is_close, ']');
            if (tr != 0) {
                t->depth--;
                if (tr == 1) yp_fail(t, "unterminated-flow", p);
                return NULL;
            }
            if (is_close) {
                *out_end = nx + 1;
                t->depth--;
                return mino_persistent(S, cur);
            }
            q = nx;
            continue;
        }
        v = yp_flow_node(t, q2, pi, 1, &q3);
        if (v == NULL) { t->depth--; return NULL; }
        {
            size_t qc = yp_flow_skip(t, q3, pi);
            if (qc == YP_NO_POS) { t->depth--; return NULL; }
            if ((mino_type_of(v) == MINO_MAP ||
                 mino_type_of(v) == MINO_VECTOR) && yp_is(t, qc, ':')) {
                yp_fail(t, "unsupported-complex-key", q2);
                t->depth--;
                return NULL;
            }
            if (yp_flow_key_colon(t, qc, quoted) &&
                yp_line_start(t, q3) == yp_line_start(t, qc)) {
                size_t qv = yp_flow_skip(t, qc + 1, pi);
                mino_val *pair;
                if (qv == YP_NO_POS) { t->depth--; return NULL; }
                if (qv < t->len && t->data[qv] != ',' && t->data[qv] != ']') {
                    mino_val *v2 = yp_flow_node(t, qv, pi, 0, &q4);
                    if (v2 == NULL) { t->depth--; return NULL; }
                    pair = yp_single_pair(t, yp_key_of(t, v), v2);
                } else {
                    pair = yp_single_pair(t, yp_key_of(t, v), mino_nil(S));
                    q4 = qv;
                }
                if (pair == NULL) { t->depth--; return NULL; }
                {
                    mino_val *c2 = mino_conj_bang(S, cur, pair);
                    if (c2 == NULL) { t->depth--; return NULL; }
                    cur = c2;
                }
            } else {
                mino_val *c2 = mino_conj_bang(S, cur, v);
                if (c2 == NULL) { t->depth--; return NULL; }
                cur = c2;
                q4 = q3;
                if (qc < t->len && t->data[qc] == ',') {
                    q = qc + 1;
                    continue;
                }
                if (qc < t->len && t->data[qc] == ']') {
                    *out_end = qc + 1;
                    t->depth--;
                    return mino_persistent(S, cur);
                }
                if (qc >= t->len) {
                    t->depth--;
                    yp_fail(t, "unterminated-flow", p);
                    return NULL;
                }
                yp_fail(t, "flow-syntax", qc);
                t->depth--;
                return NULL;
            }
            tr = yp_flow_tail(t, q4, pi, &nx, &is_close, ']');
            if (tr != 0) {
                t->depth--;
                if (tr == 1) yp_fail(t, "unterminated-flow", p);
                return NULL;
            }
            if (is_close) {
                *out_end = nx + 1;
                t->depth--;
                return mino_persistent(S, cur);
            }
            q = nx;
            continue;
        }
    }
}

static mino_val *yp_flow_map(yp_t *t, size_t p, long pi, size_t *out_end)
{
    mino_state *S = t->S;
    mino_val *m = mino_map(S, NULL, NULL, 0);
    size_t q;
    if (m == NULL) return NULL;
    if (++t->depth > YP_MAX_DEPTH) {
        t->depth--;
        yp_fail(t, "flow-syntax", p);
        return NULL;
    }
    q = p + 1;
    for (;;) {
        size_t q2 = yp_flow_skip(t, q, pi);
        mino_val *k;
        mino_val *v;
        size_t q3;
        size_t q4;
        int quoted;
        size_t nx;
        int is_close;
        int tr;
        if (q2 == YP_NO_POS) { t->depth--; return NULL; }
        if (q2 >= t->len) {
            t->depth--;
            yp_fail(t, "unterminated-flow", p);
            return NULL;
        }
        if (t->data[q2] == '}') {
            *out_end = q2 + 1;
            t->depth--;
            return m;
        }
        if (t->data[q2] == ',') {
            yp_fail(t, "flow-syntax", q2);
            t->depth--;
            return NULL;
        }
        quoted = (t->data[q2] == '"' || t->data[q2] == '\'');
        if (yp_flow_key_colon(t, q2, 0)) {
            k = mino_nil(S);
            q3 = q2 + 1;
        } else {
            k = yp_flow_node(t, q2, pi, 1, &q3);
            if (k == NULL) { t->depth--; return NULL; }
        }
        q4 = yp_flow_skip(t, q3, pi);
        if (q4 == YP_NO_POS) { t->depth--; return NULL; }
        if (yp_flow_key_colon(t, q4, quoted)) {
            size_t qv = yp_flow_skip(t, q4 + 1, pi);
            if (qv == YP_NO_POS) { t->depth--; return NULL; }
            if (qv < t->len && t->data[qv] != ',' && t->data[qv] != '}') {
                v = yp_flow_node(t, qv, pi, 0, &q3);
                if (v == NULL) { t->depth--; return NULL; }
            } else {
                v = mino_nil(S);
                q3 = qv;
            }
        } else {
            v = mino_nil(S);
            q3 = q4;
        }
        m = yp_assoc(t, m, yp_key_of(t, k), v);
        if (m == NULL) { t->depth--; return NULL; }
        tr = yp_flow_tail(t, q3, pi, &nx, &is_close, '}');
        if (tr != 0) {
            t->depth--;
            if (tr == 1) yp_fail(t, "unterminated-flow", p);
            return NULL;
        }
        if (is_close) {
            *out_end = nx + 1;
            t->depth--;
            return m;
        }
        q = nx;
    }
}

/* ---- block collections ---- */

static int yp_find_keysep(yp_t *t, size_t p, size_t *stop, int *found)
{
    size_t q = p;
    *found = 0;
    while (q < t->len) {
        unsigned char c = t->data[q];
        if (c == '\n') { *stop = q; return 0; }
        if (c == ':') {
            unsigned char d = (q + 1 < t->len) ? t->data[q + 1] : 0;
            if (d == 0 || d == '\n' || yp_ws_p(d)) {
                *stop = q;
                *found = 1;
                return 0;
            }
            q++;
            continue;
        }
        if (c == '#' && (q == p || yp_ws_p(t->data[q - 1]))) {
            *stop = q;
            return 0;
        }
        q++;
    }
    *stop = q;
    return 0;
}

/* Parse a mapping key at content start p. Returns the key value
 * (resolved for plain, raw for quoted) or NULL on failure. quoted
 * out-param tells whether the key was quoted. */
static mino_val *yp_map_key(yp_t *t, size_t p, size_t *colon,
                            int *quoted)
{
    unsigned char c;
    if (yp_is(t, p, '\t')) {
        yp_fail(t, "tab-indentation", p);
        return NULL;
    }
    if (yp_is(t, p, '&')) {
        yp_fail(t, "unsupported-anchor", p);
        return NULL;
    }
    if (yp_is(t, p, '*')) {
        yp_fail(t, "unsupported-alias", p);
        return NULL;
    }
    if (yp_is(t, p, '!')) {
        yp_fail(t, "unsupported-tag", p);
        return NULL;
    }
    c = t->data[p];
    *quoted = (c == '"' || c == '\'');
    if (*quoted) {
        size_t q;
        mino_val *v = yp_quoted(t, p, -1, 1, &q);
        size_t qc;
        if (v == NULL) return NULL;
        qc = yp_skip_ws(t, q);
        if (!yp_is(t, qc, ':')) {
            yp_fail(t, "unexpected-content", qc);
            return NULL;
        }
        *colon = qc;
        return v;
    }
    {
        size_t stop;
        int found;
        const unsigned char *s;
        size_t n;
        if (yp_find_keysep(t, p, &stop, &found) != 0) return NULL;
        if (!found) {
            yp_fail(t, "unexpected-content", p);
            return NULL;
        }
        *colon = stop;
        yp_span_trim(t, p, stop, &s, &n);
        return yp_resolve(t, p, s, n);
    }
}

static size_t yp_after_value_next(yp_t *t, size_t rv)
{
    return yp_next_content(t, yp_next_line(t, rv));
}

static size_t yp_settle(yp_t *t, size_t rv, size_t cs)
{
    if (cs == YP_NO_POS) return rv;
    {
        size_t ls = yp_line_start(t, cs);
        return (ls > 0) ? ls - 1 : rv;
    }
}

static mino_val *yp_lookahead_value(yp_t *t, size_t p, long I,
                                    int dash_value, size_t *resume)
{
    size_t cs = yp_after_value_next(t, p);
    if (cs == YP_NO_POS || yp_doc_marker(t, cs)) {
        *resume = p;
        return mino_nil(t->S);
    }
    if (yp_line_indent(t, cs) < I) {
        *resume = p;
        return mino_nil(t->S);
    }
    if (yp_line_indent(t, cs) > I) {
        return yp_block_node_r(t, cs, I, resume);
    }
    if (dash_value && yp_dash_entry(t, cs)) {
        return yp_block_seq(t, cs, I, resume);
    }
    *resume = p;
    return mino_nil(t->S);
}

static mino_val *yp_inline_value(yp_t *t, size_t p, long I, size_t *resume)
{
    unsigned char c;
    if (yp_is(t, p, '\t')) {
        yp_fail(t, "tab-indentation", p);
        return NULL;
    }
    if (p >= t->len) {
        *resume = p;
        return mino_nil(t->S);
    }
    c = t->data[p];
    if (yp_dash_entry(t, p)) {
        yp_fail(t, "unexpected-content", p);
        return NULL;
    }
    if (c == '"' || c == '\'') {
        return yp_quoted(t, p, I, 0, resume);
    }
    if (c == '[') return yp_flow_seq(t, p, I, resume);
    if (c == '{') return yp_flow_map(t, p, I, resume);
    if (c == '|' || c == '>') {
        mino_val *out = NULL;
        if (yp_block_scalar(t, p, I, resume, &out) != 0) return NULL;
        return out;
    }
    if (c == '&') {
        yp_fail(t, "unsupported-anchor", p);
        return NULL;
    }
    if (c == '*') {
        yp_fail(t, "unsupported-alias", p);
        return NULL;
    }
    if (c == '!') {
        yp_fail(t, "unsupported-tag", p);
        return NULL;
    }
    return yp_plain_block(t, p, I, resume);
}

static mino_val *yp_block_map(yp_t *t, size_t p, long I, size_t *resume)
{
    mino_state *S = t->S;
    mino_val *m = mino_map(S, NULL, NULL, 0);
    size_t q = p;
    if (m == NULL) return NULL;
    if (++t->depth > YP_MAX_DEPTH) {
        t->depth--;
        yp_fail(t, "flow-syntax", p);
        return NULL;
    }
    for (;;) {
        int quoted = 0;
        size_t colon;
        mino_val *k = yp_map_key(t, q, &colon, &quoted);
        mino_val *v;
        size_t rv;
        size_t cs;
        if (k == NULL) { t->depth--; return NULL; }
        (void)quoted;
        k = yp_key_of(t, k);
        if (yp_line_clear(t, colon + 1)) {
            v = yp_lookahead_value(t, colon, I, 1, &rv);
        } else {
            v = yp_inline_value(t, yp_skip_ws(t, colon + 1), I, &rv);
        }
        if (v == NULL) { t->depth--; return NULL; }
        if (rv != colon) {
            yp_check_line_clear(t, rv);
            if (t->err_reason != NULL) { t->depth--; return NULL; }
        }
        m = yp_assoc(t, m, k, v);
        if (m == NULL) { t->depth--; return NULL; }
        cs = yp_after_value_next(t, rv);
        if (cs == YP_NO_POS || yp_doc_marker(t, cs)) {
            *resume = yp_settle(t, rv, cs);
            t->depth--;
            return m;
        }
        if (yp_line_indent(t, cs) < I) {
            *resume = yp_settle(t, rv, cs);
            t->depth--;
            return m;
        }
        if (yp_line_indent(t, cs) > I) {
            yp_fail(t, "bad-indentation", yp_line_start(t, cs));
            t->depth--;
            return NULL;
        }
        if (yp_dash_entry(t, cs)) {
            yp_fail(t, "unexpected-content", cs);
            t->depth--;
            return NULL;
        }
        q = cs;
    }
}

static mino_val *yp_block_seq(yp_t *t, size_t p, long I, size_t *resume)
{
    mino_state *S = t->S;
    mino_val *acc = mino_vector(S, NULL, 0);
    size_t q = p;
    if (acc == NULL) return NULL;
    if (++t->depth > YP_MAX_DEPTH) {
        t->depth--;
        yp_fail(t, "flow-syntax", p);
        return NULL;
    }
    for (;;) {
        size_t after_dash = yp_skip_ws(t, q + 1);
        mino_val *v;
        size_t rv;
        size_t cs;
        mino_val *acc2;
        if (yp_line_clear(t, q + 1)) {
            v = yp_lookahead_value(t, q, I, 0, &rv);
        } else if (yp_dash_entry(t, after_dash)) {
            v = yp_block_seq(t, after_dash, yp_col(t, after_dash), &rv);
        } else if (yp_is(t, after_dash, '?')) {
            unsigned char d = yp_peek(t, after_dash + 1);
            if (d == 0 || d == '\n' || yp_ws_p(d)) {
                yp_fail(t, "unsupported-complex-key", after_dash);
                t->depth--;
                return NULL;
            }
            v = yp_inline_value(t, after_dash, I, &rv);
        } else if (t->data[after_dash] == '[' || t->data[after_dash] == '{' ||
                   t->data[after_dash] == '|' || t->data[after_dash] == '>' ||
                   t->data[after_dash] == '&' || t->data[after_dash] == '*' ||
                   t->data[after_dash] == '!' ||
                   t->data[after_dash] == '"' ||
                   t->data[after_dash] == '\'') {
            v = yp_inline_value(t, after_dash, I, &rv);
        } else {
            size_t stop;
            int found;
            if (yp_find_keysep(t, after_dash, &stop, &found) != 0) {
                t->depth--;
                return NULL;
            }
            if (found) {
                v = yp_block_map(t, after_dash, yp_col(t, after_dash), &rv);
            } else {
                v = yp_inline_value(t, after_dash, I, &rv);
            }
        }
        if (v == NULL) { t->depth--; return NULL; }
        if (rv != q && rv != after_dash) {
            yp_check_line_clear(t, rv);
            if (t->err_reason != NULL) { t->depth--; return NULL; }
        }
        acc2 = vec_conj1(S, acc, v);
        if (acc2 == NULL) { t->depth--; return NULL; }
        acc = acc2;
        cs = yp_after_value_next(t, rv);
        if (cs == YP_NO_POS || yp_doc_marker(t, cs)) {
            *resume = yp_settle(t, rv, cs);
            t->depth--;
            return acc;
        }
        if (yp_line_indent(t, cs) < I) {
            *resume = yp_settle(t, rv, cs);
            t->depth--;
            return acc;
        }
        if (yp_line_indent(t, cs) > I) {
            yp_fail(t, "bad-indentation", yp_line_start(t, cs));
            t->depth--;
            return NULL;
        }
        if (yp_dash_entry(t, cs)) {
            q = cs;
            continue;
        }
        *resume = yp_settle(t, rv, cs);
        t->depth--;
        return acc;
    }
}

static mino_val *yp_block_node_r(yp_t *t, size_t p, long pi, size_t *resume)
{
    unsigned char c;
    if (yp_is(t, p, '\t')) {
        yp_fail(t, "tab-indentation", p);
        return NULL;
    }
    if (p >= t->len) {
        *resume = p;
        return mino_nil(t->S);
    }
    c = t->data[p];
    if (yp_dash_entry(t, p)) {
        return yp_block_seq(t, p, yp_line_indent(t, p), resume);
    }
    if (c == '&') {
        yp_fail(t, "unsupported-anchor", p);
        return NULL;
    }
    if (c == '*') {
        yp_fail(t, "unsupported-alias", p);
        return NULL;
    }
    if (c == '!') {
        yp_fail(t, "unsupported-tag", p);
        return NULL;
    }
    if (c == '?' &&
        (yp_peek(t, p + 1) == 0 || yp_peek(t, p + 1) == '\n' ||
         yp_ws_p(yp_peek(t, p + 1)))) {
        yp_fail(t, "unsupported-complex-key", p);
        return NULL;
    }
    if (c == '[' || c == '{') {
        size_t q;
        mino_val *v = (c == '[') ? yp_flow_seq(t, p, pi, &q)
                                 : yp_flow_map(t, p, pi, &q);
        if (v == NULL) return NULL;
        if (yp_flow_key_colon(t, yp_skip_ws(t, q), 0)) {
            yp_fail(t, "unsupported-complex-key", p);
            return NULL;
        }
        *resume = q;
        return v;
    }
    if (c == '"' || c == '\'') {
        size_t q;
        mino_val *v = yp_quoted(t, p, pi, 0, &q);
        if (v == NULL) return NULL;
        if (yp_flow_key_colon(t, yp_skip_ws(t, q), 0)) {
            return yp_block_map(t, p, yp_line_indent(t, p), resume);
        }
        *resume = q;
        return v;
    }
    if (c == '|' || c == '>') {
        return yp_inline_value(t, p, pi, resume);
    }
    {
        size_t stop;
        int found;
        if (yp_find_keysep(t, p, &stop, &found) != 0) return NULL;
        if (found) {
            return yp_block_map(t, p, yp_line_indent(t, p), resume);
        }
        return yp_plain_block(t, p, pi, resume);
    }
}

/* ---- documents ---- */

static void yp_check_footer_line(yp_t *t, size_t p)
{
    size_t q = yp_skip_ws(t, p + 3);
    if (yp_at_eol(t, q)) return;
    if (t->data[q] == '#' && yp_ws_p(t->data[q - 1])) return;
    yp_fail(t, "unexpected-content", q);
}

static int yp_doc_finish(yp_t *t, size_t rv, mino_val *node,
                         size_t *resume, mino_val **doc)
{
    size_t cs;
    if (!yp_is(t, rv, ':')) {
        yp_check_line_clear(t, rv);
        if (t->err_reason != NULL) return -1;
    }
    cs = yp_next_content(t, yp_next_line(t, rv));
    if (cs == YP_NO_POS) {
        *resume = cs;
        *doc = node;
        return 0;
    }
    if (yp_doc_marker(t, cs)) {
        if (t->data[cs] == '.') {
            yp_check_footer_line(t, cs);
            if (t->err_reason != NULL) return -1;
            *resume = yp_next_line(t, cs);
        } else {
            *resume = cs;
        }
        *doc = node;
        return 0;
    }
    yp_fail(t, "unexpected-content", cs);
    return -1;
}

static mino_val *yp_marker_line_root(yp_t *t, size_t p, size_t *resume)
{
    unsigned char c;
    if (yp_is(t, p, '\t')) {
        yp_fail(t, "tab-indentation", p);
        return NULL;
    }
    if (p >= t->len) {
        *resume = p;
        return mino_nil(t->S);
    }
    c = t->data[p];
    if (c == '[' || c == '{') {
        size_t q;
        mino_val *v = (c == '[') ? yp_flow_seq(t, p, -1, &q)
                                 : yp_flow_map(t, p, -1, &q);
        if (v == NULL) return NULL;
        *resume = q;
        return v;
    }
    if (c == '"' || c == '\'') {
        return yp_quoted(t, p, -1, 0, resume);
    }
    if (c == '|' || c == '>') {
        return yp_inline_value(t, p, -1, resume);
    }
    if (c == '&') {
        yp_fail(t, "unsupported-anchor", p);
        return NULL;
    }
    if (c == '*') {
        yp_fail(t, "unsupported-alias", p);
        return NULL;
    }
    if (c == '!') {
        yp_fail(t, "unsupported-tag", p);
        return NULL;
    }
    return yp_plain_block(t, p, -1, resume);
}

static int yp_next_doc(yp_t *t, size_t p, size_t *resume, mino_val **doc,
                       int *emitted)
{
    size_t cs = yp_next_content(t, p);
    *emitted = 0;
    *doc = NULL;
    *resume = YP_NO_POS;
    if (cs == YP_NO_POS) return 0;
    if (yp_doc_marker(t, cs)) {
        if (t->data[cs] == '.') {
            yp_check_footer_line(t, cs);
            if (t->err_reason != NULL) return -1;
            *resume = yp_next_line(t, cs);
            return 0;
        }
        if (yp_line_clear(t, cs + 3)) {
            size_t cs2 = yp_next_content(t, yp_next_line(t, cs));
            if (cs2 == YP_NO_POS || yp_doc_marker(t, cs2)) {
                *resume = cs2;
                *doc = mino_nil(t->S);
                if (*doc == NULL) return -1;
                *emitted = 1;
                return 0;
            }
            {
                size_t rv;
                mino_val *node = yp_block_node_r(t, cs2, -1, &rv);
                int r2;
                if (node == NULL) return -1;
                r2 = yp_doc_finish(t, rv, node, resume, doc);
                if (r2 == 0) *emitted = 1;
                return r2;
            }
        }
        {
            size_t rv;
            mino_val *node = yp_marker_line_root(
                t, yp_skip_ws(t, cs + 3), &rv);
            int r2;
            if (node == NULL) return -1;
            yp_check_line_clear(t, rv);
            if (t->err_reason != NULL) return -1;
            r2 = yp_doc_finish(t, rv, node, resume, doc);
            if (r2 == 0) *emitted = 1;
            return r2;
        }
    }
    if (yp_is(t, cs, '%')) {
        yp_fail(t, "unsupported-directive", cs);
        return -1;
    }
    {
        size_t rv;
        mino_val *node = yp_block_node_r(t, cs, -1, &rv);
        int r2;
        if (node == NULL) return -1;
        r2 = yp_doc_finish(t, rv, node, resume, doc);
        if (r2 == 0) *emitted = 1;
        return r2;
    }
}

/* ---- prim ---- */

static mino_val *prim_yaml_parse(mino_state *S, mino_val *args,
                                 mino_env *env)
{
    mino_val *s_val;
    mino_val *kw_val;
    yp_t t;
    const unsigned char *data;
    size_t len;
    unsigned char *buf;
    size_t w;
    mino_val *docs_tr;
    size_t p = 0;
    int saved;
    (void)env;
    if (!mino_is_cons(args) ||
        !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "yaml-parse requires two arguments");
    }
    s_val = args->as.cons.car;
    kw_val = args->as.cons.cdr->as.cons.car;
    if (s_val == NULL || mino_type_of(s_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "yaml-parse: first argument must be a string");
    }
    data = (const unsigned char *)s_val->as.s.data;
    len = s_val->as.s.len;
    /* normalize CRLF to LF and append the trailing newline sentinel */
    saved = mino_current_ctx(S)->gc_depth;
    mino_current_ctx(S)->gc_depth = saved + 1;
    buf = (unsigned char *)gc_alloc_typed_inner(S, GC_T_RAW, len + 2);
    mino_current_ctx(S)->gc_depth = saved;
    if (buf == NULL) return NULL;
    w = 0;
    {
        size_t i;
        int pending_cr = 0;
        for (i = 0; i < len; i++) {
            if (data[i] == '\r') {
                if (i + 1 < len && data[i + 1] == '\n') i++;
                buf[w++] = '\n';
                pending_cr = 0;
            } else {
                buf[w++] = data[i];
            }
        }
        (void)pending_cr;
    }
    buf[w++] = '\n';
    data = buf;
    len = w;

    memset(&t, 0, sizeof(t));
    t.S = S;
    t.env = env;
    t.data = data;
    t.len = len;
    t.keywords = (kw_val != NULL && mino_type_of(kw_val) == MINO_BOOL &&
                  mino_val_bool_get(kw_val));
    docs_tr = mino_transient(S, mino_vector(S, NULL, 0));
    if (docs_tr == NULL) return NULL;
    while (p != YP_NO_POS && t.err_reason == NULL) {
        size_t resume = YP_NO_POS;
        mino_val *doc = NULL;
        int emitted = 0;
        if (yp_next_doc(&t, p, &resume, &doc, &emitted) != 0) break;
        if (emitted) {
            mino_val *nx = mino_conj_bang(S, docs_tr, doc);
            if (nx == NULL) return NULL;
            docs_tr = nx;
        }
        if (resume == YP_NO_POS && !emitted) break;
        if (resume == p) break; /* safety: always advance */
        p = resume;
    }
    if (t.err_reason != NULL) {
        mino_val *items[5];
        size_t line;
        size_t col;
        size_t eol;
        yp_loc(&t, t.err_pos, &line, &col);
        eol = yp_eol(&t, t.err_pos);
        items[0] = mino_keyword(S, "yaml/error");
        items[1] = mino_string(S, t.err_reason);
        items[2] = mino_int(S, (long long)line);
        items[3] = mino_int(S, (long long)col);
        items[4] = mino_string_n(S, (const char *)(t.data + t.err_pos),
                                 eol - t.err_pos);
        return mino_vector(S, items, 5);
    }
    return mino_persistent(S, docs_tr);
}

const mino_prim_def k_prims_yaml[] = {
    {"yaml-parse", prim_yaml_parse,
     "Parses YAML subset text into a vector of documents (ADR 26). "
     "Second argument selects keyword keys for string map keys. "
     "Returns the documents vector, or an error descriptor vector "
     "the mino.yaml facade converts to ex-info."},
};

const size_t k_prims_yaml_count =
    sizeof(k_prims_yaml) / sizeof(k_prims_yaml[0]);
