/*
 * http.c -- HTTP/1.1 message codec primitives: request serialization
 * (http-encode-request, http-encode-chunk) and response parsing
 * (http-parse-response, http-parse-response-chunks).
 *
 * The codec is a pure layer over buffers with no sockets and no
 * state outside one parse: the response parser is an incremental
 * machine owned by the caller, and every walk over the wire bytes it
 * is fed is bounded by an explicit cap (header-section bytes, header
 * count, chunk-size line, accumulated body). The prims are stateless:
 * http-parse-response re-feeds the bytes it is given through a fresh
 * parser, and http-parse-response-chunks drives one parser across a
 * vector of buffers exactly like a socket read loop, which is the
 * shape the keep-alive pool will use.
 *
 * Parsing leniency, decided per modern hygiene: bare LF line endings
 * are accepted (real servers emit them); obs-fold continuation lines
 * are rejected; responses to 1xx are skipped up to five times; 204
 * and 304 are bodiless regardless of framing headers; a message
 * carrying both Content-Length and Transfer-Encoding, or conflicting
 * Content-Length values, is rejected as smuggling-shaped. Header
 * names are emitted lowercased; a repeated header collects its
 * values into a vector in arrival order (Set-Cookie survives).
 *
 * Chunked request bodies are out of scope at the namespace layer for
 * v1, but the chunk frame encoder exists so tests and the future
 * writer surface share one framing implementation.
 */

#include "prim/internal.h"
#include "mino.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HTTP_DEFAULT_MAX_HEADER_BYTES (64u * 1024u)
#define HTTP_DEFAULT_MAX_HEADERS      100u
#define HTTP_DEFAULT_MAX_BODY_BYTES   (16ll * 1024ll * 1024ll)
#define HTTP_MAX_CHUNK_LINE           1024u
#define HTTP_MAX_INFO_RESPONSES       5
#define HTTP_CL_LIMIT                 (1ll << 53)
#define HTTP_OPT_MAX_HEADER_BYTES     (64ll * 1024ll * 1024ll)
#define HTTP_OPT_MAX_HEADERS          1024ll
#define HTTP_ERR_CAP                  160

enum {
    HTTP_MORE = 0,
    HTTP_DONE = 1,
    HTTP_ERR  = 2
};

enum {
    HTTP_FR_NONE = 0,
    HTTP_FR_CL,
    HTTP_FR_CHUNKED,
    HTTP_FR_CLOSE
};

enum {
    HTTP_ST_LINE = 0,
    HTTP_ST_HDRS,
    HTTP_ST_BODY_CL,
    HTTP_ST_BODY_CLOSE,
    HTTP_ST_CHUNK_SIZE,
    HTTP_ST_CHUNK_DATA,
    HTTP_ST_CHUNK_END,
    HTTP_ST_TRAILERS
};

typedef struct {
    size_t name_off, name_len;
    size_t val_off, val_len;
} http_row_t;

typedef struct mino_http_parser mino_http_parser_t;

struct mino_http_parser {
    unsigned char *buf;      /* every byte fed; the message is a prefix */
    size_t         buf_cap, buf_len;
    unsigned char *hdr;      /* lowercased names + verbatim values */
    size_t         hdr_cap, hdr_len;
    unsigned char *body;     /* de-chunked body; NULL until first chunk */
    size_t         body_cap, body_len;
    http_row_t    *rows;
    size_t         rows_cap, nrows;
    size_t         trailer_start;
    size_t         pos;
    size_t         section_start;  /* cap accounting origin for lines */
    size_t         body_start;     /* CL / close body offset in buf */
    size_t         reason_off, reason_len;
    long long      content_length; /* -1 until decided */
    long long      chunk_remaining;
    int            state, status, framing;
    int            code, http10;
    int            info_count;
    size_t         max_header_bytes;
    size_t         max_headers;
    long long      max_body_bytes;
    char           err[HTTP_ERR_CAP];
};

/* ---- shared character tables ---- */

static int http_lower(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
    return c;
}

/* RFC 7230 tchar: the name-safe alphabet for methods and header names. */
static int http_tchar(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= 'a' && c <= 'z') return 1;
    if (c >= '0' && c <= '9') return 1;
    return c == '!' || c == '#' || c == '$' || c == '%' || c == '&'
        || c == '\'' || c == '*' || c == '+' || c == '-' || c == '.'
        || c == '^' || c == '_' || c == '`' || c == '|' || c == '~';
}

static int http_ows(unsigned char c)
{
    return c == ' ' || c == '\t';
}

