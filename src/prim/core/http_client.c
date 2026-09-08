/*
 * http_client.c -- the HTTP/1.1 network client: http-request driven end
 * to end over a live socket. One C execution path over the codec layers
 * in http_codec.c: pool-checkout for a live keep-alive socket,
 * net-connect (plus tls-connect for https) on a miss, encode, send,
 * receive into the response parser, redirect hops through the
 * redirect-next policy, gzip/deflate decode of the final body. The prim
 * takes the already-normalized parts map (mino.http normalizes) and
 * validates it; lower-layer errors (:net/:tls/:codec) pass through
 * unchanged. The codec parser, encoder, and URL helpers this reuses are
 * declared in http_internal.h.
 *
 * Rooting: send and recv yield the state lock, so a sibling worker's
 * allocation can collect while this loop holds only C locals. Every
 * value referenced across such a park window or an allocation point
 * rides in a mino_root root (the pool.c entry pattern): the request,
 * the current hop's request, the socket handle, the headers map, the
 * final body, each trace URI. gc_pin is reserved for short balanced
 * scopes around nested prim calls, so the LIFO save stack never
 * carries a value across a hop and stays a small constant deep.
 */

#include "prim/internal.h"
#include "mino.h"
#include "prim/core/http_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define HTTPREQ_MAX_HOPS    32
#define HTTPREQ_READ_CHUNK  16384
#define HTTPREQ_HOST_CAP    256
#define HTTPREQ_METHOD_MAX  64
#define HTTPREQ_DEFAULT_KEEPALIVE_MS 120000LL
/* Mirror the net prim defaults; net.c owns the originals. */
#define HTTPREQ_CONNECT_MS  10000LL
#define HTTPREQ_READ_MS     30000LL
#define HTTPREQ_WRITE_MS    30000LL

typedef struct {
    int         is_https;
    const char *method;      /* borrowed from the current request map */
    size_t      method_len;
    const char *host;
    size_t      host_len;
    const char *target;
    size_t      target_len;
    long long   port;
    mino_val   *headers_val; /* vector of pairs or a map; NULL: none */
    mino_val   *body_val;    /* string or bytes; NULL: none */
    long long   keepalive;
    long long   connect_ms;
    long long   read_ms;
    long long   write_ms;
    int         follow;
    int         insecure;
    int         decompress;
    long long   max_redirects;
    long long   max_bytes;
} httpreq_parts_t;

static int httpreq_bool_default(mino_state *S, const mino_val *m,
                                const char *key, int def, int *out);

static int httpreq_headers_array(mino_state *S, mino_val *headers,
                                 size_t extra,
                                 http_hdr_in_t **out, size_t *n_out);

static int httpreq_bool_default(mino_state *S, const mino_val *m,
                                const char *key, int def, int *out)
{
    mino_val *v;
    *out = def;
    if (m == NULL || mino_type_of(m) != MINO_MAP) return 0;
    v = map_get_val(m, mino_keyword(S, key));
    if (v == NULL || mino_type_of(v) == MINO_NIL) return 0;
    if (mino_type_of(v) != MINO_BOOL) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "http-request: :%s must be a boolean", key);
        throw_classified(S, "http/request", "MHR003", msg);
        return -1;
    }
    *out = mino_val_bool_get(v);
    return 0;
}

/* Read and validate the parts of one request map into C fields. The
 * borrowed pointers stay valid while the owning map stays ref-rooted
 * (the collector never moves values); the loop re-reads at every
 * hop. */
