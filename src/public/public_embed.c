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
#include "runtime_internal.h"
#include "prim_internal.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>

/* ------------------------------------------------------------------------- */
/* mino_throw                                                                */
/* ------------------------------------------------------------------------- */

mino_val_t *mino_throw(mino_state_t *S, mino_val_t *ex)
{
    if (S == NULL) return NULL;

    if (S->try_depth <= 0) {
        /* No enclosing try: surface as a classified error. */
        char msg[512];
        if (ex != NULL && ex->type == MINO_STRING) {
            snprintf(msg, sizeof(msg), "unhandled exception: %.*s",
                     (int)ex->as.s.len, ex->as.s.data);
        } else {
            snprintf(msg, sizeof(msg), "unhandled exception");
        }
        return prim_throw_classified(S, "user", "MUS001", msg);
    }

    S->try_stack[S->try_depth - 1].exception = ex;
    longjmp(S->try_stack[S->try_depth - 1].buf, 1);
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
    default:  return "?";
    }
}

static int args_type_match(char spec, const mino_val_t *v)
{
    if (v == NULL) return spec == 'v' || spec == 'L';
    switch (spec) {
    case 'i': return v->type == MINO_INT;
    case 'f': return v->type == MINO_FLOAT || v->type == MINO_INT;
    case 's': case 'S': return v->type == MINO_STRING;
    case 'k': return v->type == MINO_KEYWORD;
    case 'y': return v->type == MINO_SYMBOL;
    case 'b': return v->type == MINO_BOOL;
    case 'c': return v->type == MINO_CHAR;
    case 'v': return 1;
    case 'V': return v->type == MINO_VECTOR;
    case 'M': return v->type == MINO_MAP;
    case 'L': return v->type == MINO_CONS || v->type == MINO_NIL;
    case 'H': return v->type == MINO_HANDLE;
    case 'A': return v->type == MINO_ATOM;
    default:  return 0;
    }
}

int mino_args_parse(mino_state_t *S, const char *name, mino_val_t *args,
                    const char *fmt, ...)
{
    va_list ap;
    size_t  idx = 0;
    size_t  expected = 0;
    const char *f;
    mino_val_t *cursor = args;
    char msg[256];

    for (f = fmt; *f != '\0'; f++) expected++;

    va_start(ap, fmt);
    for (f = fmt; *f != '\0'; f++, idx++) {
        mino_val_t *v;

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
            *out = v->as.i;
            break;
        }
        case 'f': {
            double *out = va_arg(ap, double *);
            *out = (v->type == MINO_INT) ? (double)v->as.i : v->as.f;
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
            *out = v->as.b;
            break;
        }
        case 'c': {
            int *out = va_arg(ap, int *);
            *out = v->as.ch;
            break;
        }
        case 'v': case 'V': case 'M': case 'L':
        case 'H': case 'A': {
            mino_val_t **out = va_arg(ap, mino_val_t **);
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