static int http_hex_val(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int http_name_is(const unsigned char *name, size_t len,
                        const char *want, size_t want_len)
{
    return len == want_len && memcmp(name, want, len) == 0;
}

static int http_value_is_ci(const unsigned char *v, size_t len,
                            const char *want, size_t want_len)
{
    size_t i;
    if (len != want_len) return 0;
    for (i = 0; i < len; i++) {
        if (http_lower(v[i]) != want[i]) return 0;
    }
    return 1;
}

static void http_fail(mino_http_parser_t *p, const char *fmt, ...)
{
    va_list ap;
    if (p->status == HTTP_ERR) return;
    va_start(ap, fmt);
    vsnprintf(p->err, sizeof(p->err), fmt, ap);
    va_end(ap);
    p->status = HTTP_ERR;
}

/* Grow buf/hdr/body to hold at least want total bytes. Returns 0 on
 * success; on failure the parser is failed with an OOM message. */
static int http_reserve(mino_http_parser_t *p, unsigned char **buf,
                        size_t *cap, size_t want)
{
    size_t nc;
    unsigned char *nb;
    if (want <= *cap) return 0;
    if (want > SIZE_MAX / 2) {
        http_fail(p, "http: buffer size overflow");
        return -1;
    }
    nc = *cap > 0 ? *cap : 256;
    while (nc < want) nc *= 2;
    nb = (unsigned char *)realloc(*buf, nc);
    if (nb == NULL) {
        http_fail(p, "http: out of memory");
        return -1;
    }
    *buf = nb;
    *cap = nc;
    return 0;
}

/* ---- response parser (untrusted input) ----
 *
 * The parser stays TU-local: the request and pool prims that will
 * drive it land in this same file (the net.c precedent of one TU per
 * layer), so the C API is static until a second TU needs it. */

static mino_http_parser_t *http_parser_new(size_t max_header_bytes,
                                           size_t max_headers,
                                           long long max_body_bytes)
{
    mino_http_parser_t *p =
        (mino_http_parser_t *)malloc(sizeof(*p));
    if (p == NULL) return NULL;
    memset(p, 0, sizeof(*p));
    p->rows = (http_row_t *)malloc(max_headers * sizeof(*p->rows));
    if (p->rows == NULL) {
        free(p);
        return NULL;
    }
    p->rows_cap         = max_headers;
    p->max_header_bytes = max_header_bytes;
    p->max_headers      = max_headers;
    p->max_body_bytes   = max_body_bytes;
    p->content_length   = -1;
    p->trailer_start    = (size_t)-1;
    p->state            = HTTP_ST_LINE;
    p->status           = HTTP_MORE;
    p->framing          = HTTP_FR_NONE;
    return p;
}

static void http_parser_free(mino_http_parser_t *p)
{
    if (p == NULL) return;
    free(p->buf);
    free(p->hdr);
    free(p->body);
    free(p->rows);
    free(p);
}

/* Next complete line within [pos, buf_len). On success returns 1,
 * sets the start and end of the content (trailing CR stripped) and
 * *advance to the first byte past the newline. Returns 0 with the
 * status set to MORE (no newline yet) or ERR (cap exceeded); cap
 * accounting runs from origin, which is the section start for header
 * blocks and the line start for chunk framing lines. */
static int http_next_line(mino_http_parser_t *p, size_t origin, size_t cap,
                          size_t *start, size_t *end, size_t *advance)
{
    size_t i;
    size_t reach;
    for (i = p->pos; i < p->buf_len; i++) {
        if (p->buf[i] == '\n') break;
    }
    reach = (i < p->buf_len) ? i + 1 : p->buf_len;
    if (reach - origin > cap) {
        http_fail(p, "http: line exceeds the %lu byte cap",
                  (unsigned long)cap);
        return 0;
    }
    if (i >= p->buf_len) return 0;
    *start = p->pos;
    *end   = i;
    if (*end > *start && p->buf[*end - 1] == '\r') (*end)--;
    *advance = i + 1;
    return 1;
}

static void http_reset_head(mino_http_parser_t *p)
{
    p->nrows           = 0;
    p->hdr_len         = 0;
    p->content_length  = -1;
    p->framing         = HTTP_FR_NONE;
    p->section_start   = p->pos;
    p->state           = HTTP_ST_LINE;
}

static void http_parse_status_line(mino_http_parser_t *p, size_t start,
                                   size_t end)
{
    size_t i;
    int code = 0;
    static const char k_prefix[] = "HTTP/1.";
    if (end - start < 12 || memcmp(p->buf + start, k_prefix, 7) != 0) {
        http_fail(p, "http: malformed status line");
        return;
    }
    if (p->buf[start + 7] == '0') {
        p->http10 = 1;
    } else if (p->buf[start + 7] == '1') {
        p->http10 = 0;
    } else {
        http_fail(p, "http: unsupported HTTP version");
        return;
    }
    if (p->buf[start + 8] != ' ') {
        http_fail(p, "http: malformed status line");
        return;
    }
    for (i = start + 9; i < start + 12; i++) {
        if (p->buf[i] < '0' || p->buf[i] > '9') {
            http_fail(p, "http: malformed status code");
            return;
        }
        code = code * 10 + (p->buf[i] - '0');
    }
    if (code < 100 || code > 599) {
        http_fail(p, "http: status code out of range");
        return;
    }
    p->code = code;
    for (i = start + 12; i < end; i++) {
        if (p->buf[i] == 0) {
            http_fail(p, "http: NUL in status line");
            return;
        }
    }
    if (end == start + 12) {
        p->reason_off = end;
        p->reason_len = 0;
        return;
    }
    /* exactly one SP separates the code from the reason phrase */
    if (p->buf[start + 12] != ' ') {
        http_fail(p, "http: malformed status line");
        return;
    }
    p->reason_off = start + 13;
    p->reason_len = end - p->reason_off;
}

/* Parse one header/trailer line into the row store. The name must be
 * a nonempty token, the value is OWS-trimmed and may carry only
 * field-content bytes (printables, SP, HTAB; RFC 7230), and a leading
 * SP/HTAB is an obs-fold continuation (rejected). */
static void http_store_header_row(mino_http_parser_t *p, size_t start,
                                  size_t end)
{
    size_t c, v, vend, k;
    http_row_t *row;
    if (end > start && (p->buf[start] == ' ' || p->buf[start] == '\t')) {
        http_fail(p, "http: obsolete line folding is rejected");
        return;
    }
    for (c = start; c < end; c++) {
        if (p->buf[c] == ':') break;
        if (!http_tchar(p->buf[c])) {
            http_fail(p, "http: invalid character in header name");
            return;
        }
    }
    if (c == end) {
        http_fail(p, "http: header line has no colon");
        return;
    }
    if (c == start) {
        http_fail(p, "http: empty header name");
        return;
    }
    v = c + 1;
    while (v < end && http_ows(p->buf[v])) v++;
    vend = end;
    while (vend > v && http_ows(p->buf[vend - 1])) vend--;
    for (k = v; k < vend; k++) {
        unsigned char c2 = p->buf[k];
        if ((c2 < 0x20 && c2 != '\t') || c2 == 0x7f) {
            http_fail(p, "http: control byte in header value");
            return;
        }
    }
    if (p->nrows >= p->rows_cap) {
        http_fail(p, "http: more than %lu headers",
                  (unsigned long)p->rows_cap);
        return;
    }
    if (http_reserve(p, &p->hdr, &p->hdr_cap,
                     p->hdr_len + (c - start) + (vend - v)) != 0)
        return;
    row = &p->rows[p->nrows];
    row->name_off = p->hdr_len;
    row->name_len = c - start;
    for (k = 0; k < c - start; k++) {
        p->hdr[p->hdr_len + k] = (unsigned char)http_lower(p->buf[start + k]);
    }
    p->hdr_len += c - start;
    row->val_off = p->hdr_len;
    row->val_len = vend - v;
    if (vend > v) {
        memcpy(p->hdr + p->hdr_len, p->buf + v, vend - v);
        p->hdr_len += vend - v;
    }
    p->nrows++;
}

/* Content-Length: digits only, capped below 2^53 so the value is both
 * an exact integer and a plausible body size. */
static int http_parse_cl(const unsigned char *v, size_t len,
                         long long *out)
{
    size_t i;
    unsigned long long acc = 0;
    if (len == 0) return -1;
    for (i = 0; i < len; i++) {
        if (v[i] < '0' || v[i] > '9') return -1;
        acc = acc * 10u + (unsigned long long)(v[i] - '0');
        if (acc > (unsigned long long)HTTP_CL_LIMIT) return -1;
    }
    *out = (long long)acc;
    return 0;
}

/* Framing decision at the blank line. Runs the smuggling checks on
 * every message (including bodiless ones) before choosing. */
static void http_decide_framing(mino_http_parser_t *p)
{
    size_t i;
    int have_cl = 0, have_te = 0;
    long long cl = -1;
    for (i = 0; i < p->nrows; i++) {
        const unsigned char *n = p->hdr + p->rows[i].name_off;
        const unsigned char *v = p->hdr + p->rows[i].val_off;
        if (http_name_is(n, p->rows[i].name_len, "content-length", 14)) {
            long long this_cl = -1;
            if (http_parse_cl(v, p->rows[i].val_len, &this_cl) != 0) {
                http_fail(p, "http: malformed content-length");
                return;
            }
            if (have_cl && this_cl != cl) {
                http_fail(p, "http: conflicting content-length values");
                return;
            }
            cl = this_cl;
            have_cl = 1;
        } else if (http_name_is(n, p->rows[i].name_len,
                                 "transfer-encoding", 17)) {
            if (have_te) {
                http_fail(p, "http: multiple transfer-encoding headers");
                return;
            }
            have_te = 1;
            if (!http_value_is_ci(v, p->rows[i].val_len, "chunked", 7)) {
                http_fail(p, "http: unsupported transfer-encoding");
                return;
            }
        }
    }
    if (have_te) {
        if (have_cl) {
            http_fail(p, "http: both content-length and transfer-encoding");
            return;
        }
        p->framing         = HTTP_FR_CHUNKED;
        p->content_length  = -1;
        return;
    }
    if (p->code == 204 || p->code == 304) {
        /* bodiless by definition; framing headers are server noise */
        p->framing = HTTP_FR_NONE;
        return;
    }
    if (have_cl) {
        if (cl > p->max_body_bytes) {
            http_fail(p, "http: content-length exceeds the %lld byte cap",
                      p->max_body_bytes);
            return;
        }
        p->framing         = HTTP_FR_CL;
        p->content_length  = cl;
        return;
    }
    p->framing = HTTP_FR_CLOSE;
}

static int http_run(mino_http_parser_t *p)
{
    for (;;) {
        size_t start, end, advance;
        if (p->status != HTTP_MORE) return p->status;
        switch (p->state) {
        case HTTP_ST_LINE:
            if (!http_next_line(p, p->section_start, p->max_header_bytes,
                                &start, &end, &advance))
                return p->status;
            p->pos = advance;
            http_parse_status_line(p, start, end);
            if (p->status == HTTP_ERR) return p->status;
            p->state = HTTP_ST_HDRS;
            continue;
        case HTTP_ST_HDRS:
            if (!http_next_line(p, p->section_start, p->max_header_bytes,
                                &start, &end, &advance))
                return p->status;
            p->pos = advance;
            if (end > start) {
                http_store_header_row(p, start, end);
                if (p->status == HTTP_ERR) return p->status;
                continue;
            }
            if (p->code >= 100 && p->code < 200) {
                p->info_count++;
                if (p->info_count > HTTP_MAX_INFO_RESPONSES) {
                    http_fail(p, "http: more than %d informational responses",
                              HTTP_MAX_INFO_RESPONSES);
                    return p->status;
                }
                http_reset_head(p);
                continue;
            }
            http_decide_framing(p);
            if (p->status == HTTP_ERR) return p->status;
            switch (p->framing) {
            case HTTP_FR_NONE:
                p->body_start = p->pos;
                p->status     = HTTP_DONE;
                return p->status;
            case HTTP_FR_CL:
                p->body_start = p->pos;
                p->state      = HTTP_ST_BODY_CL;
                continue;
            case HTTP_FR_CHUNKED:
                p->state = HTTP_ST_CHUNK_SIZE;
                continue;
            default:
                p->body_start = p->pos;
                p->state      = HTTP_ST_BODY_CLOSE;
                continue;
            }
        case HTTP_ST_BODY_CL:
            if ((long long)(p->buf_len - p->body_start)
                < p->content_length)
                return HTTP_MORE;
            p->pos    = p->body_start + (size_t)p->content_length;
            p->status = HTTP_DONE;
            return p->status;
        case HTTP_ST_BODY_CLOSE:
            if ((long long)(p->buf_len - p->body_start)
                > p->max_body_bytes) {
                http_fail(p, "http: body exceeds the %lld byte cap",
                          p->max_body_bytes);
                return p->status;
            }
            return HTTP_MORE;
        case HTTP_ST_CHUNK_SIZE: {
            size_t i;
            unsigned long long acc = 0;
            if (!http_next_line(p, p->pos, HTTP_MAX_CHUNK_LINE,
                                &start, &end, &advance))
                return p->status;
            p->pos = advance;
            i = start;
            while (i < end && http_hex_val(p->buf[i]) >= 0) {
                acc = acc * 16u + (unsigned long long)http_hex_val(p->buf[i]);
                if (acc > (unsigned long long)HTTP_CL_LIMIT) {
                    http_fail(p, "http: chunk size too large");
                    return p->status;
                }
                i++;
            }
            if (i == start) {
                http_fail(p, "http: invalid chunk size");
                return p->status;
            }
            for (; i < end; i++) {
                if (p->buf[i] != ';' && !http_ows(p->buf[i])) {
                    http_fail(p, "http: invalid chunk size");
                    return p->status;
                }
            }
            if (acc == 0) {
                p->trailer_start = p->nrows;
                p->section_start = p->pos;
                p->state         = HTTP_ST_TRAILERS;
                continue;
            }
            if ((long long)p->body_len + (long long)acc
                > p->max_body_bytes) {
                http_fail(p, "http: chunked body exceeds the %lld byte cap",
                          p->max_body_bytes);
                return p->status;
            }
            p->chunk_remaining = (long long)acc;
            p->state           = HTTP_ST_CHUNK_DATA;
            continue;
        }
        case HTTP_ST_CHUNK_DATA: {
            size_t avail = p->buf_len - p->pos;
            size_t take  = (long long)avail < p->chunk_remaining
                ? avail : (size_t)p->chunk_remaining;
            if (take > 0) {
                if (http_reserve(p, &p->body, &p->body_cap,
                                 p->body_len + take) != 0)
                    return p->status;
                memcpy(p->body + p->body_len, p->buf + p->pos, take);
                p->pos += take;
                p->body_len += take;
                p->chunk_remaining -= (long long)take;
            }
            if (p->chunk_remaining > 0) return HTTP_MORE;
            p->state = HTTP_ST_CHUNK_END;
            continue;
        }
        case HTTP_ST_CHUNK_END:
            if (!http_next_line(p, p->pos, HTTP_MAX_CHUNK_LINE,
                                &start, &end, &advance))
                return p->status;
            p->pos = advance;
            if (end != start) {
                http_fail(p, "http: malformed chunk terminator");
                return p->status;
            }
            p->state = HTTP_ST_CHUNK_SIZE;
            continue;
        case HTTP_ST_TRAILERS:
            if (!http_next_line(p, p->section_start, p->max_header_bytes,
                                &start, &end, &advance))
                return p->status;
            p->pos = advance;
            if (end > start) {
                http_store_header_row(p, start, end);
                if (p->status == HTTP_ERR) return p->status;
                continue;
            }
            p->status = HTTP_DONE;
            return p->status;
        default:
            http_fail(p, "http: parser in an impossible state");
            return p->status;
        }
    }
}

static int http_parser_feed(mino_http_parser_t *p, const unsigned char *data,
                            size_t len)
{
    if (p->status == HTTP_ERR) return HTTP_ERR;
    if (len > 0) {
        if (http_reserve(p, &p->buf, &p->buf_cap, p->buf_len + len) != 0)
            return p->status;
        memcpy(p->buf + p->buf_len, data, len);
        p->buf_len += len;
    }
    return http_run(p);
}

static int http_parser_eof(mino_http_parser_t *p)
{
    if (p->status != HTTP_MORE) return p->status;
    if (p->state == HTTP_ST_BODY_CLOSE) {
        /* the whole remainder is the close-delimited body */
        p->pos    = p->buf_len;
        p->status = HTTP_DONE;
        return p->status;
    }
    http_fail(p, "http: connection closed before the message completed");
    return p->status;
}

/* ---- request serialization ---- */

typedef struct {
    const char *name;
    size_t      name_len;
    const char *value;
    size_t      value_len;
} http_hdr_in_t;

typedef struct {
    const char        *method;
    size_t             method_len;
    const char        *target;
    size_t             target_len;
    const char        *host;
    size_t             host_len;
    const http_hdr_in_t *headers;
    size_t             nheaders;
    const unsigned char *body;   /* NULL: no body */
    size_t             body_len;
    int                http10;
    int                chunked;
} http_request_t;

typedef struct {
    unsigned char *p;
    size_t         len, cap;
    int            failed;
} http_out_t;

static int http_out_put(http_out_t *o, const void *data, size_t n)
{
    size_t nc;
    unsigned char *nb;
    if (o->failed) return -1;
    if (n > SIZE_MAX - o->len) {
        o->failed = 1;
        return -1;
    }
    if (o->len + n > o->cap) {
        if (o->len + n > SIZE_MAX / 2) {
            o->failed = 1;
            return -1;
        }
        nc = o->cap > 0 ? o->cap * 2 : 256;
        while (nc < o->len + n) nc *= 2;
        nb = (unsigned char *)realloc(o->p, nc);
        if (nb == NULL) {
            o->failed = 1;
            return -1;
        }
        o->p   = nb;
        o->cap = nc;
    }
    memcpy(o->p + o->len, data, n);
    o->len += n;
    return 0;
}

static int http_out_str(http_out_t *o, const char *s)
{
    return http_out_put(o, s, strlen(s));
}

/* Header/method bytes the layer computes itself; a caller supplying
 * them is a contract error, not a smuggling attempt, but rejecting
 * keeps one authority over framing. */
static int http_owned_name(const char *name, size_t len)
{
    static const char *const k_owned[] = {
        "host", "content-length", "transfer-encoding", NULL
    };
    size_t i, j;
    for (i = 0; k_owned[i] != NULL; i++) {
        const char *w = k_owned[i];
        size_t wl = strlen(w);
        if (len != wl) continue;
        for (j = 0; j < len; j++) {
            if (http_lower((unsigned char)name[j]) != w[j]) break;
        }
        if (j == len) return 1;
    }
    return 0;
}

static int http_valid_target(const char *s, size_t len)
{
    size_t i;
    if (len == 0) return 0;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c <= 0x20 || c == 0x7f) return 0;
    }
    return 1;
}