static int httpreq_read(mino_state *S, mino_val *m, httpreq_parts_t *p)
{
    mino_val *v;
    size_t i;

    memset(p, 0, sizeof(*p));
    p->follow     = 1;
    p->decompress = 1;

    v = map_get_val(m, mino_keyword(S, "method"));
    if (v == NULL || mino_type_of(v) != MINO_STRING
        || v->as.s.len == 0 || v->as.s.len > HTTPREQ_METHOD_MAX) {
        throw_classified(S, "http/method", "MHR001",
                              "http-request: :method must be a token "
                              "string");
        return -1;
    }
    for (i = 0; i < v->as.s.len; i++) {
        if (!http_tchar((unsigned char)v->as.s.data[i])) {
            throw_classified(S, "http/method", "MHR001",
                                  "http-request: :method must be a token "
                                  "string");
            return -1;
        }
    }
    p->method     = v->as.s.data;
    p->method_len = v->as.s.len;

    v = map_get_val(m, mino_keyword(S, "scheme"));
    {
        const char *s;
        size_t slen;
        if (http_name_arg(v, &s, &slen) && slen == 4
            && memcmp(s, "http", 4) == 0) {
            p->is_https = 0;
        } else if (http_name_arg(v, &s, &slen) && slen == 5
                   && memcmp(s, "https", 5) == 0) {
            p->is_https = 1;
        } else {
            throw_classified(S, "http/request", "MHR003",
                                  "http-request: :scheme must be :http "
                                  "or :https");
            return -1;
        }
    }

    v = map_get_val(m, mino_keyword(S, "host"));
    if (v == NULL || mino_type_of(v) != MINO_STRING
        || v->as.s.len == 0 || v->as.s.len >= HTTPREQ_HOST_CAP) {
        throw_classified(S, "http/request", "MHR003",
                              "http-request: :host must be a non-empty "
                              "string under 256 bytes");
        return -1;
    }
    p->host     = v->as.s.data;
    p->host_len = v->as.s.len;

    v = map_get_val(m, mino_keyword(S, "port"));
    if (!as_long(v, &p->port) || p->port < 1 || p->port > 65535) {
        throw_classified(S, "http/request", "MHR003",
                              "http-request: :port must be an integer in "
                              "1..65535");
        return -1;
    }

    v = map_get_val(m, mino_keyword(S, "target"));
    if (v == NULL || mino_type_of(v) != MINO_STRING
        || !http_valid_target(v->as.s.data, v->as.s.len)) {
        throw_classified(S, "http/request", "MHR003",
                              "http-request: :target must be a non-empty "
                              "string without spaces or control bytes");
        return -1;
    }
    p->target     = v->as.s.data;
    p->target_len = v->as.s.len;

    p->headers_val = map_get_val(m, mino_keyword(S, "headers"));
    if (p->headers_val != NULL
        && mino_type_of(p->headers_val) == MINO_NIL) {
        p->headers_val = NULL;
    }
    if (p->headers_val != NULL
        && mino_type_of(p->headers_val) != MINO_VECTOR
        && mino_type_of(p->headers_val) != MINO_MAP) {
        throw_classified(S, "http/headers", "MHR002",
                              "http-request: :headers must be a vector "
                              "of [name value] pairs or a map");
        return -1;
    }
    /* Shape and forbidden-name validation happens here, before any
     * connection is opened: a bad header map must never touch the
     * network. */
    if (p->headers_val != NULL) {
        http_hdr_in_t *hdrs;
        size_t nhdrs;
        if (httpreq_headers_array(S, p->headers_val, 0, &hdrs, &nhdrs)
            != 0)
            return -1;
        free(hdrs);
    }

    p->body_val = map_get_val(m, mino_keyword(S, "body"));
    if (p->body_val != NULL && mino_type_of(p->body_val) == MINO_NIL) {
        p->body_val = NULL;
    }
    if (p->body_val != NULL
        && mino_type_of(p->body_val) != MINO_STRING
        && !mino_is_bytes(p->body_val)) {
        throw_classified(S, "http/request", "MHR003",
                              "http-request: :body must be a string, "
                              "bytes, or nil");
        return -1;
    }

    v = map_get_val(m, mino_keyword(S, "keepalive"));
    p->keepalive = HTTPREQ_DEFAULT_KEEPALIVE_MS;
    if (v != NULL && mino_type_of(v) != MINO_NIL && !as_long(v,
                                                             &p->keepalive)) {
        throw_classified(S, "http/request", "MHR003",
                              "http-request: :keepalive must be an "
                              "integer");
        return -1;
    }
    if (net_opt_ms(S, m, "connect-timeout", HTTPREQ_CONNECT_MS,
                   &p->connect_ms) != 0)
        return -1;
    if (net_opt_ms(S, m, "read-timeout", HTTPREQ_READ_MS, &p->read_ms)
        != 0)
        return -1;
    if (net_opt_ms(S, m, "write-timeout", HTTPREQ_WRITE_MS, &p->write_ms)
        != 0)
        return -1;
    if (httpreq_bool_default(S, m, "follow-redirects", 1, &p->follow) != 0)
        return -1;
    if (httpreq_bool_default(S, m, "decompress-body?", 1,
                             &p->decompress) != 0)
        return -1;
    if (httpreq_bool_default(S, m, "insecure?", 0, &p->insecure) != 0)
        return -1;
    if (http_opt_long(S, m, "max-redirects", 10, 0, 1000000,
                      &p->max_redirects) != 0)
        return -1;
    if (http_opt_long(S, m, "max-bytes", HTTP_DEFAULT_MAX_BODY_BYTES, 0,
                      HTTP_CL_LIMIT, &p->max_bytes) != 0)
        return -1;
    return 0;
}

/* One header row out of a name and value pair, with the layer-owned
 * names rejected here so the caller sees :http/headers, not the
 * encoder's contract text. Pure classifier: fills err and returns -1
 * rather than throwing, because a throw inside a try block longjmps
 * past the caller's cleanup of the malloc'd header array. */
static int httpreq_header_row(mino_val *k, mino_val *val,
                              http_hdr_in_t *out, char *err, size_t err_cap)
{
    if (!http_name_arg(k, &out->name, &out->name_len)
        || val == NULL || mino_type_of(val) != MINO_STRING) {
        snprintf(err, err_cap, "http-request: header names must be "
                 "strings or keywords and values strings");
        return -1;
    }
    out->value     = val->as.s.data;
    out->value_len = val->as.s.len;
    if (out->name_len == 0) {
        snprintf(err, err_cap, "http-request: header name is empty");
        return -1;
    }
    if (http_name_in(k_request_owned, out->name, out->name_len)) {
        snprintf(err, err_cap, "http-request: header %.*s is computed "
                 "by the request layer",
                 (int)(out->name_len < 48 ? out->name_len : 48),
                 out->name);
        return -1;
    }
    return 0;
}

/* Headers value into a malloc'd encoder array (caller frees), with
 * room for one injected Connection header. Row rejections free the
 * array BEFORE throwing: throw_classified longjmps to the
 * nearest try frame, so any throw with the array live leaks it. */
static int httpreq_headers_array(mino_state *S, mino_val *headers,
                                 size_t extra,
                                 http_hdr_in_t **out, size_t *n_out)
{
    http_hdr_in_t *hdrs;
    char msg[128];
    size_t n = 0, i;

    if (headers == NULL) {
        *out  = extra > 0
            ? (http_hdr_in_t *)malloc(extra * sizeof(*hdrs)) : NULL;
        *n_out = 0;
        if (extra > 0 && *out == NULL) goto oom;
        return 0;
    }
    if (mino_type_of(headers) == MINO_VECTOR) {
        n = headers->as.vec.len;
        hdrs = (http_hdr_in_t *)malloc((n + extra) * sizeof(*hdrs));
        if (hdrs == NULL) goto oom;
        for (i = 0; i < n; i++) {
            mino_val *entry = vec_nth(headers, i);
            if (entry == NULL || mino_type_of(entry) != MINO_VECTOR
                || entry->as.vec.len != 2) {
                free(hdrs);
                return -1;
            }
            if (httpreq_header_row(vec_nth(entry, 0), vec_nth(entry, 1),
                                   &hdrs[i], msg, sizeof(msg)) != 0) {
                free(hdrs);
                throw_classified(S, "http/headers", "MHR002", msg);
                return -1;
            }
        }
    } else {
        n = headers->as.map.len;
        hdrs = (http_hdr_in_t *)malloc((n + extra) * sizeof(*hdrs));
        if (hdrs == NULL) goto oom;
        for (i = 0; i < n; i++) {
            mino_val *k = vec_nth(headers->as.map.key_order, i);
            if (httpreq_header_row(k, map_get_val(headers, k),
                                   &hdrs[i], msg, sizeof(msg)) != 0) {
                free(hdrs);
                throw_classified(S, "http/headers", "MHR002", msg);
                return -1;
            }
        }
    }
    *out  = hdrs;
    *n_out = n;
    return 0;

oom:
    throw_classified(S, "internal", "MIN001",
                          "http-request: out of memory");
    return -1;
}

