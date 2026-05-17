/*
 * string.c -- string primitives: str, pr-str, format, read-string,
 *                  char-at, subs, split, join, starts-with?, ends-with?,
 *                  includes?, upper-case, lower-case, trim.
 */

#include "prim/internal.h"
#include "regex/re.h"

/* Grow `buf` so that `len + extra + 1` bytes fit. Returns the (possibly
 * realloc'd) buffer, or NULL if allocation failed (in which case an
 * MIN001 diagnostic has already been recorded on S). The caller updates
 * its own `cap` via cap_ptr; `buf` is replaced by the return value. */
static inline char *fmt_ensure(mino_state_t *S, char *buf,
                               size_t len, size_t *cap_ptr, size_t extra)
{
    size_t need;
    /* len + extra + 1 must not wrap. */
    if (extra > SIZE_MAX - len - 1) {
        free(buf);
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                      "internal", "MIN001",
                      "format: result size overflow");
        return NULL;
    }
    need = len + extra + 1;
    if (need > *cap_ptr) {
        size_t newcap = *cap_ptr == 0 ? 128 : *cap_ptr;
        char  *newbuf;
        while (newcap < need) {
            if (newcap > SIZE_MAX / 2) {
                free(buf);
                set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                              "internal", "MIN001",
                              "format: result size overflow");
                return NULL;
            }
            newcap *= 2;
        }
        newbuf = (char *)realloc(buf, newcap);
        if (newbuf == NULL) {
            /* realloc failure leaves `buf` valid; free it before the
             * caller overwrites its variable with our NULL return.
             * The size-overflow branch above already does the same. */
            free(buf);
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "internal", "MIN001",
                          "out of memory");
            return NULL;
        }
        *cap_ptr = newcap;
        return newbuf;
    }
    return buf;
}

/*
 * (format fmt & args) — simple string formatting.
 * Directives: %s (str of arg), %d (integer), %f (float), %% (literal %).
 */
mino_val_t *prim_format(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *fmt_val;
    const char *fmt;
    size_t      fmt_len;
    mino_val_t *arg_list;
    char  *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    size_t i;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "format requires at least a format string");
    }
    fmt_val = args->as.cons.car;
    if (fmt_val == NULL || mino_type_of(fmt_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "format: first argument must be a string");
    }
    fmt     = fmt_val->as.s.data;
    fmt_len = fmt_val->as.s.len;
    arg_list = args->as.cons.cdr;

    for (i = 0; i < fmt_len; i++) {
        if (fmt[i] == '%' && i + 1 < fmt_len) {
            /* Collect the full format directive: %[flags][width][.prec]spec
             * Flags: '-', '+', ' ', '0', '#'
             * Width/precision: digits and '.'
             * Spec: d, f, e, g, s, x, o, % */
            char   directive[32];
            size_t di = 0;
            char   spec;
            directive[di++] = '%';
            i++;
            /* Flags. */
            while (i < fmt_len && di < sizeof(directive) - 4 &&
                   (fmt[i] == '-' || fmt[i] == '+' || fmt[i] == ' ' ||
                    fmt[i] == '0' || fmt[i] == '#')) {
                directive[di++] = fmt[i++];
            }
            /* Width. */
            while (i < fmt_len && di < sizeof(directive) - 4 &&
                   fmt[i] >= '0' && fmt[i] <= '9') {
                directive[di++] = fmt[i++];
            }
            /* Precision. */
            if (i < fmt_len && fmt[i] == '.') {
                directive[di++] = fmt[i++];
                while (i < fmt_len && di < sizeof(directive) - 4 &&
                       fmt[i] >= '0' && fmt[i] <= '9') {
                    directive[di++] = fmt[i++];
                }
            }
            if (i >= fmt_len) {
                /* Incomplete directive at end of string: emit literal. */
                buf = fmt_ensure(S, buf, len, &cap, di);
                if (buf == NULL) return NULL;
                memcpy(buf + len, directive, di);
                len += di;
                break;
            }
            spec = fmt[i];
            if (spec == '%') {
                buf = fmt_ensure(S, buf, len, &cap, 1);
                if (buf == NULL) return NULL;
                buf[len++] = '%';
            } else if (spec == 's') {
                mino_val_t *a;
                if (!mino_is_cons(arg_list)) {
                    free(buf);
                    return prim_throw_classified(S, "eval/arity", "MAR001", "format: not enough arguments for format string");
                }
                a = arg_list->as.cons.car;
                arg_list = arg_list->as.cons.cdr;
                if (a != NULL && mino_type_of(a) == MINO_STRING) {
                    buf = fmt_ensure(S, buf, len, &cap, a->as.s.len);
                    if (buf == NULL) return NULL;
                    memcpy(buf + len, a->as.s.data, a->as.s.len);
                    len += a->as.s.len;
                } else {
                    mino_val_t *s = print_to_string(S, a);
                    if (s == NULL) { free(buf); return NULL; }
                    buf = fmt_ensure(S, buf, len, &cap, s->as.s.len);
                    if (buf == NULL) return NULL;
                    memcpy(buf + len, s->as.s.data, s->as.s.len);
                    len += s->as.s.len;
                }
            } else if (spec == 'd' || spec == 'x' || spec == 'o') {
                long long n2;
                char tmp[64];
                int  tn;
                if (!mino_is_cons(arg_list)) {
                    free(buf);
                    return prim_throw_classified(S, "eval/arity", "MAR001", "format: not enough arguments for format string");
                }
                if (!as_long(arg_list->as.cons.car, &n2)) {
                    double d;
                    if (as_double(arg_list->as.cons.car, &d)) {
                        n2 = (long long)d;
                    } else {
                        free(buf);
                        return prim_throw_classified(S, "eval/type", "MTY001", "format: integer directive expects a number");
                    }
                }
                arg_list = arg_list->as.cons.cdr;
                /* Build snprintf format: replace spec with lld/llx/llo. */
                directive[di++] = 'l';
                directive[di++] = 'l';
                directive[di++] = spec;
                directive[di]   = '\0';
                tn = snprintf(tmp, sizeof(tmp), directive, n2);
                buf = fmt_ensure(S, buf, len, &cap, (size_t)tn);
                if (buf == NULL) return NULL;
                memcpy(buf + len, tmp, (size_t)tn);
                len += (size_t)tn;
            } else if (spec == 'f' || spec == 'e' || spec == 'g') {
                double d;
                char tmp[128];
                int  tn;
                if (!mino_is_cons(arg_list)) {
                    free(buf);
                    return prim_throw_classified(S, "eval/arity", "MAR001", "format: not enough arguments for format string");
                }
                if (!as_double(arg_list->as.cons.car, &d)) {
                    free(buf);
                    return prim_throw_classified(S, "eval/type", "MTY001", "format: float directive expects a number");
                }
                arg_list = arg_list->as.cons.cdr;
                directive[di++] = spec;
                directive[di]   = '\0';
                tn = snprintf(tmp, sizeof(tmp), directive, d);
                buf = fmt_ensure(S, buf, len, &cap, (size_t)tn);
                if (buf == NULL) return NULL;
                memcpy(buf + len, tmp, (size_t)tn);
                len += (size_t)tn;
            } else {
                /* Unknown directive: emit literal. */
                buf = fmt_ensure(S, buf, len, &cap, di + 1);
                if (buf == NULL) return NULL;
                memcpy(buf + len, directive, di);
                len += di;
                buf[len++] = spec;
            }
        } else {
            buf = fmt_ensure(S, buf, len, &cap, 1);
            if (buf == NULL) return NULL;
            buf[len++] = fmt[i];
        }
    }
    {
        mino_val_t *result = mino_string_n(S, buf != NULL ? buf : "", len);
        free(buf);
        return result;
    }
}