static int http_valid_field_value(const char *s, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\r' || c == '\n' || c == 0 || c == 0x7f) return 0;
        if (c < 0x20 && c != '\t') return 0;
    }
    return 1;
}

/* Serialize a validated request into a malloc'd buffer. Pure; the
 * caller frees *out. Returns 0 on success, -1 with err filled. */
static int http_encode_request(const http_request_t *req,
                               unsigned char **out, size_t *out_len,
                               char *err, size_t err_cap)
{
    http_out_t o;
    char numbuf[32];
    size_t i;
    int numlen;
    memset(&o, 0, sizeof(o));
    *out = NULL;
    *out_len = 0;

    if (req->method_len == 0) {
        snprintf(err, err_cap, "http-encode-request: :method is empty");
        return -1;
    }
    for (i = 0; i < req->method_len; i++) {
        if (!http_tchar((unsigned char)req->method[i])) {
            snprintf(err, err_cap,
                     "http-encode-request: :method has a non-token character");
            return -1;
        }
    }
    if (!http_valid_target(req->target, req->target_len)) {
        snprintf(err, err_cap,
                 "http-encode-request: :target is empty or has a "
                 "space or control byte");
        return -1;
    }
    if (!http_valid_target(req->host, req->host_len)) {
        snprintf(err, err_cap,
                 "http-encode-request: :host is empty or has a "
                 "space or control byte");
        return -1;
    }
    if (req->chunked && req->http10) {
        snprintf(err, err_cap,
                 "http-encode-request: chunked requests need HTTP/1.1");
        return -1;
    }
    if (req->chunked && req->body != NULL) {
        snprintf(err, err_cap,
                 "http-encode-request: :chunked? cannot carry a fixed "
                 ":body");
        return -1;
    }
    for (i = 0; i < req->nheaders; i++) {
        const http_hdr_in_t *h = &req->headers[i];
        size_t j;
        if (h->name_len == 0) {
            snprintf(err, err_cap,
                     "http-encode-request: header name is empty");
            return -1;
        }
        for (j = 0; j < h->name_len; j++) {
            if (!http_tchar((unsigned char)h->name[j])) {
                snprintf(err, err_cap,
                         "http-encode-request: header name has a "
                         "non-token character");
                return -1;
            }
        }
        if (!http_valid_field_value(h->value, h->value_len)) {
            snprintf(err, err_cap,
                     "http-encode-request: header value has a CR, LF, "
                     "or NUL byte");
            return -1;
        }
        if (http_owned_name(h->name, h->name_len)) {
            snprintf(err, err_cap,
                     "http-encode-request: header %.*s is owned by the "
                     "HTTP layer",
                     (int)(h->name_len < 64 ? h->name_len : 64), h->name);
            return -1;
        }
    }

    if (http_out_put(&o, req->method, req->method_len) != 0
        || http_out_str(&o, " ") != 0
        || http_out_put(&o, req->target, req->target_len) != 0
        || http_out_str(&o, req->http10 ? " HTTP/1.0\r\n" : " HTTP/1.1\r\n") != 0
        || http_out_str(&o, "Host: ") != 0
        || http_out_put(&o, req->host, req->host_len) != 0
        || http_out_str(&o, "\r\n") != 0) {
        goto oom;
    }
    for (i = 0; i < req->nheaders; i++) {
        if (http_out_put(&o, req->headers[i].name, req->headers[i].name_len) != 0
            || http_out_str(&o, ": ") != 0
            || http_out_put(&o, req->headers[i].value,
                            req->headers[i].value_len) != 0
            || http_out_str(&o, "\r\n") != 0) {
            goto oom;
        }
    }
    if (req->chunked) {
        if (http_out_str(&o, "Transfer-Encoding: chunked\r\n") != 0)
            goto oom;
    } else if (req->body != NULL) {
        numlen = snprintf(numbuf, sizeof(numbuf), "%llu",
                          (unsigned long long)req->body_len);
        if (http_out_str(&o, "Content-Length: ") != 0
            || http_out_put(&o, numbuf, (size_t)numlen) != 0
            || http_out_str(&o, "\r\n") != 0) {
            goto oom;
        }
    }
    if (http_out_str(&o, "\r\n") != 0) goto oom;
    if (req->body != NULL && req->body_len > 0) {
        if (http_out_put(&o, req->body, req->body_len) != 0) goto oom;
    }
    *out     = o.p;
    *out_len = o.len;
    return 0;

oom:
    free(o.p);
    snprintf(err, err_cap, "http-encode-request: out of memory");
    return -1;
}

/* One chunked frame: "<hex>\r\n<data>\r\n", or the terminal "0\r\n\r\n"
 * for an empty payload. Pure; the caller frees *out. */
static int http_encode_chunk(const unsigned char *data, size_t len,
                             unsigned char **out, size_t *out_len)
{
    http_out_t o;
    char hexbuf[32];
    int hexlen;
    memset(&o, 0, sizeof(o));
    if (len == 0) {
        if (http_out_str(&o, "0\r\n\r\n") != 0) {
            free(o.p);
            return -1;
        }
    } else {
        hexlen = snprintf(hexbuf, sizeof(hexbuf), "%llx",
                          (unsigned long long)len);
        if (http_out_put(&o, hexbuf, (size_t)hexlen) != 0
            || http_out_str(&o, "\r\n") != 0
            || http_out_put(&o, data, len) != 0
            || http_out_str(&o, "\r\n") != 0) {
            free(o.p);
            return -1;
        }
    }
    *out     = o.p;
    *out_len = o.len;
    return 0;
}

/* ---- prim argument helpers ---- */