static int httpreq_handle_is_tls(mino_val *v)
{
    return v != NULL && mino_type_of(v) == MINO_HANDLE
        && v->as.handle.tag != NULL
        && strcmp(v->as.handle.tag, mino_tls_sock_tag()) == 0;
}

static void httpreq_close_handle(mino_val *v)
{
    if (httpreq_handle_is_tls(v)) mino_tls_handle_close(v);
    else mino_net_handle_close(v);
}

/* Comma-separated token membership in the Connection header values. */
static int httpreq_conn_token(const mino_http_parser_t *p,
                              const char *want)
{
    size_t want_len = strlen(want);
    size_t end = p->trailer_start == (size_t)-1
        ? p->nrows : p->trailer_start;
    size_t i;
    for (i = 0; i < end; i++) {
        const unsigned char *v = p->hdr + p->rows[i].val_off;
        size_t vlen = p->rows[i].val_len;
        size_t start = 0, j;
        if (!http_name_is(p->hdr + p->rows[i].name_off,
                          p->rows[i].name_len, "connection", 10))
            continue;
        for (j = 0; j <= vlen; j++) {
            if (j == vlen || v[j] == ',') {
                size_t s = start, e = j, k;
                while (s < e && http_ows(v[s])) s++;
                while (e > s && http_ows(v[e - 1])) e--;
                if (e - s == want_len) {
                    for (k = 0; k < e - s; k++) {
                        if (http_lower(v[s + k]) != (unsigned char)want[k])
                            break;
                    }
                    if (k == e - s) return 1;
                }
                start = j + 1;
            }
        }
    }
    return 0;
}

/* A response leaves its socket reusable only when the framing did not
 * end at EOF, the peer did not ask to close, a 1.0 peer opted into
 * keep-alive, and no bytes trail the message (a trailing byte is a
 * desync the next request would read as its response). */
static int httpreq_reusable(const mino_http_parser_t *p,
                            long long keepalive)
{
    if (keepalive <= 0) return 0;
    if (p->framing == HTTP_FR_CLOSE) return 0;
    if (httpreq_conn_token(p, "close")) return 0;
    if (p->http10 && !httpreq_conn_token(p, "keep-alive")) return 0;
    if (p->buf_len > p->pos) return 0;
    return 1;
}

static void httpreq_dispose(mino_state *S, mino_val *sock,
                            const httpreq_parts_t *p,
                            const mino_http_parser_t *parser)
{
    if (httpreq_reusable(parser, p->keepalive)) {
        (void)mino_net_pool_return(S, p->host, p->host_len,
                                   (int)p->port, p->is_https, p->insecure,
                                   sock, p->keepalive);
    } else {
        httpreq_close_handle(sock);
    }
}

/* Absolute request URL from the parts (the redirect policy's input
 * and the trace-redirects form). */
static int httpreq_uri(const httpreq_parts_t *p, char *buf, size_t cap,
                       size_t *len_out)
{
    size_t len = 0;
    if (http_buf_put(buf, cap, &len, p->is_https ? "https://" : "http://",
                     p->is_https ? 8 : 7) != 0
        || http_buf_put(buf, cap, &len, p->host, p->host_len) != 0
        || http_put_port(buf, cap, &len, p->is_https, (int)p->port) != 0
        || http_buf_put(buf, cap, &len, p->target, p->target_len) != 0)
        return -1;
    *len_out = len;
    return 0;
}

/* Request map carrying every original key plus a canonical :uri
 * (replacing a stale one); the redirect policy requires :uri. */
static mino_val *httpreq_with_uri(mino_state *S, mino_val *m,
                                  const char *uri, size_t uri_len)
{
    mino_val **keys, **vals, *result;
    size_t n = m->as.map.len, kept = 0, i;
    keys = (mino_val **)malloc((n + 1) * sizeof(*keys));
    vals = (mino_val **)malloc((n + 1) * sizeof(*vals));
    if (keys == NULL || vals == NULL) {
        free(keys);
        free(vals);
        throw_classified(S, "internal", "MIN001",
                              "http-request: out of memory");
        return NULL;
    }
    mino_current_ctx(S)->gc_depth++;
    for (i = 0; i < n; i++) {
        mino_val *k = vec_nth(m->as.map.key_order, i);
        if (k == mino_keyword(S, "uri") || k == mino_keyword(S, "url"))
            continue;
        keys[kept] = k;
        vals[kept] = map_get_val(m, k);
        kept++;
    }
    keys[kept] = mino_keyword(S, "uri");
    vals[kept] = mino_string_n(S, uri, uri_len);
    kept++;
    result = mino_map(S, keys, vals, kept);
    mino_current_ctx(S)->gc_depth--;
    free(keys);
    free(vals);
    return result;
}

/* Translate a redirect-next follow request (ns shape, :uri) back into
 * the parts shape (:scheme :host :port :target, method uppercased).
 * Every other key travels. Returns 0 with *out set. */