mino_val_t *prim_read_string(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s;
    mino_val_t *opts = NULL;
    mino_val_t *result;
    int         saved_mode = S->reader_cond_mode;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "read-string requires one string argument");
    }
    /* Two-arg form: (read-string opts s). The opts map currently
     * recognises :read-cond → :allow / :preserve / :disallow. */
    if (mino_is_cons(args->as.cons.cdr)) {
        if (mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                "read-string takes one or two arguments");
        }
        opts = args->as.cons.car;
        s    = args->as.cons.cdr->as.cons.car;
    } else {
        s = args->as.cons.car;
    }
    if (s == NULL || mino_type_of(s) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "read-string: argument must be a string");
    }
    if (opts != NULL && mino_type_of(opts) != MINO_NIL) {
        mino_val_t *rc;
        if (mino_type_of(opts) != MINO_MAP) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "read-string: opts must be a map");
        }
        rc = map_get_val(opts, mino_keyword(S, "read-cond"));
        if (rc != NULL) {
            if (mino_type_of(rc) != MINO_KEYWORD) {
                return prim_throw_classified(S, "eval/type", "MTY001",
                    "read-string: :read-cond must be a keyword");
            }
            if (strcmp(rc->as.s.data, "allow") == 0)         S->reader_cond_mode = 0;
            else if (strcmp(rc->as.s.data, "preserve") == 0) S->reader_cond_mode = 1;
            else if (strcmp(rc->as.s.data, "disallow") == 0) S->reader_cond_mode = 2;
            else {
                return prim_throw_classified(S, "eval/contract", "MCT001",
                    "read-string: :read-cond must be :allow, :preserve, or :disallow");
            }
        }
    }
    clear_error(S);
    result = mino_read(S, s->as.s.data, NULL);
    S->reader_cond_mode = saved_mode;
    if (result == NULL && mino_last_error(S) != NULL) {
        /* Throw parse errors as catchable exceptions so user code can
         * handle them via try/catch. */
        mino_val_t *ex = mino_string(S, mino_last_error(S));
        if (mino_current_ctx(S)->try_depth > 0) {
            mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth - 1].exception = ex;
            longjmp(mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth - 1].buf, 1);
        }
        /* No enclosing try — propagate as fatal error. */
        {
            char msg[512];
            snprintf(msg, sizeof(msg), "unhandled exception: %.*s",
                     (int)ex->as.s.len, ex->as.s.data);
            return prim_throw_classified(S, "eval/type", "MTY001", msg);
        }
    }
    return result != NULL ? result : mino_nil(S);
}

mino_val_t *prim_pr_str(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    char  *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    int    first = 1;
    (void)env;
    while (mino_is_cons(args)) {
        mino_val_t *printed = print_to_string(S, args->as.cons.car);
        size_t      need;
        if (printed == NULL) {
            free(buf);
            return NULL;
        }
        need = len + (!first ? 1 : 0) + printed->as.s.len + 1;
        if (need > cap) {
            char *newbuf;
            cap = cap == 0 ? 128 : cap;
            while (cap < need) cap *= 2;
            newbuf = (char *)realloc(buf, cap);
            if (newbuf == NULL) { free(buf); set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "internal", "MIN001", "out of memory"); return NULL; }
            buf = newbuf;
        }
        if (!first) buf[len++] = ' ';
        memcpy(buf + len, printed->as.s.data, printed->as.s.len);
        len += printed->as.s.len;
        first = 0;
        args = args->as.cons.cdr;
    }
    {
        mino_val_t *result = mino_string_n(S, buf != NULL ? buf : "", len);
        free(buf);
        return result;
    }
}

mino_val_t *prim_char_at(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s, *idx_val;
    long long idx;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "char-at requires two arguments");
    }
    s       = args->as.cons.car;
    idx_val = args->as.cons.cdr->as.cons.car;
    if (s == NULL || mino_type_of(s) != MINO_STRING
        || idx_val == NULL || !mino_val_int_p(idx_val)) {
        return prim_throw_classified(S, "eval/type", "MTY001", "char-at: requires a string and integer index");
    }
    idx = mino_val_int_get(idx_val);
    if (idx < 0 || (size_t)idx >= s->as.s.len) {
        return prim_throw_classified(S, "eval/bounds", "MBD001", "char-at: index out of range");
    }
    return mino_string_n(S, s->as.s.data + idx, 1);
}

/* ------------------------------------------------------------------------- */
/* String primitives                                                         */
/* ------------------------------------------------------------------------- */

/* Step one UTF-8 codepoint forward starting at byte index `pos` in
 * `data` (length `bytes`). Returns the byte length of the codepoint;
 * malformed leading bytes step by 1 to keep the walk bounded. */
size_t utf8_codepoint_step(const char *data, size_t bytes, size_t pos)
{
    unsigned char b;
    if (pos >= bytes) return 0;
    b = (unsigned char)data[pos];
    if (b < 0x80) return 1;
    if ((b & 0xE0) == 0xC0 && pos + 1 < bytes) return 2;
    if ((b & 0xF0) == 0xE0 && pos + 2 < bytes) return 3;
    if ((b & 0xF8) == 0xF0 && pos + 3 < bytes) return 4;
    return 1;
}

/* Walk `n` codepoints into `data` starting at `pos`; return the
 * resulting byte offset, capped at `bytes`. */
size_t utf8_skip_codepoints(const char *data, size_t bytes,
                            size_t pos, long long n)
{
    while (n > 0 && pos < bytes) {
        pos += utf8_codepoint_step(data, bytes, pos);
        n--;
    }
    return pos;
}

/* Count codepoints in [data, data+bytes). */
long long utf8_codepoint_count(const char *data, size_t bytes)
{
    long long count = 0;
    size_t pos = 0;
    while (pos < bytes) {
        pos += utf8_codepoint_step(data, bytes, pos);
        count++;
    }
    return count;
}

