/*
 * internal.h -- shared declarations for primitive implementation files.
 *
 * Not part of the public API. Each prim/<domain>.c includes this for
 * access to shared helpers and to declare its primitives for
 * registration.
 *
 * Error classes emitted (see diag/diag_contract.h):
 *
 *   MINO_ERR_RECOVERABLE -- the dominant path.  prim_throw_classified
 *      either longjmps into the active try frame (catchable user error)
 *      or sets a host-visible diagnostic and returns NULL when no try
 *      frame exists.  Every prim/<domain>.c arity / type / contract
 *      check uses this helper.  Diagnostic kinds: :eval/arity,
 *      :eval/type, :eval/contract.
 *   MINO_ERR_HOST -- prim/proc.c (sh fork/exec failures), prim/io.c
 *      (slurp / spit / file I/O), prim/fs.c.  Diagnostic kinds:
 *      :host/MHO001, :io/...
 *   MINO_ERR_CORRUPT -- install.c (core.clj bootstrap parse / eval
 *      failures, malloc OOM during the very first install).  At this
 *      point the state has not finished initializing, so abort is the
 *      only meaningful response.
 */

#ifndef PRIM_INTERNAL_H
#define PRIM_INTERNAL_H

#include "runtime/internal.h"

/* Shared helpers (defined in prim.c).
 * These operate on borrowed args and return GC-owned values unless noted. */
int          args_have_float(mino_val_t *args);             /* pure predicate */
mino_val_t  *prim_throw_error(mino_state_t *S, const char *msg); /* longjmp or set_error+NULL */
mino_val_t  *prim_throw_classified(mino_state_t *S, const char *kind,
                                   const char *code, const char *msg);
int          as_double(const mino_val_t *v, double *out);   /* pure extraction */
int          as_long(const mino_val_t *v, long long *out);  /* pure extraction */
size_t       list_length(mino_state_t *S, mino_val_t *list); /* pure traversal */
int          arg_count(mino_state_t *S, mino_val_t *args, size_t *out); /* pure */
mino_val_t  *print_to_string(mino_state_t *S, const mino_val_t *v); /* GC-owned */

/* Sequence iterator: borrows the collection being iterated.
 * seq_iter_val returns a borrowed pointer into the collection's storage;
 * do not retain it across allocations without gc_pin. */
typedef struct {
    const mino_val_t *coll;
    size_t            idx;       /* for vectors, maps, sets */
    const mino_val_t *cons_p;   /* for cons lists */
} seq_iter_t;

void         seq_iter_init(mino_state_t *S, seq_iter_t *it,
                           const mino_val_t *coll);
int          seq_iter_done(const seq_iter_t *it);
mino_val_t  *seq_iter_val(mino_state_t *S, const seq_iter_t *it); /* borrowed */
void         seq_iter_next(mino_state_t *S, seq_iter_t *it);

/* val_to_seq: coerce a value to a cons-list sequence (GC-owned). */
mino_val_t  *val_to_seq(mino_state_t *S, mino_val_t *v);

/* set_conj1: return a new set with elem added (GC-owned). */
mino_val_t  *set_conj1(mino_state_t *S, const mino_val_t *s,
                       mino_val_t *elem);

/* print_str_to: write v to out; strings as raw bytes, others via printer. */
void         print_str_to(mino_state_t *S, FILE *out, const mino_val_t *v);

/* Primitive install tables.  Each prim_*.c defines a static array of
 * mino_prim_def at file bottom; the central install.c composes them
 * into k_core_domains[] and iterates via prim_install_table to bind
 * each entry into the env and attach its docstring. */
typedef struct {
    const char  *name;     /* binding name visible to mino code */
    mino_prim_fn fn;       /* C implementation; signature in mino.h */
    const char  *doc;      /* docstring for (doc name); never NULL */
} mino_prim_def;

typedef struct {
    const char           *domain;     /* short label, e.g. "numeric" */
    const mino_prim_def  *defs;
    const size_t         *count_ptr;  /* &k_prims_<domain>_count */
} mino_prim_domain;