static int httpreq_translate(mino_state *S, mino_val *next,
                             mino_val **out)
{
    mino_val *uri_val, *parsed, *result;
    mino_val **keys, **vals;
    http_url_parts_t up;
    char mbuf[HTTPREQ_METHOD_MAX + 1];
    const char *mtext;
    size_t mlen, tlen, n, kept = 0, i;
    int r = -1;

    if (next == NULL) {
        throw_classified(S, "http/request", "MHR003",
                              "http-request: redirect follow decision "
                              "has no :request");
        return -1;
    }
    uri_val = map_get_val(next, mino_keyword(S, "uri"));
    if (uri_val == NULL || mino_type_of(uri_val) != MINO_STRING) {
        throw_classified(S, "http/request", "MHR003",
                              "http-request: redirect target has no "
                              ":uri");
        return -1;
    }
    parsed = http_parse_url_str(S, uri_val->as.s.data,
                                uri_val->as.s.len);
    if (parsed == NULL) return -1;
    gc_pin(parsed);
    if (!http_url_parts(S, parsed, &up)) {
        throw_classified(S, "http/request", "MHR003",
                              "http-request: redirect target is not an "
                              "http(s) URL");
        goto unpin;
    }

    keys = (mino_val **)malloc((next->as.map.len + 8) * sizeof(*keys));
    vals = (mino_val **)malloc((next->as.map.len + 8) * sizeof(*vals));
    if (keys == NULL || vals == NULL) {
        free(keys);
        free(vals);
        throw_classified(S, "internal", "MIN001",
                              "http-request: out of memory");
        goto unpin;
    }
    mino_current_ctx(S)->gc_depth++;
    {
        mino_val *mv = map_get_val(next, mino_keyword(S, "method"));
        char *tbuf;
        if (!http_name_arg(mv, &mtext, &mlen) || mlen > sizeof mbuf - 1) {
            mino_current_ctx(S)->gc_depth--;
            free(keys);
            free(vals);
            throw_classified(S, "http/request", "MHR003",
                                  "http-request: redirect target has no "
                                  "usable :method");
            goto unpin;
        }
        for (i = 0; i < mlen; i++) {
            mbuf[i] = (char)http_upper((unsigned char)mtext[i]);
        }
        tlen = up.path_len + (up.query != NULL ? 1 + up.query_len : 0);
        tbuf = (char *)malloc(tlen + 1);
        if (tbuf == NULL) {
            mino_current_ctx(S)->gc_depth--;
            free(keys);
            free(vals);
            throw_classified(S, "internal", "MIN001",
                                  "http-request: out of memory");
            goto unpin;
        }
        memcpy(tbuf, up.path, up.path_len);
        if (up.query != NULL) {
            tbuf[up.path_len] = '?';
            memcpy(tbuf + up.path_len + 1, up.query, up.query_len);
        }
        n = next->as.map.len;
        for (i = 0; i < n; i++) {
            mino_val *k = vec_nth(next->as.map.key_order, i);
            if (k == mino_keyword(S, "uri") || k == mino_keyword(S, "url")
                || k == mino_keyword(S, "scheme")
                || k == mino_keyword(S, "host")
                || k == mino_keyword(S, "port")
                || k == mino_keyword(S, "target")
                || k == mino_keyword(S, "method")
                || k == mino_keyword(S, "headers")
                || k == mino_keyword(S, "body"))
                continue;
            keys[kept] = k;
            vals[kept] = map_get_val(next, k);
            kept++;
        }
        keys[kept] = mino_keyword(S, "scheme");
        vals[kept] = mino_keyword(S, up.is_https ? "https" : "http");
        kept++;
        keys[kept] = mino_keyword(S, "host");
        vals[kept] = mino_string_n(S, up.host, up.host_len);
        kept++;
        keys[kept] = mino_keyword(S, "port");
        vals[kept] = mino_int(S, up.port);
        kept++;
        keys[kept] = mino_keyword(S, "target");
        vals[kept] = mino_string_n(S, tbuf, tlen);
        kept++;
        keys[kept] = mino_keyword(S, "method");
        vals[kept] = mino_string_n(S, mbuf, mlen);
        kept++;
        keys[kept] = mino_keyword(S, "headers");
        {
            mino_val *hv = map_get_val(next, mino_keyword(S, "headers"));
            vals[kept] = hv != NULL ? hv : mino_nil(S);
        }
        kept++;
        keys[kept] = mino_keyword(S, "body");
        {
            mino_val *bv = map_get_val(next, mino_keyword(S, "body"));
            vals[kept] = bv != NULL ? bv : mino_nil(S);
        }
        kept++;
        result = mino_map(S, keys, vals, kept);
        mino_current_ctx(S)->gc_depth--;
        free(keys);
        free(vals);
        free(tbuf);
        *out = result;
        r = 0;
    }

unpin:
    gc_unpin(1);
    return r;
}

/* Append one redirect URI to the trace as its own root; the values
 * outlive every hop and feed the final :trace-redirects vector. 0 ok,
 * -1 on allocation failure. */
static int httpreq_trace_push(mino_state *S, mino_val *uri,
                              mino_root ***refs, size_t *len,
                              size_t *cap)
{
    mino_root *r;
    if (*len == *cap) {
        size_t nc = *cap > 0 ? *cap * 2 : 8;
        mino_root **nr;
        if (nc > SIZE_MAX / sizeof(*nr)) return -1;
        nr = (mino_root **)realloc(*refs, nc * sizeof(*nr));
        if (nr == NULL) return -1;
        *refs = nr;
        *cap  = nc;
    }
    r = mino_root_new(S, uri);
    if (r == NULL) return -1;
    (*refs)[(*len)++] = r;
    return 0;
}