mino_val_t *prim_subs(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s_val;
    long long   start, end_idx;
    size_t      n;
    size_t      byte_start, byte_end;
    long long   total_cps;
    (void)env;
    arg_count(S, args, &n);
    if (n != 2 && n != 3) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "subs requires 2 or 3 arguments");
    }
    s_val = args->as.cons.car;
    if (s_val == NULL || mino_type_of(s_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "subs: first argument must be a string");
    }
    if (args->as.cons.cdr->as.cons.car == NULL
        || !mino_val_int_p(args->as.cons.cdr->as.cons.car)) {
        return prim_throw_classified(S, "eval/type", "MTY001", "subs: start index must be an integer");
    }
    start = mino_val_int_get(args->as.cons.cdr->as.cons.car);
    /* Indices are codepoint-counted, matching Clojure where strings
     * are sequences of chars (UTF-16 code units there, codepoints
     * here -- mino has no surrogates). For ASCII content the byte
     * walk is identical to the codepoint walk. */
    total_cps = utf8_codepoint_count(s_val->as.s.data, s_val->as.s.len);
    if (n == 3) {
        if (args->as.cons.cdr->as.cons.cdr->as.cons.car == NULL
            || !mino_val_int_p(args->as.cons.cdr->as.cons.cdr->as.cons.car)) {
            return prim_throw_classified(S, "eval/type", "MTY001", "subs: end index must be an integer");
        }
        end_idx = mino_val_int_get(args->as.cons.cdr->as.cons.cdr->as.cons.car);
    } else {
        end_idx = total_cps;
    }
    if (start < 0 || end_idx < start || end_idx > total_cps) {
        return prim_throw_classified(S, "eval/bounds", "MBD001", "subs: index out of range");
    }
    byte_start = utf8_skip_codepoints(s_val->as.s.data, s_val->as.s.len,
                                      0, start);
    byte_end = utf8_skip_codepoints(s_val->as.s.data, s_val->as.s.len,
                                    byte_start, end_idx - start);
    return mino_string_n(S, s_val->as.s.data + byte_start,
                         byte_end - byte_start);
}

mino_val_t *prim_split(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t  *s_val;
    mino_val_t  *sep_val;
    mino_val_t  *limit_val = NULL;
    const char  *s;
    size_t       slen;
    const char  *sep;
    size_t       sep_len;
    long long    limit = 0;       /* 0 / negative = no cap */
    mino_val_t **buf = NULL;
    size_t       cap = 0, len = 0;
    const char  *p;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "split requires a string and a separator");
    }
    s_val   = args->as.cons.car;
    sep_val = args->as.cons.cdr->as.cons.car;
    if (mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        limit_val = args->as.cons.cdr->as.cons.cdr->as.cons.car;
        if (mino_is_cons(args->as.cons.cdr->as.cons.cdr->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                "split takes at most 3 arguments");
        }
        if (limit_val == NULL || !mino_val_int_p(limit_val)) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "split: limit must be an integer");
        }
        limit = mino_val_int_get(limit_val);
    }
    if (s_val == NULL || mino_type_of(s_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "split: first argument must be a string");
    }
    if (sep_val == NULL
        || (mino_type_of(sep_val) != MINO_STRING && mino_type_of(sep_val) != MINO_REGEX)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "split: separator must be a string or regex");
    }
    s    = s_val->as.s.data;
    slen = s_val->as.s.len;
    /* Empty input: return [""] (a single empty-string element) per
     * Clojure / JVM String.split semantics. mino previously returned
     * an empty vector here. The single-empty form is what downstream
     * Clojure code expects from (str/split "" re). */
    if (slen == 0) {
        mino_val_t **buf1 = (mino_val_t **)gc_alloc_typed(S,
            GC_T_VALARR, 1 * sizeof(*buf1));
        if (buf1 == NULL) return NULL;
        buf1[0] = mino_string_n(S, "", 0);
        return mino_vector(S, buf1, 1);
    }
    if (mino_type_of(sep_val) == MINO_REGEX
        && sep_val->as.regex.source != NULL
        && mino_type_of(sep_val->as.regex.source) == MINO_STRING) {
        /* Regex separators: walk the compiled pattern across the input,
         * emitting the substring between each match site. Follows
         * Clojure's String.split(re, limit) semantics — limit > 0 caps
         * the result and the final piece absorbs the rest of the input;
         * limit <= 0 strips trailing empty pieces (only the limit == 0
         * case strips by JVM convention, but mino has historically
         * conflated 0 and negative for the literal-string path, so the
         * regex path matches that). */
        const char *pat_src = sep_val->as.regex.source->as.s.data;
        re_t        compiled = re_compile(pat_src);
        size_t      pos = 0;
        if (compiled == NULL) {
            return prim_throw_classified(S, "eval/contract", "MCT001",
                "split: invalid regex pattern");
        }
        for (;;) {
            int  mlen = 0;
            int  idx;
            if (pos > slen) break;
            idx = re_matchp(compiled, s + pos, &mlen);
            if (len == cap) {
                size_t new_cap = cap == 0 ? 8 : cap * 2;
                mino_val_t **nb = (mino_val_t **)gc_alloc_typed(S,
                    GC_T_VALARR, new_cap * sizeof(*nb));
                if (buf != NULL && len > 0) memcpy(nb, buf, len * sizeof(*nb));
                buf = nb;
                cap = new_cap;
            }
            if (limit > 0 && (long long)len + 1 == limit) {
                buf[len++] = mino_string_n(S, s + pos,
                                           (size_t)(slen - pos));
                break;
            }
            if (idx < 0) {
                buf[len++] = mino_string_n(S, s + pos,
                                           (size_t)(slen - pos));
                break;
            }
            buf[len++] = mino_string_n(S, s + pos, (size_t)idx);
            /* Zero-width match: advance by one character so we don't
             * loop forever on patterns like #"a*". */
            if (mlen <= 0) {
                if (pos + (size_t)idx >= slen) break;
                pos += (size_t)idx + 1;
            } else {
                pos += (size_t)idx + (size_t)mlen;
            }
        }
        re_free(compiled);
        /* Trim trailing empty pieces unless limit > 0 (matches JVM
         * Clojure default split-with-limit-0 behaviour). */
        if (limit <= 0) {
            while (len > 0
                   && buf[len - 1] != NULL
                   && mino_type_of(buf[len - 1]) == MINO_STRING
                   && buf[len - 1]->as.s.len == 0) {
                len--;
            }
        }
        return mino_vector(S, buf, len);
    } else {
        sep     = sep_val->as.s.data;
        sep_len = sep_val->as.s.len;
    }
    p       = s;
    if (sep_len == 0) {
        /* Split into individual characters. */
        size_t i;
        size_t out_len = (limit > 0 && (size_t)limit < slen) ? (size_t)limit : slen;
        buf = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR,
              (out_len > 0 ? out_len : 1) * sizeof(*buf));
        if (limit > 0 && (size_t)limit < slen) {
            for (i = 0; i + 1 < out_len; i++) {
                buf[i] = mino_string_n(S, s + i, 1);
            }
            buf[out_len - 1] = mino_string_n(S, s + (out_len - 1),
                slen - (out_len - 1));
        } else {
            for (i = 0; i < slen; i++) {
                buf[i] = mino_string_n(S, s + i, 1);
            }
        }
        return mino_vector(S, buf, out_len);
    }
    while (p <= s + slen) {
        const char *found = NULL;
        const char *q;
        for (q = p; q + sep_len <= s + slen; q++) {
            if (memcmp(q, sep, sep_len) == 0) {
                found = q;
                break;
            }
        }
        if (len == cap) {
            size_t new_cap = cap == 0 ? 8 : cap * 2;
            mino_val_t **nb = (mino_val_t **)gc_alloc_typed(S,
                GC_T_VALARR, new_cap * sizeof(*nb));
            if (buf != NULL && len > 0) memcpy(nb, buf, len * sizeof(*nb));
            buf = nb;
            cap = new_cap;
        }
        /* Limit reached: emit one final item that absorbs the rest of
         * the string (matches canon's String.split(re, limit > 0). */
        if (limit > 0 && (long long)len + 1 == limit) {
            buf[len++] = mino_string_n(S, p, (size_t)(s + slen - p));
            break;
        }
        if (found != NULL) {
            buf[len++] = mino_string_n(S, p, (size_t)(found - p));
            p = found + sep_len;
        } else {
            buf[len++] = mino_string_n(S, p, (size_t)(s + slen - p));
            break;
        }
    }
    return mino_vector(S, buf, len);
}

