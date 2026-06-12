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
int          args_have_float(mino_val *args);             /* pure predicate */
mino_val  *prim_throw_error(mino_state *S, const char *msg); /* longjmp or set_error+NULL */
mino_val  *prim_throw_classified(mino_state *S, const char *kind,
                                   const char *code, const char *msg);
int          as_double(const mino_val *v, double *out);   /* pure extraction */
int          as_long(const mino_val *v, long long *out);  /* pure extraction */
double       tower_to_double(const mino_val *v);           /* full numeric tower */
size_t       list_length(mino_state *S, mino_val *list); /* pure traversal */
int          arg_count(mino_state *S, mino_val *args, size_t *out); /* pure */
mino_val  *print_to_string(mino_state *S, const mino_val *v); /* GC-owned */

/* Print dynvar plumbing. Top-level print entrypoints (pr / prn / print
 * / println / pr-str) call resolve to snapshot *print-length* and
 * *print-level* / *print-readably* / *print-meta* / *print-dup* /
 * *print-namespace-maps* / *flush-on-newline* from the current
 * binding stack into the state's cached fields, do the print, then
 * call restore with the saved values. Both helpers are no-ops when
 * env is NULL or the dynvars are unset.
 *
 * The cached values mean the per-collection printers read a single
 * int field instead of walking the binding stack per nested value. */
typedef struct {
    int length;
    int level;
    int readably;
    int meta;
    int dup;
    int ns_maps;
    int flush_nl;
} print_dynvars_saved_t;

void print_dynvars_resolve(mino_state *S, mino_env *env,
                           print_dynvars_saved_t *saved);
void print_dynvars_restore(mino_state *S, const print_dynvars_saved_t *saved);

/* Sequence iterator: borrows the collection being iterated.
 * seq_iter_val returns a borrowed pointer into the collection's storage;
 * do not retain it across allocations without gc_pin. */
typedef struct {
    const mino_val *coll;
    size_t            idx;       /* for vectors, maps, sets */
    const mino_val *cons_p;   /* for cons lists */
} seq_iter_t;

void         seq_iter_init(mino_state *S, seq_iter_t *it,
                           const mino_val *coll);
int          seq_iter_done(const seq_iter_t *it);
mino_val  *seq_iter_val(mino_state *S, const seq_iter_t *it); /* borrowed */
void         seq_iter_next(mino_state *S, seq_iter_t *it);

/* val_to_seq: coerce a value to a cons-list sequence (GC-owned). */
mino_val  *val_to_seq(mino_state *S, mino_val *v);

/* set_conj1: return a new set with elem added (GC-owned). */
mino_val  *set_conj1(mino_state *S, const mino_val *s,
                       mino_val *elem);

/* Owner-tagged set conj/disj: mirror the persistent path but route
 * the HAMT walk and the key_order conj through the owned variants so
 * a transient batch reuses spine nodes and tail-chunk slots in place
 * after the first touch. The persistent path stays the default;
 * transient.c calls these only when `owner_id != 0`. */
mino_val  *set_conj1_owned(mino_state *S, mino_val *s,
                              mino_val *elem, uintptr_t owner);
mino_val  *set_disj1_owned(mino_state *S, mino_val *s,
                              const mino_val *elem, uintptr_t owner);

/* print_str_to: write v to out; strings as raw bytes, others via printer. */
void         print_str_to(mino_state *S, FILE *out, const mino_val *v);

/* Primitive install tables.  Each prim_*.c defines a static array of
 * mino_prim_def at file bottom; the central install.c composes them
 * into k_core_domains[] and iterates via prim_install_table to bind
 * each entry into the env and attach its docstring. */
typedef struct {
    const char   *name;     /* binding name visible to mino code */
    mino_prim_fn  fn;       /* C implementation; signature in mino.h */
    const char   *doc;      /* docstring for (doc name); never NULL */
    /* Extension fields. Default-initialised to NULL when omitted from
     * brace-list initialisers, so existing static tables compile
     * unchanged. fn2 takes precedence over fn when both are set. */
    mino_prim_fn2 fn2;      /* argv ABI; non-NULL switches the install
                             * path to register an argv-style prim */
} mino_prim_def;

typedef struct {
    const char           *domain;     /* short label, e.g. "numeric" */
    const mino_prim_def  *defs;
    const size_t         *count_ptr;  /* &k_prims_<domain>_count */
} mino_prim_domain;

