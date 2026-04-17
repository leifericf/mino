/*
 * prim_internal.h -- shared declarations for primitive implementation files.
 *
 * Not part of the public API. Each prim_*.c includes this for access to
 * shared helpers and to declare its primitives for registration.
 */

#ifndef PRIM_INTERNAL_H
#define PRIM_INTERNAL_H

#include "mino_internal.h"

/* Shared helpers (defined in prim.c).
 * These operate on borrowed args and return GC-owned values unless noted. */
int          args_have_float(mino_val_t *args);             /* pure predicate */
mino_val_t  *prim_throw_error(mino_state_t *S, const char *msg); /* longjmp, never returns normally */
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

/* Primitives declared per domain (each prim_*.c defines these).
 * All follow the standard primitive signature: args are borrowed,
 * return value is GC-owned (NULL on error via set_error). */

/* prim_numeric.c */
mino_val_t *prim_add(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_sub(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_mul(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_div(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_mod(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_rem(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_quot(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_int(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_float(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_bit_and(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_bit_or(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_bit_xor(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_bit_not(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_bit_shift_left(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_bit_shift_right(mino_state_t *S, mino_val_t *args, mino_env_t *env);
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
mino_val_t *prim_identical(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_lt(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_compare(mino_state_t *S, mino_val_t *args, mino_env_t *env);

/* prim_meta.c */
mino_val_t *prim_meta(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_with_meta(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_vary_meta(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_alter_meta(mino_state_t *S, mino_val_t *args, mino_env_t *env);

/* prim_collections.c */
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

/* prim_sequences.c */
mino_val_t *prim_reduce(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_reduced(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_reduced_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_into(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_apply(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_reverse(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_sort(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_rangev(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_mapv(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_filterv(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_peek(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_pop(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_find(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_empty(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_rseq(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_sorted_map(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_sorted_set(mino_state_t *S, mino_val_t *args, mino_env_t *env);

/* prim_string.c */
mino_val_t *prim_str(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_pr_str(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_format(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_read_string(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_char_at(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_subs(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_split(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_join(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_starts_with_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_ends_with_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_includes_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_upper_case(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_lower_case(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_trim(mino_state_t *S, mino_val_t *args, mino_env_t *env);

/* prim_io.c */
mino_val_t *prim_println(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_prn(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_slurp(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_spit(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_exit(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_time_ms(mino_state_t *S, mino_val_t *args, mino_env_t *env);

/* prim_reflection.c */
mino_val_t *prim_name(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_rand(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_eval(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_symbol(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_keyword(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_hash(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_type(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_macroexpand_1(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_macroexpand(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_gensym(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_throw(mino_state_t *S, mino_val_t *args, mino_env_t *env);

/* prim_regex.c */
mino_val_t *prim_re_find(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_re_matches(mino_state_t *S, mino_val_t *args, mino_env_t *env);

/* prim_stateful.c */
mino_val_t *prim_atom(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_deref(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_reset_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_swap_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_atom_p(mino_state_t *S, mino_val_t *args, mino_env_t *env);

/* prim_module.c */
mino_val_t *prim_require(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_doc(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_source(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_apropos(mino_state_t *S, mino_val_t *args, mino_env_t *env);

#endif /* PRIM_INTERNAL_H */