mino_val_t *prim_join(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t  *sep_val;
    mino_val_t  *coll;
    const char  *sep = "";
    size_t       sep_len = 0;
    char        *buf = NULL;
    size_t       buf_len = 0, buf_cap = 0;
    int          first = 1;
    seq_iter_t   it;
    size_t       n;
    (void)env;
    arg_count(S, args, &n);
    if (n == 1) {
        /* (join coll) — no separator. */
        coll = args->as.cons.car;
    } else if (n == 2) {
        /* (join sep coll) */
        sep_val = args->as.cons.car;
        coll    = args->as.cons.cdr->as.cons.car;
        if (sep_val != NULL && mino_type_of(sep_val) == MINO_STRING) {
            sep     = sep_val->as.s.data;
            sep_len = sep_val->as.s.len;
        } else if (sep_val != NULL && mino_type_of(sep_val) != MINO_NIL) {
            return prim_throw_classified(S, "eval/type", "MTY001", "join: separator must be a string or nil");
        }
    } else {
        return prim_throw_classified(S, "eval/arity", "MAR001", "join requires 1 or 2 arguments");
    }
    if (coll == NULL || mino_type_of(coll) == MINO_NIL) {
        return mino_string(S, "");
    }
    seq_iter_init(S, &it, coll);
    while (!seq_iter_done(&it)) {
        mino_val_t *elem = seq_iter_val(S, &it);
        const char *part;
        size_t      part_len;
        size_t      need;
        if (elem == NULL || mino_type_of(elem) == MINO_NIL) {
            seq_iter_next(S, &it);
            continue;
        }
        if (mino_type_of(elem) == MINO_STRING) {
            part     = elem->as.s.data;
            part_len = elem->as.s.len;
        } else {
            /* Convert to string. */
            mino_val_t *str_a = mino_cons(S, elem, mino_nil(S));
            mino_val_t *str   = prim_str(S, str_a, env);
            if (str == NULL) return NULL;
            part     = str->as.s.data;
            part_len = str->as.s.len;
        }
        need = buf_len + (first ? 0 : sep_len) + part_len + 1;
        if (need > buf_cap) {
            char *newbuf;
            buf_cap = buf_cap == 0 ? 128 : buf_cap;
            while (buf_cap < need) buf_cap *= 2;
            newbuf = (char *)realloc(buf, buf_cap);
            if (newbuf == NULL) { free(buf); set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "internal", "MIN001", "out of memory"); return NULL; }
            buf = newbuf;
        }
        if (!first && sep_len > 0) {
            memcpy(buf + buf_len, sep, sep_len);
            buf_len += sep_len;
        }
        memcpy(buf + buf_len, part, part_len);
        buf_len += part_len;
        first = 0;
        seq_iter_next(S, &it);
    }
    {
        mino_val_t *result = mino_string_n(S, buf != NULL ? buf : "", buf_len);
        free(buf);
        return result;
    }
}

/* Grow `*pbuf` so `*plen + n + 1` bytes fit, then append `n` bytes from
 * `src`. On OOM, frees `*pbuf` and returns -1 (caller must surface the
 * error to S). Used by the regex-replace and template-expansion paths. */
static int str_replace_buf_append(mino_state_t *S, char **pbuf,
                                  size_t *plen, size_t *pcap,
                                  const char *src, size_t n)
{
    size_t need;
    if (n > SIZE_MAX - *plen - 1) {
        free(*pbuf); *pbuf = NULL;
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                      "internal", "MIN001",
                      "str-replace: result size overflow");
        return -1;
    }
    need = *plen + n + 1;
    if (need > *pcap) {
        size_t new_cap = *pcap == 0 ? 256 : *pcap;
        char  *nb;
        while (new_cap < need) {
            if (new_cap > SIZE_MAX / 2) {
                free(*pbuf); *pbuf = NULL;
                set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                              "internal", "MIN001",
                              "str-replace: result size overflow");
                return -1;
            }
            new_cap *= 2;
        }
        nb = (char *)realloc(*pbuf, new_cap);
        if (nb == NULL) {
            free(*pbuf); *pbuf = NULL;
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                          "internal", "MIN001", "out of memory");
            return -1;
        }
        *pbuf = nb;
        *pcap = new_cap;
    }
    if (n > 0) memcpy(*pbuf + *plen, src, n);
    *plen += n;
    return 0;
}

/* JVM-style replacement-template expansion. Recognised escapes:
 *   \$  -> literal $
 *   \\  -> literal \
 *   $N  -> capture group N (0 = whole match, 1..9 = positional groups).
 * Other characters are copied verbatim. `text_base` plus the offsets in
 * `g` and `match_idx` resolves to absolute group bytes in the search
 * input. Returns 0 on success or -1 on error (S has the diag). */
static int str_replace_expand_template(mino_state_t *S,
                                       char **pbuf, size_t *plen, size_t *pcap,
                                       const char *tmpl, size_t tlen,
                                       const char *text_base,
                                       int match_idx, int match_len,
                                       const re_groups_t *g)
{
    size_t i = 0;
    while (i < tlen) {
        char c = tmpl[i];
        if (c == '\\' && i + 1 < tlen) {
            if (str_replace_buf_append(S, pbuf, plen, pcap, tmpl + i + 1, 1) < 0)
                return -1;
            i += 2;
        } else if (c == '$' && i + 1 < tlen
                   && tmpl[i + 1] >= '0' && tmpl[i + 1] <= '9') {
            int n = tmpl[i + 1] - '0';
            if (n == 0) {
                if (str_replace_buf_append(S, pbuf, plen, pcap,
                                           text_base + match_idx,
                                           (size_t)match_len) < 0)
                    return -1;
            } else if (n <= g->n) {
                int gs = g->starts[n - 1], ge = g->ends[n - 1];
                if (gs >= 0 && ge >= gs) {
                    if (str_replace_buf_append(S, pbuf, plen, pcap,
                                               text_base + gs,
                                               (size_t)(ge - gs)) < 0)
                        return -1;
                }
                /* unmatched group: contributes nothing, mirroring JVM */
            } else {
                free(*pbuf); *pbuf = NULL;
                prim_throw_classified(S, "eval/contract", "MCT001",
                    "str-replace: replacement references missing capture group");
                return -1;
            }
            i += 2;
        } else {
            if (str_replace_buf_append(S, pbuf, plen, pcap, &c, 1) < 0)
                return -1;
            i += 1;
        }
    }
    return 0;
}

