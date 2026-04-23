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

/* Throw a classified catchable exception from a primitive.  If inside a
 * try block, this longjmps to the catch handler.  Otherwise it sets a
 * fatal error and the caller returns NULL to propagate to the host. */
mino_val_t *prim_throw_classified(mino_state_t *S, const char *kind,
                                  const char *code, const char *msg)
{
    if (S->try_depth > 0) {
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
        S->try_stack[S->try_depth - 1].exception = ex;
        longjmp(S->try_stack[S->try_depth - 1].buf, 1);
    }
    set_eval_diag(S, S->eval_current_form, kind, code, msg);
    append_trace(S);
    return NULL;
}

/* Backward-compat wrapper: throws with internal/MIN001 classification. */
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

        S->reader_file = intern_filename(S, "<core>");
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
                if (end != NULL && end > src) {
                    src = end; /* reader conditional produced nothing; skip */
                    continue;
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
    DEF_PRIM(env, "+",        prim_add,
             "Returns the sum of the arguments.");
    DEF_PRIM(env, "inc",      prim_inc,
             "Returns x plus 1.");
    DEF_PRIM(env, "dec",      prim_dec,
             "Returns x minus 1.");
    DEF_PRIM(env, "-",        prim_sub,
             "Returns the difference of the arguments. With one arg, returns the negation.");
    DEF_PRIM(env, "*",        prim_mul,
             "Returns the product of the arguments.");
    DEF_PRIM(env, "/",        prim_div,
             "Returns the quotient of the arguments.");
    DEF_PRIM(env, "=",        prim_eq,
             "Returns true if all arguments are equal.");
    DEF_PRIM(env, "identical?", prim_identical,
             "Returns true if the arguments are the same object.");
    DEF_PRIM(env, "meta",      prim_meta,
             "Returns the metadata map of the given value, or nil.");
    DEF_PRIM(env, "with-meta", prim_with_meta,
             "Returns a copy of the value with the given metadata map.");
    DEF_PRIM(env, "vary-meta", prim_vary_meta,
             "Returns a copy of the value with (apply f meta args) as its metadata.");
    DEF_PRIM(env, "alter-meta!", prim_alter_meta,
             "Atomically applies f to the metadata of a reference.");
    DEF_PRIM(env, "<",        prim_lt,
             "Returns true if nums are in monotonically increasing order.");
    DEF_PRIM(env, "<=",       prim_lte,
             "Returns true if nums are in monotonically non-decreasing order.");
    DEF_PRIM(env, ">",        prim_gt,
             "Returns true if nums are in monotonically decreasing order.");
    DEF_PRIM(env, ">=",       prim_gte,
             "Returns true if nums are in monotonically non-increasing order.");
    DEF_PRIM(env, "mod",      prim_mod,
             "Returns the modulus of dividing num by div. Truncates toward negative infinity.");
    DEF_PRIM(env, "rem",      prim_rem,
             "Returns the remainder of dividing num by div.");
    DEF_PRIM(env, "quot",     prim_quot,
             "Returns the quotient of dividing num by div, truncated toward zero.");
    /* math */
    DEF_PRIM(env, "math-floor", prim_math_floor,
             "Returns the largest integer not greater than n.");
    DEF_PRIM(env, "math-ceil",  prim_math_ceil,
             "Returns the smallest integer not less than n.");
    DEF_PRIM(env, "math-round", prim_math_round,
             "Returns the closest integer to n.");
    DEF_PRIM(env, "math-sqrt",  prim_math_sqrt,
             "Returns the square root of n.");
    DEF_PRIM(env, "math-pow",   prim_math_pow,
             "Returns base raised to the power of exp.");
    DEF_PRIM(env, "math-log",   prim_math_log,
             "Returns the natural logarithm of n.");
    DEF_PRIM(env, "math-exp",   prim_math_exp,
             "Returns e raised to the power of n.");
    DEF_PRIM(env, "math-sin",   prim_math_sin,
             "Returns the sine of n (in radians).");
    DEF_PRIM(env, "math-cos",   prim_math_cos,
             "Returns the cosine of n (in radians).");
    DEF_PRIM(env, "math-tan",   prim_math_tan,
             "Returns the tangent of n (in radians).");
    DEF_PRIM(env, "math-atan2", prim_math_atan2,
             "Returns the angle in radians between the positive x-axis and the point (x, y).");
    mino_env_set(S, env, "math-pi",    mino_float(S, 3.14159265358979323846));
    DEF_PRIM(env, "bit-and", prim_bit_and,
             "Returns the bitwise AND of the arguments.");
    DEF_PRIM(env, "bit-or",  prim_bit_or,
             "Returns the bitwise OR of the arguments.");
    DEF_PRIM(env, "bit-xor", prim_bit_xor,
             "Returns the bitwise XOR of the arguments.");
    DEF_PRIM(env, "bit-not", prim_bit_not,
             "Returns the bitwise complement of n.");
    DEF_PRIM(env, "bit-shift-left", prim_bit_shift_left,
             "Returns n shifted left by count bits.");
    DEF_PRIM(env, "bit-shift-right", prim_bit_shift_right,
             "Returns n arithmetically shifted right by count bits.");
    DEF_PRIM(env, "unsigned-bit-shift-right", prim_unsigned_bit_shift_right,
             "Returns n logically shifted right by count bits.");
    DEF_PRIM(env, "car",      prim_car,
             "Returns the first element of a cons cell.");
    DEF_PRIM(env, "cdr",      prim_cdr,
             "Returns the rest of a cons cell.");
    DEF_PRIM(env, "cons",     prim_cons,
             "Returns a new list with x prepended to coll.");
    DEF_PRIM(env, "count",    prim_count,
             "Returns the number of items in a collection.");
    DEF_PRIM(env, "nth",      prim_nth,
             "Returns the item at index n in a collection.");
    DEF_PRIM(env, "first",    prim_first,
             "Returns the first item in a collection, or nil if empty.");
    DEF_PRIM(env, "rest",     prim_rest,
             "Returns all but the first item in a collection.");
    DEF_PRIM(env, "vector",   prim_vector,
             "Returns a new vector containing the arguments.");
    DEF_PRIM(env, "hash-map", prim_hash_map,
             "Returns a new hash map with the given key-value pairs.");
    DEF_PRIM(env, "assoc",    prim_assoc,
             "Returns a new map with the given key-value pairs added.");
    DEF_PRIM(env, "get",      prim_get,
             "Returns the value mapped to key in a collection, or not-found.");
    DEF_PRIM(env, "conj",     prim_conj,
             "Returns a new collection with items added.");
    DEF_PRIM(env, "keys",     prim_keys,
             "Returns a sequence of the keys in a map.");
    DEF_PRIM(env, "vals",     prim_vals,
             "Returns a sequence of the values in a map.");
    DEF_PRIM(env, "macroexpand-1", prim_macroexpand_1,
             "Expands a macro form once.");
    DEF_PRIM(env, "macroexpand", prim_macroexpand,
             "Repeatedly expands a macro form until it is no longer a macro call.");
    DEF_PRIM(env, "gensym",   prim_gensym,
             "Returns a new symbol with a unique name.");
    DEF_PRIM(env, "type",     prim_type,
             "Returns a keyword indicating the type of the value.");
    DEF_PRIM(env, "nil?",     prim_nil_p,
             "Returns true if x is nil.");
    DEF_PRIM(env, "cons?",    prim_cons_p,
             "Returns true if x is a list (cons cell).");
    DEF_PRIM(env, "vector?",  prim_vector_p,
             "Returns true if x is a vector.");
    DEF_PRIM(env, "int?",     prim_int_p,
             "Returns true if x is an integer.");
    DEF_PRIM(env, "float?",   prim_float_p,
             "Returns true if x is a float.");
    DEF_PRIM(env, "string?",  prim_string_p,
             "Returns true if x is a string.");
    DEF_PRIM(env, "keyword?", prim_keyword_p,
             "Returns true if x is a keyword.");
    DEF_PRIM(env, "symbol?",  prim_symbol_p,
             "Returns true if x is a symbol.");
    DEF_PRIM(env, "fn?",      prim_fn_p,
             "Returns true if x is callable as a function (fn or prim).");
    DEF_PRIM(env, "char?",    prim_char_p,
             "Returns true if x is a one-character string.");
    DEF_PRIM(env, "number?",  prim_number_p,
             "Returns true if x is a number (int or float).");
    DEF_PRIM(env, "map?",     prim_map_p,
             "Returns true if x is a map (including sorted-map).");
    DEF_PRIM(env, "set?",     prim_set_p,
             "Returns true if x is a set (including sorted-set).");
    DEF_PRIM(env, "seq?",     prim_seq_p,
             "Returns true if x is a cons cell or lazy-seq.");
    DEF_PRIM(env, "boolean?", prim_boolean_p,
             "Returns true if x is true or false.");
    DEF_PRIM(env, "true?",    prim_true_p,
             "Returns true if x is the value true.");
    DEF_PRIM(env, "false?",   prim_false_p,
             "Returns true if x is the value false.");
    DEF_PRIM(env, "not",      prim_not,
             "Returns true if x is logical false, false otherwise.");
    DEF_PRIM(env, "some?",    prim_some_p,
             "Returns true if x is not nil.");
    DEF_PRIM(env, "empty?",   prim_empty_p,
             "Returns true if coll has no items.");
    DEF_PRIM(env, "zero?",    prim_zero_p,
             "Returns true if x is zero.");
    DEF_PRIM(env, "pos?",     prim_pos_p,
             "Returns true if x is greater than zero.");
    DEF_PRIM(env, "neg?",     prim_neg_p,
             "Returns true if x is less than zero.");
    DEF_PRIM(env, "odd?",     prim_odd_p,
             "Returns true if x is an odd integer.");
    DEF_PRIM(env, "even?",    prim_even_p,
             "Returns true if x is an even integer.");
    DEF_PRIM(env, "name",      prim_name,
             "Returns the name string of a symbol, keyword, or string.");
    DEF_PRIM(env, "namespace", prim_namespace,
             "Returns the namespace string of a symbol or keyword, or nil.");
    DEF_PRIM(env, "var?",      prim_var_p,
             "Returns true if x is a var.");
    DEF_PRIM(env, "resolve",   prim_resolve,
             "Returns the var to which a symbol resolves, or nil.");
    DEF_PRIM(env, "rand",      prim_rand,
             "Returns a random float between 0 inclusive and 1 exclusive, or between 0 and n.");
    /* regex */
    DEF_PRIM(env, "re-find",    prim_re_find,
             "Returns the first regex match in the string, or nil.");
    DEF_PRIM(env, "re-matches", prim_re_matches,
             "Returns the match if the entire string matches the regex, or nil.");
    DEF_PRIM(env, "eval",     prim_eval,
             "Evaluates the given form.");
    DEF_PRIM(env, "symbol",   prim_symbol,
             "Returns a symbol with the given name.");
    DEF_PRIM(env, "keyword",  prim_keyword,
             "Returns a keyword with the given name.");
    DEF_PRIM(env, "hash",     prim_hash,
             "Returns the hash code of the value.");
    DEF_PRIM(env, "compare",  prim_compare,
             "Returns a negative, zero, or positive integer comparing x and y.");
    DEF_PRIM(env, "NaN?",     prim_nan_p,
             "Returns true if x is NaN.");
    DEF_PRIM(env, "infinite?", prim_infinite_p,
             "Returns true if x is positive or negative infinity.");
    DEF_PRIM(env, "int",      prim_int,
             "Coerces x to an integer.");
    DEF_PRIM(env, "float",    prim_float,
             "Coerces x to a float.");
    DEF_PRIM(env, "parse-long",   prim_parse_long,
             "Parses a string into a long integer, or returns nil on failure.");
    DEF_PRIM(env, "parse-double", prim_parse_double,
             "Parses a string into a double, or returns nil on failure.");
    DEF_PRIM(env, "str",      prim_str,
             "Returns the string representation of the arguments concatenated.");
    DEF_PRIM(env, "pr-str",   prim_pr_str,
             "Returns a readable string representation of the arguments.");
    DEF_PRIM(env, "read-string", prim_read_string,
             "Reads one form from the string.");
    DEF_PRIM(env, "format",   prim_format,
             "Returns a formatted string using a format specifier and arguments.");
    DEF_PRIM(env, "throw",    prim_throw,
             "Throws an exception with the given value.");
    DEF_PRIM(env, "last-error", prim_last_error,
             "Returns the last error as a diagnostic map, or nil.");
    DEF_PRIM(env, "error?",  prim_error_p,
             "Returns true if the value is a diagnostic map.");
    DEF_PRIM(env, "require",  prim_require,
             "Loads and evaluates a mino source file.");
    DEF_PRIM(env, "doc",      prim_doc,
             "Prints the documentation for the named var.");
    DEF_PRIM(env, "source",   prim_source,
             "Prints the source code of the named var.");
    DEF_PRIM(env, "apropos",  prim_apropos,
             "Returns a list of vars whose names match the given pattern.");
    /* set operations */
    DEF_PRIM(env, "hash-set", prim_hash_set,
             "Returns a new hash set containing the arguments.");
    DEF_PRIM(env, "set",      prim_set,
             "Returns a set of the items in coll.");
    DEF_PRIM(env, "contains?", prim_contains_p,
             "Returns true if the collection contains the key.");
    DEF_PRIM(env, "disj",     prim_disj,
             "Returns a set with the given keys removed.");
    DEF_PRIM(env, "dissoc",   prim_dissoc,
             "Returns a map with the given keys removed.");
    /* sequence operations */
    DEF_PRIM(env, "reduce",   prim_reduce,
             "Reduces a collection using f. With no init, uses the first item.");
    DEF_PRIM(env, "reduced",  prim_reduced,
             "Wraps a value to signal early termination of reduce.");
    DEF_PRIM(env, "reduced?", prim_reduced_p,
             "Returns true if x is a reduced value.");
    DEF_PRIM(env, "into",     prim_into,
             "Returns a new collection with all items from the source conj'd in.");
    DEF_PRIM(env, "range",    prim_range,
             "Returns a lazy sequence of nums from start (inclusive) to end (exclusive), by step. With no args, returns an infinite sequence from 0.");
    DEF_PRIM(env, "lazy-map-1", prim_lazy_map_1,
             "Internal fast path for single-collection lazy map.");
    DEF_PRIM(env, "lazy-filter", prim_lazy_filter,
             "Internal fast path for lazy filter.");
    DEF_PRIM(env, "lazy-take", prim_lazy_take,
             "Internal fast path for lazy take.");
    DEF_PRIM(env, "drop-seq", prim_drop_seq,
             "Internal fast path for eager drop.");
    DEF_PRIM(env, "doall",    prim_doall,
             "Forces realization of a lazy sequence. Returns coll.");
    DEF_PRIM(env, "dorun",    prim_dorun,
             "Forces realization of a lazy sequence. Returns nil.");
    /* eager collection builders */
    DEF_PRIM(env, "rangev",   prim_rangev,
             "Returns a vector of integers from start (inclusive) to end (exclusive).");
    DEF_PRIM(env, "mapv",     prim_mapv,
             "Returns a vector of applying f to each item in one or more collections.");
    DEF_PRIM(env, "filterv",  prim_filterv,
             "Returns a vector of items in coll for which pred returns logical true.");
    DEF_PRIM(env, "apply",    prim_apply,
             "Applies f to the arguments, with the last argument spread as a sequence.");
    DEF_PRIM(env, "reverse",  prim_reverse,
             "Returns a sequence of the items in coll in reverse order.");
    DEF_PRIM(env, "sort",     prim_sort,
             "Returns a sorted sequence of the items in coll.");
    DEF_PRIM(env, "peek",     prim_peek,
             "Returns the first item of a list or last item of a vector.");
    DEF_PRIM(env, "pop",      prim_pop,
             "Returns a collection without the peek item.");
    DEF_PRIM(env, "find",     prim_find,
             "Returns the map entry for the key, or nil.");
    DEF_PRIM(env, "empty",    prim_empty,
             "Returns an empty collection of the same type.");
    DEF_PRIM(env, "rseq",     prim_rseq,
             "Returns a reverse sequence of a vector, or nil if empty.");
    DEF_PRIM(env, "subvec",   prim_subvec,
             "Returns a subvector from start (inclusive) to end (exclusive).");
    DEF_PRIM(env, "sorted-map", prim_sorted_map,
             "Returns a new sorted map with the given key-value pairs.");
    DEF_PRIM(env, "sorted-set", prim_sorted_set,
             "Returns a new sorted set containing the arguments.");
    /* string operations */
    DEF_PRIM(env, "subs",     prim_subs,
             "Returns a substring from start (inclusive) to end (exclusive).");
    DEF_PRIM(env, "split",    prim_split,
             "Splits a string on a regex pattern.");
    DEF_PRIM(env, "join",     prim_join,
             "Returns a string of the items in coll joined by separator.");
    DEF_PRIM(env, "str-replace", prim_str_replace,
             "Replaces all occurrences of match in s with replacement.");
    DEF_PRIM(env, "starts-with?", prim_starts_with_p,
             "Returns true if the string starts with the given prefix.");
    DEF_PRIM(env, "ends-with?", prim_ends_with_p,
             "Returns true if the string ends with the given suffix.");
    DEF_PRIM(env, "includes?", prim_includes_p,
             "Returns true if the string contains the given substring.");
    DEF_PRIM(env, "upper-case", prim_upper_case,
             "Returns the string converted to upper case.");
    DEF_PRIM(env, "lower-case", prim_lower_case,
             "Returns the string converted to lower case.");
    DEF_PRIM(env, "trim",     prim_trim,
             "Returns the string with leading and trailing whitespace removed.");
    DEF_PRIM(env, "char-at",  prim_char_at,
             "Returns the character at the given index as a string.");
    DEF_PRIM(env, "random-uuid", prim_random_uuid,
             "Returns a random UUID v4 string.");
    /* sequences */
    DEF_PRIM(env, "seq",       prim_seq,
             "Returns a seq on the collection, or nil if empty.");
    DEF_PRIM(env, "realized?", prim_realized_p,
             "Returns true if the lazy value has been realized.");
    /* atoms */
    DEF_PRIM(env, "atom",           prim_atom,
             "Creates an atom with the given initial value.");
    DEF_PRIM(env, "deref",          prim_deref,
             "Returns the current value of a reference (atom, delay, etc.).");
    DEF_PRIM(env, "reset!",         prim_reset_bang,
             "Sets the value of an atom to newval and returns newval.");
    DEF_PRIM(env, "swap!",          prim_swap_bang,
             "Atomically applies f to the current value of the atom and any additional args.");
    DEF_PRIM(env, "atom?",          prim_atom_p,
             "Returns true if x is an atom.");
    DEF_PRIM(env, "add-watch",      prim_add_watch,
             "Adds a watch function to an atom, called on state changes.");
    DEF_PRIM(env, "remove-watch",   prim_remove_watch,
             "Removes a watch function from an atom by key.");
    DEF_PRIM(env, "set-validator!", prim_set_validator,
             "Sets a validator function on an atom.");
    DEF_PRIM(env, "get-validator",  prim_get_validator,
             "Returns the validator function of an atom, or nil.");
    DEF_PRIM(env, "reset-vals!",    prim_reset_vals,
             "Sets the value of an atom and returns [old new].");
    DEF_PRIM(env, "swap-vals!",     prim_swap_vals,
             "Atomically applies f to the atom and returns [old new].");
    /* fault injection (testing only) */
    DEF_PRIM(env, "set-fail-alloc-at!",  prim_set_fail_alloc_at,
             "Make the n-th GC allocation fail (simulated OOM). Pass 0 to disable.");
    /* host interop */
    mino_install_host(S, env);
    /* async channels */
    mino_install_async(S, env);
    install_core_mino(S, env);
}

