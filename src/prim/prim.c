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

/* Throw a classified catchable exception from a primitive.  If inside a
 * try block, this longjmps to the catch handler.  Otherwise it sets a
 * fatal error and the caller returns NULL to propagate to the host. */
mino_val *prim_throw_classified(mino_state *S, const char *kind,
                                  const char *code, const char *msg)
{
    if (mino_current_ctx(S)->try_depth > 0) {
        /* Build a diagnostic map so catch normalization preserves the
         * classification instead of wrapping it as :user/MUS001.
         * Attach :mino/location when a source span can be derived from
         * either the call form or the bc cursor; the catch handler
         * surfaces it as part of (ex-data). */
        mino_val *keys[6], *vals[6];
        mino_val *ex;
        const char *loc_file = NULL;
        int         loc_line = 0;
        int         loc_col  = 0;
        /* Prefer the BC PC lookup (inner instruction inside a compiled
         * fn body) over eval_current_form (the outer call site), so
         * a throw inside (defn f [] (assoc nil)) reports the assoc
         * line, not the caller's (f) line. eval_current_form is the
         * fallback for tree-walker frames that have no BC cursor. */
        const mino_bc_fn_t *cur_bc = mino_current_ctx(S)->bc_current_bc;
        size_t              cur_pc = mino_current_ctx(S)->bc_current_pc;
        if (cur_bc != NULL) {
            (void)mino_bc_source_lookup(cur_bc, cur_pc,
                                        &loc_file, &loc_line, &loc_col);
        }
        if (loc_file == NULL || loc_line <= 0) {
            const mino_val *form = mino_current_ctx(S)->eval_current_form;
            if (form != NULL && mino_is_cons(form)
                && form->as.cons.file != NULL && form->as.cons.line > 0) {
                loc_file = form->as.cons.file;
                loc_line = form->as.cons.line;
                loc_col  = form->as.cons.column;
            }
        }
        size_t n = 0;
        keys[n] = mino_keyword(S, "mino/kind");
        vals[n] = mino_keyword(S, kind);
        n++;
        keys[n] = mino_keyword(S, "mino/code");
        vals[n] = mino_string(S, code);
        n++;
        keys[n] = mino_keyword(S, "mino/phase");
        vals[n] = mino_keyword(S, "eval");
        n++;
        keys[n] = mino_keyword(S, "mino/message");
        vals[n] = mino_string(S, msg);
        n++;
        keys[n] = mino_keyword(S, "mino/data");
        vals[n] = mino_nil(S);
        n++;
        if (loc_file != NULL && loc_line > 0) {
            mino_val *lkeys[3], *lvals[3];
            lkeys[0] = mino_keyword(S, "file");
            lvals[0] = mino_string(S, loc_file);
            lkeys[1] = mino_keyword(S, "line");
            lvals[1] = mino_int(S, loc_line);
            lkeys[2] = mino_keyword(S, "column");
            lvals[2] = mino_int(S, loc_col);
            keys[n] = mino_keyword(S, "mino/location");
            vals[n] = mino_map(S, lkeys, lvals, 3);
            n++;
        }
        ex = mino_map(S, keys, vals, n);
        mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth - 1].exception = ex;
        longjmp(mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth - 1].buf, 1);
    }
    set_eval_diag(S, mino_current_ctx(S)->eval_current_form, kind, code, msg);
    append_trace(S);
    return NULL;
}

/* Throws with internal/MIN001 classification.  Used by call sites that
 * predate the explicit-classification API; new sites should call
 * prim_throw_classified directly. */
mino_val *prim_throw_error(mino_state *S, const char *msg)
{
    return prim_throw_classified(S, "internal", "MIN001", msg);
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
    FILE  *f = tmpfile();
    long   n;
    char  *buf;
    mino_val *result;
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