/* Build the match value handed to a callable replacement: the whole-
 * match string when the pattern has no capture groups, or
 * `[whole g1 g2 ...]` when it does. Returns NULL on allocation failure
 * (diag already set). */
static mino_val_t *str_replace_match_arg(mino_state_t *S,
                                         const char *text_base,
                                         int match_idx, int match_len,
                                         const re_groups_t *g)
{
    if (g->n == 0) {
        return mino_string_n(S, text_base + match_idx, (size_t)match_len);
    }
    {
        mino_val_t *items[1 + RE_MAX_GROUPS];
        size_t      n = 1;
        int         i;
        items[0] = mino_string_n(S, text_base + match_idx, (size_t)match_len);
        for (i = 0; i < g->n; i++) {
            if (g->starts[i] < 0 || g->ends[i] < 0
             || g->ends[i] < g->starts[i]) {
                items[n++] = mino_nil(S);
            } else {
                items[n++] = mino_string_n(S, text_base + g->starts[i],
                                           (size_t)(g->ends[i] - g->starts[i]));
            }
        }
        return mino_vector(S, items, n);
    }
}

/* (str-replace s match replacement)
 *
 *   match=string : literal substring replacement (single-pass).
 *   match=regex  : regex-driven replacement. When `replacement` is a
 *                  string, $N references and \$ / \\ escapes are
 *                  expanded JVM-style; otherwise `replacement` is
 *                  called as a fn with the match value (a string when
 *                  the pattern has no groups, or `[whole g1 g2 ...]`
 *                  when it does) and its (str-coerced) result is used
 *                  literally. */
mino_val_t *prim_str_replace(mino_state_t *S, mino_val_t *args,
                             mino_env_t *env)
{
    mino_val_t *s_val, *match_val, *repl_val;
    const char *s;
    size_t      slen;
    char       *buf = NULL;
    size_t      buf_len = 0, buf_cap = 0;
    (void)env;

    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || !mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "str-replace requires three arguments");
    }
    s_val     = args->as.cons.car;
    match_val = args->as.cons.cdr->as.cons.car;
    repl_val  = args->as.cons.cdr->as.cons.cdr->as.cons.car;
    if (s_val == NULL || mino_type_of(s_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "str-replace: first argument must be a string");
    }
    if (match_val == NULL
        || (mino_type_of(match_val) != MINO_STRING
            && mino_type_of(match_val) != MINO_REGEX)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "str-replace: match must be a string or regex");
    }
    s    = s_val->as.s.data;
    slen = s_val->as.s.len;

    /* ----- literal-string match: the original fast path ----- */
    if (mino_type_of(match_val) == MINO_STRING) {
        const char *match, *repl, *p;
        size_t mlen, rlen;
        if (repl_val == NULL || mino_type_of(repl_val) != MINO_STRING) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "str-replace: replacement must be a string when match is a string");
        }
        match = match_val->as.s.data; mlen = match_val->as.s.len;
        repl  = repl_val->as.s.data;  rlen = repl_val->as.s.len;
        if (mlen == 0) return s_val;
        buf_cap = slen + 256;
        buf = (char *)malloc(buf_cap);
        if (buf == NULL) {
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                          "internal", "MIN001", "out of memory");
            return NULL;
        }
        p = s;
        while (p < s + slen) {
            const char *found = NULL;
            const char *q;
            for (q = p; q + mlen <= s + slen; q++) {
                if (memcmp(q, match, mlen) == 0) { found = q; break; }
            }
            if (found != NULL) {
                size_t prefix_len = (size_t)(found - p);
                if (str_replace_buf_append(S, &buf, &buf_len, &buf_cap,
                                           p, prefix_len) < 0) return NULL;
                if (str_replace_buf_append(S, &buf, &buf_len, &buf_cap,
                                           repl, rlen) < 0) return NULL;
                p = found + mlen;
            } else {
                size_t tail_len = (size_t)(s + slen - p);
                if (str_replace_buf_append(S, &buf, &buf_len, &buf_cap,
                                           p, tail_len) < 0) return NULL;
                break;
            }
        }
        {
            mino_val_t *result = mino_string_n(S, buf, buf_len);
            free(buf);
            return result;
        }
    }

    /* ----- regex match ----- */
    {
        const char *pat_src;
        re_t        compiled;
        size_t      pos = 0;
        int         repl_is_string = (repl_val != NULL
                                      && mino_type_of(repl_val) == MINO_STRING);
        const char *tmpl = repl_is_string ? repl_val->as.s.data  : NULL;
        size_t      tlen = repl_is_string ? repl_val->as.s.len   : 0;
        if (match_val->as.regex.source == NULL
            || mino_type_of(match_val->as.regex.source) != MINO_STRING) {
            return prim_throw_classified(S, "eval/contract", "MCT001",
                "str-replace: regex has no source pattern");
        }
        pat_src  = match_val->as.regex.source->as.s.data;
        compiled = re_compile(pat_src);
        if (compiled == NULL) {
            return prim_throw_classified(S, "eval/contract", "MCT001",
                "str-replace: invalid regex pattern");
        }
        buf_cap = slen + 256;
        buf = (char *)malloc(buf_cap);
        if (buf == NULL) {
            re_free(compiled);
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                          "internal", "MIN001", "out of memory");
            return NULL;
        }
        while (pos <= slen) {
            int         match_len = 0;
            re_groups_t groups;
            int         idx;
            idx = re_matchp_groups(compiled, s + pos, &match_len, &groups);
            if (idx < 0) {
                /* No more matches: copy the tail and finish. */
                if (str_replace_buf_append(S, &buf, &buf_len, &buf_cap,
                                           s + pos, slen - pos) < 0) {
                    re_free(compiled); return NULL;
                }
                break;
            }
            /* Emit the prefix between `pos` and the match start. */
            if (str_replace_buf_append(S, &buf, &buf_len, &buf_cap,
                                       s + pos, (size_t)idx) < 0) {
                re_free(compiled); return NULL;
            }
            /* Emit the replacement: template expansion or fn call. */
            if (repl_is_string) {
                if (str_replace_expand_template(S, &buf, &buf_len, &buf_cap,
                                                tmpl, tlen,
                                                s + pos, idx, match_len,
                                                &groups) < 0) {
                    re_free(compiled);
                    return NULL;
                }
            } else {
                mino_val_t *argv1[1];
                mino_val_t *call_arg;
                mino_val_t *call_res;
                call_arg = str_replace_match_arg(S, s + pos, idx, match_len,
                                                 &groups);
                if (call_arg == NULL) {
                    free(buf); re_free(compiled); return NULL;
                }
                argv1[0] = call_arg;
                call_res = apply_callable_argv(S, repl_val, argv1, 1, env);
                if (call_res == NULL) {
                    free(buf); re_free(compiled); return NULL;
                }
                if (mino_type_of(call_res) == MINO_STRING) {
                    if (str_replace_buf_append(S, &buf, &buf_len, &buf_cap,
                                               call_res->as.s.data,
                                               call_res->as.s.len) < 0) {
                        re_free(compiled); return NULL;
                    }
                } else if (mino_type_of(call_res) == MINO_CHAR) {
                    /* Single-codepoint result: render as the literal char
                     * byte. Avoids forcing every fn-result through `str`
                     * for the common upper/lower-case style mapping. */
                    char cb = (char)(call_res->as.ch & 0xff);
                    if (str_replace_buf_append(S, &buf, &buf_len, &buf_cap,
                                               &cb, 1) < 0) {
                        re_free(compiled); return NULL;
                    }
                } else {
                    free(buf); re_free(compiled);
                    return prim_throw_classified(S, "eval/type", "MTY001",
                        "str-replace: fn replacement must return a string or char");
                }
            }
            /* Advance past the match. For zero-width matches, step by
             * one byte to avoid an infinite loop -- copy the byte at
             * the match site so the result mirrors the input shape. */
            if (match_len <= 0) {
                if (pos + (size_t)idx >= slen) {
                    /* Zero-width match at end-of-string: nothing left. */
                    break;
                }
                if (str_replace_buf_append(S, &buf, &buf_len, &buf_cap,
                                           s + pos + idx, 1) < 0) {
                    re_free(compiled); return NULL;
                }
                pos += (size_t)idx + 1;
            } else {
                pos += (size_t)idx + (size_t)match_len;
            }
        }
        re_free(compiled);
        {
            mino_val_t *result = mino_string_n(S, buf, buf_len);
            free(buf);
            return result;
        }
    }
}

