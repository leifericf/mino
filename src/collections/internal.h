/*
 * collections_internal.h -- persistent vector / HAMT / red-black tree /
 * intern table types and value-layer helpers.
 *
 * Internal to the runtime; embedders should only use mino.h.
 *
 * Error classes emitted (see diag/diag_contract.h):
 *
 *   MINO_ERR_RECOVERABLE -- transient.c, val.c, and the public
 *      collection APIs reach prim_throw_classified for type errors
 *      and contract violations (e.g. (assoc! v k) on a frozen
 *      transient, mismatched arity on rb-tree comparators).
 *      Diagnostic kinds: :eval/type, :eval/contract, :eval/arity.
 *   MINO_ERR_CORRUPT -- map.c HAMT walks assert internal invariants
 *      in debug builds; release builds rely on the GC trace and
 *      collector contracts to keep nodes consistent.
 */

#ifndef COLLECTIONS_INTERNAL_H
#define COLLECTIONS_INTERNAL_H

#include "mino_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ------------------------------------------------------------------------- */
/* Intern table                                                              */
/* ------------------------------------------------------------------------- */

/* Intern table with hash index for O(1) lookup. Bucket sentinels:
 * INTERN_HT_EMPTY = SIZE_MAX terminates probe chains; INTERN_HT_
 * TOMBSTONE = SIZE_MAX - 1 is left behind when major sweep prunes
 * an unreached entry so future inserts can reuse the slot without
 * extending the probe chain. */
#define INTERN_HT_EMPTY     SIZE_MAX
#define INTERN_HT_TOMBSTONE (SIZE_MAX - 1)

struct intern_table {
    mino_val **entries;
    size_t       len;
    size_t       cap;
    size_t      *ht_buckets;  /* open-addressing hash table: index into entries[] */
    size_t       ht_cap;      /* power of 2 */
};
typedef struct intern_table intern_table_t;

/* ------------------------------------------------------------------------- */
/* Persistent vector                                                         */
/* ------------------------------------------------------------------------- */

#define MINO_VEC_B     5u
#define MINO_VEC_WIDTH (1u << MINO_VEC_B)
#define MINO_VEC_MASK  (MINO_VEC_WIDTH - 1u)

struct mino_vec_node {
    unsigned char is_leaf;
    unsigned char count;
    /* 2 bytes of padding here, then owner uses 4 bytes; together the
     * pre-`slots` block stays at the original 8 bytes (count was
     * `unsigned int` in the persistent-only layout). The transient-
     * ownership marker is therefore *free* in struct size: every
     * persistent-path clone / alloc copies the same 264 bytes as
     * before. 0 = immutable; non-zero matches `transient.owner_id`. */
    uint32_t      owner;
    void         *slots[MINO_VEC_WIDTH];
};

/* ------------------------------------------------------------------------- */
/* HAMT                                                                      */
/* ------------------------------------------------------------------------- */

#define HAMT_B     5u
#define HAMT_W     (1u << HAMT_B)
#define HAMT_MASK  (HAMT_W - 1u)

/* Flatmap (small persistent map) threshold. Below this size, MINO_MAP
 * uses a linear-scan layout (key_order + val_order vectors) instead of
 * a HAMT — no per-entry hash on lookup, no hamt_entry_t allocations,
 * no bitmap-node allocations. Past this size, the map promotes to a
 * HAMT and never demotes back. 8 is the cache-line crossover where
 * linear scan beats hash+HAMT-walk for typical Clojure keyword keys
 * (which compare pointer-equal via mino_eq's identity short-circuit). */
#define MINO_FLATMAP_THRESHOLD 8u

typedef struct {
    mino_val *key;
    mino_val *val;
} hamt_entry_t;

struct mino_hamt_node {
    uint32_t        bitmap;
    uint32_t        subnode_mask;
    uint32_t        collision_hash;
    unsigned        collision_count;
    /* Transient-ownership marker. 0 = persistent (immutable from any
     * batch's perspective); non-zero matches `transient.owner_id` for
     * the editing batch. Owned bitmap / collision walks mutate slot
     * pointers (and bitmap / subnode_mask / collision_count) in place
     * when the node's owner matches, and clone-then-stamp when not.
     * gc_alloc_typed zero-inits so all persistent allocations land at
     * owner = 0. */
    uint32_t        owner;
    void          **slots;
};

/* ------------------------------------------------------------------------- */
/* Red-black tree                                                            */
/* ------------------------------------------------------------------------- */

struct mino_rb_node {
    mino_val     *key;
    mino_val     *val;    /* NULL sentinel for sorted sets */
    mino_rb_node_t *left;
    mino_rb_node_t *right;
    unsigned char   red;    /* 1 = red, 0 = black */
};

