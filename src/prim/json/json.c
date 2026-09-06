/*
 * json.c -- native JSON reader.
 *
 * One-pass byte-cursor recursive descent over the input string,
 * allocating values directly with no intermediate token strings.
 * Backs clojure.data.json/read-str; the writer stays Clojure (it is
 * already linear through the str builder). Error messages match the
 * historical Clojure reader so catch sites survive unchanged.
 *
 * Rooting discipline: in-flight transients and recent values live in
 * C locals of these frames, which the collector's conservative stack
 * scan sees; a parse error unwinds via longjmp with no malloc'd state
 * outstanding. The one raw-pointer window (the escaped-string decode
 * buffer) is GC_T_RAW memory held under a gc_depth guard exactly like
 * mino_string_n's dup_n window in values/val.c.
 */

#include "prim/internal.h"
#include "mino.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define JP_MAX_DEPTH 512
#define JP_NUM_MAX   512

typedef struct {
    mino_state           *S;
    mino_env             *env;
    const unsigned char  *start; /* buffer head, for line/col of an error */
    const unsigned char  *p;
    const unsigned char  *end;
    mino_val             *key_fn; /* nil or per-key transform */
    int                   depth;
} jp_t;

static mino_val *jp_value(jp_t *j);

/* ---- errors ---- */

/* Classify a parse failure as :json/parse with the 1-based line/col of
 * the cursor in ex-data's :location, matching the toml/yaml/xml readers.
 * Line/col are counted over the bytes consumed so far; a lone '\r' and a
 * '\r\n' pair each advance one line. */
static mino_val *jp_err(jp_t *j, const char *msg)
{
    long line = 1, col = 1;
    const unsigned char *c;
    mino_val *lkeys[2], *lvals[2];
    mino_val *dkeys[1], *dvals[1];
    mino_val *loc, *data;
    for (c = j->start; c < j->p && c < j->end; c++) {
        if (*c == '\n') {
            line++; col = 1;
        } else if (*c == '\r') {
            line++; col = 1;
            if (c + 1 < j->end && c[1] == '\n') c++;
        } else {
            col++;
        }
    }
    lkeys[0] = mino_keyword(j->S, "line");  lvals[0] = mino_int(j->S, line);
    lkeys[1] = mino_keyword(j->S, "col");   lvals[1] = mino_int(j->S, col);
    loc = mino_map(j->S, lkeys, lvals, 2);
    dkeys[0] = mino_keyword(j->S, "location"); dvals[0] = loc;
    data = mino_map(j->S, dkeys, dvals, 1);
    return prim_throw_classified_data(j->S, "json/parse", "MJP001",
                                      msg, data);
}

static mino_val *jp_err_eof(jp_t *j, const char *what)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "Unexpected EOF%s", what);
    return jp_err(j, buf);
}

static mino_val *jp_err_char(jp_t *j, unsigned char c)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "Unexpected character: %c", (char)c);
    return jp_err(j, buf);
}

/* ---- cursor helpers ---- */

static void jp_ws(jp_t *j)
{
    while (j->p < j->end) {
        unsigned char c = *j->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            j->p++;
        } else {
            break;
        }
    }
}

static int jp_at_end(jp_t *j)
{
    return j->p >= j->end;
}

static unsigned char jp_peek(jp_t *j)
{
    return *j->p;
}

static void jp_adv(jp_t *j)
{
    j->p++;
}

/* ---- strings ---- */

