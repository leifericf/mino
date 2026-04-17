/*
 * prim.c -- shared helpers and primitive registration (bootstrap).
 *
 * Domain-specific primitives live in separate files:
 *   prim_numeric.c     -- arithmetic, bitwise, math, comparison
 *   prim_collections.c -- list/vector/map/set operations
 *   prim_sequences.c   -- reduce, into, sort, apply, eager builders
 *   prim_string.c      -- string operations
 *   prim_io.c          -- println, slurp, spit, exit, time-ms
 *   prim_reflection.c  -- type, name, eval, symbol, keyword, gensym, throw
 *   prim_meta.c        -- meta, with-meta, vary-meta, alter-meta!
 *   prim_regex.c       -- re-find, re-matches
 *   prim_stateful.c    -- atom, deref, reset!, swap!, atom?
 *   prim_module.c      -- require, doc, source, apropos
 */

#include "prim_internal.h"

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

/* Throw a catchable exception from a primitive.  If inside a try block,
 * this longjmps to the catch handler.  Otherwise it sets a fatal error
 * and the caller returns NULL to propagate to the host. */
mino_val_t *prim_throw_error(mino_state_t *S, const char *msg)
{
    mino_val_t *ex = mino_string(S, msg);
    if (S->try_depth > 0) {
        S->try_stack[S->try_depth - 1].exception = ex;
        longjmp(S->try_stack[S->try_depth - 1].buf, 1);
    }
    set_error(S, msg);
    return NULL;
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
        set_error(S, "pr-str: tmpfile failed");
        return NULL;
    }
    mino_print_to(S, f, v);
    n = ftell(f);
    if (n < 0) n = 0;
    rewind(f);
    buf = (char *)malloc((size_t)n + 1);
    if (buf == NULL) {
        fclose(f);
        set_error(S, "out of memory");
        return NULL;
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

/* ------------------------------------------------------------------------- */
/* Core library bootstrap                                                    */
/* ------------------------------------------------------------------------- */

#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Woverlength-strings"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
#include "core_mino.h"
#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

static void install_core_mino(mino_state_t *S, mino_env_t *env)
{
    size_t i;

    /* On first call for this state, parse and eval each form, caching
     * the parsed ASTs for subsequent mino_install_core calls.  We set
     * S->core_forms immediately and update core_forms_len as we go so
     * that the GC root walker can pin the forms during collection. */
    if (S->core_forms == NULL) {
        const char  *src        = core_mino_src;
        const char  *saved_file = S->reader_file;
        int          saved_line = S->reader_line;
        size_t       cap        = 256;

        S->core_forms     = malloc(cap * sizeof(mino_val_t *));
        S->core_forms_len = 0;
        if (!S->core_forms) {
            /* Class I: init-time; no try-frame to recover through */
            fprintf(stderr, "core.mino: out of memory\n"); abort();
        }

        S->reader_file = intern_filename("<core>");
        S->reader_line = 1;
        while (*src != '\0') {
            const char *end  = NULL;
            mino_val_t *form = mino_read(S, src, &end);
            if (form == NULL) {
                if (mino_last_error(S) != NULL) {
                    /* Class I: core library parse failure is unrecoverable */
                    fprintf(stderr, "core.mino parse error: %s\n",
                            mino_last_error(S));
                    abort();
                }
                break;
            }
            if (S->core_forms_len >= cap) {
                cap *= 2;
                S->core_forms = realloc(S->core_forms,
                                        cap * sizeof(mino_val_t *));
                if (!S->core_forms) {
                    /* Class I: init-time; no try-frame to recover through */
                    fprintf(stderr, "core.mino: out of memory\n");
                    abort();
                }
            }
            S->core_forms[S->core_forms_len++] = form;
            if (mino_eval(S, form, env) == NULL) {
                /* Class I: core library eval failure is unrecoverable */
                fprintf(stderr, "core.mino eval error: %s\n",
                        mino_last_error(S));
                abort();
            }
            src = end;
        }
        S->reader_file = saved_file;
        S->reader_line = saved_line;
        return;
    }

    /* Subsequent calls: evaluate cached forms into the target env. */
    for (i = 0; i < S->core_forms_len; i++) {
        if (mino_eval(S, S->core_forms[i], env) == NULL) {
            /* Class I: cached core form eval failure is unrecoverable */
            fprintf(stderr, "core.mino eval error: %s\n", mino_last_error(S));
            abort();
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Primitive registration                                                    */
/* ------------------------------------------------------------------------- */

void mino_install_core(mino_state_t *S, mino_env_t *env)
{
    volatile char probe = 0;
    gc_note_host_frame(S, (void *)&probe);
    (void)probe;
    mino_env_set(S, env, "+",        mino_prim(S, "+",        prim_add));
    mino_env_set(S, env, "-",        mino_prim(S, "-",        prim_sub));
    mino_env_set(S, env, "*",        mino_prim(S, "*",        prim_mul));
    mino_env_set(S, env, "/",        mino_prim(S, "/",        prim_div));
    mino_env_set(S, env, "=",        mino_prim(S, "=",        prim_eq));
    mino_env_set(S, env, "identical?", mino_prim(S, "identical?", prim_identical));
    mino_env_set(S, env, "meta",      mino_prim(S, "meta",      prim_meta));
    mino_env_set(S, env, "with-meta", mino_prim(S, "with-meta", prim_with_meta));
    mino_env_set(S, env, "vary-meta", mino_prim(S, "vary-meta", prim_vary_meta));
    mino_env_set(S, env, "alter-meta!", mino_prim(S, "alter-meta!", prim_alter_meta));
    mino_env_set(S, env, "<",        mino_prim(S, "<",        prim_lt));
    mino_env_set(S, env, "mod",      mino_prim(S, "mod",      prim_mod));
    mino_env_set(S, env, "rem",      mino_prim(S, "rem",      prim_rem));
    mino_env_set(S, env, "quot",     mino_prim(S, "quot",     prim_quot));
    /* math */
    mino_env_set(S, env, "math-floor", mino_prim(S, "math-floor", prim_math_floor));
    mino_env_set(S, env, "math-ceil",  mino_prim(S, "math-ceil",  prim_math_ceil));
    mino_env_set(S, env, "math-round", mino_prim(S, "math-round", prim_math_round));
    mino_env_set(S, env, "math-sqrt",  mino_prim(S, "math-sqrt",  prim_math_sqrt));
    mino_env_set(S, env, "math-pow",   mino_prim(S, "math-pow",   prim_math_pow));
    mino_env_set(S, env, "math-log",   mino_prim(S, "math-log",   prim_math_log));
    mino_env_set(S, env, "math-exp",   mino_prim(S, "math-exp",   prim_math_exp));
    mino_env_set(S, env, "math-sin",   mino_prim(S, "math-sin",   prim_math_sin));
    mino_env_set(S, env, "math-cos",   mino_prim(S, "math-cos",   prim_math_cos));
    mino_env_set(S, env, "math-tan",   mino_prim(S, "math-tan",   prim_math_tan));
    mino_env_set(S, env, "math-atan2", mino_prim(S, "math-atan2", prim_math_atan2));
    mino_env_set(S, env, "math-pi",    mino_float(S, 3.14159265358979323846));
    mino_env_set(S, env, "bit-and", mino_prim(S, "bit-and", prim_bit_and));
    mino_env_set(S, env, "bit-or",  mino_prim(S, "bit-or",  prim_bit_or));
    mino_env_set(S, env, "bit-xor", mino_prim(S, "bit-xor", prim_bit_xor));
    mino_env_set(S, env, "bit-not", mino_prim(S, "bit-not", prim_bit_not));
    mino_env_set(S, env, "bit-shift-left",
                 mino_prim(S, "bit-shift-left", prim_bit_shift_left));
    mino_env_set(S, env, "bit-shift-right",
                 mino_prim(S, "bit-shift-right", prim_bit_shift_right));
    mino_env_set(S, env, "car",      mino_prim(S, "car",      prim_car));
    mino_env_set(S, env, "cdr",      mino_prim(S, "cdr",      prim_cdr));
    mino_env_set(S, env, "cons",     mino_prim(S, "cons",     prim_cons));
    mino_env_set(S, env, "count",    mino_prim(S, "count",    prim_count));
    mino_env_set(S, env, "nth",      mino_prim(S, "nth",      prim_nth));
    mino_env_set(S, env, "first",    mino_prim(S, "first",    prim_first));
    mino_env_set(S, env, "rest",     mino_prim(S, "rest",     prim_rest));
    mino_env_set(S, env, "vector",   mino_prim(S, "vector",   prim_vector));
    mino_env_set(S, env, "hash-map", mino_prim(S, "hash-map", prim_hash_map));
    mino_env_set(S, env, "assoc",    mino_prim(S, "assoc",    prim_assoc));
    mino_env_set(S, env, "get",      mino_prim(S, "get",      prim_get));
    mino_env_set(S, env, "conj",     mino_prim(S, "conj",     prim_conj));
    mino_env_set(S, env, "keys",     mino_prim(S, "keys",     prim_keys));
    mino_env_set(S, env, "vals",     mino_prim(S, "vals",     prim_vals));
    mino_env_set(S, env, "macroexpand-1",
                 mino_prim(S, "macroexpand-1", prim_macroexpand_1));
    mino_env_set(S, env, "macroexpand",
                 mino_prim(S, "macroexpand", prim_macroexpand));
    mino_env_set(S, env, "gensym",   mino_prim(S, "gensym",   prim_gensym));
    mino_env_set(S, env, "type",     mino_prim(S, "type",     prim_type));
    mino_env_set(S, env, "name",     mino_prim(S, "name",     prim_name));
    mino_env_set(S, env, "rand",     mino_prim(S, "rand",     prim_rand));
    /* regex */
    mino_env_set(S, env, "re-find",    mino_prim(S, "re-find",    prim_re_find));
    mino_env_set(S, env, "re-matches", mino_prim(S, "re-matches", prim_re_matches));
    mino_env_set(S, env, "eval",     mino_prim(S, "eval",     prim_eval));
    mino_env_set(S, env, "symbol",   mino_prim(S, "symbol",   prim_symbol));
    mino_env_set(S, env, "keyword",  mino_prim(S, "keyword",  prim_keyword));
    mino_env_set(S, env, "hash",     mino_prim(S, "hash",     prim_hash));
    mino_env_set(S, env, "compare",  mino_prim(S, "compare",  prim_compare));
    mino_env_set(S, env, "int",      mino_prim(S, "int",      prim_int));
    mino_env_set(S, env, "float",    mino_prim(S, "float",    prim_float));
    mino_env_set(S, env, "str",      mino_prim(S, "str",      prim_str));
    mino_env_set(S, env, "pr-str",   mino_prim(S, "pr-str",   prim_pr_str));
    mino_env_set(S, env, "read-string",
                 mino_prim(S, "read-string", prim_read_string));
    mino_env_set(S, env, "format",   mino_prim(S, "format",   prim_format));
    mino_env_set(S, env, "throw",    mino_prim(S, "throw",    prim_throw));
    mino_env_set(S, env, "require",  mino_prim(S, "require",  prim_require));
    mino_env_set(S, env, "doc",      mino_prim(S, "doc",      prim_doc));
    mino_env_set(S, env, "source",   mino_prim(S, "source",   prim_source));
    mino_env_set(S, env, "apropos",  mino_prim(S, "apropos",  prim_apropos));
    /* set operations */
    mino_env_set(S, env, "hash-set", mino_prim(S, "hash-set", prim_hash_set));
    mino_env_set(S, env, "set",      mino_prim(S, "set",      prim_set));
    mino_env_set(S, env, "contains?",mino_prim(S, "contains?",prim_contains_p));
    mino_env_set(S, env, "disj",     mino_prim(S, "disj",     prim_disj));
    mino_env_set(S, env, "dissoc",   mino_prim(S, "dissoc",   prim_dissoc));
    /* sequence operations */
    mino_env_set(S, env, "reduce",   mino_prim(S, "reduce",   prim_reduce));
    mino_env_set(S, env, "reduced",  mino_prim(S, "reduced",  prim_reduced));
    mino_env_set(S, env, "reduced?", mino_prim(S, "reduced?", prim_reduced_p));
    mino_env_set(S, env, "into",     mino_prim(S, "into",     prim_into));
    /* eager collection builders */
    mino_env_set(S, env, "rangev",   mino_prim(S, "rangev",   prim_rangev));
    mino_env_set(S, env, "mapv",     mino_prim(S, "mapv",     prim_mapv));
    mino_env_set(S, env, "filterv",  mino_prim(S, "filterv",  prim_filterv));
    mino_env_set(S, env, "apply",    mino_prim(S, "apply",    prim_apply));
    mino_env_set(S, env, "reverse",  mino_prim(S, "reverse",  prim_reverse));
    mino_env_set(S, env, "sort",     mino_prim(S, "sort",     prim_sort));
    /* string operations */
    mino_env_set(S, env, "subs",     mino_prim(S, "subs",     prim_subs));
    mino_env_set(S, env, "split",    mino_prim(S, "split",    prim_split));
    mino_env_set(S, env, "join",     mino_prim(S, "join",     prim_join));
    mino_env_set(S, env, "starts-with?",
                 mino_prim(S, "starts-with?", prim_starts_with_p));
    mino_env_set(S, env, "ends-with?",
                 mino_prim(S, "ends-with?", prim_ends_with_p));
    mino_env_set(S, env, "includes?",
                 mino_prim(S, "includes?", prim_includes_p));
    mino_env_set(S, env, "upper-case",
                 mino_prim(S, "upper-case", prim_upper_case));
    mino_env_set(S, env, "lower-case",
                 mino_prim(S, "lower-case", prim_lower_case));
    mino_env_set(S, env, "trim",     mino_prim(S, "trim",     prim_trim));
    mino_env_set(S, env, "char-at",  mino_prim(S, "char-at",  prim_char_at));
    /* sequences */
    mino_env_set(S, env, "seq",       mino_prim(S, "seq",       prim_seq));
    mino_env_set(S, env, "realized?", mino_prim(S, "realized?", prim_realized_p));
    /* atoms */
    mino_env_set(S, env, "atom",     mino_prim(S, "atom",     prim_atom));
    mino_env_set(S, env, "deref",    mino_prim(S, "deref",    prim_deref));
    mino_env_set(S, env, "reset!",   mino_prim(S, "reset!",   prim_reset_bang));
    mino_env_set(S, env, "swap!",    mino_prim(S, "swap!",    prim_swap_bang));
    mino_env_set(S, env, "atom?",    mino_prim(S, "atom?",    prim_atom_p));
    /* actors */
    mino_env_set(S, env, "spawn*",   mino_prim(S, "spawn*",   prim_spawn));
    mino_env_set(S, env, "send!",    mino_prim(S, "send!",    prim_send_bang));
    mino_env_set(S, env, "receive",  mino_prim(S, "receive",  prim_receive));
    install_core_mino(S, env);
}

void mino_install_io(mino_state_t *S, mino_env_t *env)
{
    mino_env_set(S, env, "println",  mino_prim(S, "println",  prim_println));
    mino_env_set(S, env, "prn",      mino_prim(S, "prn",      prim_prn));
    mino_env_set(S, env, "slurp",    mino_prim(S, "slurp",    prim_slurp));
    mino_env_set(S, env, "spit",     mino_prim(S, "spit",     prim_spit));
    mino_env_set(S, env, "exit",     mino_prim(S, "exit",     prim_exit));
    mino_env_set(S, env, "time-ms",  mino_prim(S, "time-ms",  prim_time_ms));
}
