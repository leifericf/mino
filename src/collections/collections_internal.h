/*
 * collections_internal.h -- persistent vector / HAMT / red-black tree /
 * intern table types and value-layer helpers.
 *
 * Internal to the runtime; embedders should only use mino.h.
 */

#ifndef COLLECTIONS_INTERNAL_H
#define COLLECTIONS_INTERNAL_H

#include "mino.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ------------------------------------------------------------------------- */
/* Intern table                                                              */
/* ------------------------------------------------------------------------- */

/* Intern table with hash index for O(1) lookup. */
typedef struct {
    mino_val_t **entries;
    size_t       len;
    size_t       cap;
    size_t      *ht_buckets;  /* open-addressing hash table: index into entries[] */
    size_t       ht_cap;      /* power of 2; SIZE_MAX marks empty slots */
} intern_table_t;

/* ------------------------------------------------------------------------- */
/* Persistent vector                                                         */
/* ------------------------------------------------------------------------- */

#define MINO_VEC_B     5u
#define MINO_VEC_WIDTH (1u << MINO_VEC_B)
#define MINO_VEC_MASK  (MINO_VEC_WIDTH - 1u)

struct mino_vec_node {
    unsigned char is_leaf;
    unsigned      count;
    void         *slots[MINO_VEC_WIDTH];
};

/* ------------------------------------------------------------------------- */
/* HAMT                                                                      */
/* ------------------------------------------------------------------------- */

#define HAMT_B     5u
#define HAMT_W     (1u << HAMT_B)
#define HAMT_MASK  (HAMT_W - 1u)

typedef struct {
    mino_val_t *key;
    mino_val_t *val;
} hamt_entry_t;

struct mino_hamt_node {
    uint32_t        bitmap;
    uint32_t        subnode_mask;
    uint32_t        collision_hash;
    unsigned        collision_count;
    void          **slots;
};

/* ------------------------------------------------------------------------- */
/* Red-black tree                                                            */
/* ------------------------------------------------------------------------- */

struct mino_rb_node {
    mino_val_t     *key;
    mino_val_t     *val;    /* NULL sentinel for sorted sets */
    mino_rb_node_t *left;
    mino_rb_node_t *right;
    unsigned char   red;    /* 1 = red, 0 = black */
};

/* ------------------------------------------------------------------------- */
/* val.c: constructors, interning, hashing, equality                         */
/* ------------------------------------------------------------------------- */

/* Interned values are GC-owned singletons (deduplicated by content). */
mino_val_t *intern_lookup_or_create(mino_state_t *S, intern_table_t *tbl,
                                    mino_type_t type,
                                    const char *s, size_t len);  /* GC-owned */
mino_val_t *make_fn(mino_state_t *S, mino_val_t *params, mino_val_t *body,
                    mino_env_t *env);                            /* GC-owned */

/* Hashing (pure, no allocation). */
uint32_t hash_val(const mino_val_t *v);
uint32_t fnv_mix(uint32_t h, unsigned char b);
uint32_t fnv_bytes(uint32_t h, const unsigned char *p, size_t n);

/* Equality (may force lazy seqs, triggering allocation). */
int mino_eq_force(mino_state_t *S, const mino_val_t *a, const mino_val_t *b);

/* val.c: var constructor. */
mino_val_t *mino_mk_var(mino_state_t *S, const char *ns, const char *name,
                        mino_val_t *root);

/* ------------------------------------------------------------------------- */
/* vec.c: persistent vector operations                                       */
/* ------------------------------------------------------------------------- */

/* vec_nth returns a borrowed pointer into existing trie storage.
 * vec_conj1/vec_assoc1/vec_pop/vec_from_array return new GC-owned vectors. */
mino_val_t *vec_nth(const mino_val_t *v, size_t i);              /* borrowed */
mino_val_t *vec_conj1(mino_state_t *S, const mino_val_t *v,
                      mino_val_t *item);                          /* GC-owned */
mino_val_t *vec_assoc1(mino_state_t *S, const mino_val_t *v, size_t i,
                       mino_val_t *item);                         /* GC-owned */
mino_val_t *vec_pop(mino_state_t *S, const mino_val_t *v);       /* GC-owned */
mino_val_t *vec_subvec(mino_state_t *S, const mino_val_t *v,
                       size_t start, size_t end);                 /* GC-owned */