static int jp_hex(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse a quoted string starting at the opening quote (j->p points at
 * it). Two passes: scan for the closing quote and note escapes, then
 * decode. The no-escape fast path hands the span straight to
 * mino_string_n. */
static mino_val *jp_string(jp_t *j)
{
    const unsigned char *start;
    const unsigned char *q;
    int has_escape = 0;
    size_t need;
    jp_adv(j); /* opening quote */
    start = j->p;
    q = j->p;
    while (q < j->end && *q != '"') {
        if (*q == '\\') {
            has_escape = 1;
            if (q + 1 >= j->end) {
                q = j->end; /* trailing backslash: EOF below */
                break;
            }
            if (q[1] == 'u') {
                int k;
                q += 2;
                for (k = 0; k < 4 && q < j->end; k++) {
                    q++;
                }
            } else {
                q += 2;
            }
        } else {
            q++;
        }
    }
    if (q >= j->end) {
        jp_err_eof(j, " in string");
        return NULL;
    }
    if (!has_escape) {
        j->p = q + 1;
        return mino_string_n(j->S, (const char *)start,
                             (size_t)(q - start));
    }
    /* Escaped content. The decoded size never exceeds the raw span
     * (\uXXXX shrinks to at most 3 bytes, two-char escapes to 1), so
     * the span length is a safe buffer bound. */
    need = (size_t)(q - start);
    {
        int saved_gc_depth = mino_current_ctx(j->S)->gc_depth;
        char *buf;
        unsigned char *w;
        const unsigned char *r;
        mino_val *out;
        mino_current_ctx(j->S)->gc_depth = saved_gc_depth + 1;
        buf = (char *)gc_alloc_typed_inner(j->S, GC_T_RAW, need + 1);
        if (buf == NULL) {
            mino_current_ctx(j->S)->gc_depth = saved_gc_depth;
            return NULL;
        }
        w = (unsigned char *)buf;
        r = start;
        while (r < q) {
            if (*r == '\\') {
                unsigned char e = r[1];
                r += 2;
                switch (e) {
                case '"':  *w++ = '"';  break;
                case '\\': *w++ = '\\'; break;
                case '/':  *w++ = '/';  break;
                case 'b':  *w++ = '\b'; break;
                case 'f':  *w++ = '\f'; break;
                case 'n':  *w++ = '\n'; break;
                case 'r':  *w++ = '\r'; break;
                case 't':  *w++ = '\t'; break;
                case 'u': {
                    int h[4];
                    unsigned long cp;
                    int i;
                    if (q - r < 4) {
                        /* Truncated \u: same failure the historical
                         * tokenizer produced when \\u did not carry
                         * four hex digits. */
                        mino_current_ctx(j->S)->gc_depth = saved_gc_depth;
                        jp_err(j, "Invalid escape: \\u");
                        return NULL;
                    }
                    for (i = 0; i < 4; i++) {
                        h[i] = jp_hex(r[i]);
                        if (h[i] < 0) {
                            mino_current_ctx(j->S)->gc_depth
                                = saved_gc_depth;
                            jp_err(j, "Invalid hex digit in \\u escape");
                            return NULL;
                        }
                    }
                    r += 4;
                    cp = ((unsigned long)h[0] << 12)
                       | ((unsigned long)h[1] << 8)
                       | ((unsigned long)h[2] << 4)
                       | (unsigned long)h[3];
                    /* Surrogate pair: a high surrogate followed by a
                     * low surrogate escapes one astral codepoint; the
                     * writer emits exactly this shape, so pairing
                     * keeps write/read round-trips exact. A lone
                     * surrogate keeps its (historical) 3-byte form. */
                    if (cp >= 0xD800UL && cp <= 0xDBFFUL
                        && (q - r) >= 6 && r[0] == '\\'
                        && r[1] == 'u') {
                        int h2[4];
                        int ok = 1;
                        unsigned long lo;
                        for (i = 0; i < 4; i++) {
                            h2[i] = jp_hex(r[2 + i]);
                            if (h2[i] < 0) {
                                ok = 0;
                                break;
                            }
                        }
                        if (ok) {
                            lo = ((unsigned long)h2[0] << 12)
                               | ((unsigned long)h2[1] << 8)
                               | ((unsigned long)h2[2] << 4)
                               | (unsigned long)h2[3];
                            if (lo >= 0xDC00UL && lo <= 0xDFFFUL) {
                                unsigned long full =
                                    0x10000UL
                                    + ((cp - 0xD800UL) << 10)
                                    + (lo - 0xDC00UL);
                                r += 6;
                                *w++ = (unsigned char)(0xF0 | (full >> 18));
                                *w++ = (unsigned char)(0x80 | ((full >> 12) & 0x3F));
                                *w++ = (unsigned char)(0x80 | ((full >> 6) & 0x3F));
                                *w++ = (unsigned char)(0x80 | (full & 0x3F));
                                break;
                            }
                        }
                    }
                    if (cp < 0x80) {
                        *w++ = (unsigned char)cp;
                    } else if (cp < 0x800) {
                        *w++ = (unsigned char)(0xC0 | (cp >> 6));
                        *w++ = (unsigned char)(0x80 | (cp & 0x3F));
                    } else {
                        *w++ = (unsigned char)(0xE0 | (cp >> 12));
                        *w++ = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                        *w++ = (unsigned char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: {
                    char msg[32];
                    snprintf(msg, sizeof(msg), "Invalid escape: \\%c",
                             (char)e);
                    mino_current_ctx(j->S)->gc_depth = saved_gc_depth;
                    jp_err(j, msg);
                    return NULL;
                }
                }
            } else {
                *w++ = *r++;
            }
        }
        out = mino_string_n(j->S, buf, (size_t)(w - (unsigned char *)buf));
        mino_current_ctx(j->S)->gc_depth = saved_gc_depth;
        j->p = q + 1;
        return out;
    }
}

/* ---- numbers ---- */

static mino_val *jp_number(jp_t *j)
{
    const unsigned char *start = j->p;
    size_t n;
    int is_float = 0;
    if (jp_peek(j) == '-') {
        jp_adv(j);
    }
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
        jp_adv(j);
    }
    if (j->p < j->end && *j->p == '.'
        && j->p + 1 < j->end && j->p[1] >= '0' && j->p[1] <= '9') {
        is_float = 1;
        jp_adv(j);
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
            jp_adv(j);
        }
    }
    if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')
        && j->p + 1 < j->end
        && ((j->p[1] >= '0' && j->p[1] <= '9')
            || ((j->p[1] == '+' || j->p[1] == '-')
                && j->p + 2 < j->end
                && j->p[2] >= '0' && j->p[2] <= '9'))) {
        is_float = 1;
        jp_adv(j);
        if (*j->p == '+' || *j->p == '-') {
            jp_adv(j);
        }
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
            jp_adv(j);
        }
    }
    n = (size_t)(j->p - start);
    if (n == 0 || (n == 1 && *start == '-')) {
        char msg[48];
        snprintf(msg, sizeof(msg), "Invalid JSON number: %c",
                 (char)*start);
        return jp_err(j, msg);
    }
    if (n <= JP_NUM_MAX) {
        char buf[JP_NUM_MAX + 1];
        memcpy(buf, start, n);
        buf[n] = '\0';
        if (is_float) {
            return mino_float(j->S, strtod(buf, NULL));
        }
        {
            long long v;
            const char *endp = NULL;
            errno = 0;
            v = strtoll(buf, (char **)&endp, 10);
            if (errno != ERANGE && endp == buf + n) {
                return mino_int(j->S, v);
            }
            return mino_bigint_from_string(j->S, buf);
        }
    }
    /* Overlong token: only a valid integer can be this long, and the
     * bigint parser wants NUL-terminated input. Copy through GC raw
     * memory under a gc_depth guard, matching the string-decode
     * discipline. */
    {
        int saved_gc_depth = mino_current_ctx(j->S)->gc_depth;
        char *buf;
        mino_val *out;
        if (is_float) {
            return jp_err(j, "Invalid JSON number: token too long");
        }
        mino_current_ctx(j->S)->gc_depth = saved_gc_depth + 1;
        buf = (char *)gc_alloc_typed_inner(j->S, GC_T_RAW, n + 1);
        if (buf == NULL) {
            mino_current_ctx(j->S)->gc_depth = saved_gc_depth;
            return NULL;
        }
        memcpy(buf, start, n);
        buf[n] = '\0';
        out = mino_bigint_from_string(j->S, buf);
        mino_current_ctx(j->S)->gc_depth = saved_gc_depth;
        return out;
    }
}