void prim_install_table(mino_state *S, mino_env *env, const char *ns_name,
                        const mino_prim_def *defs, size_t count);

/* Same as prim_install_table but also tags each registered binding's
 * meta_entry with `capability` (the install-group label). User-visible
 * via (doc fn) and (mino-capability 'fn) so script writers see which
 * group their code requires. NULL or "" capability falls back to the
 * unlabelled install. */
void prim_install_table_with_capability(mino_state *S, mino_env *env,
                                        const char *ns_name,
                                        const mino_prim_def *defs,
                                        size_t count,
                                        const char *capability);

/* Primitives declared per domain (each prim_*.c defines these).
 * All follow the standard primitive signature: args are borrowed,
 * return value is GC-owned (NULL on error via set_error). */

/* numeric.c -- fast-path arithmetic/compare/inc/dec/mod/quot/rem and their argv
 * variants are cross-TU: eval/bc/vm.c, eval/bc/jit/helpers.c, eval/bc/compile.c,
 * and eval/special.c use them as fallback function pointers.
 * prim_num_eq/prim_identical/prim_nan_p/prim_infinite_p and all unchecked-*
 * prims are file-local static (no eval fast-path). */

/* eval/bc/vm.c, jit/helpers.c, compile.c, special.c fast-path fallbacks: */
mino_val *prim_add(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_inc(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_incp(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_dec(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_decp(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_sub(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_mul(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_div(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_mod(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_rem(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_quot(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_eq(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_lt(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_lte(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_gt(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_gte(mino_state *S, mino_val *args, mino_env *env);

/* prim_addp/subp/mulp: called by vm.c inside #ifdef MINO_CALL_SITE_SHAPES
 * (pointer comparison only; not a fallback call) AND by intra-numeric.c
 * helpers. Keep extern with this note to prevent silent #ifdef mis-grep. */
mino_val *prim_addp(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_subp(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_mulp(mino_state *S, mino_val *args, mino_env *env);

/* src/values/val.c keeps its own local extern decl for prim_compare by
 * layering choice (val.c predates prim/internal.h); leave val.c untouched. */
mino_val *prim_compare(mino_state *S, mino_val *args, mino_env *env);

/* numeric_bit.c -- all bit ops cross-TU: eval fast-path fallbacks. */
mino_val *prim_bit_and(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_bit_or(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_bit_xor(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_bit_not(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_bit_shift_left(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_bit_shift_right(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_unsigned_bit_shift_right(mino_state *S, mino_val *args, mino_env *env);

/* numeric_math.c -- ALL prims are cross-TU: k_prims_numeric in numeric.c
 * references all of them (TU aggregation); a subset is also in jvm_statics.c. */
mino_val *prim_math_floor(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_ceil(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_round(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_sqrt(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_log(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_exp(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_sin(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_cos(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_tan(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_pow(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_atan2(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_asin(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_acos(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_atan(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_sinh(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_cosh(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_tanh(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_log10(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_log1p(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_expm1(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_cbrt(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_signum(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_to_radians(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_to_degrees(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_hypot(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_copy_sign(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_next_up(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_next_down(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_ieee_remainder(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_rint(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_ulp(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_scalb(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_get_exponent(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_math_next_after(mino_state *S, mino_val *args, mino_env *env);

/* numeric_coerce.c -- ALL prims are cross-TU: k_prims_numeric in numeric.c
 * references all of them (TU aggregation). */
mino_val *prim_int(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_long(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_short(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_byte(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_char(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_float(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_double(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_parse_long(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_parse_double(mino_state *S, mino_val *args, mino_env *env);

/* argv-ABI variants for hot prims -- cross-TU: lazy.c calls prim_addp_argv
 * and prim_lt_argv directly; all other argv variants are file-local static. */
mino_val *prim_addp_argv(mino_state *S, mino_val **argv, int argc, mino_env *env);
mino_val *prim_lt_argv(mino_state *S, mino_val **argv, int argc, mino_env *env);

/* meta.c -- all prims are file-local static; no extern declarations needed. */

/* collections.c -- prim_car/cdr/cons/vector/keys/vals/contains_p/subvec
 * and argv variants (prim_cons_argv etc.) are file-local static.
 * Cross-TU callers: */

/* jvm_statics.c calls these three. */
mino_val *prim_list(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_hash_map(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_hash_set(mino_state *S, mino_val *args, mino_env *env);

/* eval/bc/vm.c, eval/bc/compile.c, eval/bc/jit/helpers.c use these as
 * fallback function pointers in the fast-dispatch loop. */
mino_val *prim_count(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_nth(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_first(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_assoc(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_get(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_conj(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_dissoc(mino_state *S, mino_val *args, mino_env *env);

/* sequences_seq.c calls prim_rest. */
mino_val *prim_rest(mino_state *S, mino_val *args, mino_env *env);

/* src/collections/transient.c calls prim_disj. */
mino_val *prim_disj(mino_state *S, mino_val *args, mino_env *env);

/* collections_transient.c -- all prims are cross-TU:
 * registered in collections.c's k_prims_collections table, and
 * assoc_bang/conj_bang/dissoc_bang/disj_bang also referenced from eval. */
mino_val *prim_transient(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_persistent_bang(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_assoc_bang(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_conj_bang(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_dissoc_bang(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_disj_bang(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_pop_bang(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_transient_p(mino_state *S, mino_val *args, mino_env *env);

/* sequences.c -- most prims are file-local static.
 * Cross-TU callers: */

/* prim_pop: called by src/collections/transient.c. */
mino_val *prim_pop(mino_state *S, mino_val *args, mino_env *env);

/* sequences_seq.c -- prim_seq is cross-TU: called by lazy.c and sequences.c.
 * prim_realized_p is cross-TU: registered in sequences.c's k_prims_sequences. */
mino_val *prim_seq(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_realized_p(mino_state *S, mino_val *args, mino_env *env);

/* lazy.c -- all 5 prims below are cross-TU: registered in sequences.c's
 * k_prims_sequences table. prim_range also appears in sequences.c table. */
mino_val *prim_range(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_lazy_map_1(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_lazy_filter(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_lazy_take(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_drop_seq(mino_state *S, mino_val *args, mino_env *env);

/* Lazy-seq pipeline helpers: defined in lazy.c, used by sequences.c's
 * prim_reduce fusion pass and sequences_seq.c's prim_seq. */
int         lazy_is_int_range(const mino_val *coll, long long *start_out,
                              long long *end_out, long long *step_out,
                              int *infinite_out);
int         lazy_thunk_is_map1  (const mino_val *coll);
int         lazy_thunk_is_filter(const mino_val *coll);
int         lazy_thunk_is_take  (const mino_val *coll);

/* string.c -- most prims are file-local static.
 * Cross-TU callers: jvm_statics.c calls prim_str/prim_random_uuid/prim_parse_uuid;
 * io.c calls prim_format. mino_uuid_* and utf8_* are shared helpers. */
mino_val *prim_str(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_format(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_random_uuid(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_parse_uuid(mino_state *S, mino_val *args, mino_env *env);
/* mino_uuid_* canonical declarations live in runtime/internal.h. */
/* UTF-8 helpers used by string and collection primitives to count and
 * walk codepoints. Step returns the byte length of the codepoint at
 * pos (1..4); skip walks n codepoints forward; count returns the
 * total codepoint count. Lenient: malformed sequences advance one
 * byte rather than aborting. */
size_t    utf8_codepoint_step(const char *data, size_t bytes, size_t pos);
size_t    utf8_skip_codepoints(const char *data, size_t bytes,
                               size_t pos, long long n);
long long utf8_codepoint_count(const char *data, size_t bytes);

/* io.c -- most prims are file-local static. */

/* bignum.c -- prim_bigint/prim_biginteger/prim_bigint_p are file-local static.
 * ratio.c prims (prim_numerator/prim_denominator/prim_ratio_p/prim_rational_p/
 * prim_rationalize) and bigdec.c prims (prim_bigdec/prim_decimal_p) are
 * cross-TU: bignum.c's k_prims_bignum table references them.
 * Internal bignum/ratio/bigdec helpers (mino_bigint_*, mino_ratio_*,
 * mino_bigdec_*) are declared in collections_internal.h because val.c
 * equality, the printer, and the GC sweep hook all need them. */
mino_val *prim_numerator(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_denominator(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_ratio_p(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_rational_p(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_rationalize(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_bigdec(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_decimal_p(mino_state *S, mino_val *args, mino_env *env);

/* io.c (continued) -- cross-TU callers: jvm_statics.c calls the three below. */
mino_val *prim_exit(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_nano_time(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_getenv(mino_state *S, mino_val *args, mino_env *env);

/* reflection.c -- most prims are file-local static.
 * Type predicates (nil?/int?/string?/map?/...) and their _argv twins
 * are file-local static, generated by DEFINE_TYPE_PRED / DEFINE_TYPE_PRED_NOARGV.
 * Numeric predicate argv variants (prim_zero_p_argv etc.) are also static.
 * Cross-TU callers below: */

/* eval/bc/vm.c, eval/bc/compile.c, eval/bc/jit/helpers.c use these as
 * fallback function pointers in the fast-dispatch loop. */
mino_val *prim_empty_p(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_zero_p(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_pos_p(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_neg_p(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_odd_p(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_even_p(mino_state *S, mino_val *args, mino_env *env);

/* io.c's k_prims_io_core table registers these two gc prims
 * that are defined in reflection.c. */
mino_val *prim_gc_stats(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_gc_bang(mino_state *S, mino_val *args, mino_env *env);

/* DEFINED in src/eval/bindings.c; registered by reflection.c's table.
 * eval/bc/compile.c also calls it at compile time to expand destructuring. */
mino_val *prim_destructure(mino_state *S, mino_val *args, mino_env *env);

/* argv-ABI variants for numeric predicates are file-local static in
 * reflection.c (prim_zero_p_argv etc.) and numeric.c (prim_inc_argv etc.) --
 * only referenced via the install table's fn2 field in the same file.
 * Exceptions (cross-TU argv variants) live in the numeric section above:
 * prim_addp_argv (called by lazy.c) and prim_lt_argv (called by lazy.c). */

/* regex.c -- prim_re_* are file-local static.
 * mino_regex_from_source is a shared helper used by src/eval/read.c;
 * its canonical declaration lives in runtime/internal.h. */

/* stateful.c -- all prims are file-local static; no extern declarations needed. */

/* module.c -- prim_use/doc/source/apropos are file-local static.
 * prim_require is cross-TU: called from src/eval/defs.c and src/prim/ns.c. */
mino_val *prim_require(mino_state *S, mino_val *args, mino_env *env);

/* ns.c -- all prims are file-local static; no extern declarations needed. */

/* proc.c -- prim_sh/prim_sh_bang are file-local static.
 * prim_thread_sleep is cross-TU: called from jvm_statics.c. */
mino_val *prim_thread_sleep(mino_state *S, mino_val *args, mino_env *env);
void mino_install_proc(mino_state *S, mino_env *env);

/* fs.c -- all prims are file-local static; no extern declarations needed. */
void mino_install_fs(mino_state *S, mino_env *env);

/* host.c */
void mino_install_host(mino_state *S, mino_env *env);

/* async.c */
void mino_install_async(mino_state *S, mino_env *env);

/* jvm_statics.c -- JVM Clojure surface-parity statics. */
void mino_install_jvm_statics(mino_state *S, mino_env *env);

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

extern const mino_prim_def k_prims_bits[];
extern const size_t        k_prims_bits_count;

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

extern const mino_prim_def k_prims_ns[];
extern const size_t        k_prims_ns_count;

extern const mino_prim_def k_prims_clojure_repl[];
extern const size_t        k_prims_clojure_repl_count;

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

extern const mino_prim_def k_prims_stm[];
extern const size_t        k_prims_stm_count;

/* Defined in prim/agent.c. Walks a tx_state_t.pending_sends list
 * (LIFO -- head holds the most recent (agent fn . extra) triple),
 * reverses onto a fresh stack, and dispatches each action through
 * the same path as a top-level synchronous send. Called from stm.c
 * after a successful commit with the list already detached from
 * the tx, so a recursive dosync triggered by an action's body
 * cannot mutate the list being walked. */
void mino_agent_drain_pending(mino_state *S, mino_val *pending,
                               mino_env *env);

/* Stop the per-state agent worker thread. Sets agents_shutdown,
 * wakes the worker, drops state_lock, joins. Idempotent. Safe to
 * call when no worker has spawned -- becomes a flag flip.
 *
 * Must NOT be called from inside an agent action body running on
 * the worker thread (would self-join). prim_shutdown_agents catches
 * that case and throws; mino_state_free runs after eval has
 * unwound, so the embedder thread is the caller. */
void mino_agent_quiesce_workers(mino_state *S);

#endif /* PRIM_INTERNAL_H */