void prim_install_table(mino_state_t *S, mino_env_t *env, const char *ns_name,
                        const mino_prim_def *defs, size_t count);

/* Primitives declared per domain (each prim_*.c defines these).
 * All follow the standard primitive signature: args are borrowed,
 * return value is GC-owned (NULL on error via set_error). */

/* numeric.c */
mino_val_t *prim_add(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_inc(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_dec(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_sub(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_mul(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_addq(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_subq(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_mulq(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_incq(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_decq(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_div(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_mod(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_rem(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_quot(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_int(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_float(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_parse_long(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_parse_double(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_bit_and(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_bit_or(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_bit_xor(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_bit_not(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_bit_shift_left(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_bit_shift_right(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_unsigned_bit_shift_right(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_math_floor(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_math_ceil(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_math_round(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_math_sqrt(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_math_log(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_math_exp(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_math_sin(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_math_cos(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_math_tan(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_math_pow(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_math_atan2(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_eq(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_num_eq(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_identical(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_lt(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_lte(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_gt(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_gte(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_compare(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_nan_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_infinite_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);

/* meta.c */
mino_val_t *prim_meta(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_with_meta(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_vary_meta(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_alter_meta(mino_state_t *S, mino_val_t *args, mino_env_t *env);

/* collections.c */
mino_val_t *prim_car(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_cdr(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_cons(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_count(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_nth(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_first(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_rest(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_vector(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_hash_map(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_assoc(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_get(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_conj(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_keys(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_vals(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_hash_set(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_set(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_contains_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_disj(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_dissoc(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_seq(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_realized_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_transient(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_persistent_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_assoc_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_conj_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_dissoc_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_disj_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_pop_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_transient_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);

/* sequences.c */
mino_val_t *prim_reduce(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_reduced(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_reduced_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_into(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_apply(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_reverse(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_sort(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_range(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_rangev(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_lazy_map_1(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_lazy_filter(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_lazy_take(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_drop_seq(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_doall(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_dorun(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_mapv(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_filterv(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_peek(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_pop(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_find(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_empty(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_rseq(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_subvec(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_sorted_map(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_sorted_set(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_sorted_map_by(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_sorted_set_by(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_subseq(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_rsubseq(mino_state_t *S, mino_val_t *args, mino_env_t *env);

/* string.c */
mino_val_t *prim_str(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_pr_str(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_format(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_read_string(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_char_at(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_subs(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_split(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_join(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_str_replace(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_starts_with_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_ends_with_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_includes_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_upper_case(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_lower_case(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_trim(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_random_uuid(mino_state_t *S, mino_val_t *args, mino_env_t *env);

/* io.c */
mino_val_t *prim_println(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_prn(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_print(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_pr(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_newline(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_pr_builtin(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_set_print_method_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env);

/* bignum.c */
mino_val_t *prim_bigint(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_biginteger(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_bigint_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_numerator(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_denominator(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_ratio_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_rational_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_rationalize(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_bigdec(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_decimal_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
/* Internal bignum/ratio/bigdec helpers (mino_bigint_*, mino_ratio_*,
 * mino_bigdec_*) are declared in collections_internal.h because val.c
 * equality, the printer, and the GC sweep hook all need them. */

/* io.c (continued) */
mino_val_t *prim_slurp(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_spit(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_exit(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_time_ms(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_nano_time(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_file_seq(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_getenv(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_getcwd(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_chdir(mino_state_t *S, mino_val_t *args, mino_env_t *env);

/* reflection.c */
mino_val_t *prim_name(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_namespace(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_var_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_resolve(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_rand(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_eval(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_symbol(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_keyword(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_hash(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_type(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_nil_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_cons_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_vector_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_int_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_float_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_string_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_keyword_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_symbol_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_fn_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_char_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_number_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_map_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_set_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_seq_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_boolean_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_true_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_false_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_not(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_some_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_empty_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_zero_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_pos_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_neg_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_odd_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_even_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_macroexpand_1(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_macroexpand(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_gensym(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_destructure(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_throw(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_last_error(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_error_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_ex_data(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_ex_message(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_gc_stats(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_gc_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env);

/* regex.c */
mino_val_t *prim_re_find(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_re_matches(mino_state_t *S, mino_val_t *args, mino_env_t *env);

/* stateful.c */
mino_val_t *prim_atom(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_deref(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_reset_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_swap_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_atom_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_add_watch(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_remove_watch(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_set_validator(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_get_validator(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_reset_vals(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_swap_vals(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_set_fail_alloc_at(mino_state_t *S, mino_val_t *args, mino_env_t *env);

/* module.c */
mino_val_t *prim_require(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_use(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_doc(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_source(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_apropos(mino_state_t *S, mino_val_t *args, mino_env_t *env);

/* ns.c -- namespace and var introspection */
mino_val_t *prim_in_ns(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_find_ns(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_the_ns(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_create_ns(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_remove_ns(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_ns_name(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_ns_publics(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_ns_interns(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_ns_refers(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_ns_map(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_ns_aliases(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_alias(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_refer(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_ns_unalias(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_ns_unmap(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_all_ns(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_loaded_libs(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_find_var(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_ns_resolve(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_requiring_resolve(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_intern(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_var_get(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_var_set(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_alter_var_root(mino_state_t *S, mino_val_t *args, mino_env_t *env);
extern const mino_prim_def k_prims_ns[];
extern const size_t        k_prims_ns_count;

/* proc.c */
mino_val_t *prim_sh(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_sh_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env);
void mino_install_proc(mino_state_t *S, mino_env_t *env);

/* fs.c */
mino_val_t *prim_file_exists_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_directory_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_mkdir_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_rm_rf(mino_state_t *S, mino_val_t *args, mino_env_t *env);
void mino_install_fs(mino_state_t *S, mino_env_t *env);

/* host.c */
void mino_install_host(mino_state_t *S, mino_env_t *env);

/* async.c */
void mino_install_async(mino_state_t *S, mino_env_t *env);

/* Per-domain primitive tables.  Each prim_*.c exports the table and
 * its element count; prim/install.c composes them into k_core_domains[]
 * (and io.c keeps two tables: io_core for bootstrap-essential printer
 * hooks, io for the I/O surface installed via mino_install_io).
 */

extern const mino_prim_def k_prims_numeric[];
extern const size_t        k_prims_numeric_count;

extern const mino_prim_def k_prims_meta[];
extern const size_t        k_prims_meta_count;

extern const mino_prim_def k_prims_collections[];
extern const size_t        k_prims_collections_count;

extern const mino_prim_def k_prims_sequences[];
extern const size_t        k_prims_sequences_count;

extern const mino_prim_def k_prims_lazy[];
extern const size_t        k_prims_lazy_count;

extern const mino_prim_def k_prims_string[];
extern const size_t        k_prims_string_count;

extern const mino_prim_def k_prims_clojure_string[];
extern const size_t        k_prims_clojure_string_count;

extern const mino_prim_def k_prims_reflection[];
extern const size_t        k_prims_reflection_count;

extern const mino_prim_def k_prims_regex[];
extern const size_t        k_prims_regex_count;

extern const mino_prim_def k_prims_stateful[];
extern const size_t        k_prims_stateful_count;

extern const mino_prim_def k_prims_module[];
extern const size_t        k_prims_module_count;

extern const mino_prim_def k_prims_bignum[];
extern const size_t        k_prims_bignum_count;

extern const mino_prim_def k_prims_io_core[];
extern const size_t        k_prims_io_core_count;

extern const mino_prim_def k_prims_io[];
extern const size_t        k_prims_io_count;

extern const mino_prim_def k_prims_host[];
extern const size_t        k_prims_host_count;

extern const mino_prim_def k_prims_async[];
extern const size_t        k_prims_async_count;

extern const mino_prim_def k_prims_fs[];
extern const size_t        k_prims_fs_count;

extern const mino_prim_def k_prims_proc[];
extern const size_t        k_prims_proc_count;

#endif /* PRIM_INTERNAL_H */
