/*
 * prim.c -- shared helpers used by every prim/<domain>.c.
 *
 * Per-domain primitives live in their own files (prim/numeric.c,
 * prim/collections.c, ...).  Each domain file exports a static
 * mino_prim_def table at TU bottom; prim/install.c composes the tables
 * into mino_install_core.
 */

#include "prim/internal.h"

/* ------------------------------------------------------------------------- */
/* Shared helpers                                                            */
/* ------------------------------------------------------------------------- */

/*
 * Numeric coercion: if any argument is a float, the result is a float.
 * Otherwise integer arithmetic is used end-to-end.
 */

int args_have_float(mino_val_t *args)
{
    while (mino_is_cons(args)) {
        mino_val_t *a = args->as.cons.car;
        if (a != NULL && a->type == MINO_FLOAT) {
            return 1;
        }
        args = args->as.cons.cdr;
    }
    return 0;
}

/* Throw a classified catchable exception from a primitive.  If inside a
 * try block, this longjmps to the catch handler.  Otherwise it sets a
 * fatal error and the caller returns NULL to propagate to the host. */
mino_val_t *prim_throw_classified(mino_state_t *S, const char *kind,
                                  const char *code, const char *msg)
{
    if (S->ctx->try_depth > 0) {
        /* Build a diagnostic map so catch normalization preserves the
         * classification instead of wrapping it as :user/MUS001. */
        mino_val_t *keys[5], *vals[5];
        mino_val_t *ex;
        keys[0] = mino_keyword(S, "mino/kind");
        vals[0] = mino_keyword(S, kind);
        keys[1] = mino_keyword(S, "mino/code");
        vals[1] = mino_string(S, code);
        keys[2] = mino_keyword(S, "mino/phase");
        vals[2] = mino_keyword(S, "eval");
        keys[3] = mino_keyword(S, "mino/message");
        vals[3] = mino_string(S, msg);
        keys[4] = mino_keyword(S, "mino/data");
        vals[4] = mino_nil(S);
        ex = mino_map(S, keys, vals, 5);
        S->ctx->try_stack[S->ctx->try_depth - 1].exception = ex;
        longjmp(S->ctx->try_stack[S->ctx->try_depth - 1].buf, 1);
    }
    set_eval_diag(S, S->ctx->eval_current_form, kind, code, msg);
    append_trace(S);
    return NULL;
}

/* Throws with internal/MIN001 classification.  Used by call sites that
 * predate the explicit-classification API; new sites should call
 * prim_throw_classified directly. */
mino_val_t *prim_throw_error(mino_state_t *S, const char *msg)
{
    return prim_throw_classified(S, "internal", "MIN001", msg);
}

int as_double(const mino_val_t *v, double *out)
{
    if (v == NULL) {
        return 0;
    }
    if (v->type == MINO_INT) {
        *out = (double)v->as.i;
        return 1;
    }
    if (v->type == MINO_FLOAT) {
        *out = v->as.f;
        return 1;
    }
    return 0;
}

int as_long(const mino_val_t *v, long long *out)
{
    if (v == NULL || v->type != MINO_INT) {
        return 0;
    }
    *out = v->as.i;
    return 1;
}


/*
 * Helper: print a value to a string buffer using the standard printer.
 * Returns a mino string. Uses tmpfile() for ANSI C portability.
 */
mino_val_t *print_to_string(mino_state_t *S, const mino_val_t *v)
{
    FILE  *f = tmpfile();
    long   n;
    char  *buf;
    mino_val_t *result;
    if (f == NULL) {
        return prim_throw_classified(S, "host", "MHO001", "pr-str: tmpfile failed");
    }
    mino_print_to(S, f, v);
    n = ftell(f);
    if (n < 0) n = 0;
    rewind(f);
    buf = (char *)malloc((size_t)n + 1);
    if (buf == NULL) {
        fclose(f);
        return prim_throw_classified(S, "internal", "MIN001", "out of memory");
    }
    if (n > 0) {
        size_t rd = fread(buf, 1, (size_t)n, f);
        (void)rd;
    }
    buf[n] = '\0';
    fclose(f);
    result = mino_string_n(S, buf, (size_t)n);
    free(buf);
    return result;
}
