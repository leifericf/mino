/*
 * string.c -- string primitives: str, pr-str, format, read-string,
 *                  char-at, subs, split, join, starts-with?, ends-with?,
 *                  includes?, upper-case, lower-case, trim.
 */

#include "prim/internal.h"

/* Grow `buf` so that `len + extra + 1` bytes fit. Returns the (possibly
 * realloc'd) buffer, or NULL if allocation failed (in which case an
 * MIN001 diagnostic has already been recorded on S). The caller updates
 * its own `cap` via cap_ptr; `buf` is replaced by the return value. */
static inline char *fmt_ensure(mino_state_t *S, char *buf,
                               size_t len, size_t *cap_ptr, size_t extra)
{
    size_t need = len + extra + 1;
    if (need > *cap_ptr) {
        size_t newcap = *cap_ptr == 0 ? 128 : *cap_ptr;
        char  *newbuf;
        while (newcap < need) newcap *= 2;
        newbuf = (char *)realloc(buf, newcap);
        if (newbuf == NULL) {
            set_eval_diag(S, S->eval_current_form, "internal", "MIN001",
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
    if (fmt_val == NULL || fmt_val->type != MINO_STRING) {
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
                if (a != NULL && a->type == MINO_STRING) {
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
    mino_val_t *result;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "read-string requires one string argument");
    }
    s = args->as.cons.car;
    if (s == NULL || s->type != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "read-string: argument must be a string");
    }
    clear_error(S);
    result = mino_read(S, s->as.s.data, NULL);
    if (result == NULL && mino_last_error(S) != NULL) {
        /* Throw parse errors as catchable exceptions so user code can
         * handle them via try/catch. */
        mino_val_t *ex = mino_string(S, mino_last_error(S));
        if (S->try_depth > 0) {
            S->try_stack[S->try_depth - 1].exception = ex;
            longjmp(S->try_stack[S->try_depth - 1].buf, 1);
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
            cap = cap == 0 ? 128 : cap;
            while (cap < need) cap *= 2;
            buf = (char *)realloc(buf, cap);
            if (buf == NULL) { set_eval_diag(S, S->eval_current_form, "internal", "MIN001", "out of memory"); return NULL; }
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
    if (s == NULL || s->type != MINO_STRING
        || idx_val == NULL || idx_val->type != MINO_INT) {
        return prim_throw_classified(S, "eval/type", "MTY001", "char-at: requires a string and integer index");
    }
    idx = idx_val->as.i;
    if (idx < 0 || (size_t)idx >= s->as.s.len) {
        return prim_throw_classified(S, "eval/bounds", "MBD001", "char-at: index out of range");
    }
    return mino_string_n(S, s->as.s.data + idx, 1);
}

/* ------------------------------------------------------------------------- */
/* String primitives                                                         */
/* ------------------------------------------------------------------------- */

mino_val_t *prim_subs(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s_val;
    long long   start, end_idx;
    size_t      n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 2 && n != 3) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "subs requires 2 or 3 arguments");
    }
    s_val = args->as.cons.car;
    if (s_val == NULL || s_val->type != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "subs: first argument must be a string");
    }
    if (args->as.cons.cdr->as.cons.car == NULL
        || args->as.cons.cdr->as.cons.car->type != MINO_INT) {
        return prim_throw_classified(S, "eval/type", "MTY001", "subs: start index must be an integer");
    }
    start = args->as.cons.cdr->as.cons.car->as.i;
    if (n == 3) {
        if (args->as.cons.cdr->as.cons.cdr->as.cons.car == NULL
            || args->as.cons.cdr->as.cons.cdr->as.cons.car->type != MINO_INT) {
            return prim_throw_classified(S, "eval/type", "MTY001", "subs: end index must be an integer");
        }
        end_idx = args->as.cons.cdr->as.cons.cdr->as.cons.car->as.i;
    } else {
        end_idx = (long long)s_val->as.s.len;
    }
    if (start < 0 || end_idx < start || (size_t)end_idx > s_val->as.s.len) {
        return prim_throw_classified(S, "eval/bounds", "MBD001", "subs: index out of range");
    }
    return mino_string_n(S, s_val->as.s.data + start, (size_t)(end_idx - start));
}