mino_val_t *vec_from_array(mino_state_t *S, mino_val_t **items,
                           size_t len);                           /* GC-owned */

/* ------------------------------------------------------------------------- */
/* map.c: HAMT operations                                                    */
/* ------------------------------------------------------------------------- */

/* hamt_get/map_get_val return borrowed pointers into existing HAMT nodes.
 * hamt_assoc/hamt_entry_new return new GC-owned nodes. */
unsigned     popcount32(uint32_t x);                              /* pure */
hamt_entry_t *hamt_entry_new(mino_state_t *S, mino_val_t *key,
                             mino_val_t *val);                    /* GC-owned */
mino_hamt_node_t *hamt_assoc(mino_state_t *S, const mino_hamt_node_t *n,
                              hamt_entry_t *entry, uint32_t hash,
                              unsigned shift, int *replaced);     /* GC-owned */
mino_val_t *hamt_get(const mino_hamt_node_t *n, const mino_val_t *key,
                     uint32_t hash, unsigned shift);              /* borrowed */
mino_val_t *map_get_val(const mino_val_t *m, const mino_val_t *key); /* borrowed */

/* ------------------------------------------------------------------------- */
/* rbtree.c: persistent red-black tree operations for sorted map/set         */
/* ------------------------------------------------------------------------- */

/* rb_get returns a borrowed pointer; rb_assoc/rb_dissoc return new trees. */
int val_compare(const mino_val_t *a, const mino_val_t *b);          /* pure */
int rb_compare(mino_state_t *S, const mino_val_t *a, const mino_val_t *b,
               mino_val_t *comparator);
mino_val_t *rb_get(mino_state_t *S, const mino_rb_node_t *n,
                   const mino_val_t *key, mino_val_t *comparator);  /* borrowed */
int rb_contains(mino_state_t *S, const mino_rb_node_t *n,
                const mino_val_t *key, mino_val_t *comparator);
mino_rb_node_t *rb_assoc(mino_state_t *S, const mino_rb_node_t *n,
                          mino_val_t *key, mino_val_t *val,
                          mino_val_t *comparator, int *replaced);    /* GC-owned */
mino_rb_node_t *rb_dissoc(mino_state_t *S, const mino_rb_node_t *n,
                           const mino_val_t *key,
                           mino_val_t *comparator);                  /* GC-owned */
void rb_to_list(mino_state_t *S, const mino_rb_node_t *n,
                mino_val_t **head, mino_val_t **tail);
int rb_trees_equal(const mino_rb_node_t *a, const mino_rb_node_t *b,
                   int compare_vals);
mino_val_t *mino_sorted_map_by(mino_state_t *S, mino_val_t *comparator,
                                mino_val_t **keys, mino_val_t **vals,
                                size_t len);
mino_val_t *mino_sorted_set_by(mino_state_t *S, mino_val_t *comparator,
                                mino_val_t **items, size_t len);
mino_val_t *mino_sorted_map(mino_state_t *S, mino_val_t **keys,
                             mino_val_t **vals, size_t len);
mino_val_t *mino_sorted_set(mino_state_t *S, mino_val_t **items,
                             size_t len);
mino_val_t *sorted_map_assoc1(mino_state_t *S, const mino_val_t *m,
                               mino_val_t *key, mino_val_t *val);
mino_val_t *sorted_map_dissoc1(mino_state_t *S, const mino_val_t *m,
                                const mino_val_t *key);
mino_val_t *sorted_set_conj1(mino_state_t *S, const mino_val_t *s,
                              mino_val_t *elem);
mino_val_t *sorted_set_disj1(mino_state_t *S, const mino_val_t *s,
                              const mino_val_t *elem);
mino_val_t *sorted_seq(mino_state_t *S, const mino_val_t *coll);
mino_val_t *sorted_rest(mino_state_t *S, const mino_val_t *coll);
void rb_bounded_seq(mino_state_t *S, const mino_rb_node_t *n, int is_map,
                    int has_lo, int lo_inclusive, mino_val_t *lo,
                    int has_hi, int hi_inclusive, mino_val_t *hi,
                    mino_val_t *comparator, int reverse,
                    mino_val_t **head, mino_val_t **tail);