/* val.c API (constructors, interning, hashing, equality, mino_mk_var)
 * lives in values/internal.h. */
#include "values/internal.h"

/* Register per-tag GC tracers for the collection node layouts
 * (vec/hamt/hamt_entry/rb). Called from runtime/state.c::state_init
 * before the first allocation. Implemented in
 * src/collections/gc_handlers.c. */
void mino_collections_register_gc_handlers(mino_state *S);

/* ------------------------------------------------------------------------- */
/* vec.c: persistent vector operations                                       */
/* ------------------------------------------------------------------------- */

/* vec_nth returns a borrowed pointer into existing trie storage.
 * vec_conj1/vec_assoc1/vec_pop/vec_from_array return new GC-owned vectors. */
mino_val *vec_nth(const mino_val *v, size_t i);              /* borrowed */
mino_val *vec_conj1(mino_state *S, const mino_val *v,
                      mino_val *item);                          /* GC-owned */
mino_val *vec_assoc1(mino_state *S, const mino_val *v, size_t i,
                       mino_val *item);                         /* GC-owned */
mino_val *vec_pop(mino_state *S, const mino_val *v);       /* GC-owned */
mino_val *vec_subvec(mino_state *S, const mino_val *v,
                       size_t start, size_t end);                 /* GC-owned */
mino_val *vec_from_array(mino_state *S, mino_val **items,
                           size_t len);                           /* GC-owned */

/* Owned-edit conj / assoc / pop variants. The `owner` argument is the
 * editing transient's monotonic ID (from S->ns_vars.transient_owner_next);
 * nodes whose owner field matches are mutated in place, others are
 * cloned once with `owner` stamped and then mutated in place by
 * subsequent calls. Used by `conj!` / `assoc!` / `pop!` on transient
 * wrappers around persistent vectors (`transient.c`). */
mino_val *vec_conj1_owned(mino_state *S, mino_val *v,
                            mino_val *item, uintptr_t owner);       /* GC-owned */
mino_val *vec_assoc1_owned(mino_state *S, mino_val *v, size_t i,
                             mino_val *item, uintptr_t owner);       /* GC-owned */
mino_val *vec_pop_owned(mino_state *S, mino_val *v, uintptr_t owner); /* GC-owned */

/* ------------------------------------------------------------------------- */
/* map.c: HAMT operations                                                    */
/* ------------------------------------------------------------------------- */

/* hamt_get/map_get_val return borrowed pointers into existing HAMT nodes.
 * hamt_assoc/hamt_entry_new return new GC-owned nodes. */
unsigned     popcount32(uint32_t x);                              /* pure */
hamt_entry_t *hamt_entry_new(mino_state *S, mino_val *key,
                             mino_val *val);                    /* GC-owned */
mino_hamt_node_t *hamt_assoc(mino_state *S, const mino_hamt_node_t *n,
                              hamt_entry_t *entry, uint32_t hash,
                              unsigned shift, int *replaced);     /* GC-owned */
mino_val *hamt_get(const mino_hamt_node_t *n, const mino_val *key,
                     uint32_t hash, unsigned shift);              /* borrowed */
mino_val *map_get_val(const mino_val *m, const mino_val *key); /* borrowed */

/* Owned-edit assoc / dissoc variants for HAMT walks. Mirrors the
 * vec_*_owned pattern: `owner` is the editing transient's monotonic
 * ID; nodes whose owner field matches are mutated in place, others
 * are cloned once with `owner` stamped and then mutated in place by
 * subsequent calls. Caller is responsible for routing through these
 * only when `transient.owner_id != 0`. */
mino_hamt_node_t *hamt_assoc_owned(mino_state *S, mino_hamt_node_t *n,
                                    hamt_entry_t *entry, uint32_t hash,
                                    unsigned shift, int *replaced,
                                    uintptr_t owner);                  /* GC-owned */
mino_hamt_node_t *hamt_dissoc_owned(mino_state *S, mino_hamt_node_t *n,
                                     const mino_val *key, uint32_t hash,
                                     unsigned shift, int *removed,
                                     uintptr_t owner);                 /* GC-owned, may be NULL when node empties */

/* Owned-edit map / set mutators. The persistent path stays the
 * default; transient.c calls these only when `owner_id != 0`. */
mino_val *mino_map_assoc1_owned(mino_state *S, mino_val *m,
                                   mino_val *key, mino_val *val,
                                   uintptr_t owner);                   /* GC-owned */
mino_val *mino_map_dissoc1_owned(mino_state *S, mino_val *m,
                                    mino_val *key,
                                    uintptr_t owner);                  /* GC-owned */

