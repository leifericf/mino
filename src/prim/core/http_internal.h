/*
 * http_internal.h -- the shared HTTP/1.1 codec surface the network
 * client layer reuses. http_codec.c owns the pure buffer transforms
 * (parse, encode, URL, redirect) and defines everything declared here;
 * http_client.c drives the same parser, encoder, and URL helpers over a
 * live socket. The types and prototypes below are the exact contract
 * between the two: the codec keeps its remaining helpers file-local.
 */
#ifndef MINO_PRIM_HTTP_INTERNAL_H
#define MINO_PRIM_HTTP_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "mino.h"

#define HTTP_DEFAULT_MAX_HEADER_BYTES (64u * 1024u)
#define HTTP_DEFAULT_MAX_HEADERS      100u
#define HTTP_DEFAULT_MAX_BODY_BYTES   (16ll * 1024ll * 1024ll)
#define HTTP_CL_LIMIT                 (1ll << 53)
#define HTTP_ERR_CAP                  160
#define HTTP_REDIRECT_MAX_URI         4096

/* Parser terminal status; the client tests for DONE and ERR. */
enum {
    HTTP_MORE = 0,
    HTTP_DONE = 1,
    HTTP_ERR  = 2
};

/* Body framing decided at the blank line; the client tests for CLOSE
 * (never reusable) and CHUNKED (de-chunked body lives in p->body). */
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
    size_t         method_off, method_len;
    size_t         target_off, target_len;
    long long      content_length; /* -1 until decided */
    long long      chunk_remaining;
    int            state, status, framing;
    int            code, http10;
    int            is_request;
    int            info_count;
    int            bodiless;  /* caller knows the method has no body */
    int            informational; /* surface a 1xx head instead of skipping */
    size_t         max_header_bytes;
    size_t         max_headers;
    long long      max_body_bytes;
    int            limit_err;  /* the failure is the body cap, not corruption */
    char           err[HTTP_ERR_CAP];
};

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

/* Parts a redirect needs out of a parsed URL map, copied so no GC
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

/* Header/method names the request layer computes itself and rejects in
 * caller headers: "host", "content-length", "transfer-encoding", NULL. */
extern const char *const k_request_owned[];

/* Character classifiers and case folding (RFC 7230 tables). */
int http_lower(int c);
int http_upper(int c);
int http_tchar(unsigned char c);
int http_ows(unsigned char c);
int http_valid_target(const char *s, size_t len);

/* Name and value comparison over the parser's byte store. */
int http_name_is(const unsigned char *name, size_t len,
                 const char *want, size_t want_len);
int http_value_is_ci(const unsigned char *v, size_t len,
                     const char *want, size_t want_len);
int http_name_in(const char *const *owned, const char *name, size_t len);
int http_ci_starts(const char *s, size_t len, const char *pfx,
                   size_t pfx_len);

/* Prim argument views shared by the codec and the client. */
int http_name_arg(const mino_val *v, const char **name, size_t *len);
int http_text_arg(const mino_val *v, const unsigned char **data,
                  size_t *len);
int http_opt_long(mino_state *S, const mino_val *opts, const char *key,
                  long long def, long long lo, long long hi,
                  long long *out);

/* Bounded URI assembly used by redirect and request-URL building. */
int http_buf_put(char *buf, size_t cap, size_t *len, const char *s,
                 size_t n);
int http_put_port(char *buf, size_t cap, size_t *len, int is_https,
                  int port);

/* Incremental message parser. */
mino_http_parser_t *http_parser_new(size_t max_header_bytes,
                                    size_t max_headers,
                                    long long max_body_bytes,
                                    int is_request);
void http_parser_free(mino_http_parser_t *p);
int  http_parser_feed(mino_http_parser_t *p, const unsigned char *data,
                      size_t len);
int  http_parser_eof(mino_http_parser_t *p);

/* Parser rows [from,to) as a headers map: names lowercased, repeats
 * collected into vectors in arrival order. */
mino_val *http_rows_map(mino_state *S, const mino_http_parser_t *p,
                        size_t from, size_t to);

/* Request serialization. Pure; the caller frees *out. */
int http_encode_request(const http_request_t *req, unsigned char **out,
                        size_t *out_len, char *err, size_t err_cap);

/* URL parsing and part extraction for redirect-target resolution. */
mino_val *http_parse_url_str(mino_state *S, const char *url, size_t len);
int http_url_parts(mino_state *S, const mino_val *m, http_url_parts_t *p);

/* Redirect policy prim; the client drives one hop through it. */
mino_val *prim_redirect_next(mino_state *S, mino_val *args, mino_env *env);

#endif /* MINO_PRIM_HTTP_INTERNAL_H */