/* (http-request m) -> response map. See the table entry below. */
static mino_val *prim_http_request(mino_state *S, mino_val *args,
                                   mino_env *env)
{
    mino_val *req, *cur, *res = NULL;
    httpreq_parts_t parts;
    mino_val *sock = NULL;
    mino_root *req_ref = NULL, *cur_ref = NULL, *sock_ref = NULL;
    mino_root *hmap_ref = NULL, *body_ref = NULL;
    mino_root **trace_refs = NULL;
    size_t trace_len = 0, trace_cap = 0;
    unsigned char *wire = NULL;
    size_t wire_len = 0;
    mino_http_parser_t *parser = NULL;
    long long t0;
    size_t hop;
    const char *kind = "net", *code = "MNE004";
    char msg[240];

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return throw_classified(S, "eval/arity", "MAR001",
                                     "http-request requires one request "
                                     "map");
    }
    req = args->as.cons.car;
    if (req == NULL || mino_type_of(req) != MINO_MAP) {
        return throw_classified(S, "eval/type", "MTY001",
                                     "http-request: argument must be a "
                                     "request map");
    }
    if (httpreq_read(S, req, &parts) != 0) return NULL;

    t0 = mino_monotonic_ns();
    req_ref = mino_root_new(S, req);
    cur_ref = mino_root_new(S, req);
    if (req_ref == NULL || cur_ref == NULL) {
        mino_unroot(S, req_ref);
        mino_unroot(S, cur_ref);
        return throw_classified(S, "internal", "MIN001",
                                     "http-request: out of memory");
    }
    cur = req;

    for (hop = 0;; hop++) {
        mino_val *hmap = NULL;
        unsigned char chunk[HTTPREQ_READ_CHUNK];
        char hostport[HTTPREQ_HOST_CAP + 8];
        char uribuf[HTTP_REDIRECT_MAX_URI];
        size_t hp_len;
        int from_pool;
        int is_tls_sock;
        uintptr_t fd = 0;

        if (httpreq_read(S, cur, &parts) != 0) goto done;

        /* Connect: a live pooled handle, or net-connect (plus TLS for
         * https; SNI from :host, verification on unless :insecure?)
         * on a miss. The pool never hands an entry pooled under a
         * different verification mode to this request. */
        sock = mino_net_pool_checkout(S, parts.host, parts.host_len,
                                      (int)parts.port, parts.is_https,
                                      parts.insecure, parts.keepalive);
        from_pool = sock != NULL;
        if (sock == NULL) {
            mino_val *host_str = mino_string_n(S, parts.host,
                                               parts.host_len);
            mino_val *ckeys[3], *cvals[3], *cargs, *csock;
            if (host_str == NULL) goto done;
            mino_current_ctx(S)->gc_depth++;
            ckeys[0] = mino_keyword(S, "connect-timeout");
            cvals[0] = mino_int(S, parts.connect_ms);
            ckeys[1] = mino_keyword(S, "read-timeout");
            cvals[1] = mino_int(S, parts.read_ms);
            ckeys[2] = mino_keyword(S, "write-timeout");
            cvals[2] = mino_int(S, parts.write_ms);
            cargs = mino_map(S, ckeys, cvals, 3);
            cargs = mino_cons(S, cargs, mino_nil(S));
            cargs = mino_cons(S, mino_int(S, parts.port), cargs);
            cargs = mino_cons(S, host_str, cargs);
            mino_current_ctx(S)->gc_depth--;
            gc_pin(cargs);
            csock = prim_net_connect(S, cargs, env);
            gc_unpin(1);
            if (csock == NULL) {
                sock = NULL;
                goto done;
            }
            sock = csock;
        }
        sock_ref = mino_root_new(S, sock);
        if (sock_ref == NULL) {
            httpreq_close_handle(sock);
            sock = NULL;
            res = throw_classified(S, "internal", "MIN001",
                                        "http-request: out of memory");
            goto done;
        }
        is_tls_sock = httpreq_handle_is_tls(sock);

        if (parts.is_https && !from_pool) {
            mino_val *tkeys[3], *tvals[3], *targs, *tls;
            mino_val *host_str = mino_string_n(S, parts.host,
                                               parts.host_len);
            if (host_str == NULL) goto close_and_done;
            mino_current_ctx(S)->gc_depth++;
            tkeys[0] = mino_keyword(S, "insecure?");
            tvals[0] = parts.insecure ? mino_true(S) : mino_false(S);
            tkeys[1] = mino_keyword(S, "read-timeout");
            tvals[1] = mino_int(S, parts.read_ms);
            tkeys[2] = mino_keyword(S, "write-timeout");
            tvals[2] = mino_int(S, parts.write_ms);
            targs = mino_map(S, tkeys, tvals, 3);
            targs = mino_cons(S, targs, mino_nil(S));
            targs = mino_cons(S, host_str, targs);
            targs = mino_cons(S, sock, targs);
            mino_current_ctx(S)->gc_depth--;
            gc_pin(targs);
            tls = prim_tls_connect(S, targs, env);
            gc_unpin(1);
            if (tls == NULL) {
                /* The TCP descriptor was adopted (and closed) or is
                 * still owned by the net handle; close covers both. */
                goto close_and_done;
            }
            /* The fresh session takes over the root; the spent TCP
             * handle's root drops. No allocation sits between. */
            {
                mino_root *tls_ref = mino_root_new(S, tls);
                if (tls_ref == NULL) {
                    httpreq_close_handle(tls);
                    sock = tls;
                    res = throw_classified(S, "internal", "MIN001",
                                                "http-request: out of "
                                                "memory");
                    goto close_and_done;
                }
                mino_unroot(S, sock_ref);
                sock_ref = tls_ref;
            }
            sock = tls;
            is_tls_sock = 1;
        } else if (parts.is_https && !is_tls_sock) {
            /* The pool keys endpoints by scheme and verification mode,
             * so a plain socket under an https endpoint cannot happen;
             * refusing here keeps plaintext off TLS ports even if it
             * ever did. */
            res = throw_classified(S, "tls", "MTL004",
                                        "http-request: pooled socket is "
                                        "not a TLS session");
            goto close_and_done;
        }

        /* Encode. The codec core re-validates what the parts reader
         * let through; Host carries the port unless it is the scheme
         * default, and :keepalive 0 or less sends Connection: close. */
        {
            http_request_t rq;
            http_hdr_in_t *hdrs = NULL;
            size_t nhdrs = 0, extra = parts.keepalive <= 0 ? 1 : 0;
            char err[HTTP_ERR_CAP];
            int rc;
            memset(&rq, 0, sizeof(rq));
            if (httpreq_headers_array(S, parts.headers_val, extra,
                                      &hdrs, &nhdrs) != 0)
                goto close_and_done;
            hp_len = parts.host_len;
            memcpy(hostport, parts.host, hp_len);
            if (parts.is_https ? parts.port != 443 : parts.port != 80) {
                hp_len += (size_t)snprintf(hostport + hp_len,
                                           sizeof(hostport) - hp_len,
                                           ":%lld", parts.port);
            }
            rq.method     = parts.method;
            rq.method_len = parts.method_len;
            rq.target     = parts.target;
            rq.target_len = parts.target_len;
            rq.host       = hostport;
            rq.host_len   = hp_len;
            rq.headers    = hdrs;
            rq.nheaders   = nhdrs;
            if (extra > 0) {
                hdrs[nhdrs].name      = "Connection";
                hdrs[nhdrs].name_len  = 10;
                hdrs[nhdrs].value     = "close";
                hdrs[nhdrs].value_len = 5;
                rq.nheaders++;
            }
            if (parts.body_val != NULL) {
                if (!http_text_arg(parts.body_val, &rq.body,
                                   &rq.body_len)) {
                    free(hdrs);
                    res = throw_classified(S, "http/request",
                                                "MHR003",
                                                "http-request: :body must "
                                                "be a string, bytes, or "
                                                "nil");
                    goto close_and_done;
                }
            }
            rc = http_encode_request(&rq, &wire, &wire_len, err,
                                     sizeof(err));
            free(hdrs);
            if (rc != 0) {
                res = throw_classified(S, "http/headers", "MHR002",
                                            err);
                goto close_and_done;
            }
        }

        /* Send. */
        if (is_tls_sock) {
            if (mino_tls_handle_send(S, sock, wire, wire_len,
                                     parts.write_ms, &kind, &code, msg,
                                     sizeof(msg)) != 0)
                goto fail_io;
        } else {
            if (mino_net_handle_fd(sock, &fd) == 0) {
                kind = "net";
                code = "MNE004";
                snprintf(msg, sizeof(msg),
                         "http-request: socket closed before send");
                goto fail_io;
            }
            mino_net_apply_timeouts_raw(fd, parts.read_ms,
                                        parts.write_ms);
            if (mino_net_send_raw(S, fd, wire, wire_len, parts.write_ms,
                                  &kind, &code, msg, sizeof(msg)) != 0)
                goto fail_io;
        }
        free(wire);
        wire = NULL;

        /* Receive until the parser completes or the peer closes. A
         * HEAD response is complete at the blank line whatever the
         * framing headers claim (they describe the entity, not the
         * wire bytes that follow). */
        parser = http_parser_new(HTTP_DEFAULT_MAX_HEADER_BYTES,
                                 HTTP_DEFAULT_MAX_HEADERS,
                                 parts.max_bytes, 0);
        if (parser == NULL) {
            res = throw_classified(S, "internal", "MIN001",
                                        "http-request: out of memory");
            goto close_and_done;
        }
        parser->bodiless = parts.method_len == 4
            && http_ci_starts(parts.method, parts.method_len, "head", 4);
        for (;;) {
            size_t got = 0;
            int rc;
            if (parser->status == HTTP_DONE) break;
            if (is_tls_sock) {
                rc = mino_tls_handle_recv(S, sock, chunk, sizeof(chunk),
                                          &got, parts.read_ms, &kind,
                                          &code, msg, sizeof(msg));
            } else {
                rc = mino_net_recv_raw(S, fd, chunk, sizeof(chunk), &got,
                                       parts.read_ms, &kind, &code, msg,
                                       sizeof(msg));
            }
            if (rc < 0) goto fail_io;
            if (rc == 0) {
                if (http_parser_eof(parser) != HTTP_DONE) {
                    kind = "codec/truncated";
                    code = "MHC003";
                    snprintf(msg, sizeof(msg), "http-request: connection "
                             "closed before the response completed");
                    goto fail_io;
                }
                break;
            }
            if (http_parser_feed(parser, chunk, got) == HTTP_ERR) {
                const char *detail = parser->err;
                if (strncmp(detail, "http: ", 6) == 0) detail += 6;
                if (parser->limit_err) {
                    kind = "codec/limit";
                    code = "MHC002";
                } else {
                    kind = "codec/corrupt";
                    code = "MHC001";
                }
                snprintf(msg, sizeof(msg), "http-request: %s", detail);
                goto fail_io;
            }
        }

        /* Headers map while the parser rows are live; it feeds both
         * the redirect decision and the final response map, so it
         * stays rooted until the loop answers. */
        hmap = http_rows_map(S, parser, 0,
                             parser->trailer_start == (size_t)-1
                                 ? parser->nrows
                                 : parser->trailer_start);
        hmap_ref = mino_root_new(S, hmap);
        if (hmap_ref == NULL) {
            res = throw_classified(S, "internal", "MIN001",
                                        "http-request: out of memory");
            goto close_and_done;
        }

        /* Redirect hop, or the final response. */
        if (parts.follow && hop + 1 < HTTPREQ_MAX_HOPS) {
            mino_val *ns_req, *resp_map, *ropts, *dargs, *dec;
            mino_val *rkeys[3], *rvals[3], *ckeys[2], *cvals[2];
            size_t uri_len;
            if (httpreq_uri(&parts, uribuf, sizeof(uribuf), &uri_len)
                != 0) {
                res = throw_classified(S, "http/request", "MHR003",
                                            "http-request: request URL "
                                            "is too long");
                goto close_and_done;
            }
            mino_current_ctx(S)->gc_depth++;
            ns_req = httpreq_with_uri(S, cur, uribuf, uri_len);
            ckeys[0] = mino_keyword(S, "code");
            cvals[0] = mino_int(S, parser->code);
            ckeys[1] = mino_keyword(S, "headers");
            cvals[1] = hmap;
            resp_map = mino_map(S, ckeys, cvals, 2);
            rkeys[0] = mino_keyword(S, "follow-redirects");
            rvals[0] = mino_true(S);
            rkeys[1] = mino_keyword(S, "max-redirects");
            rvals[1] = mino_int(S, parts.max_redirects);
            rkeys[2] = mino_keyword(S, "redirect-count");
            rvals[2] = mino_int(S, (long long)hop);
            ropts = mino_map(S, rkeys, rvals, 3);
            dargs = mino_cons(S, ropts, mino_nil(S));
            dargs = mino_cons(S, resp_map, dargs);
            dargs = mino_cons(S, ns_req, dargs);
            mino_current_ctx(S)->gc_depth--;
            if (ns_req == NULL) goto close_and_done;
            gc_pin(dargs);
            dec = prim_redirect_next(S, dargs, NULL);
            gc_unpin(1);
            if (dec == NULL) goto close_and_done;
            /* dec stays LIFO-pinned across translate's allocations;
             * the pin is released before any other scope opens. */
            gc_pin(dec);
            if (map_get_val(dec, mino_keyword(S, "action"))
                == mino_keyword(S, "follow")) {
                mino_val *next, *new_cur, *uval;
                mino_root *next_ref;
                int ok;
                next = map_get_val(dec, mino_keyword(S, "request"));
                uval = next != NULL
                    ? map_get_val(next, mino_keyword(S, "uri")) : NULL;
                if (uval != NULL && mino_type_of(uval) == MINO_STRING
                    && trace_len < HTTPREQ_MAX_HOPS) {
                    if (httpreq_trace_push(S, uval, &trace_refs,
                                           &trace_len, &trace_cap) != 0) {
                        gc_unpin(1);
                        res = throw_classified(
                            S, "internal", "MIN001",
                            "http-request: out of memory");
                        goto close_and_done;
                    }
                }
                ok = httpreq_translate(S, next, &new_cur) == 0;
                gc_unpin(1);
                if (!ok) goto close_and_done;
                next_ref = mino_root_new(S, new_cur);
                if (next_ref == NULL) {
                    res = throw_classified(S, "internal", "MIN001",
                                                "http-request: out of "
                                                "memory");
                    goto close_and_done;
                }
                mino_unroot(S, cur_ref);
                cur_ref = next_ref;
                cur     = new_cur;
                httpreq_dispose(S, sock, &parts, parser);
                sock = NULL;
                mino_unroot(S, sock_ref);
                sock_ref = NULL;
                http_parser_free(parser);
                parser = NULL;
                mino_unroot(S, hmap_ref);
                hmap_ref = NULL;
                hmap = NULL;
                continue;
            }
            gc_unpin(1);
        }

        /* Final response: copy everything out of the parser, then
         * decode and assemble. */
        {
            mino_val *body_val, *fkeys[9], *fvals[9], *trace_vec, *resp;
            mino_val **trace_vals;
            const unsigned char *body;
            char encbuf[64];
            const char *enc = NULL;
            size_t body_len, enc_len = 0, hdr_end, i, nkeys = 8;
            int decoded = 0;
            int code_final = parser->code;
            int http10_final = parser->http10;

            hdr_end = parser->trailer_start == (size_t)-1
                ? parser->nrows : parser->trailer_start;
            for (i = 0; i < hdr_end; i++) {
                if (http_name_is(parser->hdr + parser->rows[i].name_off,
                                 parser->rows[i].name_len,
                                 "content-encoding", 16)) {
                    const char *v = (const char *)(parser->hdr
                                     + parser->rows[i].val_off);
                    enc_len = parser->rows[i].val_len;
                    /* copied out: the parser is freed below */
                    if (enc_len > sizeof(encbuf) - 1)
                        enc_len = sizeof(encbuf) - 1;
                    memcpy(encbuf, v, enc_len);
                    encbuf[enc_len] = '\0';
                    enc = encbuf;
                    break;
                }
            }
            if (parser->framing == HTTP_FR_CHUNKED) {
                body     = parser->body;
                body_len = parser->body_len;
            } else {
                body     = parser->buf + parser->body_start;
                body_len = parser->pos - parser->body_start;
            }
            body_val = mino_bytes(S, body, body_len);
            if (body_val == NULL) goto close_and_done;
            body_ref = mino_root_new(S, body_val);
            if (body_ref == NULL) {
                res = throw_classified(S, "internal", "MIN001",
                                            "http-request: out of memory");
                goto close_and_done;
            }

            httpreq_dispose(S, sock, &parts, parser);
            sock = NULL;
            mino_unroot(S, sock_ref);
            sock_ref = NULL;
            http_parser_free(parser);
            parser = NULL;

            if (enc != NULL && body_len > 0 && parts.decompress) {
                int is_gzip =
                    (enc_len == 4
                     && http_value_is_ci((const unsigned char *)enc, 4,
                                         "gzip", 4))
                    || (enc_len == 6
                        && http_value_is_ci((const unsigned char *)enc,
                                            6, "x-gzip", 6));
                int is_deflate =
                    enc_len == 7
                    && http_value_is_ci((const unsigned char *)enc, 7,
                                        "deflate", 7);
                if (is_gzip || is_deflate) {
                    mino_val *gkeys[1], *gvals[1], *gopts, *gargs, *out;
                    mino_root *out_ref;
                    mino_current_ctx(S)->gc_depth++;
                    gkeys[0] = mino_keyword(S, "max-bytes");
                    gvals[0] = mino_int(S, parts.max_bytes);
                    gopts = mino_map(S, gkeys, gvals, 1);
                    gargs = mino_cons(S, gopts, mino_nil(S));
                    gargs = mino_cons(S, body_val, gargs);
                    mino_current_ctx(S)->gc_depth--;
                    gc_pin(gargs);
                    out = is_gzip
                        ? prim_gzip_decompress(S, gargs, NULL)
                        : prim_deflate_decompress(S, gargs, NULL);
                    gc_unpin(1);
                    if (out == NULL) goto close_and_done;
                    out_ref = mino_root_new(S, out);
                    if (out_ref == NULL) {
                        res = throw_classified(S, "internal",
                                                    "MIN001",
                                                    "http-request: out "
                                                    "of memory");
                        goto done;
                    }
                    mino_unroot(S, body_ref);
                    body_ref = out_ref;
                    body_val = out;
                    decoded = 1;
                }
            }

            mino_current_ctx(S)->gc_depth++;
            trace_vals = trace_len > 0
                ? (mino_val **)malloc(trace_len * sizeof(*trace_vals))
                : NULL;
            if (trace_len > 0 && trace_vals == NULL) {
                mino_current_ctx(S)->gc_depth--;
                res = throw_classified(S, "internal", "MIN001",
                                            "http-request: out of "
                                            "memory");
                goto done;
            }
            for (i = 0; i < trace_len; i++) {
                trace_vals[i] = mino_root_get(trace_refs[i]);
            }
            trace_vec = mino_vector(S, trace_vals, trace_len);
            free(trace_vals);
            fkeys[0] = mino_keyword(S, "status");
            fvals[0] = mino_int(S, code_final);
            fkeys[1] = mino_keyword(S, "headers");
            fvals[1] = hmap;
            fkeys[2] = mino_keyword(S, "body-bytes");
            fvals[2] = body_val;
            fkeys[3] = mino_keyword(S, "http-version");
            fvals[3] = mino_string_n(S, http10_final ? "1.0" : "1.1", 3);
            fkeys[4] = mino_keyword(S, "from-pool?");
            fvals[4] = from_pool ? mino_true(S) : mino_false(S);
            fkeys[5] = mino_keyword(S, "request-time-ms");
            fvals[5] = mino_int(S, (mino_monotonic_ns() - t0) / 1000000LL);
            fkeys[6] = mino_keyword(S, "request");
            fvals[6] = req;
            fkeys[7] = mino_keyword(S, "trace-redirects");
            fvals[7] = trace_vec;
            if (enc != NULL && !decoded) {
                fkeys[8] = mino_keyword(S, "content-encoding");
                fvals[8] = mino_string_n(S, enc, enc_len);
                nkeys++;
            }
            resp = mino_map(S, fkeys, fvals, nkeys);
            mino_current_ctx(S)->gc_depth--;
            res = resp;
            goto done;
        }

close_and_done:
        if (sock != NULL) {
            httpreq_close_handle(sock);
            sock = NULL;
        }
        if (parser != NULL) {
            http_parser_free(parser);
            parser = NULL;
        }
        goto done;
    }