mino_val_t *prim_starts_with_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s, *prefix;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "starts-with? requires two string arguments");
    }
    s      = args->as.cons.car;
    prefix = args->as.cons.cdr->as.cons.car;
    if (s == NULL || mino_type_of(s) != MINO_STRING
        || prefix == NULL || mino_type_of(prefix) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "starts-with? requires two string arguments");
    }
    if (prefix->as.s.len > s->as.s.len) return mino_false(S);
    return memcmp(s->as.s.data, prefix->as.s.data, prefix->as.s.len) == 0
        ? mino_true(S) : mino_false(S);
}

mino_val_t *prim_ends_with_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s, *suffix;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "ends-with? requires two string arguments");
    }
    s      = args->as.cons.car;
    suffix = args->as.cons.cdr->as.cons.car;
    if (s == NULL || mino_type_of(s) != MINO_STRING
        || suffix == NULL || mino_type_of(suffix) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "ends-with? requires two string arguments");
    }
    if (suffix->as.s.len > s->as.s.len) return mino_false(S);
    return memcmp(s->as.s.data + s->as.s.len - suffix->as.s.len,
                  suffix->as.s.data, suffix->as.s.len) == 0
        ? mino_true(S) : mino_false(S);
}

mino_val_t *prim_includes_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s, *sub;
    const char *p;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "includes? requires two string arguments");
    }
    s   = args->as.cons.car;
    sub = args->as.cons.cdr->as.cons.car;
    if (s == NULL || mino_type_of(s) != MINO_STRING
        || sub == NULL || mino_type_of(sub) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "includes? requires two string arguments");
    }
    if (sub->as.s.len == 0) return mino_true(S);
    if (sub->as.s.len > s->as.s.len) return mino_false(S);
    for (p = s->as.s.data; p + sub->as.s.len <= s->as.s.data + s->as.s.len; p++) {
        if (memcmp(p, sub->as.s.data, sub->as.s.len) == 0) {
            return mino_true(S);
        }
    }
    return mino_false(S);
}

mino_val_t *prim_upper_case(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s;
    char       *buf;
    size_t      i;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "upper-case requires one string argument");
    }
    s = args->as.cons.car;
    if (s == NULL || mino_type_of(s) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "upper-case requires one string argument");
    }
    buf = (char *)malloc(s->as.s.len);
    if (buf == NULL && s->as.s.len > 0) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "internal", "MIN001", "out of memory"); return NULL;
    }
    for (i = 0; i < s->as.s.len; i++) {
        buf[i] = (char)toupper((unsigned char)s->as.s.data[i]);
    }
    {
        mino_val_t *result = mino_string_n(S, buf, s->as.s.len);
        free(buf);
        return result;
    }
}

mino_val_t *prim_lower_case(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s;
    char       *buf;
    size_t      i;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "lower-case requires one string argument");
    }
    s = args->as.cons.car;
    if (s == NULL || mino_type_of(s) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "lower-case requires one string argument");
    }
    buf = (char *)malloc(s->as.s.len);
    if (buf == NULL && s->as.s.len > 0) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "internal", "MIN001", "out of memory"); return NULL;
    }
    for (i = 0; i < s->as.s.len; i++) {
        buf[i] = (char)tolower((unsigned char)s->as.s.data[i]);
    }
    {
        mino_val_t *result = mino_string_n(S, buf, s->as.s.len);
        free(buf);
        return result;
    }
}

mino_val_t *prim_trim(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s;
    const char *start, *end_ptr;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "trim requires one string argument");
    }
    s = args->as.cons.car;
    if (s == NULL || mino_type_of(s) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "trim requires one string argument");
    }
    start   = s->as.s.data;
    end_ptr = s->as.s.data + s->as.s.len;
    while (start < end_ptr && isspace((unsigned char)*start)) start++;
    while (end_ptr > start && isspace((unsigned char)*(end_ptr - 1))) end_ptr--;
    return mino_string_n(S, start, (size_t)(end_ptr - start));
}

/*
 * (str & args) — concatenate printed representations. Strings contribute
 * their raw content (no quotes); everything else uses the printer form.
 */