/* String-or-bytes byte view, the codec-family convention. */
static int http_text_arg(const mino_val *v, const unsigned char **data,
                         size_t *len)
{
    if (v == NULL) return 0;
    if (mino_type_of(v) == MINO_STRING) {
        *data = (const unsigned char *)v->as.s.data;
        *len  = v->as.s.len;
        return 1;
    }
    if (mino_is_bytes(v)) {
        *data = mino_bytes_data(v);
        *len  = mino_bytes_len(v);
        return 1;
    }
    return 0;
}

/* Header name as string or plain keyword (the namespace part, if
 * any, stays in the name and will fail token validation). */
static int http_name_arg(const mino_val *v, const char **name,
                         size_t *len)
{
    if (v == NULL) return 0;
    if (mino_type_of(v) == MINO_STRING) {
        *name = v->as.s.data;
        *len  = v->as.s.len;
        return 1;
    }
    if (mino_type_of(v) == MINO_KEYWORD || mino_type_of(v) == MINO_SYMBOL) {
        size_t skip = v->as.s.ns_len > 0 ? v->as.s.ns_len + 1 : 0;
        if (skip >= v->as.s.len) return 0;
        *name = v->as.s.data + skip;
        *len  = v->as.s.len - skip;
        return 1;
    }
    return 0;
}

static int http_opt_bool(mino_state *S, const mino_val *opts, const char *key,
                         int *out)
{
    const mino_val *v;
    *out = 0;
    if (opts == NULL || mino_type_of(opts) != MINO_MAP) return 0;
    v = map_get_val(opts, mino_keyword(S, key));
    if (v == NULL || mino_type_of(v) == MINO_NIL) return 0;
    if (mino_type_of(v) != MINO_BOOL) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "http: opts key :%s must be a boolean", key);
        prim_throw_classified(S, "eval/contract", "MCT001", msg);
        return -1;
    }
    *out = mino_val_bool_get(v);
    return 0;
}

static int http_opt_long(mino_state *S, const mino_val *opts, const char *key,
                         long long def, long long lo, long long hi,
                         long long *out)
{
    const mino_val *v;
    long long n;
    *out = def;
    if (opts == NULL || mino_type_of(opts) != MINO_MAP) return 0;
    v = map_get_val(opts, mino_keyword(S, key));
    if (v == NULL || mino_type_of(v) == MINO_NIL) return 0;
    if (!as_long(v, &n) || n < lo || n > hi) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "http: opts key :%s must be an integer in %lld..%lld",
                 key, lo, hi);
        prim_throw_classified(S, "eval/contract", "MCT001", msg);
        return -1;
    }
    *out = n;
    return 0;
}

/* ---- result map assembly ---- */

/* Map over parser rows [from,to): names lowercased, single values as
 * strings, repeats collected into vectors in arrival order. */
static mino_val *http_rows_map(mino_state *S, const mino_http_parser_t *p,
                               size_t from, size_t to)
{
    mino_val **keys, **vals, **tmp;
    size_t n = to - from, nd = 0, i, j;
    mino_val *m;
    if (n == 0) return mino_map(S, NULL, NULL, 0);
    keys = (mino_val **)malloc(n * sizeof(*keys));
    vals = (mino_val **)malloc(n * sizeof(*vals));
    tmp  = (mino_val **)malloc(n * sizeof(*tmp));
    if (keys == NULL || vals == NULL || tmp == NULL) {
        free(keys); free(vals); free(tmp);
        return prim_throw_classified(S, "internal", "MIN001",
                                     "http: out of memory");
    }
    /* GC is suppressed while the C arrays hold freshly built values
     * no GC root can see (the map_assoc_pairs precedent). */
    mino_current_ctx(S)->gc_depth++;
    for (i = from; i < to; i++) {
        size_t count = 0;
        int is_new = 1;
        for (j = from; j < i; j++) {
            if (p->rows[j].name_len == p->rows[i].name_len
                && memcmp(p->hdr + p->rows[j].name_off,
                          p->hdr + p->rows[i].name_off,
                          p->rows[i].name_len) == 0) {
                is_new = 0;
                break;
            }
        }
        if (!is_new) continue;
        for (j = i; j < to; j++) {
            if (p->rows[j].name_len == p->rows[i].name_len
                && memcmp(p->hdr + p->rows[j].name_off,
                          p->hdr + p->rows[i].name_off,
                          p->rows[i].name_len) == 0) {
                tmp[count++] = mino_string_n(
                    S, (const char *)(p->hdr + p->rows[j].val_off),
                    p->rows[j].val_len);
            }
        }
        keys[nd] = mino_string_n(
            S, (const char *)(p->hdr + p->rows[i].name_off),
            p->rows[i].name_len);
        vals[nd] = count == 1 ? tmp[0] : mino_vector(S, tmp, count);
        nd++;
    }
    m = mino_map(S, keys, vals, nd);
    mino_current_ctx(S)->gc_depth--;
    free(keys); free(vals); free(tmp);
    return m;
}

static mino_val *http_result_map(mino_state *S,
                                 const mino_http_parser_t *p)
{
    mino_val *keys[8], *vals[8];
    const unsigned char *body;
    size_t body_len;
    if (p->status == HTTP_ERR) {
        keys[0] = mino_keyword(S, "status");
        vals[0] = mino_keyword(S, "error");
        keys[1] = mino_keyword(S, "error");
        vals[1] = mino_string_n(S, p->err, strlen(p->err));
        return mino_map(S, keys, vals, 2);
    }
    if (p->status == HTTP_MORE) {
        keys[0] = mino_keyword(S, "status");
        vals[0] = mino_keyword(S, "need-more");
        return mino_map(S, keys, vals, 1);
    }
    if (p->framing == HTTP_FR_CHUNKED) {
        body     = p->body;
        body_len = p->body_len;
    } else {
        body     = p->buf + p->body_start;
        body_len = p->pos - p->body_start;
    }
    mino_current_ctx(S)->gc_depth++;
    keys[0] = mino_keyword(S, "status");
    vals[0] = mino_keyword(S, "done");
    keys[1] = mino_keyword(S, "code");
    vals[1] = mino_int(S, p->code);
    keys[2] = mino_keyword(S, "reason");
    vals[2] = mino_string_n(S, (const char *)(p->buf + p->reason_off),
                            p->reason_len);
    keys[3] = mino_keyword(S, "http-version");
    vals[3] = mino_string_n(S, p->http10 ? "HTTP/1.0" : "HTTP/1.1", 8);
    keys[4] = mino_keyword(S, "headers");
    vals[4] = http_rows_map(S, p, 0,
                            p->trailer_start == (size_t)-1
                                ? p->nrows : p->trailer_start);
    keys[5] = mino_keyword(S, "body");
    vals[5] = mino_bytes(S, body, body_len);
    keys[6] = mino_keyword(S, "chunked?");
    vals[6] = p->framing == HTTP_FR_CHUNKED
        ? mino_true(S) : mino_false(S);
    keys[7] = mino_keyword(S, "trailers");
    vals[7] = p->trailer_start == (size_t)-1
        ? mino_map(S, NULL, NULL, 0)
        : http_rows_map(S, p, p->trailer_start, p->nrows);
    {
        mino_val *m = mino_map(S, keys, vals, 8);
        mino_current_ctx(S)->gc_depth--;
        return m;
    }
}

/* Parse-opts triple shared by both parse prims. */
typedef struct {
    size_t    max_header_bytes;
    size_t    max_headers;
    long long max_body_bytes;
    int       eof;
} http_parse_opts_t;

static int http_read_parse_opts(mino_state *S, const mino_val *opts,
                                http_parse_opts_t *out)
{
    long long v;
    out->max_header_bytes = HTTP_DEFAULT_MAX_HEADER_BYTES;
    out->max_headers      = HTTP_DEFAULT_MAX_HEADERS;
    out->max_body_bytes   = HTTP_DEFAULT_MAX_BODY_BYTES;
    out->eof              = 0;
    if (opts == NULL || mino_type_of(opts) == MINO_NIL) return 0;
    if (mino_type_of(opts) != MINO_MAP) {
        prim_throw_classified(S, "eval/type", "MTY001",
                              "http: opts must be a map");
        return -1;
    }
    if (http_opt_long(S, opts, "max-header-bytes", HTTP_DEFAULT_MAX_HEADER_BYTES,
                      1, HTTP_OPT_MAX_HEADER_BYTES, &v) != 0)
        return -1;
    out->max_header_bytes = (size_t)v;
    if (http_opt_long(S, opts, "max-headers", HTTP_DEFAULT_MAX_HEADERS,
                      1, HTTP_OPT_MAX_HEADERS, &v) != 0)
        return -1;
    out->max_headers = (size_t)v;
    if (http_opt_long(S, opts, "max-body-bytes", HTTP_DEFAULT_MAX_BODY_BYTES,
                      0, HTTP_CL_LIMIT, &v) != 0)
        return -1;
    out->max_body_bytes = v;
    if (http_opt_bool(S, opts, "eof", &out->eof) != 0) return -1;
    return 0;
}

/* Run one parser to its terminal status. */
static mino_val *http_parse_drive(mino_state *S, const http_parse_opts_t *o,
                                  mino_http_parser_t *p)
{
    mino_val *result;
    if (o->eof) http_parser_eof(p);
    result = http_result_map(S, p);
    http_parser_free(p);
    return result;
}

/* ---- prims ---- */

/* (http-encode-request m) -> bytes. Keys :method :target :host
 * (required strings), :headers (vector of [name value] pairs or a
 * map), :body (bytes or string), :chunked?, :http10?. */