fail_io:
    /* A socket that errored mid-conversation is never pooled. */
    free(wire);
    wire = NULL;
    if (sock != NULL) {
        httpreq_close_handle(sock);
        sock = NULL;
    }
    if (parser != NULL) {
        http_parser_free(parser);
        parser = NULL;
    }
    res = throw_classified(S, kind, code, msg);
    goto done;

done:
    free(wire);
    if (parser != NULL) http_parser_free(parser);
    if (sock != NULL) httpreq_close_handle(sock);
    mino_unroot(S, req_ref);
    mino_unroot(S, cur_ref);
    mino_unroot(S, sock_ref);
    mino_unroot(S, hmap_ref);
    mino_unroot(S, body_ref);
    while (trace_len > 0) mino_unroot(S, trace_refs[--trace_len]);
    free(trace_refs);
    return res;
}

/* ---- http-request install (MINO_CAP_NET) ---- */

const mino_prim_def k_prims_http_client[] = {
    {"http-request", prim_http_request,
     "Runs one HTTP request end to end from a normalized parts map and "
     "returns {:status :headers :body-bytes :http-version :from-pool? "
     ":request-time-ms :request :trace-redirects} (plus "
     ":content-encoding when a compressed body came back undecoded). "
     "The map names the endpoint (:method string token, :scheme :http "
     "| :https, :host, :port, :target path and query) plus :headers "
     "[name value] pairs or a map (Host, Content-Length, and "
     "Transfer-Encoding are computed here and rejected in :headers), "
     ":body (string or bytes), and the policy keys :keepalive ms "
     "(default 120000; 0 or less disables reuse), :connect-timeout "
     ":read-timeout :write-timeout, :follow-redirects (default true), "
     ":max-redirects (default 10), :decompress-body? (default true), "
      ":max-bytes body cap (default 16777216), and :insecure? (skips "
      "TLS verification for local fixtures). HEAD responses are "
      "bodiless: they complete at the header block whatever the "
      "framing headers say. Keep-alive sockets are pooled per endpoint "
      "and verification mode and reused (an insecure session never "
      "serves a verifying request); a socket that errors "
      "mid-conversation is closed, never pooled. gzip and deflate "
     "response bodies are decoded (the :max-bytes cap applies to the "
     "decoded size too); an unknown Content-Encoding comes back "
     "undecoded with :content-encoding naming it. Redirects follow the "
     "redirect-next policy (303 to GET, 301/302 rewrite non-GET/HEAD, "
     "307/308 preserve) and record each hop URL in :trace-redirects. "
     "Lower-layer errors (:net, :tls, :codec) pass through; request "
     "validation throws :http/method, :http/headers, or :http/request."},
};

const size_t k_prims_http_client_count =
    sizeof(k_prims_http_client) / sizeof(k_prims_http_client[0]);

void mino_install_http_client(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_http_client,
                                       k_prims_http_client_count,
                                       "net");
    mino_install_mino_http(S, env);
    S->caps_installed |= MINO_CAP_NET;
}
