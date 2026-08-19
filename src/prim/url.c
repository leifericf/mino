/*
 * url.c -- URL text primitives: percent-encoding and URL parsing
 * (RFC 3986).
 *
 * Pure data in, data out. The codecs and the parser are plain
 * functions over buffers with no state; the prims wrap them and map
 * malformed input to classified :eval/contract errors. Input is
 * untrusted bytes from the script side, so every walk is bounded by
 * the input length and every buffer is sized from it before the walk
 * starts.
 */

#include "prim/internal.h"
#include "mino.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int url_unreserved(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9')
        || c == '-' || c == '.' || c == '_' || c == '~';
}

static int url_hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const char k_url_hex_upper[] = "0123456789ABCDEF";

/* Extract a byte view from a string or bytes argument. Returns 0 on
 * any other type. */
static int url_text_arg(const mino_val *v, const unsigned char **data,
                        size_t *len, int *is_string)
{
    if (v == NULL) return 0;
    if (mino_type_of(v) == MINO_STRING) {
        *data      = (const unsigned char *)v->as.s.data;
        *len       = v->as.s.len;
        *is_string = 1;
        return 1;
    }
    if (mino_is_bytes(v)) {
        *data      = mino_bytes_data(v);
        *len       = mino_bytes_len(v);
        *is_string = 0;
        return 1;
    }
    return 0;
}

/* (percent-encode v) -- RFC 3986 percent-encoding. */
static mino_val *prim_percent_encode(mino_state *S, mino_val *args,
                                     mino_env *env)
{
    mino_val           *v;
    const unsigned char *src;
    unsigned char      *out;
    mino_val           *result;
    size_t             len, i, o = 0;
    int                is_string;
    (void)env;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "percent-encode requires one argument");
    }
    v = args->as.cons.car;
    if (!url_text_arg(v, &src, &len, &is_string)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "percent-encode: argument must be a "
                                     "string or bytes value");
    }
    if (len > (SIZE_MAX - 1) / 3) {
        return prim_throw_classified(S, "eval/bounds", "MBD001",
                                     "percent-encode: input too large");
    }
    out = (unsigned char *)malloc(len * 3 + 1);
    if (out == NULL) {
        return prim_throw_classified(S, "internal", "MIN001",
                                     "percent-encode: out of memory");
    }
    for (i = 0; i < len; i++) {
        unsigned char c = src[i];
        if (url_unreserved(c)) {
            out[o++] = c;
        } else {
            out[o++] = '%';
            out[o++] = (unsigned char)k_url_hex_upper[c >> 4];
            out[o++] = (unsigned char)k_url_hex_upper[c & 0x0f];
        }
    }
    result = is_string
        ? mino_string_n(S, (const char *)out, o)
        : mino_bytes(S, out, o);
    free(out);
    return result;
}

/* Strict UTF-8 validation (RFC 3629): rejects overlong forms,
 * surrogate halves, codepoints above U+10FFFF, and truncated
 * sequences. On failure writes the offending byte offset. */
static int url_utf8_valid(const unsigned char *s, size_t n, size_t *bad_off)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c   = s[i];
        unsigned char lo  = 0x80, hi = 0xBF;
        size_t         need;
        size_t         k;
        if (c < 0x80) { i++; continue; }
        if (c >= 0xC2 && c <= 0xDF) {
            need = 1;
        } else if (c == 0xE0) {
            need = 2; lo = 0xA0;
        } else if ((c >= 0xE1 && c <= 0xEC) || c == 0xED || c == 0xEE
                   || c == 0xEF) {
            need = 2;
            if (c == 0xED) hi = 0x9F;
        } else if (c == 0xF0) {
            need = 3; lo = 0x90;
        } else if (c >= 0xF1 && c <= 0xF3) {
            need = 3;
        } else if (c == 0xF4) {
            need = 3; hi = 0x8F;
        } else {
            *bad_off = i;
            return 0;
        }
        if (i + need >= n) {
            *bad_off = i;
            return 0;
        }
        if (s[i + 1] < lo || s[i + 1] > hi) {
            *bad_off = i + 1;
            return 0;
        }
        for (k = 2; k <= need; k++) {
            if (s[i + k] < 0x80 || s[i + k] > 0xBF) {
                *bad_off = i + k;
                return 0;
            }
        }
        i += need + 1;
    }
    return 1;
}