static mino_val *prim_http_encode_request(mino_state *S, mino_val *args,
                                          mino_env *env)
{
    mino_val *m, *v;
    http_request_t req;
    http_hdr_in_t *hdrs = NULL;
    unsigned char *out = NULL;
    size_t out_len = 0;
    char err[160];
    (void)env;
    memset(&req, 0, sizeof(req));

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "http-encode-request requires one "
                                     "map");
    }
    m = args->as.cons.car;
    if (m == NULL || mino_type_of(m) != MINO_MAP) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "http-encode-request: argument must "
                                     "be a map");
    }
    v = map_get_val(m, mino_keyword(S, "method"));
    if (v == NULL || mino_type_of(v) != MINO_STRING) {
        return prim_throw_classified(S, "eval/contract", "MCT001",
                                     "http-encode-request: :method must "
                                     "be a string");
    }
    req.method     = v->as.s.data;
    req.method_len = v->as.s.len;
    v = map_get_val(m, mino_keyword(S, "target"));
    if (v == NULL || mino_type_of(v) != MINO_STRING) {
        return prim_throw_classified(S, "eval/contract", "MCT001",
                                     "http-encode-request: :target must "
                                     "be a string");
    }
    req.target     = v->as.s.data;
    req.target_len = v->as.s.len;
    v = map_get_val(m, mino_keyword(S, "host"));
    if (v == NULL || mino_type_of(v) != MINO_STRING) {
        return prim_throw_classified(S, "eval/contract", "MCT001",
                                     "http-encode-request: :host must "
                                     "be a string");
    }
    req.host     = v->as.s.data;
    req.host_len = v->as.s.len;
    if (http_opt_bool(S, m, "http10?", &req.http10) != 0) return NULL;
    if (http_opt_bool(S, m, "chunked?", &req.chunked) != 0) return NULL;

    v = map_get_val(m, mino_keyword(S, "headers"));
    if (v != NULL && mino_type_of(v) != MINO_NIL) {
        size_t n, i;
        if (mino_type_of(v) == MINO_VECTOR) {
            n = v->as.vec.len;
            hdrs = n > 0 ? (http_hdr_in_t *)malloc(n * sizeof(*hdrs)) : NULL;
            for (i = 0; i < n; i++) {
                mino_val *entry = vec_nth(v, i);
                mino_val *k, *val;
                if (entry == NULL || mino_type_of(entry) != MINO_VECTOR
                    || entry->as.vec.len != 2) {
                    free(hdrs);
                    return prim_throw_classified(S, "eval/contract",
                        "MCT001",
                        "http-encode-request: :headers entries must be "
                        "[name value] pairs");
                }
                k   = vec_nth(entry, 0);
                val = vec_nth(entry, 1);
                if (!http_name_arg(k, &hdrs[i].name, &hdrs[i].name_len)
                    || val == NULL || mino_type_of(val) != MINO_STRING) {
                    free(hdrs);
                    return prim_throw_classified(S, "eval/contract",
                        "MCT001",
                        "http-encode-request: header names must be "
                        "strings or keywords and values strings");
                }
                hdrs[i].value     = val->as.s.data;
                hdrs[i].value_len = val->as.s.len;
            }
        } else if (mino_type_of(v) == MINO_MAP) {
            n = v->as.map.len;
            hdrs = n > 0 ? (http_hdr_in_t *)malloc(n * sizeof(*hdrs)) : NULL;
            for (i = 0; i < n; i++) {
                mino_val *k   = vec_nth(v->as.map.key_order, i);
                mino_val *val = vec_nth(v->as.map.val_order, i);
                if (!http_name_arg(k, &hdrs[i].name, &hdrs[i].name_len)
                    || val == NULL || mino_type_of(val) != MINO_STRING) {
                    free(hdrs);
                    return prim_throw_classified(S, "eval/contract",
                        "MCT001",
                        "http-encode-request: header names must be "
                        "strings or keywords and values strings");
                }
                hdrs[i].value     = val->as.s.data;
                hdrs[i].value_len = val->as.s.len;
            }
        } else {
            return prim_throw_classified(S, "eval/contract", "MCT001",
                                         "http-encode-request: :headers "
                                         "must be a vector of pairs or a "
                                         "map");
        }
        req.nheaders = n;
        req.headers  = hdrs;
    }

    v = map_get_val(m, mino_keyword(S, "body"));
    if (v != NULL && mino_type_of(v) != MINO_NIL) {
        if (!http_text_arg(v, &req.body, &req.body_len)) {
            free(hdrs);
            return prim_throw_classified(S, "eval/type", "MTY001",
                                         "http-encode-request: :body must "
                                         "be bytes or a string");
        }
    }

    if (http_encode_request(&req, &out, &out_len, err, sizeof(err)) != 0) {
        free(hdrs);
        return prim_throw_classified(S, "eval/contract", "MCT001", err);
    }
    free(hdrs);
    {
        mino_val *result = mino_bytes(S, out, out_len);
        free(out);
        return result;
    }
}

/* (http-encode-chunk data) -> one chunk frame; empty data is the
 * terminal chunk. */
static mino_val *prim_http_encode_chunk(mino_state *S, mino_val *args,
                                        mino_env *env)
{
    mino_val *v;
    const unsigned char *data;
    size_t len;
    unsigned char *out = NULL;
    size_t out_len = 0;
    mino_val *result;
    (void)env;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "http-encode-chunk requires one "
                                     "argument");
    }
    v = args->as.cons.car;
    if (!http_text_arg(v, &data, &len)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "http-encode-chunk: argument must "
                                     "be a string or bytes value");
    }
    if (len > (size_t)HTTP_CL_LIMIT) {
        return prim_throw_classified(S, "eval/contract", "MCT001",
                                     "http-encode-chunk: chunk is too "
                                     "large");
    }
    if (http_encode_chunk(data, len, &out, &out_len) != 0) {
        return prim_throw_classified(S, "internal", "MIN001",
                                     "http-encode-chunk: out of memory");
    }
    result = mino_bytes(S, out, out_len);
    free(out);
    return result;
}

/* (http-parse-response data opts?) -> map. :status is :need-more,
 * :done, or :error; a :done map carries :code :reason :http-version
 * :headers :body :chunked? :trailers. Re-feeds the given prefix
 * through a fresh parser each call. */
static mino_val *prim_http_parse_response(mino_state *S, mino_val *args,
                                          mino_env *env)
{
    mino_val *data_val, *opts = NULL;
    const unsigned char *data;
    size_t len;
    http_parse_opts_t o;
    mino_http_parser_t *p;
    (void)env;

    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "http-parse-response requires "
                                     "data");
    }
    data_val = args->as.cons.car;
    args = args->as.cons.cdr;
    if (mino_is_cons(args)) {
        opts = args->as.cons.car;
        if (mino_is_cons(args->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                                         "http-parse-response takes at "
                                         "most 2 arguments");
        }
    }
    if (!http_text_arg(data_val, &data, &len)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "http-parse-response: data must be "
                                     "a string or bytes value");
    }
    if (http_read_parse_opts(S, opts, &o) != 0) return NULL;
    p = http_parser_new(o.max_header_bytes, o.max_headers,
                        o.max_body_bytes);
    if (p == NULL) {
        return prim_throw_classified(S, "internal", "MIN001",
                                     "http: out of memory");
    }
    http_parser_feed(p, data, len);
    return http_parse_drive(S, &o, p);
}

/* (http-parse-response-chunks chunks opts?) -> same map, fed through
 * one parser across every buffer in the vector: the split-read
 * surface. */
static mino_val *prim_http_parse_response_chunks(mino_state *S,
                                                 mino_val *args,
                                                 mino_env *env)
{
    mino_val *vec, *opts = NULL;
    http_parse_opts_t o;
    mino_http_parser_t *p;
    size_t i;
    (void)env;

    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "http-parse-response-chunks requires "
                                     "a vector of buffers");
    }
    vec = args->as.cons.car;
    args = args->as.cons.cdr;
    if (mino_is_cons(args)) {
        opts = args->as.cons.car;
        if (mino_is_cons(args->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                                         "http-parse-response-chunks takes "
                                         "at most 2 arguments");
        }
    }
    if (vec == NULL || mino_type_of(vec) != MINO_VECTOR) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "http-parse-response-chunks: "
                                     "argument must be a vector");
    }
    if (http_read_parse_opts(S, opts, &o) != 0) return NULL;
    p = http_parser_new(o.max_header_bytes, o.max_headers,
                        o.max_body_bytes);
    if (p == NULL) {
        return prim_throw_classified(S, "internal", "MIN001",
                                     "http: out of memory");
    }
    for (i = 0; i < vec->as.vec.len; i++) {
        const unsigned char *data;
        size_t len;
        mino_val *el = vec_nth(vec, i);
        if (!http_text_arg(el, &data, &len)) {
            http_parser_free(p);
            return prim_throw_classified(S, "eval/type", "MTY001",
                                         "http-parse-response-chunks: "
                                         "every chunk must be a string or "
                                         "bytes value");
        }
        if (http_parser_feed(p, data, len) == HTTP_ERR) break;
    }
    return http_parse_drive(S, &o, p);
}

/* ---- redirect policy ---- */

/* (redirect-next request response opts?) decides one redirect hop.
 * Pure data in, data out: no sockets, no state, nothing but the two
 * maps and the parsed base URL. The response is untrusted; every
 * branch degrades to a :stop action rather than throwing, and the
 * request side is validated up front (contract errors there are the
 * caller's bug). */

#define HTTP_REDIRECT_MAX_URI 4096

static int http_ci_starts(const char *s, size_t len, const char *pfx,
                          size_t pfx_len)
{
    size_t i;
    if (len < pfx_len) return 0;
    for (i = 0; i < pfx_len; i++) {
        if (http_lower((unsigned char)s[i]) != (unsigned char)pfx[i])
            return 0;
    }
    return 1;
}

