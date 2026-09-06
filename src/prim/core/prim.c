/*
 * prim.c -- shared helpers used by every prim/<domain>.c.
 *
 * Per-domain primitives live in their own files (prim/numeric.c,
 * prim/collections.c, ...).  Each domain file exports a static
 * mino_prim_def table at TU bottom; prim/install.c composes the tables
 * into mino_install / mino_install_clojure_core.
 */

#include "prim/internal.h"
#include "eval/bc/internal.h"

/* ------------------------------------------------------------------------- */
/* Shared helpers                                                            */
/* ------------------------------------------------------------------------- */

/*
 * Numeric coercion: if any argument is a float, the result is a float.
 * Otherwise integer arithmetic is used end-to-end.
 */

int args_have_float(mino_val *args)
{
    while (mino_is_cons(args)) {
        mino_val *a = args->as.cons.car;
        if (a != NULL && mino_type_of(a) == MINO_FLOAT) {
            return 1;
        }
        args = args->as.cons.cdr;
    }
    return 0;
}

/* Throws with internal/MIN001 classification.  Used by call sites that
 * predate the explicit-classification API; new sites should call
 * throw_classified directly. */
mino_val *prim_throw_error(mino_state *S, const char *msg)
{
    return throw_classified(S, "internal", "MIN001", msg);
}

/* Throw the JVM-parity value error for a key-value or option tail that
 * ends on a key with no value: the count is within the signature, only
 * the value shape is wrong, so eval/type MTY001, not eval/arity MAR001
 * (ADR 34). Message matches the JVM, naming the dangling key. */
mino_val *prim_throw_dangling_key(mino_state *S, const mino_val *key)
{
    char        buf[256];
    mino_val *printed = print_to_string(S, key);
    snprintf(buf, sizeof(buf), "No value supplied for key: %.*s",
             printed != NULL ? (int)printed->as.s.len : 0,
             printed != NULL ? printed->as.s.data : "");
    return throw_classified(S, "eval/type", "MTY001", buf);
}

int as_double(const mino_val *v, double *out)
{
    if (v == NULL) {
        return 0;
    }
    if (mino_val_int_p(v)) {
        *out = (double)mino_val_int_get(v);
        return 1;
    }
    if (mino_type_of(v) == MINO_FLOAT) {
        *out = v->as.f;
        return 1;
    }
    return 0;
}

int as_long(const mino_val *v, long long *out)
{
    if (v == NULL || !mino_val_int_p(v)) {
        return 0;
    }
    *out = mino_val_int_get(v);
    return 1;
}


/*
 * Helper: print a value to a string buffer using the standard printer.
 * Returns a mino string. Uses tmpfile() for ANSI C portability.
 */
mino_val *print_to_string(mino_state *S, const mino_val *v)
{
    FILE      *f = tmpfile();
    long long  n;
    char      *buf;
    mino_val  *result;
    if (f == NULL) {
        return throw_classified(S, "host", "MHO001", "pr-str: tmpfile failed");
    }
    mino_print_to(S, f, v);
#if defined(_WIN32) && defined(_MSC_VER)
    n = _ftelli64(f);
#else
    n = (long long)ftell(f);
#endif
    if (n < 0) n = 0;
    rewind(f);
    buf = (char *)malloc((size_t)n + 1);
    if (buf == NULL) {
        fclose(f);
        return throw_classified(S, "internal", "MIN001", "out of memory");
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