mino_val_t *prim_str(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    char  *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    (void)env;
    while (mino_is_cons(args)) {
        mino_val_t *a = args->as.cons.car;
        if (a != NULL && mino_type_of(a) == MINO_STRING) {
            /* Append raw string content without quotes. */
            size_t need = len + a->as.s.len + 1;
            if (need > cap) {
                char *newbuf;
                cap = cap == 0 ? 128 : cap;
                while (cap < need) cap *= 2;
                newbuf = (char *)realloc(buf, cap);
                if (newbuf == NULL) { free(buf); set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "internal", "MIN001", "out of memory"); return NULL; }
                buf = newbuf;
            }
            memcpy(buf + len, a->as.s.data, a->as.s.len);
            len += a->as.s.len;
        } else if (a != NULL && mino_type_of(a) == MINO_NIL) {
            /* nil contributes nothing. */
        } else if (a == NULL) {
            /* NULL treated as nil. */
        } else {
            /* Print to a temp buffer using the standard printer. */
            char tmp[256];
            int  n;
            switch (mino_type_of(a)) {
            case MINO_BOOL:
                n = snprintf(tmp, sizeof(tmp), "%s", mino_val_bool_get(a) ? "true" : "false");
                break;
            case MINO_INT:
                n = snprintf(tmp, sizeof(tmp), "%lld", mino_val_int_get(a));
                break;
            case MINO_BIGINT: {
                /* `str` strips the readable-form N suffix. */
                char *digits = mino_bigint_to_cstr(a);
                if (digits != NULL) {
                    size_t plen = strlen(digits);
                    size_t need2 = len + plen + 1;
                    if (need2 > cap) {
                        char *newbuf;
                        cap = cap == 0 ? 128 : cap;
                        while (cap < need2) cap *= 2;
                        newbuf = (char *)realloc(buf, cap);
                        if (newbuf == NULL) {
                            free(digits);
                            free(buf);
                            set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                                          "internal", "MIN001", "out of memory");
                            return NULL;
                        }
                        buf = newbuf;
                    }
                    memcpy(buf + len, digits, plen);
                    len += plen;
                    free(digits);
                }
                n = 0;
                break;
            }
            case MINO_BIGDEC: {
                /* `str` strips the readable-form M suffix. */
                char *digits = mino_bigint_to_cstr(a->as.bigdec.unscaled);
                if (digits != NULL) {
                    int scale = a->as.bigdec.scale;
                    int neg = (digits[0] == '-');
                    int dlen = (int)strlen(digits);
                    char fb[256];
                    int fn2 = 0;
                    if (scale == 0) {
                        fn2 = snprintf(fb, sizeof(fb), "%s", digits);
                    } else {
                        int int_part_len = dlen - (neg ? 1 : 0) - scale;
                        if (int_part_len > 0) {
                            int j, k = 0;
                            for (j = 0; j < (neg ? 1 : 0) + int_part_len &&
                                        k < (int)sizeof(fb) - 1; j++) {
                                fb[k++] = digits[j];
                            }
                            if (k < (int)sizeof(fb) - 1) fb[k++] = '.';
                            for (; j < dlen && k < (int)sizeof(fb) - 1; j++) {
                                fb[k++] = digits[j];
                            }
                            fb[k] = '\0';
                            fn2 = k;
                        } else {
                            int pad;
                            int k = 0;
                            if (neg && k < (int)sizeof(fb) - 1) fb[k++] = '-';
                            if (k + 2 < (int)sizeof(fb)) {
                                fb[k++] = '0'; fb[k++] = '.';
                            }
                            for (pad = 0; pad < -int_part_len &&
                                          k < (int)sizeof(fb) - 1; pad++) {
                                fb[k++] = '0';
                            }
                            {
                                const char *src = digits + (neg ? 1 : 0);
                                while (*src && k < (int)sizeof(fb) - 1) {
                                    fb[k++] = *src++;
                                }
                            }
                            fb[k] = '\0';
                            fn2 = k;
                        }
                    }
                    free(digits);
                    if (fn2 > 0) {
                        size_t need2 = len + (size_t)fn2 + 1;
                        if (need2 > cap) {
                            char *newbuf;
                            cap = cap == 0 ? 128 : cap;
                            while (cap < need2) cap *= 2;
                            newbuf = (char *)realloc(buf, cap);
                            if (newbuf == NULL) {
                                free(buf);
                                set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                                              "internal", "MIN001", "out of memory");
                                return NULL;
                            }
                            buf = newbuf;
                        }
                        memcpy(buf + len, fb, (size_t)fn2);
                        len += (size_t)fn2;
                    }
                }
                n = 0;
                break;
            }
            case MINO_CHAR: {
                /* str of a char emits the codepoint's UTF-8 encoding. */
                unsigned cp = (unsigned)mino_val_char_get(a);
                if (cp <= 0x7F) {
                    tmp[0] = (char)cp; n = 1;
                } else if (cp <= 0x7FF) {
                    tmp[0] = (char)(0xC0 | (cp >> 6));
                    tmp[1] = (char)(0x80 | (cp & 0x3F));
                    n = 2;
                } else if (cp <= 0xFFFF) {
                    tmp[0] = (char)(0xE0 | (cp >> 12));
                    tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    tmp[2] = (char)(0x80 | (cp & 0x3F));
                    n = 3;
                } else {
                    tmp[0] = (char)(0xF0 | (cp >> 18));
                    tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                    tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    tmp[3] = (char)(0x80 | (cp & 0x3F));
                    n = 4;
                }
                tmp[n] = '\0';
                break;
            }
            case MINO_FLOAT: {
                if (isnan(a->as.f)) {
                    n = snprintf(tmp, sizeof(tmp), "NaN");
                } else if (isinf(a->as.f)) {
                    n = snprintf(tmp, sizeof(tmp), "%sInfinity",
                                 a->as.f > 0 ? "" : "-");
                } else {
                    char fb[64];
                    int fn2 = snprintf(fb, sizeof(fb), "%g", a->as.f);
                    int needs_dot = 1, k;
                    for (k = 0; k < fn2; k++) {
                        if (fb[k] == '.' || fb[k] == 'e' || fb[k] == 'E') {
                            needs_dot = 0; break;
                        }
                    }
                    if (needs_dot) {
                        fb[fn2] = '.'; fb[fn2+1] = '0'; fb[fn2+2] = '\0';
                        fn2 += 2;
                    }
                    n = fn2;
                    memcpy(tmp, fb, (size_t)n + 1);
                }
                break;
            }
            case MINO_SYMBOL:
            case MINO_KEYWORD: {
                size_t slen = a->as.s.len;
                int    off  = mino_type_of(a) == MINO_KEYWORD ? 1 : 0;
                if (off + slen + 1 > sizeof(tmp)) slen = sizeof(tmp) - off - 1;
                if (off) tmp[0] = ':';
                memcpy(tmp + off, a->as.s.data, slen);
                n = (int)(off + slen);
                tmp[n] = '\0';
                break;
            }
            default: {
                /* Collections (vector, map, set, cons, lazy, atom) and
                 * opaque types: print via the standard printer so str
                 * produces readable output, not #<?>. */
                mino_val_t *printed = print_to_string(S, a);
                if (printed != NULL && mino_type_of(printed) == MINO_STRING) {
                    size_t plen = printed->as.s.len;
                    size_t need2 = len + plen + 1;
                    if (need2 > cap) {
                        char *newbuf;
                        cap = cap == 0 ? 128 : cap;
                        while (cap < need2) cap *= 2;
                        newbuf = (char *)realloc(buf, cap);
                        if (newbuf == NULL) { free(buf); set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "internal", "MIN001", "out of memory"); return NULL; }
                        buf = newbuf;
                    }
                    memcpy(buf + len, printed->as.s.data, plen);
                    len += plen;
                    n = 0; /* already appended */
                } else {
                    n = snprintf(tmp, sizeof(tmp), "#<%s>",
                                 mino_type_of(a) == MINO_PRIM ? "prim" :
                                 mino_type_of(a) == MINO_FN   ? "fn" :
                                 mino_type_of(a) == MINO_MACRO ? "macro" :
                                 mino_type_of(a) == MINO_HANDLE ? "handle" : "?");
                }
                break;
            }
            }
            if (n > 0) {
                size_t need = len + (size_t)n + 1;
                if (need > cap) {
                    char *newbuf;
                    cap = cap == 0 ? 128 : cap;
                    while (cap < need) cap *= 2;
                    newbuf = (char *)realloc(buf, cap);
                    if (newbuf == NULL) { free(buf); set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "internal", "MIN001", "out of memory"); return NULL; }
                    buf = newbuf;
                }
                memcpy(buf + len, tmp, (size_t)n);
                len += (size_t)n;
            }
        }
        args = args->as.cons.cdr;
    }
    {
        mino_val_t *result = mino_string_n(S, buf != NULL ? buf : "", len);
        free(buf);
        return result;
    }
}

