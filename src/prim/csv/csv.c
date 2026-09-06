/*
 * csv.c -- native CSV reader.
 *
 * One-pass byte-cursor parser backing clojure.data.csv/read-csv; the
 * writer stays Clojure. Walks UTF-8 bytes directly and matches the
 * separator and quote as byte sequences, so multibyte delimiters work
 * and field slicing is a byte-span copy. Semantics pin to the python3
 * csv oracle (see docs/adr/24-csv-reader-in-c-writer-stays-clojure.md):
 * lenient about stray quotes, a lone \r ends a record, blank lines
 * yield empty rows, a separator at end of input closes an empty
 * trailing field, and an unterminated quote takes the remainder.
 *
 * Rooting discipline follows json.c: in-flight transients and recent
 * values live in C locals the collector's conservative stack scan
 * sees; the one raw-pointer window (the quote-collapse buffer) is
 * GC_T_RAW memory held under a gc_depth guard exactly like
 * jp_string's decode buffer.
 */

#include "prim/internal.h"
#include "mino.h"

#include <string.h>

typedef struct {
    mino_state          *S;
    const unsigned char *p;
    const unsigned char *end;
    const unsigned char *sep;      /* separator byte sequence */
    size_t               sep_len;
    const unsigned char *quote;    /* quote byte sequence */
    size_t               quote_len;
} cv_t;

/* ---- cursor helpers ---- */

static int cv_at_delim(const cv_t *c, const unsigned char *p)
{
    if (p >= c->end) return 1;
    if (*p == '\n' || *p == '\r') return 1;
    if ((size_t)(c->end - p) >= c->sep_len
        && memcmp(p, c->sep, c->sep_len) == 0) {
        return 1;
    }
    return 0;
}

static int cv_is_quote(const cv_t *c, const unsigned char *p)
{
    return (size_t)(c->end - p) >= c->quote_len
           && memcmp(p, c->quote, c->quote_len) == 0;
}

/* Skip a record end at p (\n, \r, or the \r\n pair). */
static const unsigned char *cv_skip_eol(const cv_t *c,
                                        const unsigned char *p)
{
    if (*p == '\r'
        && p + 1 < c->end
        && p[1] == '\n') {
        return p + 2;
    }
    return p + 1;
}

/* ---- fields ---- */

/* Copy the span [from, to) into a string, collapsing doubled quote
 * sequences to one. Bound is the span length, which doubles never
 * exceed. */
static mino_val *cv_collapsed(const cv_t *c, const unsigned char *from,
                              const unsigned char *to,
                              const unsigned char *junk_from,
                              const unsigned char *junk_to)
{
    mino_state *S = c->S;
    size_t need = (size_t)(to - from) + (size_t)(junk_to - junk_from);
    int saved_gc_depth = mino_current_ctx(S)->gc_depth;
    char *buf;
    unsigned char *w;
    const unsigned char *r;
    mino_val *out;
    mino_current_ctx(S)->gc_depth = saved_gc_depth + 1;
    buf = (char *)gc_alloc_typed_inner(S, GC_T_RAW, need + 1);
    if (buf == NULL) {
        mino_current_ctx(S)->gc_depth = saved_gc_depth;
        return NULL;
    }
    w = (unsigned char *)buf;
    r = from;
    while (r < to) {
        if (cv_is_quote(c, r) && r + c->quote_len < to
            && cv_is_quote(c, r + c->quote_len)) {
            memcpy(w, r, c->quote_len);
            w += c->quote_len;
            r += c->quote_len * 2;
        } else {
            *w++ = *r++;
        }
    }
    if (junk_to > junk_from) {
        memcpy(w, junk_from, (size_t)(junk_to - junk_from));
        w += junk_to - junk_from;
    }
    out = mino_string_n(S, buf, (size_t)(w - (unsigned char *)buf));
    mino_current_ctx(S)->gc_depth = saved_gc_depth;
    return out;
}

/* Parse one field starting at c->p. Advances c->p past the field and
 * the separator or record end that follows it when consume_tail is
 * set; returns the field string, or NULL on error. */
static mino_val *cv_field_quoted(cv_t *c)
{
    const unsigned char *start = c->p + c->quote_len;
    const unsigned char *r = start;
    const unsigned char *close = NULL;
    int has_dbl = 0;
    while (r < c->end) {
        if (cv_is_quote(c, r)) {
            if ((size_t)(c->end - (r + c->quote_len)) >= c->quote_len
                && cv_is_quote(c, r + c->quote_len)) {
                has_dbl = 1;
                r += c->quote_len * 2;
                continue;
            }
            close = r;
            break;
        }
        r++;
    }
    if (close == NULL) {
        /* Unterminated: the whole remainder is the field text. */
        c->p = c->end;
        if (!has_dbl) {
            return mino_string_n(c->S, (const char *)start,
                                 (size_t)(c->end - start));
        }
        return cv_collapsed(c, start, c->end, c->end, c->end);
    }
    {
        const unsigned char *junk_from = close + c->quote_len;
        const unsigned char *j = junk_from;
        while (!cv_at_delim(c, j)) {
            j++;
        }
        c->p = j;
        if (!has_dbl && j == junk_from) {
            return mino_string_n(c->S, (const char *)start,
                                 (size_t)(close - start));
        }
        return cv_collapsed(c, start, close, junk_from, j);
    }
}