static int http_hdr_name_is(const mino_val *k, const char *want)
{
    const char *n;
    size_t len, i;
    if (!http_name_arg(k, &n, &len)) return 0;
    if (len != strlen(want)) return 0;
    for (i = 0; i < len; i++) {
        if (http_lower((unsigned char)n[i]) != (unsigned char)want[i])
            return 0;
    }
    return 1;
}

/* Response status: the mino.http shape (:status int) or the codec
 * shape (:code int). -1 when neither carries an integer. */
static int http_status_of(mino_state *S, const mino_val *resp)
{
    mino_val *v;
    long long n;
    v = map_get_val(resp, mino_keyword(S, "status"));
    if (v != NULL && as_long(v, &n) && n >= 100 && n <= 599)
        return (int)n;
    v = map_get_val(resp, mino_keyword(S, "code"));
    if (v != NULL && as_long(v, &n) && n >= 100 && n <= 599)
        return (int)n;
    return -1;
}

/* Location header out of a response :headers map or pair vector.
 * 1 = string found, 0 = absent, -1 = present but not a usable
 * string. The first Location wins when headers repeat. */
static int http_loc_header(mino_val *headers,
                           const char **out, size_t *out_len)
{
    size_t i;
    if (headers == NULL) return 0;
    if (mino_type_of(headers) == MINO_MAP) {
        size_t n = headers->as.map.len;
        for (i = 0; i < n; i++) {
            mino_val *k = vec_nth(headers->as.map.key_order, i);
            if (http_hdr_name_is(k, "location")) {
                mino_val *v = vec_nth(headers->as.map.val_order, i);
                if (v != NULL && mino_type_of(v) == MINO_STRING) {
                    *out = v->as.s.data;
                    *out_len = v->as.s.len;
                    return 1;
                }
                if (v != NULL && mino_type_of(v) == MINO_VECTOR
                    && v->as.vec.len > 0) {
                    mino_val *first = vec_nth(v, 0);
                    if (first != NULL
                        && mino_type_of(first) == MINO_STRING) {
                        *out = first->as.s.data;
                        *out_len = first->as.s.len;
                        return 1;
                    }
                }
                return -1;
            }
        }
        return 0;
    }
    if (mino_type_of(headers) == MINO_VECTOR) {
        size_t n = headers->as.vec.len;
        for (i = 0; i < n; i++) {
            mino_val *pair = vec_nth(headers, i);
            if (pair != NULL && mino_type_of(pair) == MINO_VECTOR
                && pair->as.vec.len == 2
                && http_hdr_name_is(vec_nth(pair, 0), "location")) {
                mino_val *v = vec_nth(pair, 1);
                if (v != NULL && mino_type_of(v) == MINO_STRING) {
                    *out = v->as.s.data;
                    *out_len = v->as.s.len;
                    return 1;
                }
                return -1;
            }
        }
        return 0;
    }
    return 0;
}

/* Validate an absolute http(s) URL text far enough that parse-url
 * cannot throw on it: known scheme, nonempty host, port digits in
 * range, and no space or control byte anywhere (the response is
 * untrusted; parse-url itself does not police spaces). 0 ok, -1 bad. */
static int http_abs_url_ok(const char *s, size_t len)
{
    size_t i, host_start;
    long long port;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c <= 0x20 || c == 0x7f) return -1;
    }
    if (http_ci_starts(s, len, "http://", 7)) {
        i = 7;
    } else if (http_ci_starts(s, len, "https://", 8)) {
        i = 8;
    } else {
        return -1;
    }
    host_start = i;
    if (i < len && s[i] == '[') {
        while (i < len && s[i] != ']') i++;
        if (i >= len) return -1;
        i++;
    } else {
        while (i < len && s[i] != ':' && s[i] != '/' && s[i] != '?'
               && s[i] != '#') {
            i++;
        }
    }
    if (i == host_start) return -1;
    if (i < len && s[i] == ':') {
        size_t digits = 0;
        i++;
        port = 0;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            port = port * 10 + (s[i] - '0');
            if (port > 65535) return -1;
            digits++;
            i++;
        }
        (void)digits;
        if (i < len && s[i] != '/' && s[i] != '?' && s[i] != '#')
            return -1;
    }
    return 0;
}

enum {
    HTTP_LOC_OK = 0,
    HTTP_LOC_EMPTY,
    HTTP_LOC_BAD
};

/* Append text to the assembly buffer; 0 ok, -1 past the cap. */
static int http_buf_put(char *buf, size_t cap, size_t *len,
                        const char *s, size_t n)
{
    if (*len > cap || n > cap - *len) return -1;
    memcpy(buf + *len, s, n);
    *len += n;
    return 0;
}

/* ":port" only when the port was spelled out and is not the scheme
 * default (canonical next-uri form). */
static int http_put_port(char *buf, size_t cap, size_t *len,
                         int is_https, int port)
{
    char num[16];
    int n;
    if (is_https ? port == 443 : port == 80) return 0;
    n = snprintf(num, sizeof(num), ":%d", port);
    return http_buf_put(buf, cap, len, num, (size_t)n);
}

/* Resolve a Location reference against the request URL's parts into
 * an absolute URL string (RFC 3986 merge, fragments dropped). */
static int http_loc_resolve(const char *loc, size_t loc_len,
                            int b_is_https, const char *b_host,
                            size_t b_host_len, int b_port,
                            const char *b_path, size_t b_path_len,
                            const char *b_query, size_t b_query_len,
                            char *out, size_t cap, size_t *out_len)
{
    const char *scheme = b_is_https ? "https://" : "http://";
    size_t scheme_len  = b_is_https ? 8 : 7;
    size_t i;
    size_t frag = loc_len;

    *out_len = 0;
    for (i = 0; i < loc_len; i++) {
        if (loc[i] == '#') { frag = i; break; }
    }
    /* A fragment-only reference targets the base URL itself (path
     * and query both carried over, RFC 3986 5.2.2). */
    if (frag == 0 && loc_len > 0) {
        if (http_buf_put(out, cap, out_len, scheme, scheme_len) != 0
            || http_buf_put(out, cap, out_len, b_host, b_host_len) != 0
            || http_put_port(out, cap, out_len, b_is_https, b_port) != 0
            || http_buf_put(out, cap, out_len, b_path, b_path_len) != 0
            || (b_query != NULL
                && (http_buf_put(out, cap, out_len, "?", 1) != 0
                    || http_buf_put(out, cap, out_len, b_query,
                                    b_query_len) != 0)))
            return HTTP_LOC_BAD;
        return HTTP_LOC_OK;
    }
    loc_len = frag;
    if (loc_len == 0) return HTTP_LOC_EMPTY;

    if (http_ci_starts(loc, loc_len, "http://", 7)
        || http_ci_starts(loc, loc_len, "https://", 8)) {
        if (http_abs_url_ok(loc, loc_len) != 0) return HTTP_LOC_BAD;
        if (http_buf_put(out, cap, out_len, loc, loc_len) != 0)
            return HTTP_LOC_BAD;
        return HTTP_LOC_OK;
    }
    if (loc_len >= 2 && loc[0] == '/' && loc[1] == '/') {
        if (http_buf_put(out, cap, out_len, scheme, scheme_len) != 0
            || http_buf_put(out, cap, out_len, loc + 2, loc_len - 2) != 0)
            return HTTP_LOC_BAD;
        if (http_abs_url_ok(out, *out_len) != 0) return HTTP_LOC_BAD;
        return HTTP_LOC_OK;
    }
    /* some other scheme ("ftp:..."): an http client cannot follow. */
    {
        size_t j = 0;
        if (loc_len > 0
            && ((loc[0] >= 'A' && loc[0] <= 'Z')
                || (loc[0] >= 'a' && loc[0] <= 'z'))) {
            j = 1;
            while (j < loc_len) {
                unsigned char c = (unsigned char)loc[j];
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                    || (c >= '0' && c <= '9') || c == '+' || c == '-'
                    || c == '.') {
                    j++;
                    continue;
                }
                break;
            }
            if (j < loc_len && loc[j] == ':' && j > 1)
                return HTTP_LOC_BAD;
        }
    }
    if (http_buf_put(out, cap, out_len, scheme, scheme_len) != 0
        || http_buf_put(out, cap, out_len, b_host, b_host_len) != 0)
        return HTTP_LOC_BAD;
    if (http_put_port(out, cap, out_len, b_is_https, b_port) != 0)
        return HTTP_LOC_BAD;
    if (loc[0] == '/') {
        if (http_buf_put(out, cap, out_len, loc, loc_len) != 0)
            return HTTP_LOC_BAD;
        return HTTP_LOC_OK;
    }
    if (loc[0] == '?') {
        if (http_buf_put(out, cap, out_len, b_path, b_path_len) != 0
            || http_buf_put(out, cap, out_len, loc, loc_len) != 0)
            return HTTP_LOC_BAD;
        return HTTP_LOC_OK;
    }
    /* relative segment: merge against the base path's directory */
    {
        size_t dir = b_path_len;
        while (dir > 0 && b_path[dir - 1] != '/') dir--;
        if (http_buf_put(out, cap, out_len, b_path, dir) != 0
            || http_buf_put(out, cap, out_len, loc, loc_len) != 0)
            return HTTP_LOC_BAD;
        return HTTP_LOC_OK;
    }
}

/* parse-url on a built string. The caller guarantees the text passed
 * http_abs_url_ok, so this cannot throw; the fresh args cons is read
 * before any allocation inside the callee (tls.c call-prim pattern). */
static mino_val *http_parse_url_str(mino_state *S, const char *url,
                                    size_t len)
{
    mino_val *uv = mino_string_n(S, url, len);
    mino_val *args;
    if (uv == NULL) return NULL;
    args = mino_cons(S, uv, mino_nil(S));
    return prim_parse_url(S, args, NULL);
}