/* mino_uuid_from_bytes -- copy 16 bytes into a freshly-allocated
 * MINO_UUID. */
mino_val_t *mino_uuid_from_bytes(mino_state_t *S, const unsigned char *b)
{
    mino_val_t *v = alloc_val(S, MINO_UUID);
    if (v == NULL) return NULL;
    memcpy(v->as.uuid.bytes, b, 16);
    return v;
}

/* mino_uuid_parse -- parse the canonical 8-4-4-4-12 hex form into
 * `out_bytes` (16 bytes). Returns 1 on success, 0 if the input is
 * malformed. Accepts upper-case and lower-case hex. */
static int hex_nibble(int c, unsigned *out)
{
    if (c >= '0' && c <= '9') { *out = (unsigned)(c - '0');      return 1; }
    if (c >= 'a' && c <= 'f') { *out = (unsigned)(c - 'a' + 10); return 1; }
    if (c >= 'A' && c <= 'F') { *out = (unsigned)(c - 'A' + 10); return 1; }
    return 0;
}
int mino_uuid_parse(const char *s, size_t len, unsigned char out[16])
{
    /* Layout: 8-4-4-4-12 with `-` at indices 8, 13, 18, 23. */
    static const int dashes[4] = {8, 13, 18, 23};
    size_t i;
    int    di = 0;
    int    bi = 0;
    if (len != 36) return 0;
    for (i = 0; i < 36; i++) {
        if (di < 4 && (int)i == dashes[di]) {
            if (s[i] != '-') return 0;
            di++;
        } else {
            unsigned hi, lo;
            if (!hex_nibble((unsigned char)s[i], &hi)) return 0;
            i++;
            if (i >= 36) return 0;
            if (!hex_nibble((unsigned char)s[i], &lo)) return 0;
            out[bi++] = (unsigned char)((hi << 4) | lo);
        }
    }
    return bi == 16;
}

/* (random-uuid) — generate a UUID v4. */
mino_val_t *prim_random_uuid(mino_state_t *S, mino_val_t *args,
                             mino_env_t *env)
{
    unsigned char bytes[16];
    int i;
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "random-uuid takes no arguments");
    }
    for (i = 0; i < 16; i += 8) {
        uint64_t r = state_rand64(S);
        bytes[i    ] = (unsigned char)(r      );
        bytes[i + 1] = (unsigned char)(r >>  8);
        bytes[i + 2] = (unsigned char)(r >> 16);
        bytes[i + 3] = (unsigned char)(r >> 24);
        bytes[i + 4] = (unsigned char)(r >> 32);
        bytes[i + 5] = (unsigned char)(r >> 40);
        bytes[i + 6] = (unsigned char)(r >> 48);
        bytes[i + 7] = (unsigned char)(r >> 56);
    }
    bytes[6] = (unsigned char)((bytes[6] & 0x0F) | 0x40); /* version 4 */
    bytes[8] = (unsigned char)((bytes[8] & 0x3F) | 0x80); /* variant 1 */
    return mino_uuid_from_bytes(S, bytes);
}

/* (parse-uuid s) — parse a canonical UUID string into a UUID value.
 * Strict canonical form (36 chars, dashes at 8/13/18/23). Throws on
 * non-string input; returns nil for strings that fail the strict
 * form. */
mino_val_t *prim_parse_uuid(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s;
    unsigned char bytes[16];
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "parse-uuid requires one argument");
    }
    s = args->as.cons.car;
    if (s == NULL || mino_type_of(s) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "parse-uuid: argument must be a string");
    }
    if (!mino_uuid_parse(s->as.s.data, s->as.s.len, bytes)) {
        return mino_nil(S);
    }
    return mino_uuid_from_bytes(S, bytes);
}

/* Core string operations live in clojure.core: the always-available
 * conversion and formatting primitives that have no Clojure-side
 * namespace either (str, format, name, namespace, subs, ...). */
const mino_prim_def k_prims_string[] = {
    {"str",          prim_str,
     "Returns the string representation of the arguments concatenated."},
    {"pr-str",       prim_pr_str,
     "Returns a readable string representation of the arguments."},
    {"read-string",  prim_read_string,
     "Reads one form from the string."},
    {"format",       prim_format,
     "Returns a formatted string using a format specifier and arguments."},
    {"char-at",      prim_char_at,
     "Returns the character at the given index as a string."},
    {"subs",         prim_subs,
     "Returns a substring from start (inclusive) to end (exclusive)."},
    {"parse-uuid",   prim_parse_uuid,
     "Parses s as a UUID; returns a UUID value or nil if s is not a "
     "valid canonical UUID string."},
    {"random-uuid",  prim_random_uuid,
     "Returns a random UUID v4 string."},
};

const size_t k_prims_string_count =
    sizeof(k_prims_string) / sizeof(k_prims_string[0]);

/* Operations that match Clojure's clojure.string namespace. mino
 * installs these into clojure.string directly so user code that
 * refers them in via :require works the way Clojure programmers
 * expect, and so :refer-clojure :exclude doesn't accidentally
 * shadow them in a fresh namespace. */
const mino_prim_def k_prims_clojure_string[] = {
    {"split",        prim_split,
     "Splits a string on a regex pattern."},
    {"join",         prim_join,
     "Returns a string of the items in coll joined by separator."},
    {"replace",      prim_str_replace,
     "Replaces all occurrences of match in s with replacement."},
    {"starts-with?", prim_starts_with_p,
     "Returns true if the string starts with the given prefix."},
    {"ends-with?",   prim_ends_with_p,
     "Returns true if the string ends with the given suffix."},
    {"includes?",    prim_includes_p,
     "Returns true if the string contains the given substring."},
    {"upper-case",   prim_upper_case,
     "Returns the string converted to upper case."},
    {"lower-case",   prim_lower_case,
     "Returns the string converted to lower case."},
    {"trim",         prim_trim,
     "Returns the string with leading and trailing whitespace removed."},
};

const size_t k_prims_clojure_string_count =
    sizeof(k_prims_clojure_string) / sizeof(k_prims_clojure_string[0]);