/* ---- literals ---- */

static int jp_lit(jp_t *j, const char *lit)
{
    size_t n = strlen(lit);
    if ((size_t)(j->end - j->p) >= n
        && memcmp(j->p, lit, n) == 0) {
        j->p += n;
        return 1;
    }
    return 0;
}

/* ---- composites ---- */

static mino_val *jp_array(jp_t *j)
{
    mino_val *acc;
    mino_val *cur;
    jp_adv(j); /* [ */
    acc = mino_transient(j->S, mino_vector(j->S, NULL, 0));
    cur = acc;
    jp_ws(j);
    if (jp_at_end(j)) {
        jp_err_eof(j, " in array");
        return NULL;
    }
    if (jp_peek(j) == ']') {
        jp_adv(j);
        return mino_persistent(j->S, cur);
    }
    for (;;) {
        mino_val *v = jp_value(j);
        mino_val *next;
        if (v == NULL) {
            return NULL;
        }
        next = mino_conj_bang(j->S, cur, v);
        if (next == NULL) {
            return NULL;
        }
        cur = next;
        jp_ws(j);
        if (jp_at_end(j)) {
            jp_err_eof(j, " in array");
            return NULL;
        }
        if (jp_peek(j) == ',') {
            jp_adv(j);
            continue;
        }
        if (jp_peek(j) == ']') {
            jp_adv(j);
            return mino_persistent(j->S, cur);
        }
        return jp_err(j, "Expected , or ]");
    }
}

