/*
 * public_embed.c -- host-facing embedder helpers.
 *
 *   mino_throw        Raise a mino exception from C. Routes through the
 *                     same try-stack longjmp path as (throw ...).
 *   mino_args_parse   Type-check and destructure a primitive's argument
 *                     list into C variables in one call, instead of a
 *                     hand-written chain of is_cons / type checks.
 */

#include "mino.h"
#include "public/internal_bridge.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>

/* ------------------------------------------------------------------------- */
/* mino_throw                                                                */
/* ------------------------------------------------------------------------- */

mino_val *mino_throw(mino_state *S, mino_val *ex)
{
    if (S == NULL) return NULL;

    if (mino_current_ctx(S)->try_depth <= 0) {
        /* No enclosing try: surface as a classified error. Route the
         * payload through pr-str-shaped formatting so non-string
         * payloads (keywords, maps, ex-info data) appear in the
         * diagnostic message instead of dropping. The structured
         * payload itself is also preserved via the same route as the
         * caught path so error_map's :mino/data carries it. */
        char msg[512];
        if (ex == NULL) {
            snprintf(msg, sizeof(msg), "uncaught exception");
        } else if (mino_type_of(ex) == MINO_STRING) {
            snprintf(msg, sizeof(msg), "uncaught exception: %.*s",
                     (int)ex->as.s.len, ex->as.s.data);
        } else {
            char buf[384];
            int  written = mino_print_to_buf(S, ex, buf, sizeof(buf));
            if (written <= 0) {
                snprintf(msg, sizeof(msg), "uncaught exception");
            } else {
                snprintf(msg, sizeof(msg), "uncaught exception: %s", buf);
            }
        }
        return prim_throw_classified(S, "user", "MUS001", msg);
    }

    mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth - 1].exception = ex;
    longjmp(mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth - 1].buf, 1);
    return NULL; /* unreachable */
}

/* ------------------------------------------------------------------------- */
/* mino_args_parse                                                           */
/* ------------------------------------------------------------------------- */

static const char *args_type_label(char spec)
{
    switch (spec) {
    case 'i': return "int";
    case 'f': return "number";
    case 's': case 'S': return "string";
    case 'k': return "keyword";
    case 'y': return "symbol";
    case 'b': return "bool";
    case 'c': return "char";
    case 'v': return "any";
    case 'V': return "vector";
    case 'M': return "map";
    case 'L': return "list";
    case 'H': return "handle";
    case 'A': return "atom";
    case 'B': return "bigint";
    case 'r': return "ratio";
    case 'd': return "bigdec";
    case 'R': return "record";
    case 'X': return "set";
    case 'F': return "callable";
    default:  return "?";
    }
}

static int args_type_match(char spec, const mino_val *v)
{
    if (v == NULL) return spec == 'v' || spec == 'L';
    switch (spec) {
    case 'i': return mino_val_int_p(v);
    case 'f': return mino_type_of(v) == MINO_FLOAT || mino_val_int_p(v);
    case 's': case 'S': return mino_type_of(v) == MINO_STRING;
    case 'k': return mino_type_of(v) == MINO_KEYWORD;
    case 'y': return mino_type_of(v) == MINO_SYMBOL;
    case 'b': return mino_type_of(v) == MINO_BOOL;
    case 'c': return mino_type_of(v) == MINO_CHAR;
    case 'v': return 1;
    case 'V': return mino_type_of(v) == MINO_VECTOR;
    case 'M': return mino_type_of(v) == MINO_MAP || mino_type_of(v) == MINO_SORTED_MAP;
    case 'L': return mino_type_of(v) == MINO_CONS || mino_type_of(v) == MINO_NIL;
    case 'H': return mino_type_of(v) == MINO_HANDLE;
    case 'A': return mino_type_of(v) == MINO_ATOM;
    case 'B': return mino_type_of(v) == MINO_BIGINT;
    case 'r': return mino_type_of(v) == MINO_RATIO;
    case 'd': return mino_type_of(v) == MINO_BIGDEC;
    case 'R': return mino_type_of(v) == MINO_RECORD;
    case 'X': return mino_type_of(v) == MINO_SET || mino_type_of(v) == MINO_SORTED_SET;
    case 'F': {
        mino_type t = mino_type_of(v);
        return t == MINO_FN || t == MINO_PRIM || t == MINO_MACRO;
    }
    default:  return 0;
    }
}