static int http_part_str(mino_state *S, const mino_val *m, const char *key,
                         const char **out, size_t *out_len)
{
    mino_val *v = map_get_val(m, mino_keyword(S, key));
    if (v == NULL || mino_type_of(v) != MINO_STRING) return 0;
    *out = v->as.s.data;
    *out_len = v->as.s.len;
    return 1;
}

static int http_part_int(mino_state *S, const mino_val *m, const char *key,
                         long long *out)
{
    mino_val *v = map_get_val(m, mino_keyword(S, key));
    return v != NULL && as_long(v, out);
}

/* Copy the parts a redirect needs out of a parsed URL map so no GC
 * interior pointer is held across the result assembly. */
typedef struct {
    int         is_https;
    char        host[256];
    size_t      host_len;
    int         port;
    const char *path;
    size_t      path_len;
    const char *query;
    size_t      query_len;
} http_url_parts_t;

static int http_url_parts(mino_state *S, const mino_val *m,
                          http_url_parts_t *p)
{
    const char *scheme, *host;
    size_t scheme_len, host_len;
    long long port;
    if (!http_part_str(S, m, "scheme", &scheme, &scheme_len)
        || !http_part_str(S, m, "host", &host, &host_len)
        || host_len == 0 || host_len >= sizeof(p->host)
        || !http_part_int(S, m, "port", &port)
        || !http_part_str(S, m, "path", &p->path, &p->path_len)) {
        return 0;
    }
    p->is_https = scheme_len == 5 && memcmp(scheme, "https", 5) == 0;
    p->port     = (int)port;
    memcpy(p->host, host, host_len);
    p->host[host_len] = '\0';
    p->host_len = host_len;
    if (!http_part_str(S, m, "query", &p->query, &p->query_len)) {
        p->query = NULL;
        p->query_len = 0;
    }
    return 1;
}

/* Rebuild a headers map or pair vector without the dropped names.
 * Returns the original value when no drop applies. */
static mino_val *http_headers_prune(mino_state *S, mino_val *headers,
                                    int drop_auth, int drop_content)
{
    static const char *const k_auth[] = { "authorization", "cookie", NULL };
    static const char *const k_content[] = {
        "content-type", "content-length", "transfer-encoding", NULL
    };
    const char *const *sets[2];
    size_t i, j, kept;
    mino_val *result;

    if (headers == NULL) return headers;
    if (!drop_auth && !drop_content) return headers;
    sets[0] = drop_auth ? k_auth : NULL;
    sets[1] = drop_content ? k_content : NULL;
    if (mino_type_of(headers) == MINO_MAP) {
        mino_val **keys, **vals;
        size_t n = headers->as.map.len;
        keys = n > 0 ? (mino_val **)malloc(n * sizeof(*keys)) : NULL;
        vals = n > 0 ? (mino_val **)malloc(n * sizeof(*vals)) : NULL;
        if (n > 0 && (keys == NULL || vals == NULL)) {
            free(keys);
            free(vals);
            prim_throw_classified(S, "internal", "MIN001",
                                  "redirect-next: out of memory");
            return NULL;
        }
        mino_current_ctx(S)->gc_depth++;
        kept = 0;
        for (i = 0; i < n; i++) {
            mino_val *k = vec_nth(headers->as.map.key_order, i);
            int drop = 0;
            for (j = 0; j < 2 && !drop; j++) {
                size_t t = 0;
                if (sets[j] == NULL) continue;
                while (sets[j][t] != NULL) {
                    if (http_hdr_name_is(k, sets[j][t])) {
                        drop = 1;
                        break;
                    }
                    t++;
                }
            }
            if (drop) continue;
            keys[kept] = k;
            vals[kept] = vec_nth(headers->as.map.val_order, i);
            kept++;
        }
        result = mino_map(S, keys, vals, kept);
        mino_current_ctx(S)->gc_depth--;
        free(keys);
        free(vals);
        return result;
    }
    if (mino_type_of(headers) == MINO_VECTOR) {
        mino_val **els;
        size_t n = headers->as.vec.len;
        els = n > 0 ? (mino_val **)malloc(n * sizeof(*els)) : NULL;
        if (n > 0 && els == NULL) {
            prim_throw_classified(S, "internal", "MIN001",
                                  "redirect-next: out of memory");
            return NULL;
        }
        mino_current_ctx(S)->gc_depth++;
        kept = 0;
        for (i = 0; i < n; i++) {
            mino_val *pair = vec_nth(headers, i);
            int drop = 0;
            if (pair == NULL || mino_type_of(pair) != MINO_VECTOR
                || pair->as.vec.len != 2) {
                els[kept++] = pair;
                continue;
            }
            for (j = 0; j < 2 && !drop; j++) {
                size_t t = 0;
                if (sets[j] == NULL) continue;
                while (sets[j][t] != NULL) {
                    if (http_hdr_name_is(vec_nth(pair, 0), sets[j][t])) {
                        drop = 1;
                        break;
                    }
                    t++;
                }
            }
            if (drop) continue;
            els[kept++] = pair;
        }
        result = mino_vector(S, els, kept);
        mino_current_ctx(S)->gc_depth--;
        free(els);
        return result;
    }
    return headers;
}

static mino_val *http_stop_map(mino_state *S, const char *reason)
{
    mino_val *keys[2], *vals[2];
    mino_current_ctx(S)->gc_depth++;
    keys[0] = mino_keyword(S, "action");
    vals[0] = mino_keyword(S, "stop");
    keys[1] = mino_keyword(S, "reason");
    vals[1] = mino_keyword(S, reason);
    {
        mino_val *m = mino_map(S, keys, vals, 2);
        mino_current_ctx(S)->gc_depth--;
        return m;
    }
}

/* (redirect-next request response opts?) -> {:action :follow
 * :request next-request} | {:action :stop :reason ...}. */