/* (percent-decode v) -- inverse of percent-encode. */
static mino_val *prim_percent_decode(mino_state *S, mino_val *args,
                                     mino_env *env)
{
    mino_val           *v;
    const unsigned char *src;
    unsigned char      *out;
    mino_val           *result;
    size_t             len, i, o = 0;
    int                is_string;
    char               msg[96];
    (void)env;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "percent-decode requires one argument");
    }
    v = args->as.cons.car;
    if (!url_text_arg(v, &src, &len, &is_string)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "percent-decode: argument must be a "
                                     "string or bytes value");
    }
    /* Pass 1: validate every escape, count the output length. */
    for (i = 0; i < len; ) {
        if (src[i] == '%') {
            if (i + 2 >= len || url_hex_digit(src[i + 1]) < 0
                || url_hex_digit(src[i + 2]) < 0) {
                snprintf(msg, sizeof(msg),
                         "percent-decode: malformed percent-escape at "
                         "byte %lu", (unsigned long)i);
                return prim_throw_classified(S, "eval/contract", "MCT001",
                                             msg);
            }
            o++;
            i += 3;
        } else {
            o++;
            i++;
        }
    }
    out = (unsigned char *)malloc(o > 0 ? o : 1);
    if (out == NULL) {
        return prim_throw_classified(S, "internal", "MIN001",
                                     "percent-decode: out of memory");
    }
    /* Pass 2: fill. Escapes were validated above; no re-check needed. */
    for (i = 0, o = 0; i < len; ) {
        if (src[i] == '%') {
            out[o++] = (unsigned char)(url_hex_digit(src[i + 1]) * 16
                                       + url_hex_digit(src[i + 2]));
            i += 3;
        } else {
            out[o++] = src[i++];
        }
    }
    if (is_string) {
        size_t bad_off = 0;
        if (!url_utf8_valid(out, o, &bad_off)) {
            snprintf(msg, sizeof(msg),
                     "percent-decode: decoded bytes are not valid UTF-8 "
                     "at byte %lu", (unsigned long)bad_off);
            free(out);
            return prim_throw_classified(S, "eval/contract", "MCT001", msg);
        }
        result = mino_string_n(S, (const char *)out, o);
    } else {
        result = mino_bytes(S, out, o);
    }
    free(out);
    return result;
}

/* ---- URL parsing ---- */