static mino_val *cv_field_plain(cv_t *c)
{
    const unsigned char *start = c->p;
    while (!cv_at_delim(c, c->p)) {
        c->p++;
    }
    return mino_string_n(c->S, (const char *)start,
                         (size_t)(c->p - start));
}

/* ---- records ---- */

/* Parse one record from c->p through its record end; returns the row
 * vector. The cursor lands just past the record end. */
static mino_val *cv_record(cv_t *c)
{
    mino_state *S = c->S;
    mino_val *acc = mino_transient(S, mino_vector(S, NULL, 0));
    mino_val *cur = acc;
    int after_sep = 0;
    if (acc == NULL) return NULL;
    for (;;) {
        mino_val *field;
        mino_val *next;
        if (c->p >= c->end) {
            if (after_sep) {
                cur = mino_conj_bang(S, cur, mino_string_n(S, "", 0));
                if (cur == NULL) return NULL;
            }
            break;
        }
        if (*c->p == '\n' || *c->p == '\r') {
            if (after_sep) {
                cur = mino_conj_bang(S, cur, mino_string_n(S, "", 0));
                if (cur == NULL) return NULL;
            }
            c->p = cv_skip_eol(c, c->p);
            break;
        }
        if (cv_is_quote(c, c->p)) {
            field = cv_field_quoted(c);
        } else {
            field = cv_field_plain(c);
        }
        if (field == NULL) return NULL;
        next = mino_conj_bang(S, cur, field);
        if (next == NULL) return NULL;
        cur = next;
        if (c->p >= c->end) break;
        if ((size_t)(c->end - c->p) >= c->sep_len
            && memcmp(c->p, c->sep, c->sep_len) == 0) {
            c->p += c->sep_len;
            after_sep = 1;
            continue;
        }
        /* Only a record end remains here: cv_field stops at delims. */
        c->p = cv_skip_eol(c, c->p);
        break;
    }
    return mino_persistent(S, cur);
}

/* ---- prim ---- */

/* Encode a codepoint as its UTF-8 byte sequence into out; returns the
 * byte count (1..4). */
static size_t cv_encode(int cp, unsigned char *out)
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

/* (csv-parse string) or (csv-parse string separator quote) -- parse
 * CSV text into a vector of vector rows of strings. */
static mino_val *prim_csv_parse(mino_state *S, mino_val *args,
                                mino_env *env)
{
    mino_val *s_val;
    mino_val *sep_val;
    mino_val *q_val;
    unsigned char sep_buf[4];
    unsigned char q_buf[4];
    cv_t c;
    mino_val *rows_acc;
    mino_val *rows_cur;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "csv-parse requires one or three arguments");
    }
    s_val = args->as.cons.car;
    if (s_val == NULL || mino_type_of(s_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "csv-parse: first argument must be a string");
    }
    if (mino_is_cons(args->as.cons.cdr)) {
        sep_val = args->as.cons.cdr->as.cons.car;
        if (!mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                "csv-parse requires one or three arguments");
        }
        q_val = args->as.cons.cdr->as.cons.cdr->as.cons.car;
        if (!mino_is_char(sep_val) || !mino_is_char(q_val)) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "csv-parse: separator and quote must be characters");
        }
    } else {
        sep_val = NULL;
        q_val = NULL;
    }
    if (sep_val != NULL) {
        c.sep_len = cv_encode(mino_val_char_get(sep_val), sep_buf);
        c.quote_len = cv_encode(mino_val_char_get(q_val), q_buf);
        c.sep = sep_buf;
        c.quote = q_buf;
    } else {
        c.sep_len = 1;
        c.quote_len = 1;
        sep_buf[0] = ',';
        q_buf[0] = '"';
        c.sep = sep_buf;
        c.quote = q_buf;
    }
    c.S = S;
    c.p = (const unsigned char *)s_val->as.s.data;
    c.end = c.p + s_val->as.s.len;
    rows_acc = mino_transient(S, mino_vector(S, NULL, 0));
    rows_cur = rows_acc;
    if (rows_acc == NULL) return NULL;
    while (c.p < c.end) {
        mino_val *row = cv_record(&c);
        mino_val *next;
        if (row == NULL) return NULL;
        next = mino_conj_bang(S, rows_cur, row);
        if (next == NULL) return NULL;
        rows_cur = next;
    }
    return mino_persistent(S, rows_cur);
}

const mino_prim_def k_prims_csv[] = {
    {"csv-parse", prim_csv_parse,
     "Parses CSV text into a vector of vector rows of strings. "
     "Optional second and third arguments set the separator and "
     "quote characters (comma and double quote by default)."},
};

const size_t k_prims_csv_count =
    sizeof(k_prims_csv) / sizeof(k_prims_csv[0]);