static mino_val *prim_redirect_next(mino_state *S, mino_val *args,
                                    mino_env *env)
{
    mino_val *req, *resp, *opts = NULL;
    mino_val *method_val, *uri_val, *headers_val, *parsed;
    mino_val **keys, **vals, *result;
    const char *loc, *base_uri;
    size_t loc_len, base_uri_len;
    long long max_redirects, redirect_count;
    int status, follow = 1;
    int drop_body, rewrite_get, cross_host;
    http_url_parts_t base, target;
    char abs[HTTP_REDIRECT_MAX_URI];
    char next_uri[HTTP_REDIRECT_MAX_URI];
    size_t abs_len, next_len;
    size_t i, n, kept;
    (void)env;

    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "redirect-next requires a request "
                                     "map and a response map");
    }
    req = args->as.cons.car;
    resp = args->as.cons.cdr->as.cons.car;
    args = args->as.cons.cdr->as.cons.cdr;
    if (mino_is_cons(args)) {
        opts = args->as.cons.car;
        if (mino_is_cons(args->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                                         "redirect-next takes at most 3 "
                                         "arguments");
        }
    }
    if (req == NULL || mino_type_of(req) != MINO_MAP) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "redirect-next: request must be a "
                                     "map");
    }
    if (resp == NULL || mino_type_of(resp) != MINO_MAP) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "redirect-next: response must be a "
                                     "map");
    }
    if (opts != NULL && mino_type_of(opts) != MINO_MAP
        && mino_type_of(opts) != MINO_NIL) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "redirect-next: opts must be a map");
    }
    if (opts != NULL && mino_type_of(opts) == MINO_MAP) {
        mino_val *v = map_get_val(opts, mino_keyword(S,
                                                     "follow-redirects"));
        if (v != NULL && mino_type_of(v) != MINO_NIL) {
            if (mino_type_of(v) != MINO_BOOL) {
                return prim_throw_classified(S, "eval/contract", "MCT001",
                                             "redirect-next: opts key "
                                             ":follow-redirects must be "
                                             "a boolean");
            }
            follow = mino_val_bool_get(v);
        }
    }
    if (http_opt_long(S, opts, "max-redirects", 10, 0, 1000000,
                      &max_redirects) != 0)
        return NULL;
    if (http_opt_long(S, opts, "redirect-count", 0, 0, 1000000,
                      &redirect_count) != 0)
        return NULL;

    status = http_status_of(S, resp);
    if (status != 301 && status != 302 && status != 303
        && status != 307 && status != 308) {
        return http_stop_map(S, "not-redirect");
    }
    if (!follow) return http_stop_map(S, "disabled");
    if (redirect_count >= max_redirects)
        return http_stop_map(S, "max-redirects");

    method_val = map_get_val(req, mino_keyword(S, "method"));
    if (method_val == NULL
        || (mino_type_of(method_val) != MINO_KEYWORD
            && mino_type_of(method_val) != MINO_SYMBOL
            && mino_type_of(method_val) != MINO_STRING)) {
        return prim_throw_classified(S, "eval/contract", "MCT001",
                                     "redirect-next: request :method "
                                     "must be a keyword or string");
    }
    uri_val = map_get_val(req, mino_keyword(S, "uri"));
    if (uri_val == NULL) {
        uri_val = map_get_val(req, mino_keyword(S, "url"));
    }
    if (uri_val == NULL || mino_type_of(uri_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/contract", "MCT001",
                                     "redirect-next: request :uri must "
                                     "be a string");
    }
    base_uri = uri_val->as.s.data;
    base_uri_len = uri_val->as.s.len;
    parsed = http_parse_url_str(S, base_uri, base_uri_len);
    if (parsed == NULL) {
        return prim_throw_classified(S, "eval/contract", "MCT001",
                                     "redirect-next: request :uri is "
                                     "not a parseable http(s) URL");
    }
    /* parsed is pinned by gc suppression-free borrowing only until the
     * parts are copied; path/query are re-borrowed from it (still
     * reachable through the local parsed ref). */
    mino_current_ctx(S)->gc_depth++;
    if (!http_url_parts(S, parsed, &base)) {
        mino_current_ctx(S)->gc_depth--;
        return prim_throw_classified(S, "eval/contract", "MCT001",
                                     "redirect-next: request :uri is "
                                     "not a parseable http(s) URL");
    }

    {
        mino_val *hdrs = map_get_val(resp, mino_keyword(S, "headers"));
        int rc = http_loc_header(hdrs, &loc, &loc_len);
        if (rc == 0) {
            mino_current_ctx(S)->gc_depth--;
            return http_stop_map(S, "no-location");
        }
        if (rc < 0) {
            mino_current_ctx(S)->gc_depth--;
            return http_stop_map(S, "bad-location");
        }
    }
    {
        int rc = http_loc_resolve(loc, loc_len, base.is_https,
                                  base.host, base.host_len, base.port,
                                  base.path, base.path_len,
                                  base.query, base.query_len,
                                  abs, sizeof(abs), &abs_len);
        if (rc == HTTP_LOC_EMPTY) {
            mino_current_ctx(S)->gc_depth--;
            return http_stop_map(S, "no-location");
        }
        if (rc == HTTP_LOC_BAD) {
            mino_current_ctx(S)->gc_depth--;
            return http_stop_map(S, "bad-location");
        }
    }
    parsed = http_parse_url_str(S, abs, abs_len);
    if (parsed == NULL
        || !http_url_parts(S, parsed, &target)) {
        mino_current_ctx(S)->gc_depth--;
        return http_stop_map(S, "bad-location");
    }
    if (base.is_https && !target.is_https) {
        mino_current_ctx(S)->gc_depth--;
        return http_stop_map(S, "downgrade-blocked");
    }
    /* fetch-spec origin: scheme, host, and port as one unit. Any of
     * the three changing moves the credentials to a different origin
     * (http to https on the same host included), not just the host
     * text. */
    cross_host = target.is_https != base.is_https
        || target.port != base.port
        || target.host_len != base.host_len
        || memcmp(target.host, base.host, base.host_len) != 0;

    /* Method and body policy. 301/302 rewriting non-GET to GET is the
     * browser-compatible divergence from strict RFC 7231 (which
     * preserves the method); documented in the docstring. */
    drop_body   = 0;
    rewrite_get = 0;
    if (status == 303) {
        drop_body   = 1;
        rewrite_get = 1;
    } else if (status == 301 || status == 302) {
        const char *m;
        size_t mlen;
        if (http_name_arg(method_val, &m, &mlen)
            && ((mlen == 3
                 && http_ci_starts(m, mlen, "get", 3))
                || (mlen == 4
                    && http_ci_starts(m, mlen, "head", 4)))) {
            /* GET and HEAD keep their method (and any body they had) */
        } else {
            drop_body   = 1;
            rewrite_get = 1;
        }
    }

    /* Canonical next :uri: scheme://host[:port]path[?query]. */
    next_len = 0;
    if (http_buf_put(next_uri, sizeof(next_uri), &next_len,
                     target.is_https ? "https://" : "http://",
                     target.is_https ? 8 : 7) != 0
        || http_buf_put(next_uri, sizeof(next_uri), &next_len,
                        target.host, target.host_len) != 0
        || http_put_port(next_uri, sizeof(next_uri), &next_len,
                         target.is_https, target.port) != 0
        || http_buf_put(next_uri, sizeof(next_uri), &next_len,
                        target.path, target.path_len) != 0
        || (target.query != NULL
            && (http_buf_put(next_uri, sizeof(next_uri), &next_len,
                             "?", 1) != 0
                || http_buf_put(next_uri, sizeof(next_uri), &next_len,
                                target.query, target.query_len) != 0))) {
        mino_current_ctx(S)->gc_depth--;
        return http_stop_map(S, "bad-location");
    }

    headers_val = map_get_val(req, mino_keyword(S, "headers"));
    if (headers_val == NULL) headers_val = mino_nil(S);
    if (cross_host || drop_body) {
        mino_val *pruned = http_headers_prune(S, headers_val,
                                              cross_host, drop_body);
        if (pruned == NULL) {
            mino_current_ctx(S)->gc_depth--;
            return NULL;
        }
        headers_val = pruned;
    }

    /* Assemble the next request: every original key travels except
     * :url (the stale alias), with :method/:headers/:body/:uri
     * replaced per policy. GC stays suppressed while the arrays hold
     * the fresh uri string and rebuilt headers (map_assoc_pairs
     * precedent). */
    n = req->as.map.len;
    keys = (mino_val **)malloc((n + 1) * sizeof(*keys));
    vals = (mino_val **)malloc((n + 1) * sizeof(*vals));
    if (keys == NULL || vals == NULL) {
        free(keys);
        free(vals);
        mino_current_ctx(S)->gc_depth--;
        return prim_throw_classified(S, "internal", "MIN001",
                                     "redirect-next: out of memory");
    }
    kept = 0;
    for (i = 0; i < n; i++) {
        mino_val *k = vec_nth(req->as.map.key_order, i);
        mino_val *v = vec_nth(req->as.map.val_order, i);
        if (k == mino_keyword(S, "method")) {
            if (rewrite_get) {
                keys[kept] = k;
                vals[kept] = mino_keyword(S, "get");
                kept++;
            } else {
                keys[kept] = k;
                vals[kept] = v;
                kept++;
            }
            continue;
        }
        if (k == mino_keyword(S, "uri") || k == mino_keyword(S, "url"))
            continue;
        if (k == mino_keyword(S, "body")) {
            if (drop_body) continue;
            keys[kept] = k;
            vals[kept] = v;
            kept++;
            continue;
        }
        if (k == mino_keyword(S, "headers")) {
            keys[kept] = k;
            vals[kept] = headers_val;
            kept++;
            continue;
        }
        keys[kept] = k;
        vals[kept] = v;
        kept++;
    }
    keys[kept] = mino_keyword(S, "uri");
    vals[kept] = mino_string_n(S, next_uri, next_len);
    kept++;
    {
        mino_val *rkeys[2], *rvals[2];
        mino_val *next_req = mino_map(S, keys, vals, kept);
        rkeys[0] = mino_keyword(S, "action");
        rvals[0] = mino_keyword(S, "follow");
        rkeys[1] = mino_keyword(S, "request");
        rvals[1] = next_req;
        result = mino_map(S, rkeys, rvals, 2);
    }
    mino_current_ctx(S)->gc_depth--;
    free(keys);
    free(vals);
    return result;
}

const mino_prim_def k_prims_http[] = {
    {"http-encode-request", prim_http_encode_request,
     "Serializes an HTTP request to bytes from a plain map: :method "
     ":target :host are required strings; :headers is a vector of "
     "[name value] pairs or a map (string or keyword names); :body is "
     "bytes or a string (emits Content-Length); :chunked? true emits "
     "Transfer-Encoding: chunked with no body (drive the frames with "
     "http-encode-chunk); :http10? true selects the HTTP/1.0 request "
     "line. Host, Content-Length, and Transfer-Encoding are computed "
     "by this layer and must not appear in :headers. Header names "
     "must be RFC 7230 tokens; values with CR, LF, or NUL throw. "
     "Returns bytes."},
    {"http-encode-chunk", prim_http_encode_chunk,
     "Encodes one HTTP chunked frame from a string or bytes value: "
     "hex size, CRLF, the payload, CRLF. An empty input is the "
     "terminal chunk (\"0\\r\\n\\r\\n\"). Returns bytes."},
    {"http-parse-response", prim_http_parse_response,
     "Parses a prefix of an HTTP response from a string or bytes "
     "value and returns {:status :need-more | :done | :error}. A "
     ":done map carries :code :reason :http-version :headers :body "
     ":chunked? :trailers; :error adds :error, a reason string. "
      "Header names are lowercased and repeats collect into vectors "
      "in order. 1xx responses are skipped (max 5); 204 and 304 are "
      "bodiless; obs-fold, both Content-Length and Transfer-Encoding, "
      "conflicting Content-Length values, and control bytes other than "
      "HTAB in field values are rejected. "
     "Opts: :eof true ends a close-delimited body, :max-header-bytes "
     "(default 65536), :max-headers (default 100), :max-body-bytes "
     "(default 16777216). Each call parses its whole input fresh."},
    {"http-parse-response-chunks", prim_http_parse_response_chunks,
     "Parses an HTTP response from a vector of string or bytes "
     "buffers fed through one parser in order, and returns the same "
     "shape as http-parse-response. Arbitrary read splits give the "
     "same result as a single feed; opts are shared, with :eof true "
     "signalling end of stream."},
    {"redirect-next", prim_redirect_next,
     "Decides one redirect hop from a request map, a response map, "
     "and opts. Returns {:action :follow :request next-request} or "
     "{:action :stop :reason k} with k one of :not-redirect "
     ":disabled :max-redirects :no-location :bad-location "
     ":downgrade-blocked. Follows only 301 302 303 307 308 with a "
     "Location; 303 always becomes GET with the body and its content "
     "headers dropped; 301 and 302 rewrite non-GET/HEAD methods to "
     "GET (browser-compatible, a deliberate divergence from strict "
     "RFC 7231 method preservation); 307 and 308 preserve method, "
      "body, and headers. Relative Locations resolve against the "
      "request :uri per RFC 3986 (fragments dropped, default ports "
      "normalized away). Authorization and Cookie headers are stripped "
      "when the redirect changes origin (scheme, host, or port; "
      "fetch-spec origin); https to http is blocked while http to "
      "https is allowed. Opts: :follow-redirects (boolean, default "
     "true), :max-redirects (default 10), :redirect-count (hops "
     "already followed, default 0; the policy stops when it reaches "
     ":max-redirects). Pure data: no sockets, no capability."},
};

const size_t k_prims_http_count =
    sizeof(k_prims_http) / sizeof(k_prims_http[0]);