/* Unified persistent-map ops covering both flatmap (small) and HAMT
 * (large) representations. These are the entry points all map mutators
 * should use; direct hamt_assoc/hamt_get on m->as.map.root is only
 * correct for HAMT-mode maps. Each returns a new map sharing structure
 * with the predecessor where possible.
 *
 *   mino_map_lookup     -- O(N) on flat, O(log32 N) on HAMT; returns
 *                          NULL when key is absent.
 *   mino_map_assoc1     -- insert/replace one k/v; may promote flat -> HAMT.
 *   mino_map_dissoc1    -- remove one key; never demotes HAMT -> flat.
 *
 * mino_map_assoc1/dissoc1 carry the source map's meta forward when the
 * caller does not need to override it; print/seq/clone all operate on
 * either representation via the existing key_order vector. */
mino_val *mino_map_lookup(const mino_val *m, const mino_val *key);
mino_val *mino_map_assoc1(mino_state *S, mino_val *m,
                            mino_val *key, mino_val *val);
mino_val *mino_map_dissoc1(mino_state *S, mino_val *m,
                             mino_val *key);

/* ------------------------------------------------------------------------- */
/* val.c: record helpers                                                     */
/* ------------------------------------------------------------------------- */

/* Return the index of a declared field by keyword, or -1 if not found.
 * Caller must pass a MINO_RECORD and a MINO_KEYWORD; behavior is
 * defined to return -1 on any other input shape (used directly by
 * the map-iso primitives so they can fall through to ext lookup or
 * the default-value branch on type mismatch). */
int record_field_index(const mino_val *r, const mino_val *key);

/* ------------------------------------------------------------------------- */
/* rbtree.c: persistent red-black tree operations for sorted map/set         */
/* ------------------------------------------------------------------------- */

/* rb_get returns a borrowed pointer; rb_assoc/rb_dissoc return new trees. */
int val_compare(const mino_val *a, const mino_val *b);          /* pure */
int rb_compare(mino_state *S, const mino_val *a, const mino_val *b,
               mino_val *comparator);
mino_val *rb_get(mino_state *S, const mino_rb_node_t *n,
                   const mino_val *key, mino_val *comparator);  /* borrowed */
int rb_contains(mino_state *S, const mino_rb_node_t *n,
                const mino_val *key, mino_val *comparator);
mino_rb_node_t *rb_assoc(mino_state *S, const mino_rb_node_t *n,
                          mino_val *key, mino_val *val,
                          mino_val *comparator, int *replaced);    /* GC-owned */
mino_rb_node_t *rb_dissoc(mino_state *S, const mino_rb_node_t *n,
                           const mino_val *key,
                           mino_val *comparator);                  /* GC-owned */
void rb_to_list(mino_state *S, const mino_rb_node_t *n,
                mino_val **head, mino_val **tail);
int rb_trees_content_equal(const mino_rb_node_t *a, const mino_rb_node_t *b,
                            int compare_vals);
int rb_trees_equal(const mino_rb_node_t *a, const mino_rb_node_t *b,
                   int compare_vals);
mino_val *mino_sorted_map_by(mino_state *S, mino_val *comparator,
                                mino_val **keys, mino_val **vals,
                                size_t len);
mino_val *mino_sorted_set_by(mino_state *S, mino_val *comparator,
                                mino_val **items, size_t len);
mino_val *mino_sorted_map(mino_state *S, mino_val **keys,
                             mino_val **vals, size_t len);
mino_val *mino_sorted_set(mino_state *S, mino_val **items,
                             size_t len);
mino_val *sorted_map_assoc1(mino_state *S, const mino_val *m,
                               mino_val *key, mino_val *val);
mino_val *sorted_map_dissoc1(mino_state *S, const mino_val *m,
                                const mino_val *key);
mino_val *sorted_set_conj1(mino_state *S, const mino_val *s,
                              mino_val *elem);
mino_val *sorted_set_disj1(mino_state *S, const mino_val *s,
                              const mino_val *elem);
mino_val *sorted_seq(mino_state *S, const mino_val *coll);
mino_val *sorted_rest(mino_state *S, const mino_val *coll);
void rb_bounded_seq(mino_state *S, const mino_rb_node_t *n, int is_map,
                    int has_lo, int lo_inclusive, mino_val *lo,
                    int has_hi, int hi_inclusive, mino_val *hi,
                    mino_val *comparator, int reverse,
                    mino_val **head, mino_val **tail);

/* ------------------------------------------------------------------------- */
/* prim/bignum.c: bignum / ratio / bigdec value support                      */
/*                                                                           */
/* Declared here (not prim/internal.h) because the GC sweep paths call       */
/* mino_bigint_free when a bigint cell dies, the printer calls               */
/* mino_bigint_print, and val.c equality calls                               */
/* mino_bigint_equals / mino_bigint_hash.                                    */
/* ------------------------------------------------------------------------- */