int mino_args_parse(mino_state *S, const char *name, mino_val *args,
                    const char *fmt, ...)
{
    va_list ap;
    size_t  idx = 0;
    size_t  expected = 0;
    const char *f;
    mino_val *cursor = args;
    char msg[256];

    for (f = fmt; *f != '\0'; f++) expected++;

    va_start(ap, fmt);
    for (f = fmt; *f != '\0'; f++, idx++) {
        mino_val *v;

        if (!mino_is_cons(cursor)) {
            snprintf(msg, sizeof(msg),
                     "%s: expected %zu argument%s, got %zu",
                     name ? name : "<prim>", expected,
                     expected == 1 ? "" : "s", idx);
            va_end(ap);
            prim_throw_classified(S, "eval/arity", "MAR001", msg);
            return -1;
        }

        v = cursor->as.cons.car;
        if (!args_type_match(*f, v)) {
            snprintf(msg, sizeof(msg),
                     "%s: argument %zu: expected %s, got %s",
                     name ? name : "<prim>", idx + 1,
                     args_type_label(*f),
                     v == NULL ? "nil" : type_tag_str(v));
            va_end(ap);
            prim_throw_classified(S, "eval/type", "MTY001", msg);
            return -1;
        }

        switch (*f) {
        case 'i': {
            long long *out = va_arg(ap, long long *);
            *out = mino_val_int_get(v);
            break;
        }
        case 'f': {
            double *out = va_arg(ap, double *);
            *out = (mino_val_int_p(v)) ? (double)mino_val_int_get(v) : v->as.f;
            break;
        }
        case 's': {
            const char **out = va_arg(ap, const char **);
            *out = v->as.s.data;
            break;
        }
        case 'S': {
            const char **out_data = va_arg(ap, const char **);
            size_t      *out_len  = va_arg(ap, size_t *);
            *out_data = v->as.s.data;
            *out_len  = v->as.s.len;
            break;
        }
        case 'k': case 'y': {
            const char **out = va_arg(ap, const char **);
            *out = v->as.s.data;
            break;
        }
        case 'b': {
            int *out = va_arg(ap, int *);
            *out = mino_val_bool_get(v);
            break;
        }
        case 'c': {
            int *out = va_arg(ap, int *);
            *out = mino_val_char_get(v);
            break;
        }
        case 'v': case 'V': case 'M': case 'L':
        case 'H': case 'A':
        case 'B': case 'r': case 'd': case 'R': case 'X': case 'F': {
            mino_val **out = va_arg(ap, mino_val **);
            *out = v;
            break;
        }
        default:
            snprintf(msg, sizeof(msg),
                     "%s: mino_args_parse: unknown format '%c'",
                     name ? name : "<prim>", *f);
            va_end(ap);
            prim_throw_classified(S, "internal", "MIN001", msg);
            return -1;
        }

        cursor = cursor->as.cons.cdr;
    }
    va_end(ap);

    if (mino_is_cons(cursor)) {
        size_t extra = 0;
        while (mino_is_cons(cursor)) {
            extra++;
            cursor = cursor->as.cons.cdr;
        }
        snprintf(msg, sizeof(msg),
                 "%s: expected %zu argument%s, got %zu",
                 name ? name : "<prim>", expected,
                 expected == 1 ? "" : "s", expected + extra);
        prim_throw_classified(S, "eval/arity", "MAR001", msg);
        return -1;
    }

    return 0;
}