void mino_install_io(mino_state_t *S, mino_env_t *env)
{
    DEF_PRIM(env, "println",  prim_println,
             "Prints the arguments followed by a newline.");
    DEF_PRIM(env, "prn",      prim_prn,
             "Prints the arguments readably followed by a newline.");
    DEF_PRIM(env, "slurp",    prim_slurp,
             "Reads the entire contents of a file as a string.");
    DEF_PRIM(env, "spit",     prim_spit,
             "Writes the string content to a file.");
    DEF_PRIM(env, "exit",     prim_exit,
             "Exits the process with the given status code.");
    DEF_PRIM(env, "time-ms",  prim_time_ms,
             "Returns the current time in milliseconds.");
    DEF_PRIM(env, "nano-time", prim_nano_time,
             "Returns monotonic wall-clock time in nanoseconds.");
    DEF_PRIM(env, "file-seq", prim_file_seq,
             "Returns a vector of all file paths under a directory, recursively.");
    DEF_PRIM(env, "getenv",   prim_getenv,
             "Returns the value of an environment variable, or nil.");
    DEF_PRIM(env, "getcwd",   prim_getcwd,
             "Returns the current working directory.");
    DEF_PRIM(env, "chdir",    prim_chdir,
             "Changes the current working directory.");
    DEF_PRIM(env, "gc-stats", prim_gc_stats,
             "Returns a map of GC statistics.");
    DEF_PRIM(env, "gc!",      prim_gc_bang,
             "Forces a full garbage collection. Returns nil.");
}