/* ------------------------------------------------------------------------- */
/* prim_bignum.c: bignum / ratio / bigdec value support                      */
/*                                                                           */
/* Declared here (not prim_internal.h) because the GC sweep paths call       */
/* mino_bigint_free when a bigint cell dies, the printer calls               */
/* mino_bigint_print, and val.c equality calls                               */
/* mino_bigint_equals / mino_bigint_hash.                                    */
/* ------------------------------------------------------------------------- */

void     mino_bigint_free(mino_val_t *v);
void     mino_bigint_print(mino_state_t *S, const mino_val_t *v, FILE *out);
int      mino_bigint_equals(const mino_val_t *a, const mino_val_t *b);
int      mino_bigint_equals_ll(const mino_val_t *a, long long n);
int      mino_bigint_cmp(const mino_val_t *a, const mino_val_t *b);
uint32_t mino_bigint_hash(const mino_val_t *v);
mino_val_t *mino_bigint_from_string_n(mino_state_t *S, const char *s, size_t len);
char    *mino_bigint_to_cstr(const mino_val_t *v);   /* malloc; caller frees */
int      mino_as_ll(const mino_val_t *v, long long *out);

/* Bigint arithmetic helpers for the promoting tower primitives. Each
 * accepts MINO_INT or MINO_BIGINT operands (callers classify first) and
 * returns a GC-owned MINO_BIGINT, or NULL on allocation failure (error
 * raised via prim_throw_classified). */
mino_val_t *mino_bigint_add(mino_state_t *S, const mino_val_t *a,
                            const mino_val_t *b);
mino_val_t *mino_bigint_sub(mino_state_t *S, const mino_val_t *a,
                            const mino_val_t *b);
mino_val_t *mino_bigint_mul(mino_state_t *S, const mino_val_t *a,
                            const mino_val_t *b);
mino_val_t *mino_bigint_neg(mino_state_t *S, const mino_val_t *a);
double   mino_bigint_to_double(const mino_val_t *v);

/* MINO_RATIO support. */
mino_val_t *mino_ratio_make(mino_state_t *S, mino_val_t *num, mino_val_t *denom);
mino_val_t *mino_ratio_make_unchecked(mino_state_t *S, mino_val_t *num,
                                      mino_val_t *denom);
void     mino_ratio_print(mino_state_t *S, const mino_val_t *v, FILE *out);
int      mino_ratio_equals(const mino_val_t *a, const mino_val_t *b);
int      mino_ratio_cmp(const mino_val_t *a, const mino_val_t *b);
uint32_t mino_ratio_hash(const mino_val_t *v);
double   mino_ratio_to_double(const mino_val_t *v);
mino_val_t *mino_ratio_add(mino_state_t *S, const mino_val_t *a,
                           const mino_val_t *b);
mino_val_t *mino_ratio_sub(mino_state_t *S, const mino_val_t *a,
                           const mino_val_t *b);
mino_val_t *mino_ratio_mul(mino_state_t *S, const mino_val_t *a,
                           const mino_val_t *b);
mino_val_t *mino_ratio_div(mino_state_t *S, const mino_val_t *a,
                           const mino_val_t *b);

/* MINO_BIGDEC support. */
mino_val_t *mino_bigdec_make(mino_state_t *S, mino_val_t *unscaled, int scale);
void     mino_bigdec_print(mino_state_t *S, const mino_val_t *v, FILE *out);
int      mino_bigdec_equals(const mino_val_t *a, const mino_val_t *b);
int      mino_bigdec_cmp(const mino_val_t *a, const mino_val_t *b);
uint32_t mino_bigdec_hash(const mino_val_t *v);
double   mino_bigdec_to_double(const mino_val_t *v);
mino_val_t *mino_bigdec_add(mino_state_t *S, const mino_val_t *a,
                            const mino_val_t *b);
mino_val_t *mino_bigdec_sub(mino_state_t *S, const mino_val_t *a,
                            const mino_val_t *b);
mino_val_t *mino_bigdec_mul(mino_state_t *S, const mino_val_t *a,
                            const mino_val_t *b);
mino_val_t *mino_bigdec_neg(mino_state_t *S, const mino_val_t *a);
mino_val_t *mino_to_bigdec(mino_state_t *S, const mino_val_t *v);

#endif /* COLLECTIONS_INTERNAL_H */