static mino_val *jp_object(jp_t *j)
{
    mino_val *acc;
    mino_val *cur;
    jp_adv(j); /* { */
    acc = mino_transient(j->S, mino_map(j->S, NULL, NULL, 0));
    cur = acc;
    jp_ws(j);
    if (jp_at_end(j)) {
        jp_err_eof(j, " in object");
        return NULL;
    }
    if (jp_peek(j) == '}') {
        jp_adv(j);
        return mino_persistent(j->S, cur);
    }
    for (;;) {
        mino_val *raw_k;
        mino_val *k;
        mino_val *v;
        mino_val *next;
        if (jp_peek(j) != '"') {
            return jp_err(j, "Expected string key");
        }
        raw_k = jp_string(j);
        if (raw_k == NULL) {
            return NULL;
        }
        if (j->key_fn != NULL) {
            mino_val *args = mino_cons(j->S, raw_k, mino_nil(j->S));
            mino_val *k2;
            if (args == NULL) {
                return NULL;
            }
            k2 = mino_call(j->S, j->key_fn, args, j->env);
            if (k2 == NULL) {
                return NULL;
            }
            k = k2;
        } else {
            k = raw_k;
        }
        jp_ws(j);
        if (jp_at_end(j)) {
            jp_err_eof(j, " in object");
            return NULL;
        }
        if (jp_peek(j) != ':') {
            return jp_err(j, "Expected : after key");
        }
        jp_adv(j);
        jp_ws(j);
        v = jp_value(j);
        if (v == NULL) {
            return NULL;
        }
        next = mino_assoc_bang(j->S, cur, k, v);
        if (next == NULL) {
            return NULL;
        }
        cur = next;
        jp_ws(j);
        if (jp_at_end(j)) {
            jp_err_eof(j, " in object");
            return NULL;
        }
        if (jp_peek(j) == ',') {
            jp_adv(j);
            jp_ws(j);
            if (jp_at_end(j)) {
                jp_err_eof(j, " in object");
                return NULL;
            }
            continue;
        }
        if (jp_peek(j) == '}') {
            jp_adv(j);
            return mino_persistent(j->S, cur);
        }
        return jp_err(j, "Expected , or }");
    }
}

static mino_val *jp_value(jp_t *j)
{
    unsigned char c;
    if (j->depth >= JP_MAX_DEPTH) {
        return jp_err(j, "JSON nesting exceeds limit");
    }
    jp_ws(j);
    if (jp_at_end(j)) {
        return jp_err(j, "Unexpected EOF");
    }
    c = jp_peek(j);
    if (c == '{') {
        mino_val *out;
        j->depth++;
        out = jp_object(j);
        j->depth--;
        return out;
    }
    if (c == '[') {
        mino_val *out;
        j->depth++;
        out = jp_array(j);
        j->depth--;
        return out;
    }
    if (c == '"') {
        return jp_string(j);
    }
    if (c == 't') {
        if (jp_lit(j, "true")) {
            return mino_true(j->S);
        }
        return jp_err_char(j, c);
    }
    if (c == 'f') {
        if (jp_lit(j, "false")) {
            return mino_false(j->S);
        }
        return jp_err_char(j, c);
    }
    if (c == 'n') {
        if (jp_lit(j, "null")) {
            return mino_nil(j->S);
        }
        return jp_err_char(j, c);
    }
    if ((c >= '0' && c <= '9') || c == '-') {
        return jp_number(j);
    }
    return jp_err_char(j, c);
}

/* ---- prim ---- */

/* (json-parse string key-fn) -- parse one JSON value from string;
 * content after the value is ignored, matching the historical reader.
 * key-fn may be nil. */
static mino_val *prim_json_parse(mino_state *S, mino_val *args,
                                 mino_env *env)
{
    mino_val *s_val;
    mino_val *key_fn;
    jp_t j;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "json-parse requires one or two arguments");
    }
    s_val = args->as.cons.car;
    if (mino_is_cons(args->as.cons.cdr)) {
        key_fn = args->as.cons.cdr->as.cons.car;
        if (key_fn != NULL && mino_type_of(key_fn) == MINO_NIL) {
            key_fn = NULL;
        }
    } else {
        key_fn = NULL;
    }
    if (s_val == NULL || mino_type_of(s_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "json-parse: first argument must be a string");
    }
    j.S      = S;
    j.env    = env;
    j.p      = (const unsigned char *)s_val->as.s.data;
    j.start  = j.p;
    j.end    = j.p + s_val->as.s.len;
    j.key_fn = key_fn;
    j.depth  = 0;
    return jp_value(&j);
}

const mino_prim_def k_prims_json[] = {
    {"json-parse", prim_json_parse,
     "Parses one JSON value from a string. Returns maps with string "
     "keys; the optional second argument transforms each object key."},
};

const size_t k_prims_json_count =
    sizeof(k_prims_json) / sizeof(k_prims_json[0]);