static char url_lower(char c)
{
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

static int url_ascii_alpha(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static int url_scheme_char(unsigned char c)
{
    return url_ascii_alpha(c) || (c >= '0' && c <= '9')
        || c == '+' || c == '-' || c == '.';
}

static int url_scheme_is(const char *s, size_t len,
                         const char *want, size_t want_len)
{
    size_t k;
    if (len != want_len) return 0;
    for (k = 0; k < len; k++) {
        if (url_lower(s[k]) != want[k]) return 0;
    }
    return 1;
}

/* (parse-url s) -- hierarchical http(s) URL string to a plain map.
 *
 * Single bounded walk: every component is a range of the input, so
 * total allocation is linear in the input length. All validation
 * happens before the first allocation; nothing is malloc'd until the
 * input has fully parsed.
 *
 * Cross-TU: prim/http.c calls it for redirect-target resolution (the
 * tls.c precedent of one prim driving another with a built arg list). */
mino_val *prim_parse_url(mino_state *S, mino_val *args,
                                mino_env *env)
{
    mino_val *v;
    const char *s;
    size_t     len;
    size_t     scheme_len, auth_start, auth_end, at_pos = 0;
    size_t     host_start, host_end;
    size_t     port_start = 0, port_len = 0;
    size_t     path_start, path_end, query_start = 0, query_len = 0;
    size_t     frag_start = 0, frag_len = 0;
    size_t     j, k;
    int        has_userinfo = 0, has_port = 0, has_query = 0, has_frag = 0;
    int        is_https, explicit_port;
    long long  port;
    char       msg[160];
    char       *scratch;
    mino_val   *keys[8], *vals[8], *m;
    size_t     pinned = 0;
    (void)env;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "parse-url requires one argument");
    }
    v = args->as.cons.car;
    if (v == NULL || mino_type_of(v) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "parse-url: argument must be a string");
    }
    s   = v->as.s.data;
    len = v->as.s.len;

    /* Scheme: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":" */
    if (len == 0 || !url_ascii_alpha((unsigned char)s[0])) {
        return prim_throw_classified(S, "eval/contract", "MCT001",
                                     "parse-url: URL has no scheme");
    }
    j = 1;
    while (j < len && url_scheme_char((unsigned char)s[j])) j++;
    if (j >= len || s[j] != ':') {
        return prim_throw_classified(S, "eval/contract", "MCT001",
                                     "parse-url: URL has no scheme");
    }
    scheme_len = j;
    is_https   = url_scheme_is(s, scheme_len, "https", 5);
    if (!is_https && !url_scheme_is(s, scheme_len, "http", 4)) {
        snprintf(msg, sizeof(msg),
                 "parse-url: unsupported scheme '%.*s' (only http and "
                 "https are supported)",
                 (int)(scheme_len < 40 ? scheme_len : 40), s);
        return prim_throw_classified(S, "eval/contract", "MCT001", msg);
    }
    if (scheme_len + 2 >= len || s[scheme_len + 1] != '/'
        || s[scheme_len + 2] != '/') {
        return prim_throw_classified(S, "eval/contract", "MCT001",
                                     "parse-url: URL must start with "
                                     "scheme://");
    }
    auth_start = scheme_len + 3;

    /* Authority: up to the first '/', '?', or '#'. */
    for (j = auth_start; j < len; j++) {
        char c = s[j];
        if (c == '/' || c == '?' || c == '#') break;
        if (c == '@') { has_userinfo = 1; at_pos = j; }
    }
    auth_end = j;

    /* Split userinfo at the last '@' (an '@' may appear inside the
     * userinfo; the host can never contain one outside brackets). */
    if (has_userinfo) {
        host_start = at_pos + 1;
    } else {
        host_start = auth_start;
    }

    /* Host and optional port. */
    if (host_start < auth_end && s[host_start] == '[') {
        size_t close = 0;
        for (k = host_start + 1; k < auth_end; k++) {
            if (s[k] == ']') { close = k; break; }
        }
        if (close == 0) {
            return prim_throw_classified(S, "eval/contract", "MCT001",
                                         "parse-url: unterminated IPv6 "
                                         "host literal");
        }
        host_end = close + 1;
        if (host_end - host_start < 2) {
            return prim_throw_classified(S, "eval/contract", "MCT001",
                                         "parse-url: URL has an empty "
                                         "host");
        }
        if (host_end < auth_end) {
            if (s[host_end] != ':') {
                return prim_throw_classified(S, "eval/contract", "MCT001",
                                             "parse-url: unexpected "
                                             "character after IPv6 host "
                                             "literal");
            }
            port_start = host_end + 1;
            port_len   = auth_end - port_start;
            has_port   = 1;
        }
    } else {
        host_end = auth_end;
        for (k = host_start; k < auth_end; k++) {
            if (s[k] == ':') {
                host_end  = k;
                port_start = k + 1;
                port_len   = auth_end - port_start;
                has_port   = 1;
                break;
            }
        }
        if (host_end == host_start) {
            return prim_throw_classified(S, "eval/contract", "MCT001",
                                         "parse-url: URL has an empty "
                                         "host");
        }
    }

    /* Port: digits only, 0..65535. An empty port text ("host:") means
     * the scheme default and is not explicit (RFC 3986 allows it). */
    explicit_port = 0;
    port = -1;
    if (has_port && port_len > 0) {
        port = 0;
        for (k = 0; k < port_len; k++) {
            unsigned char c = (unsigned char)s[port_start + k];
            if (c < '0' || c > '9') {
                return prim_throw_classified(S, "eval/contract", "MCT001",
                                             "parse-url: invalid character "
                                             "in port");
            }
            port = port * 10 + (c - '0');
            if (port > 65535) {
                return prim_throw_classified(S, "eval/contract", "MCT001",
                                             "parse-url: port is out of "
                                             "range (max 65535)");
            }
        }
        explicit_port = 1;
    }
    if (port < 0) port = is_https ? 443 : 80;

    /* Path, query, fragment: verbatim ranges of the input. */
    path_start = auth_end;
    for (j = auth_end; j < len; j++) {
        if (s[j] == '?' || s[j] == '#') break;
    }
    path_end = j;
    if (j < len && s[j] == '?') {
        has_query = 1;
        query_start = j + 1;
        for (k = j + 1; k < len; k++) {
            if (s[k] == '#') break;
        }
        query_len = k - query_start;
        j = k;
    }
    if (j < len && s[j] == '#') {
        has_frag  = 1;
        frag_start = j + 1;
        frag_len   = len - frag_start;
    }

    /* Build phase. Inputs beyond this point cannot fail validation;
     * every buffer is bounded by len. */
    scratch = (char *)malloc(len + 1);
    if (scratch == NULL) {
        return prim_throw_classified(S, "internal", "MIN001",
                                     "parse-url: out of memory");
    }
    keys[0] = mino_keyword(S, "scheme");          gc_pin(keys[0]); pinned++;
    for (k = 0; k < scheme_len; k++) scratch[k] = url_lower(s[k]);
    vals[0] = mino_string_n(S, scratch, scheme_len); gc_pin(vals[0]); pinned++;
    keys[1] = mino_keyword(S, "host");            gc_pin(keys[1]); pinned++;
    for (k = 0; k < host_end - host_start; k++) {
        scratch[k] = url_lower(s[host_start + k]);
    }
    vals[1] = mino_string_n(S, scratch, host_end - host_start);
    gc_pin(vals[1]); pinned++;
    keys[2] = mino_keyword(S, "port");            gc_pin(keys[2]); pinned++;
    vals[2] = mino_int(S, port);                  gc_pin(vals[2]); pinned++;
    keys[3] = mino_keyword(S, "path");            gc_pin(keys[3]); pinned++;
    if (path_end > path_start) {
        vals[3] = mino_string_n(S, s + path_start, path_end - path_start);
    } else {
        vals[3] = mino_string_n(S, "/", 1);
    }
    gc_pin(vals[3]); pinned++;
    free(scratch);
    keys[4] = mino_keyword(S, "query");           gc_pin(keys[4]); pinned++;
    vals[4] = has_query
        ? mino_string_n(S, s + query_start, query_len)
        : mino_nil(S);
    if (has_query) { gc_pin(vals[4]); pinned++; }
    keys[5] = mino_keyword(S, "fragment");        gc_pin(keys[5]); pinned++;
    vals[5] = has_frag
        ? mino_string_n(S, s + frag_start, frag_len)
        : mino_nil(S);
    if (has_frag) { gc_pin(vals[5]); pinned++; }
    keys[6] = mino_keyword(S, "userinfo");        gc_pin(keys[6]); pinned++;
    vals[6] = has_userinfo
        ? mino_string_n(S, s + auth_start, at_pos - auth_start)
        : mino_nil(S);
    if (has_userinfo) { gc_pin(vals[6]); pinned++; }
    keys[7] = mino_keyword(S, "explicit-port?");  gc_pin(keys[7]); pinned++;
    vals[7] = explicit_port ? mino_true(S) : mino_false(S);

    m = mino_map(S, keys, vals, 8);
    gc_unpin(pinned);
    return m;
}