mino_val_t *prim_split(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t  *s_val;
    mino_val_t  *sep_val;
    const char  *s;
    size_t       slen;
    const char  *sep;
    size_t       sep_len;
    mino_val_t **buf = NULL;
    size_t       cap = 0, len = 0;
    const char  *p;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "split requires a string and a separator");
    }
    s_val   = args->as.cons.car;
    sep_val = args->as.cons.cdr->as.cons.car;
    if (s_val == NULL || s_val->type != MINO_STRING
        || sep_val == NULL || sep_val->type != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "split: both arguments must be strings");
    }
    s       = s_val->as.s.data;
    slen    = s_val->as.s.len;
    sep     = sep_val->as.s.data;
    sep_len = sep_val->as.s.len;
    p       = s;
    if (sep_len == 0) {
        /* Split into individual characters. */
        size_t i;
        buf = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR,
              (slen > 0 ? slen : 1) * sizeof(*buf));
        for (i = 0; i < slen; i++) {
            buf[i] = mino_string_n(S, s + i, 1);
        }
        return mino_vector(S, buf, slen);
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
        if (sep_val != NULL && sep_val->type == MINO_STRING) {
            sep     = sep_val->as.s.data;
            sep_len = sep_val->as.s.len;
        } else if (sep_val != NULL && sep_val->type != MINO_NIL) {
            return prim_throw_classified(S, "eval/type", "MTY001", "join: separator must be a string or nil");
        }
    } else {
        return prim_throw_classified(S, "eval/arity", "MAR001", "join requires 1 or 2 arguments");
    }
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_string(S, "");
    }
    seq_iter_init(S, &it, coll);
    while (!seq_iter_done(&it)) {
        mino_val_t *elem = seq_iter_val(S, &it);
        const char *part;
        size_t      part_len;
        size_t      need;
        if (elem == NULL || elem->type == MINO_NIL) {
            seq_iter_next(S, &it);
            continue;
        }
        if (elem->type == MINO_STRING) {
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
            buf_cap = buf_cap == 0 ? 128 : buf_cap;
            while (buf_cap < need) buf_cap *= 2;
            buf = (char *)realloc(buf, buf_cap);
            if (buf == NULL) { set_eval_diag(S, S->eval_current_form, "internal", "MIN001", "out of memory"); return NULL; }
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

/* (str-replace s match replacement) -- single-pass string replacement. */
mino_val_t *prim_str_replace(mino_state_t *S, mino_val_t *args,
                             mino_env_t *env)
{
    mino_val_t *s_val, *match_val, *repl_val;
    const char *s, *match, *repl, *p;
    size_t slen, mlen, rlen;
    char  *buf = NULL;
    size_t buf_len = 0, buf_cap = 0;
    (void)env;

    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || !mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "str-replace requires three string arguments");
    }
    s_val     = args->as.cons.car;
    match_val = args->as.cons.cdr->as.cons.car;
    repl_val  = args->as.cons.cdr->as.cons.cdr->as.cons.car;
    if (s_val == NULL || s_val->type != MINO_STRING
        || match_val == NULL || match_val->type != MINO_STRING
        || repl_val == NULL || repl_val->type != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "str-replace: all arguments must be strings");
    }
    s    = s_val->as.s.data;     slen = s_val->as.s.len;
    match = match_val->as.s.data; mlen = match_val->as.s.len;
    repl  = repl_val->as.s.data;  rlen = repl_val->as.s.len;

    if (mlen == 0) return s_val; /* empty match: return original */

    buf_cap = slen + 256;
    buf = (char *)malloc(buf_cap);
    if (buf == NULL) {
        set_eval_diag(S, S->eval_current_form, "internal", "MIN001",
                      "out of memory");
        return NULL;
    }

    p = s;
    while (p < s + slen) {
        const char *found = NULL;
        const char *q;
        for (q = p; q + mlen <= s + slen; q++) {
            if (memcmp(q, match, mlen) == 0) {
                found = q;
                break;
            }
        }
        if (found != NULL) {
            size_t prefix_len = (size_t)(found - p);
            size_t need = buf_len + prefix_len + rlen;
            if (need > buf_cap) {
                while (buf_cap < need) buf_cap = buf_cap * 2;
                buf = (char *)realloc(buf, buf_cap);
                if (buf == NULL) { set_eval_diag(S, S->eval_current_form, "internal", "MIN001", "out of memory"); return NULL; }
            }
            memcpy(buf + buf_len, p, prefix_len);
            buf_len += prefix_len;
            memcpy(buf + buf_len, repl, rlen);
            buf_len += rlen;
            p = found + mlen;
        } else {
            size_t tail_len = (size_t)(s + slen - p);
            size_t need = buf_len + tail_len;
            if (need > buf_cap) {
                while (buf_cap < need) buf_cap = buf_cap * 2;
                buf = (char *)realloc(buf, buf_cap);
                if (buf == NULL) { set_eval_diag(S, S->eval_current_form, "internal", "MIN001", "out of memory"); return NULL; }
            }
            memcpy(buf + buf_len, p, tail_len);
            buf_len += tail_len;
            break;
        }
    }
    {
        mino_val_t *result = mino_string_n(S, buf, buf_len);
        free(buf);
        return result;
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
    if (s == NULL || s->type != MINO_STRING
        || prefix == NULL || prefix->type != MINO_STRING) {
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
    if (s == NULL || s->type != MINO_STRING
        || suffix == NULL || suffix->type != MINO_STRING) {
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
    if (s == NULL || s->type != MINO_STRING
        || sub == NULL || sub->type != MINO_STRING) {
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
    if (s == NULL || s->type != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "upper-case requires one string argument");
    }
    buf = (char *)malloc(s->as.s.len);
    if (buf == NULL && s->as.s.len > 0) {
        set_eval_diag(S, S->eval_current_form, "internal", "MIN001", "out of memory"); return NULL;
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
    if (s == NULL || s->type != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "lower-case requires one string argument");
    }
    buf = (char *)malloc(s->as.s.len);
    if (buf == NULL && s->as.s.len > 0) {
        set_eval_diag(S, S->eval_current_form, "internal", "MIN001", "out of memory"); return NULL;
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
    if (s == NULL || s->type != MINO_STRING) {
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
        if (a != NULL && a->type == MINO_STRING) {
            /* Append raw string content without quotes. */
            size_t need = len + a->as.s.len + 1;
            if (need > cap) {
                cap = cap == 0 ? 128 : cap;
                while (cap < need) cap *= 2;
                buf = (char *)realloc(buf, cap);
                if (buf == NULL) { set_eval_diag(S, S->eval_current_form, "internal", "MIN001", "out of memory"); return NULL; }
            }
            memcpy(buf + len, a->as.s.data, a->as.s.len);
            len += a->as.s.len;
        } else if (a != NULL && a->type == MINO_NIL) {
            /* nil contributes nothing. */
        } else if (a == NULL) {
            /* NULL treated as nil. */
        } else {
            /* Print to a temp buffer using the standard printer. */
            char tmp[256];
            int  n;
            switch (a->type) {
            case MINO_BOOL:
                n = snprintf(tmp, sizeof(tmp), "%s", a->as.b ? "true" : "false");
                break;
            case MINO_INT:
                n = snprintf(tmp, sizeof(tmp), "%lld", a->as.i);
                break;
            case MINO_CHAR: {
                /* str of a char emits the codepoint's UTF-8 encoding. */
                unsigned cp = (unsigned)a->as.ch;
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
                int    off  = a->type == MINO_KEYWORD ? 1 : 0;
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
                if (printed != NULL && printed->type == MINO_STRING) {
                    size_t plen = printed->as.s.len;
                    size_t need2 = len + plen + 1;
                    if (need2 > cap) {
                        cap = cap == 0 ? 128 : cap;
                        while (cap < need2) cap *= 2;
                        buf = (char *)realloc(buf, cap);
                        if (buf == NULL) { set_eval_diag(S, S->eval_current_form, "internal", "MIN001", "out of memory"); return NULL; }
                    }
                    memcpy(buf + len, printed->as.s.data, plen);
                    len += plen;
                    n = 0; /* already appended */
                } else {
                    n = snprintf(tmp, sizeof(tmp), "#<%s>",
                                 a->type == MINO_PRIM ? "prim" :
                                 a->type == MINO_FN   ? "fn" :
                                 a->type == MINO_MACRO ? "macro" :
                                 a->type == MINO_HANDLE ? "handle" : "?");
                }
                break;
            }
            }
            if (n > 0) {
                size_t need = len + (size_t)n + 1;
                if (need > cap) {
                    cap = cap == 0 ? 128 : cap;
                    while (cap < need) cap *= 2;
                    buf = (char *)realloc(buf, cap);
                    if (buf == NULL) { set_eval_diag(S, S->eval_current_form, "internal", "MIN001", "out of memory"); return NULL; }
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

/* (random-uuid) — generate a UUID v4 string. */
mino_val_t *prim_random_uuid(mino_state_t *S, mino_val_t *args,
                             mino_env_t *env)
{
    unsigned char bytes[16];
    char buf[37];
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
    sprintf(buf,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0], bytes[1], bytes[2],  bytes[3],
        bytes[4], bytes[5], bytes[6],  bytes[7],
        bytes[8], bytes[9], bytes[10], bytes[11],
        bytes[12], bytes[13], bytes[14], bytes[15]);
    return mino_string_n(S, buf, 36);
}

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
    {"split",        prim_split,
     "Splits a string on a regex pattern."},
    {"join",         prim_join,
     "Returns a string of the items in coll joined by separator."},
    {"str-replace",  prim_str_replace,
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
    {"random-uuid",  prim_random_uuid,
     "Returns a random UUID v4 string."},
};

const size_t k_prims_string_count =
    sizeof(k_prims_string) / sizeof(k_prims_string[0]);