void     mino_bigint_free(mino_val *v);
void     mino_bigint_print(mino_state *S, const mino_val *v, FILE *out);
int      mino_bigint_equals(const mino_val *a, const mino_val *b);
int      mino_bigint_equals_ll(const mino_val *a, long long n);
int      mino_bigint_cmp(const mino_val *a, const mino_val *b);
uint32_t mino_bigint_hash(const mino_val *v);
mino_val *mino_bigint_from_string_n(mino_state *S, const char *s, size_t len);
/* Digits-only base-aware parse (2..36) for the reader's hex / radix
 * literal paths; `negative` applies the sign carried by the token. */
mino_val *mino_bigint_from_digits_base(mino_state *S, const char *s,
                                       size_t len, int base, int negative);
char    *mino_bigint_to_cstr(const mino_val *v);   /* malloc; caller frees */
int      mino_as_ll(const mino_val *v, long long *out);

/* Bigint arithmetic helpers for the promoting tower primitives. Each
 * accepts MINO_INT or MINO_BIGINT operands (callers classify first) and
 * returns a GC-owned MINO_BIGINT, or NULL on allocation failure (error
 * raised via prim_throw_classified). */
mino_val *mino_bigint_add(mino_state *S, const mino_val *a,
                            const mino_val *b);
mino_val *mino_bigint_sub(mino_state *S, const mino_val *a,
                            const mino_val *b);
mino_val *mino_bigint_mul(mino_state *S, const mino_val *a,
                            const mino_val *b);
mino_val *mino_bigint_neg(mino_state *S, const mino_val *a);
mino_val *mino_bigint_quot(mino_state *S, const mino_val *a,
                             const mino_val *b);
mino_val *mino_bigint_rem(mino_state *S, const mino_val *a,
                            const mino_val *b);
mino_val *mino_bigint_mod(mino_state *S, const mino_val *a,
                            const mino_val *b);
int mino_bigint_quotrem(mino_state *S, const mino_val *a,
                        const mino_val *b, mino_val **q_out,
                        mino_val **r_out);
double   mino_bigint_to_double(const mino_val *v);

/* MINO_RATIO support. */
mino_val *mino_ratio_make(mino_state *S, mino_val *num, mino_val *denom);
mino_val *mino_ratio_make_unchecked(mino_state *S, mino_val *num,
                                      mino_val *denom);
void     mino_ratio_print(mino_state *S, const mino_val *v, FILE *out);
int      mino_ratio_equals(const mino_val *a, const mino_val *b);
int      mino_ratio_cmp(const mino_val *a, const mino_val *b);
uint32_t mino_ratio_hash(const mino_val *v);
double   mino_ratio_to_double(const mino_val *v);
mino_val *mino_ratio_add(mino_state *S, const mino_val *a,
                           const mino_val *b);
mino_val *mino_ratio_sub(mino_state *S, const mino_val *a,
                           const mino_val *b);
mino_val *mino_ratio_mul(mino_state *S, const mino_val *a,
                           const mino_val *b);
mino_val *mino_ratio_div(mino_state *S, const mino_val *a,
                           const mino_val *b);

/* MINO_BIGDEC support. */
mino_val *mino_bigdec_make(mino_state *S, mino_val *unscaled, int scale);
mino_val *mino_bigdec_quot(mino_state *S, const mino_val *a,
                             const mino_val *b);
mino_val *mino_bigdec_rem(mino_state *S, const mino_val *a,
                            const mino_val *b);
mino_val *mino_bigdec_mod(mino_state *S, const mino_val *a,
                            const mino_val *b);
void     mino_bigdec_print(mino_state *S, const mino_val *v, FILE *out);
int      mino_bigdec_equals(const mino_val *a, const mino_val *b);
int      mino_bigdec_cmp(const mino_val *a, const mino_val *b);
uint32_t mino_bigdec_hash(const mino_val *v);
double   mino_bigdec_to_double(const mino_val *v);
mino_val *mino_bigdec_add(mino_state *S, const mino_val *a,
                            const mino_val *b);
mino_val *mino_bigdec_sub(mino_state *S, const mino_val *a,
                            const mino_val *b);
mino_val *mino_bigdec_mul(mino_state *S, const mino_val *a,
                            const mino_val *b);
mino_val *mino_bigdec_div(mino_state *S, const mino_val *a,
                            const mino_val *b);
mino_val *mino_bigdec_neg(mino_state *S, const mino_val *a);
mino_val *mino_to_bigdec(mino_state *S, const mino_val *v);
mino_val *mino_bigdec_apply_math_context(mino_state *S, mino_val *bd);

#endif /* COLLECTIONS_INTERNAL_H */