const mino_prim_def k_prims_url[] = {
    {"percent-encode", prim_percent_encode,
     "Percent-encodes a string or bytes value per RFC 3986: unreserved "
     "characters (letters, digits, - . _ ~) stay literal and every other "
     "byte becomes %XX with uppercase hex. Space encodes as %20, never "
     "plus. Returns the same kind it was given."},
    {"percent-decode", prim_percent_decode,
     "Decodes %XX escapes in a string or bytes value, returning the same "
     "kind. Hex digits are case-insensitive. Malformed escapes throw; a "
     "string result must be valid UTF-8 or the decode throws."},
    {"parse-url", prim_parse_url,
     "Parses a hierarchical http or https URL into a plain map with "
     ":scheme :host :port :path :query :fragment :userinfo and "
     ":explicit-port?. Scheme and host are lowercased; the default port "
     "is 80 for http and 443 for https, with :explicit-port? true only "
     "when the URL spelled the port out. An empty path becomes \"/\". "
     "IPv6 hosts keep their brackets. Components are returned verbatim "
     "(no percent-decoding, no dot-segment or trailing-dot removal). "
     "Other schemes, IDN punycode, and non-hierarchical URLs throw "
     ":eval/contract errors."},
};

const size_t k_prims_url_count =
    sizeof(k_prims_url) / sizeof(k_prims_url[0]);
