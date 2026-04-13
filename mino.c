/*
 * mino.c — runtime implementation.
 *
 * Single-file amalgamation: this translation unit, paired with mino.h,
 * is the entire runtime. ANSI C, no external dependencies.
 */

#include "mino.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Allocation                                                                */
/* ------------------------------------------------------------------------- */

/*
 * v0.1 uses a simple per-allocation malloc with no reclamation. A real
 * collector arrives later in the roadmap; the API treats values as opaque
 * handles so the representation can change without breaking embedders.
 */
static mino_val_t *alloc_val(mino_type_t type)
{
    mino_val_t *v = (mino_val_t *)calloc(1, sizeof(*v));
    if (v == NULL) {
        /* Fatal: out of memory during construction. v0.1 has no recovery. */
        abort();
    }
    v->type = type;
    return v;
}

static char *dup_n(const char *s, size_t len)
{
    char *out = (char *)malloc(len + 1);
    if (out == NULL) {
        abort();
    }
    if (len > 0) {
        memcpy(out, s, len);
    }
    out[len] = '\0';
    return out;
}

/* ------------------------------------------------------------------------- */
/* Singletons                                                                */
/* ------------------------------------------------------------------------- */

static mino_val_t nil_singleton  = { MINO_NIL,  { 0 } };
static mino_val_t true_singleton  = { MINO_BOOL, { 1 } };
static mino_val_t false_singleton = { MINO_BOOL, { 0 } };

mino_val_t *mino_nil(void)   { return &nil_singleton; }
mino_val_t *mino_true(void)  { return &true_singleton; }
mino_val_t *mino_false(void) { return &false_singleton; }

/* ------------------------------------------------------------------------- */
/* Constructors                                                              */
/* ------------------------------------------------------------------------- */

mino_val_t *mino_int(long long n)
{
    mino_val_t *v = alloc_val(MINO_INT);
    v->as.i = n;
    return v;
}

mino_val_t *mino_float(double f)
{
    mino_val_t *v = alloc_val(MINO_FLOAT);
    v->as.f = f;
    return v;
}

mino_val_t *mino_string_n(const char *s, size_t len)
{
    mino_val_t *v = alloc_val(MINO_STRING);
    v->as.s.data = dup_n(s, len);
    v->as.s.len  = len;
    return v;
}

mino_val_t *mino_string(const char *s)
{
    return mino_string_n(s, strlen(s));
}

/*
 * Symbols and keywords are interned through small process-wide tables so
 * that identity comparison is pointer-equal after lookup. The tables are
 * flat arrays with linear scan — adequate until the v0.5 HAMT arrives and
 * the collector reclaims names. Entries live for the life of the process.
 */

typedef struct {
    mino_val_t **entries;
    size_t       len;
    size_t       cap;
} intern_table_t;

static intern_table_t sym_intern = { NULL, 0, 0 };
static intern_table_t kw_intern  = { NULL, 0, 0 };

static mino_val_t *intern_lookup_or_create(intern_table_t *tbl,
                                           mino_type_t type,
                                           const char *s, size_t len)
{
    size_t i;
    mino_val_t *v;
    for (i = 0; i < tbl->len; i++) {
        mino_val_t *e = tbl->entries[i];
        if (e->as.s.len == len && memcmp(e->as.s.data, s, len) == 0) {
            return e;
        }
    }
    if (tbl->len == tbl->cap) {
        size_t new_cap = tbl->cap == 0 ? 64 : tbl->cap * 2;
        mino_val_t **ne = (mino_val_t **)realloc(
            tbl->entries, new_cap * sizeof(*ne));
        if (ne == NULL) {
            abort();
        }
        tbl->entries = ne;
        tbl->cap = new_cap;
    }
    v = alloc_val(type);
    v->as.s.data = dup_n(s, len);
    v->as.s.len  = len;
    tbl->entries[tbl->len++] = v;
    return v;
}

mino_val_t *mino_symbol_n(const char *s, size_t len)
{
    return intern_lookup_or_create(&sym_intern, MINO_SYMBOL, s, len);
}

mino_val_t *mino_symbol(const char *s)
{
    return mino_symbol_n(s, strlen(s));
}

mino_val_t *mino_keyword_n(const char *s, size_t len)
{
    return intern_lookup_or_create(&kw_intern, MINO_KEYWORD, s, len);
}

mino_val_t *mino_keyword(const char *s)
{
    return mino_keyword_n(s, strlen(s));
}

mino_val_t *mino_cons(mino_val_t *car, mino_val_t *cdr)
{
    mino_val_t *v = alloc_val(MINO_CONS);
    v->as.cons.car = car;
    v->as.cons.cdr = cdr;
    return v;
}

/* ------------------------------------------------------------------------- */
/* Persistent vector: 32-way trie with tail                                  */
/* ------------------------------------------------------------------------- */
/*
 * Layout (Bagwell persistent vector):
 *   - tail holds the trailing 1..32 elements; appends that fit in the tail
 *     are O(1) amortized (one 32-slot copy, no trie walk).
 *   - root is a trie whose leaves each hold exactly 32 values; the rightmost
 *     spine may be partially filled (branches store a `count`).
 *   - shift encodes height: 0 means the root is itself a leaf; otherwise a
 *     branch whose children live at shift - 5.
 *   - All node mutations are path-copies: conj/assoc return fresh nodes along
 *     the walked path, leaving structural sharing with the source vector.
 *
 * Nodes are untyped unions of child pointers; the active variant is dictated
 * by the node's level during traversal rather than a per-node tag.
 */

#define MINO_VEC_B     5u
#define MINO_VEC_WIDTH (1u << MINO_VEC_B)
#define MINO_VEC_MASK  (MINO_VEC_WIDTH - 1u)

struct mino_vec_node {
    unsigned count;                    /* number of used slots */
    void    *slots[MINO_VEC_WIDTH];    /* mino_val_t* at leaves; mino_vec_node_t* at branches */
};

static mino_vec_node_t *vnode_new(unsigned count)
{
    mino_vec_node_t *n = (mino_vec_node_t *)calloc(1, sizeof(*n));
    if (n == NULL) {
        abort();
    }
    n->count = count;
    return n;
}

static mino_vec_node_t *vnode_clone(const mino_vec_node_t *src)
{
    mino_vec_node_t *n = (mino_vec_node_t *)malloc(sizeof(*n));
    if (n == NULL) {
        abort();
    }
    memcpy(n, src, sizeof(*n));
    return n;
}

/*
 * new_path: build a spine from a branch at level `shift` down to `leaf`,
 * placing the leaf in slot 0 at every level along the way.
 */
static mino_vec_node_t *new_path(unsigned shift, mino_vec_node_t *leaf)
{
    mino_vec_node_t *n;
    if (shift == 0) {
        return leaf;
    }
    n = vnode_new(1);
    n->slots[0] = new_path(shift - MINO_VEC_B, leaf);
    return n;
}

/*
 * push_tail: insert `leaf` into the subtree rooted at `node` (level `shift`),
 * at the position implied by `subindex` (the leaf's first element's flat
 * index into the trie). Path-copies the walked spine; returns the new root.
 * Caller ensures `subindex` fits within the current tree (no root overflow).
 */
static mino_vec_node_t *push_tail(const mino_vec_node_t *node, unsigned shift,
                                  size_t subindex, mino_vec_node_t *leaf)
{
    unsigned         digit = (unsigned)((subindex >> shift) & MINO_VEC_MASK);
    mino_vec_node_t *clone = vnode_clone(node);
    if (shift == MINO_VEC_B) {
        /* Children are leaves: place the tail directly. */
        clone->slots[digit] = leaf;
    } else {
        mino_vec_node_t *child = (mino_vec_node_t *)node->slots[digit];
        mino_vec_node_t *new_child = (child == NULL)
            ? new_path(shift - MINO_VEC_B, leaf)
            : push_tail(child, shift - MINO_VEC_B, subindex, leaf);
        clone->slots[digit] = new_child;
    }
    if (digit + 1u > clone->count) {
        clone->count = digit + 1u;
    }
    return clone;
}

/*
 * trie_assoc: path-copy update of the element at flat index `i`. Requires
 * i to refer to the trie (not the tail).
 */
static mino_vec_node_t *trie_assoc(const mino_vec_node_t *node, unsigned shift,
                                    size_t i, mino_val_t *item)
{
    mino_vec_node_t *clone = vnode_clone(node);
    if (shift == 0) {
        clone->slots[i & MINO_VEC_MASK] = item;
    } else {
        unsigned digit = (unsigned)((i >> shift) & MINO_VEC_MASK);
        clone->slots[digit] = trie_assoc((mino_vec_node_t *)node->slots[digit],
                                          shift - MINO_VEC_B, i, item);
    }
    return clone;
}

/* Construct a vector value from an already-built trie and tail. */
static mino_val_t *vec_assemble(mino_vec_node_t *root, mino_vec_node_t *tail,
                                 unsigned tail_len, unsigned shift, size_t len)
{
    mino_val_t *v = alloc_val(MINO_VECTOR);
    v->as.vec.root     = root;
    v->as.vec.tail     = tail;
    v->as.vec.tail_len = tail_len;
    v->as.vec.shift    = shift;
    v->as.vec.len      = len;
    return v;
}

/* Read one element by flat index; undefined if i >= len. */
static mino_val_t *vec_nth(const mino_val_t *v, size_t i)
{
    size_t                  trie_count = v->as.vec.len - v->as.vec.tail_len;
    const mino_vec_node_t  *node;
    unsigned                shift;
    if (i >= trie_count) {
        return (mino_val_t *)v->as.vec.tail->slots[i - trie_count];
    }
    node  = v->as.vec.root;
    shift = v->as.vec.shift;
    while (shift > 0) {
        node = (const mino_vec_node_t *)node->slots[(i >> shift) & MINO_VEC_MASK];
        shift -= MINO_VEC_B;
    }
    return (mino_val_t *)node->slots[i & MINO_VEC_MASK];
}

/* Append one element. O(log32 n) worst case, O(1) amortized for tail appends. */
static mino_val_t *vec_conj1(const mino_val_t *v, mino_val_t *item)
{
    mino_vec_node_t *new_tail;
    mino_vec_node_t *new_root;
    unsigned         new_shift;
    size_t           trie_count;
    if (v->as.vec.tail_len < MINO_VEC_WIDTH) {
        /* Tail has room: copy it and append. */
        if (v->as.vec.tail == NULL) {
            new_tail = vnode_new(1);
            new_tail->slots[0] = item;
        } else {
            new_tail = vnode_clone(v->as.vec.tail);
            new_tail->slots[v->as.vec.tail_len] = item;
            new_tail->count = v->as.vec.tail_len + 1u;
        }
        return vec_assemble(v->as.vec.root, new_tail,
                            v->as.vec.tail_len + 1u,
                            v->as.vec.shift, v->as.vec.len + 1u);
    }
    /* Tail is full: push it into the trie, start a fresh tail with the new item. */
    new_tail = vnode_new(1);
    new_tail->slots[0] = item;
    trie_count = v->as.vec.len - v->as.vec.tail_len; /* before incorporation */
    new_shift  = v->as.vec.shift;
    if (v->as.vec.root == NULL) {
        /* Trie was empty: old tail becomes the leaf root. */
        new_root  = v->as.vec.tail;
        new_shift = 0;
    } else if (trie_count == ((size_t)1u << (v->as.vec.shift + MINO_VEC_B))) {
        /* Root is full at the current height: add a level. */
        mino_vec_node_t *grown = vnode_new(2);
        grown->slots[0] = v->as.vec.root;
        grown->slots[1] = new_path(v->as.vec.shift, v->as.vec.tail);
        new_root  = grown;
        new_shift = v->as.vec.shift + MINO_VEC_B;
    } else {
        new_root = push_tail(v->as.vec.root, v->as.vec.shift, trie_count,
                             v->as.vec.tail);
    }
    return vec_assemble(new_root, new_tail, 1u, new_shift, v->as.vec.len + 1u);
}

/* Update index i. Index equal to len appends; any other out-of-range call is
 * the caller's responsibility to guard against. */
static mino_val_t *vec_assoc1(const mino_val_t *v, size_t i, mino_val_t *item)
{
    size_t           trie_count;
    mino_vec_node_t *new_tail;
    mino_vec_node_t *new_root;
    if (i == v->as.vec.len) {
        return vec_conj1(v, item);
    }
    trie_count = v->as.vec.len - v->as.vec.tail_len;
    if (i >= trie_count) {
        /* In the tail: copy and overwrite one slot. */
        new_tail = vnode_clone(v->as.vec.tail);
        new_tail->slots[i - trie_count] = item;
        return vec_assemble(v->as.vec.root, new_tail, v->as.vec.tail_len,
                            v->as.vec.shift, v->as.vec.len);
    }
    /* In the trie: path-copy the spine. */
    new_root = trie_assoc(v->as.vec.root, v->as.vec.shift, i, item);
    return vec_assemble(new_root, v->as.vec.tail, v->as.vec.tail_len,
                        v->as.vec.shift, v->as.vec.len);
}

/*
 * Bulk build from a flat source array — the internal "transient" path.
 *
 * Per-element vec_conj1 is O(log32 n) per append, so a naïve build walks
 * O(n log32 n) work and churns a path-copy on every boundary. vec_from_array
 * skips that: it freezes the last 1..32 elements as the tail, splits the rest
 * into full 32-slot leaves, and folds leaves into the spine layer by layer
 * (32 → 1 each pass). Every node is mutated in place during construction and
 * only becomes part of a persistent vector when vec_assemble returns, so no
 * transient state is observable outside this call.
 *
 * Total work is O(n): one pass writing leaves, then n/32 + n/1024 + ... ≤ n/31
 * more writes up the spine. Caller retains ownership of `items` and elements.
 */
static mino_val_t *vec_from_array(mino_val_t **items, size_t len)
{
    mino_vec_node_t  *tail;
    unsigned          tail_len;
    size_t            trie_count;
    size_t            num_leaves;
    mino_vec_node_t **layer;
    size_t            layer_n;
    unsigned          shift;
    size_t            i;
    if (len == 0) {
        return vec_assemble(NULL, NULL, 0u, 0u, 0);
    }
    tail_len = (unsigned)(len % MINO_VEC_WIDTH);
    if (tail_len == 0) {
        tail_len = MINO_VEC_WIDTH;
    }
    trie_count = len - tail_len;
    tail = vnode_new(tail_len);
    memcpy(tail->slots, items + trie_count, tail_len * sizeof(*items));
    if (trie_count == 0) {
        return vec_assemble(NULL, tail, tail_len, 0u, len);
    }
    num_leaves = trie_count / MINO_VEC_WIDTH;
    layer = (mino_vec_node_t **)malloc(num_leaves * sizeof(*layer));
    if (layer == NULL) {
        abort();
    }
    for (i = 0; i < num_leaves; i++) {
        mino_vec_node_t *leaf = vnode_new(MINO_VEC_WIDTH);
        memcpy(leaf->slots, items + i * MINO_VEC_WIDTH,
               MINO_VEC_WIDTH * sizeof(*items));
        layer[i] = leaf;
    }
    layer_n = num_leaves;
    shift   = 0;
    while (layer_n > 1) {
        size_t            next_n = (layer_n + MINO_VEC_WIDTH - 1) / MINO_VEC_WIDTH;
        mino_vec_node_t **next   = (mino_vec_node_t **)malloc(
                                       next_n * sizeof(*next));
        if (next == NULL) {
            abort();
        }
        for (i = 0; i < next_n; i++) {
            size_t           base = i * MINO_VEC_WIDTH;
            size_t           take = layer_n - base;
            mino_vec_node_t *node;
            if (take > MINO_VEC_WIDTH) {
                take = MINO_VEC_WIDTH;
            }
            node = vnode_new((unsigned)take);
            memcpy(node->slots, layer + base, take * sizeof(*layer));
            next[i] = node;
        }
        free(layer);
        layer   = next;
        layer_n = next_n;
        shift  += MINO_VEC_B;
    }
    {
        mino_vec_node_t *root = layer[0];
        free(layer);
        return vec_assemble(root, tail, tail_len, shift, len);
    }
}

mino_val_t *mino_vector(mino_val_t **items, size_t len)
{
    return vec_from_array(items, len);
}

/* ------------------------------------------------------------------------- */
/* Persistent map: 32-wide HAMT + insertion-order companion vector           */
/* ------------------------------------------------------------------------- */
/*
 * Each map value carries a HAMT root (for O(log32 n) lookup) and a companion
 * MINO_VECTOR of keys in insertion order (for iteration). assoc consults the
 * HAMT to decide whether the key is new (append to key_order) or a rebinding
 * (leave key_order alone; only the HAMT value changes).
 *
 * HAMT nodes come in two shapes, distinguished by collision_count:
 *   - Bitmap node (collision_count == 0):
 *       bitmap — which of 32 hash-digit slots are populated.
 *       subnode_mask — of populated slots, which hold a child node (the rest
 *         hold a direct (key, value) entry).
 *       slots — length popcount(bitmap), packed by slot index.
 *   - Collision bucket (collision_count > 0):
 *       bucket of full-hash-equal entries; slots[] holds hamt_entry_t*.
 *
 * Splitting and merging handle the depth-bounded hash: descent through five
 * bits per level admits up to seven levels before the 32-bit hash is
 * exhausted; past that, same-hash keys coexist in a collision bucket.
 *
 * The public map struct is immutable once assembled — every assoc returns a
 * fresh root that shares unmodified subtrees with its predecessor.
 */

#define HAMT_B     5u
#define HAMT_W     (1u << HAMT_B)
#define HAMT_MASK  (HAMT_W - 1u)

typedef struct {
    mino_val_t *key;
    mino_val_t *val;
} hamt_entry_t;

struct mino_hamt_node {
    uint32_t        bitmap;          /* bitmap mode: populated slot mask */
    uint32_t        subnode_mask;    /* bitmap mode: of populated, which are subnodes */
    uint32_t        collision_hash;  /* collision mode: shared hash of all entries */
    unsigned        collision_count; /* 0 in bitmap mode; >0 in collision mode */
    void          **slots;           /* length = popcount(bitmap) or collision_count */
};

static unsigned popcount32(uint32_t x)
{
    unsigned c = 0;
    while (x != 0) {
        x &= x - 1u;
        c++;
    }
    return c;
}

static hamt_entry_t *hamt_entry_new(mino_val_t *key, mino_val_t *val)
{
    hamt_entry_t *e = (hamt_entry_t *)malloc(sizeof(*e));
    if (e == NULL) {
        abort();
    }
    e->key = key;
    e->val = val;
    return e;
}

/* Forward declaration for mutual recursion. */
static uint32_t hash_val(const mino_val_t *v);

static uint32_t fnv_mix(uint32_t h, unsigned char b)
{
    h ^= (uint32_t)b;
    h *= 16777619u;
    return h;
}

static uint32_t fnv_bytes(uint32_t h, const unsigned char *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        h = fnv_mix(h, p[i]);
    }
    return h;
}

/*
 * Hash function compatible with mino_eq:
 *   - Integral floats hash the same as the equivalent int so (= 1 1.0) ⇒ 1.
 *   - Strings, symbols, and keywords each carry a distinct type tag so byte-
 *     equal values of different types hash differently (and compare unequal).
 *   - Collections hash their contents; maps XOR-fold entry hashes for
 *     order-insensitivity.
 *   - Non-hashable types (PRIM, FN) fall back to pointer identity.
 */
static uint32_t hash_val(const mino_val_t *v)
{
    uint32_t h = 2166136261u;   /* FNV-1a offset basis */
    if (v == NULL || v->type == MINO_NIL) {
        return fnv_mix(h, 0x01);
    }
    switch (v->type) {
    case MINO_NIL:
        return fnv_mix(h, 0x01);
    case MINO_BOOL:
        h = fnv_mix(h, 0x02);
        return fnv_mix(h, (unsigned char)(v->as.b ? 1 : 0));
    case MINO_INT: {
        long long n = v->as.i;
        unsigned  i;
        h = fnv_mix(h, 0x03);
        for (i = 0; i < 8; i++) {
            h = fnv_mix(h, (unsigned char)(n & 0xFFu));
            n >>= 8;
        }
        return h;
    }
    case MINO_FLOAT: {
        double    d  = v->as.f;
        long long ll = (long long)d;
        if ((double)ll == d) {
            /* Same tag as MINO_INT so (= 1 1.0) matches in hash too. */
            unsigned i;
            h = fnv_mix(h, 0x03);
            for (i = 0; i < 8; i++) {
                h = fnv_mix(h, (unsigned char)(ll & 0xFFu));
                ll >>= 8;
            }
            return h;
        }
        h = fnv_mix(h, 0x04);
        {
            unsigned char buf[sizeof(double)];
            memcpy(buf, &d, sizeof(d));
            return fnv_bytes(h, buf, sizeof(d));
        }
    }
    case MINO_STRING:
        h = fnv_mix(h, 0x05);
        return fnv_bytes(h, (const unsigned char *)v->as.s.data, v->as.s.len);
    case MINO_SYMBOL:
        h = fnv_mix(h, 0x06);
        return fnv_bytes(h, (const unsigned char *)v->as.s.data, v->as.s.len);
    case MINO_KEYWORD:
        h = fnv_mix(h, 0x07);
        return fnv_bytes(h, (const unsigned char *)v->as.s.data, v->as.s.len);
    case MINO_CONS: {
        uint32_t hc = hash_val(v->as.cons.car);
        uint32_t hd = hash_val(v->as.cons.cdr);
        unsigned i;
        h = fnv_mix(h, 0x08);
        for (i = 0; i < 4; i++) { h = fnv_mix(h, (unsigned char)(hc & 0xFFu)); hc >>= 8; }
        for (i = 0; i < 4; i++) { h = fnv_mix(h, (unsigned char)(hd & 0xFFu)); hd >>= 8; }
        return h;
    }
    case MINO_VECTOR: {
        size_t n = v->as.vec.len;
        size_t i;
        h = fnv_mix(h, 0x09);
        for (i = 0; i < n; i++) {
            uint32_t hi = hash_val(vec_nth(v, i));
            unsigned k;
            for (k = 0; k < 4; k++) {
                h = fnv_mix(h, (unsigned char)(hi & 0xFFu));
                hi >>= 8;
            }
        }
        return h;
    }
    case MINO_MAP: {
        /* XOR-fold of per-entry hashes for order independence. Each entry's
         * hash mixes key and value hashes with a prime to avoid (k ^ v)
         * self-cancellation when k == v. */
        uint32_t acc = 0;
        size_t   n   = v->as.map.len;
        size_t   i;
        unsigned k;
        for (i = 0; i < n; i++) {
            mino_val_t *key = vec_nth(v->as.map.key_order, i);
            mino_val_t *val;
            uint32_t    hk  = hash_val(key);
            uint32_t    hv;
            val = (mino_val_t *)(size_t)0; /* silence warnings */
            (void)val;
            /* Look up the value via the HAMT at the root. Use identity
             * equality via the same hash path. */
            {
                const mino_hamt_node_t *n2 = v->as.map.root;
                uint32_t                hh = hk;
                unsigned                shift = 0;
                mino_val_t             *found = NULL;
                while (n2 != NULL) {
                    if (n2->collision_count > 0) {
                        unsigned j;
                        for (j = 0; j < n2->collision_count; j++) {
                            hamt_entry_t *e = (hamt_entry_t *)n2->slots[j];
                            if (mino_eq(e->key, key)) { found = e->val; break; }
                        }
                        break;
                    } else {
                        uint32_t bit  = 1u << ((hh >> shift) & HAMT_MASK);
                        unsigned phys;
                        if ((n2->bitmap & bit) == 0) break;
                        phys = popcount32(n2->bitmap & (bit - 1u));
                        if (n2->subnode_mask & bit) {
                            n2 = (const mino_hamt_node_t *)n2->slots[phys];
                            shift += HAMT_B;
                        } else {
                            hamt_entry_t *e = (hamt_entry_t *)n2->slots[phys];
                            if (mino_eq(e->key, key)) { found = e->val; }
                            break;
                        }
                    }
                }
                hv = hash_val(found);
            }
            hk ^= hv * 2654435761u;
            acc ^= hk;
            (void)k;
        }
        h = fnv_mix(h, 0x0a);
        {
            unsigned i2;
            for (i2 = 0; i2 < 4; i2++) {
                h = fnv_mix(h, (unsigned char)(acc & 0xFFu));
                acc >>= 8;
            }
        }
        return h;
    }
    default: {
        /* PRIM, FN, RECUR: identity-based. */
        uintptr_t p = (uintptr_t)v;
        unsigned  i;
        h = fnv_mix(h, 0x0b);
        for (i = 0; i < sizeof(uintptr_t); i++) {
            h = fnv_mix(h, (unsigned char)(p & 0xFFu));
            p >>= 8;
        }
        return h;
    }
    }
}

static mino_hamt_node_t *hamt_bitmap_node(uint32_t bitmap, uint32_t subnode_mask,
                                           void **slots)
{
    mino_hamt_node_t *n = (mino_hamt_node_t *)calloc(1, sizeof(*n));
    if (n == NULL) {
        abort();
    }
    n->bitmap       = bitmap;
    n->subnode_mask = subnode_mask;
    n->slots        = slots;
    return n;
}

static mino_hamt_node_t *hamt_collision_node(uint32_t hash, void **slots,
                                              unsigned count)
{
    mino_hamt_node_t *n = (mino_hamt_node_t *)calloc(1, sizeof(*n));
    if (n == NULL) {
        abort();
    }
    n->collision_hash  = hash;
    n->collision_count = count;
    n->slots           = slots;
    return n;
}

/*
 * merge_entries: build the smallest subtree that separates two leaf entries
 * whose hashes collide at `shift - HAMT_B` (the parent level). The returned
 * subtree lives at level `shift`.
 */
static mino_hamt_node_t *merge_entries(hamt_entry_t *e1, uint32_t h1,
                                        hamt_entry_t *e2, uint32_t h2,
                                        unsigned shift)
{
    if (h1 == h2 || shift >= 32u) {
        /* Can't separate further: collision bucket. */
        void **slots = (void **)malloc(2 * sizeof(*slots));
        if (slots == NULL) { abort(); }
        slots[0] = e1;
        slots[1] = e2;
        return hamt_collision_node(h1, slots, 2);
    }
    {
        unsigned i1 = (unsigned)((h1 >> shift) & HAMT_MASK);
        unsigned i2 = (unsigned)((h2 >> shift) & HAMT_MASK);
        if (i1 == i2) {
            mino_hamt_node_t *child = merge_entries(e1, h1, e2, h2,
                                                     shift + HAMT_B);
            void **slots = (void **)malloc(sizeof(*slots));
            if (slots == NULL) { abort(); }
            slots[0] = child;
            return hamt_bitmap_node(1u << i1, 1u << i1, slots);
        } else {
            void **slots = (void **)malloc(2 * sizeof(*slots));
            if (slots == NULL) { abort(); }
            if (i1 < i2) {
                slots[0] = e1; slots[1] = e2;
            } else {
                slots[0] = e2; slots[1] = e1;
            }
            return hamt_bitmap_node((1u << i1) | (1u << i2), 0u, slots);
        }
    }
}

/*
 * hamt_assoc: insert or rebind `new_entry` in the subtree rooted at `n`.
 * Sets *replaced = 1 when the key was already present (so the map's len
 * and key_order don't grow).
 */
static mino_hamt_node_t *hamt_assoc(const mino_hamt_node_t *n,
                                     hamt_entry_t *new_entry, uint32_t h,
                                     unsigned shift, int *replaced)
{
    if (n == NULL) {
        unsigned  i     = (unsigned)((h >> shift) & HAMT_MASK);
        void    **slots = (void **)malloc(sizeof(*slots));
        if (slots == NULL) { abort(); }
        slots[0] = new_entry;
        return hamt_bitmap_node(1u << i, 0u, slots);
    }
    if (n->collision_count > 0) {
        /* Either hash matches the bucket's (update or append) or it doesn't
         * (promote to a bitmap node that routes them separately). */
        if (h == n->collision_hash) {
            unsigned j;
            for (j = 0; j < n->collision_count; j++) {
                hamt_entry_t *e = (hamt_entry_t *)n->slots[j];
                if (mino_eq(e->key, new_entry->key)) {
                    void **slots = (void **)malloc(n->collision_count * sizeof(*slots));
                    unsigned k;
                    if (slots == NULL) { abort(); }
                    for (k = 0; k < n->collision_count; k++) { slots[k] = n->slots[k]; }
                    slots[j] = new_entry;
                    *replaced = 1;
                    return hamt_collision_node(h, slots, n->collision_count);
                }
            }
            {
                void **slots = (void **)malloc((n->collision_count + 1u) * sizeof(*slots));
                unsigned k;
                if (slots == NULL) { abort(); }
                for (k = 0; k < n->collision_count; k++) { slots[k] = n->slots[k]; }
                slots[n->collision_count] = new_entry;
                return hamt_collision_node(h, slots, n->collision_count + 1u);
            }
        }
        {
            /* Promote: wrap the collision bucket so it lives in one slot of
             * a bitmap node at this level, then insert the new entry. */
            unsigned  ib     = (unsigned)((n->collision_hash >> shift) & HAMT_MASK);
            unsigned  in     = (unsigned)((h               >> shift) & HAMT_MASK);
            if (ib == in) {
                /* Deeper shared prefix: descend. */
                mino_hamt_node_t *sub   = hamt_assoc(n, new_entry, h,
                                                      shift + HAMT_B, replaced);
                void            **slots = (void **)malloc(sizeof(*slots));
                if (slots == NULL) { abort(); }
                slots[0] = sub;
                return hamt_bitmap_node(1u << ib, 1u << ib, slots);
            } else {
                void **slots = (void **)malloc(2 * sizeof(*slots));
                uint32_t bitmap       = (1u << ib) | (1u << in);
                uint32_t subnode_mask = 1u << ib;
                if (slots == NULL) { abort(); }
                if (ib < in) {
                    slots[0] = (void *)n;
                    slots[1] = new_entry;
                } else {
                    slots[0] = new_entry;
                    slots[1] = (void *)n;
                }
                return hamt_bitmap_node(bitmap, subnode_mask, slots);
            }
        }
    }
    {
        unsigned  i    = (unsigned)((h >> shift) & HAMT_MASK);
        uint32_t  bit  = 1u << i;
        unsigned  phys = popcount32(n->bitmap & (bit - 1u));
        unsigned  pop  = popcount32(n->bitmap);
        if ((n->bitmap & bit) == 0) {
            /* Empty slot: insert directly. */
            void **slots = (void **)malloc((pop + 1u) * sizeof(*slots));
            unsigned k;
            if (slots == NULL) { abort(); }
            for (k = 0; k < phys; k++)        { slots[k]        = n->slots[k];       }
            slots[phys] = new_entry;
            for (k = phys; k < pop; k++)      { slots[k + 1]    = n->slots[k];       }
            return hamt_bitmap_node(n->bitmap | bit, n->subnode_mask, slots);
        }
        if (n->subnode_mask & bit) {
            /* Child subtree: recurse, then rewrap. */
            mino_hamt_node_t *new_child = hamt_assoc(
                (const mino_hamt_node_t *)n->slots[phys], new_entry, h,
                shift + HAMT_B, replaced);
            void **slots = (void **)malloc(pop * sizeof(*slots));
            unsigned k;
            if (slots == NULL) { abort(); }
            for (k = 0; k < pop; k++) { slots[k] = n->slots[k]; }
            slots[phys] = new_child;
            return hamt_bitmap_node(n->bitmap, n->subnode_mask, slots);
        }
        {
            /* Leaf entry in slot. Same key → replace. Different key → split. */
            hamt_entry_t *existing = (hamt_entry_t *)n->slots[phys];
            if (mino_eq(existing->key, new_entry->key)) {
                void **slots = (void **)malloc(pop * sizeof(*slots));
                unsigned k;
                if (slots == NULL) { abort(); }
                for (k = 0; k < pop; k++) { slots[k] = n->slots[k]; }
                slots[phys] = new_entry;
                *replaced = 1;
                return hamt_bitmap_node(n->bitmap, n->subnode_mask, slots);
            }
            {
                uint32_t          eh   = hash_val(existing->key);
                mino_hamt_node_t *sub  = merge_entries(existing, eh,
                                                        new_entry, h,
                                                        shift + HAMT_B);
                void            **slots = (void **)malloc(pop * sizeof(*slots));
                unsigned k;
                if (slots == NULL) { abort(); }
                for (k = 0; k < pop; k++) { slots[k] = n->slots[k]; }
                slots[phys] = sub;
                return hamt_bitmap_node(n->bitmap,
                                         n->subnode_mask | bit, slots);
            }
        }
    }
}

/* Look up a key; returns NULL if absent. */
static mino_val_t *hamt_get(const mino_hamt_node_t *n, const mino_val_t *key,
                             uint32_t h, unsigned shift)
{
    while (n != NULL) {
        if (n->collision_count > 0) {
            unsigned i;
            if (h != n->collision_hash) {
                return NULL;
            }
            for (i = 0; i < n->collision_count; i++) {
                hamt_entry_t *e = (hamt_entry_t *)n->slots[i];
                if (mino_eq(e->key, key)) {
                    return e->val;
                }
            }
            return NULL;
        }
        {
            uint32_t bit = 1u << ((h >> shift) & HAMT_MASK);
            unsigned phys;
            if ((n->bitmap & bit) == 0) {
                return NULL;
            }
            phys = popcount32(n->bitmap & (bit - 1u));
            if (n->subnode_mask & bit) {
                n = (const mino_hamt_node_t *)n->slots[phys];
                shift += HAMT_B;
            } else {
                hamt_entry_t *e = (hamt_entry_t *)n->slots[phys];
                return mino_eq(e->key, key) ? e->val : NULL;
            }
        }
    }
    return NULL;
}

/* Convenience: look up a key in a map value. */
static mino_val_t *map_get_val(const mino_val_t *m, const mino_val_t *key)
{
    uint32_t h = hash_val(key);
    return hamt_get(m->as.map.root, key, h, 0u);
}

/*
 * Map construction. The semantics remain: duplicate keys resolve
 * last-write-wins, and the resulting map iterates keys in the order they
 * first appeared in the source sequence. Caller retains ownership of the
 * source arrays.
 */
mino_val_t *mino_map(mino_val_t **keys, mino_val_t **vals, size_t len)
{
    mino_val_t       *v        = alloc_val(MINO_MAP);
    mino_hamt_node_t *root     = NULL;
    mino_val_t       *order    = mino_vector(NULL, 0);
    size_t            len_out  = 0;
    size_t            i;
    for (i = 0; i < len; i++) {
        hamt_entry_t *e        = hamt_entry_new(keys[i], vals[i]);
        uint32_t      h        = hash_val(keys[i]);
        int           replaced = 0;
        root = hamt_assoc(root, e, h, 0u, &replaced);
        if (!replaced) {
            order = vec_conj1(order, keys[i]);
            len_out++;
        }
    }
    v->as.map.root      = root;
    v->as.map.key_order = order;
    v->as.map.len       = len_out;
    return v;
}

mino_val_t *mino_prim(const char *name, mino_prim_fn fn)
{
    mino_val_t *v = alloc_val(MINO_PRIM);
    v->as.prim.name = name;
    v->as.prim.fn   = fn;
    return v;
}

static mino_val_t *make_fn(mino_val_t *params, mino_val_t *body,
                           mino_env_t *env)
{
    mino_val_t *v = alloc_val(MINO_FN);
    v->as.fn.params = params;
    v->as.fn.body   = body;
    v->as.fn.env    = env;
    return v;
}

/* ------------------------------------------------------------------------- */
/* Predicates and accessors                                                  */
/* ------------------------------------------------------------------------- */

int mino_is_nil(const mino_val_t *v)
{
    return v == NULL || v->type == MINO_NIL;
}

int mino_is_truthy(const mino_val_t *v)
{
    if (v == NULL) {
        return 0;
    }
    if (v->type == MINO_NIL) {
        return 0;
    }
    if (v->type == MINO_BOOL) {
        return v->as.b != 0;
    }
    return 1;
}

int mino_is_cons(const mino_val_t *v)
{
    return v != NULL && v->type == MINO_CONS;
}

mino_val_t *mino_car(const mino_val_t *v)
{
    if (!mino_is_cons(v)) {
        return mino_nil();
    }
    return v->as.cons.car;
}

mino_val_t *mino_cdr(const mino_val_t *v)
{
    if (!mino_is_cons(v)) {
        return mino_nil();
    }
    return v->as.cons.cdr;
}

size_t mino_length(const mino_val_t *list)
{
    size_t n = 0;
    while (mino_is_cons(list)) {
        n++;
        list = list->as.cons.cdr;
    }
    return n;
}

/* ------------------------------------------------------------------------- */
/* Printer                                                                   */
/* ------------------------------------------------------------------------- */

static void print_string_escaped(FILE *out, const char *s, size_t len)
{
    size_t i;
    fputc('"', out);
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n",  out); break;
        case '\t': fputs("\\t",  out); break;
        case '\r': fputs("\\r",  out); break;
        case '\0': fputs("\\0",  out); break;
        default:   fputc((int)c, out); break;
        }
    }
    fputc('"', out);
}

void mino_print_to(FILE *out, const mino_val_t *v)
{
    if (v == NULL || v->type == MINO_NIL) {
        fputs("nil", out);
        return;
    }
    switch (v->type) {
    case MINO_NIL:
        fputs("nil", out);
        return;
    case MINO_BOOL:
        fputs(v->as.b ? "true" : "false", out);
        return;
    case MINO_INT:
        fprintf(out, "%lld", v->as.i);
        return;
    case MINO_FLOAT: {
        /*
         * Always include a decimal point so the printed form re-reads as a
         * float, not an int. %g may drop the dot for whole numbers.
         */
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "%g", v->as.f);
        int needs_dot = 1;
        int i;
        if (n < 0) {
            fputs("nan", out);
            return;
        }
        for (i = 0; i < n; i++) {
            if (buf[i] == '.' || buf[i] == 'e' || buf[i] == 'E'
                || buf[i] == 'n' || buf[i] == 'i') {
                needs_dot = 0;
                break;
            }
        }
        fputs(buf, out);
        if (needs_dot) {
            fputs(".0", out);
        }
        return;
    }
    case MINO_STRING:
        print_string_escaped(out, v->as.s.data, v->as.s.len);
        return;
    case MINO_SYMBOL:
        fwrite(v->as.s.data, 1, v->as.s.len, out);
        return;
    case MINO_KEYWORD:
        fputc(':', out);
        fwrite(v->as.s.data, 1, v->as.s.len, out);
        return;
    case MINO_CONS: {
        const mino_val_t *p = v;
        fputc('(', out);
        while (p != NULL && p->type == MINO_CONS) {
            mino_print_to(out, p->as.cons.car);
            p = p->as.cons.cdr;
            if (p != NULL && p->type == MINO_CONS) {
                fputc(' ', out);
            } else if (p != NULL && p->type != MINO_NIL) {
                fputs(" . ", out);
                mino_print_to(out, p);
                break;
            }
        }
        fputc(')', out);
        return;
    }
    case MINO_VECTOR: {
        size_t i;
        fputc('[', out);
        for (i = 0; i < v->as.vec.len; i++) {
            if (i > 0) {
                fputc(' ', out);
            }
            mino_print_to(out, vec_nth(v, i));
        }
        fputc(']', out);
        return;
    }
    case MINO_MAP: {
        size_t i;
        fputc('{', out);
        for (i = 0; i < v->as.map.len; i++) {
            mino_val_t *key = vec_nth(v->as.map.key_order, i);
            if (i > 0) {
                fputs(", ", out);
            }
            mino_print_to(out, key);
            fputc(' ', out);
            mino_print_to(out, map_get_val(v, key));
        }
        fputc('}', out);
        return;
    }
    case MINO_PRIM:
        fputs("#<prim ", out);
        if (v->as.prim.name != NULL) {
            fputs(v->as.prim.name, out);
        }
        fputc('>', out);
        return;
    case MINO_FN:
        fputs("#<fn>", out);
        return;
    case MINO_MACRO:
        fputs("#<macro>", out);
        return;
    case MINO_RECUR:
        /* Internal sentinel; should not escape to user-visible output. */
        fputs("#<recur>", out);
        return;
    }
}

void mino_print(const mino_val_t *v)
{
    mino_print_to(stdout, v);
}

void mino_println(const mino_val_t *v)
{
    mino_print_to(stdout, v);
    fputc('\n', stdout);
}

/* ------------------------------------------------------------------------- */
/* Error reporting                                                           */
/* ------------------------------------------------------------------------- */

static char error_buf[256] = { 0 };

const char *mino_last_error(void)
{
    return error_buf[0] ? error_buf : NULL;
}

static void set_error(const char *msg)
{
    size_t n = strlen(msg);
    if (n >= sizeof(error_buf)) {
        n = sizeof(error_buf) - 1;
    }
    memcpy(error_buf, msg, n);
    error_buf[n] = '\0';
}

static void clear_error(void)
{
    error_buf[0] = '\0';
}

/* ------------------------------------------------------------------------- */
/* Reader                                                                    */
/* ------------------------------------------------------------------------- */

static int is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',';
}

static int is_terminator(char c)
{
    return c == '\0' || c == '(' || c == ')' || c == '[' || c == ']'
        || c == '{' || c == '}' || c == '\'' || c == '"' || c == ';'
        || c == '`'  || c == '~'
        || is_ws(c);
}

static void skip_ws(const char **p)
{
    while (**p) {
        char c = **p;
        if (is_ws(c)) {
            (*p)++;
        } else if (c == ';') {
            while (**p && **p != '\n') {
                (*p)++;
            }
        } else {
            return;
        }
    }
}

static mino_val_t *read_form(const char **p);

static mino_val_t *read_string_form(const char **p)
{
    /* Caller has positioned *p on the opening '"'. */
    char *buf;
    size_t cap = 16;
    size_t len = 0;
    (*p)++; /* skip opening quote */
    buf = (char *)malloc(cap);
    if (buf == NULL) {
        abort();
    }
    while (**p && **p != '"') {
        char c = **p;
        if (c == '\\') {
            (*p)++;
            switch (**p) {
            case 'n':  c = '\n'; break;
            case 't':  c = '\t'; break;
            case 'r':  c = '\r'; break;
            case '\\': c = '\\'; break;
            case '"':  c = '"';  break;
            case '0':  c = '\0'; break;
            case '\0':
                free(buf);
                set_error("unterminated string literal");
                return NULL;
            default:
                /* Unknown escape: keep the character literally. */
                c = **p;
                break;
            }
        }
        if (len + 1 >= cap) {
            cap *= 2;
            buf = (char *)realloc(buf, cap);
            if (buf == NULL) {
                abort();
            }
        }
        buf[len++] = c;
        (*p)++;
    }
    if (**p != '"') {
        free(buf);
        set_error("unterminated string literal");
        return NULL;
    }
    (*p)++; /* skip closing quote */
    {
        mino_val_t *v = mino_string_n(buf, len);
        free(buf);
        return v;
    }
}

static mino_val_t *read_list_form(const char **p)
{
    /* Caller has positioned *p on the opening '('. */
    mino_val_t *head = mino_nil();
    mino_val_t *tail = NULL;
    (*p)++; /* skip '(' */
    for (;;) {
        skip_ws(p);
        if (**p == '\0') {
            set_error("unterminated list");
            return NULL;
        }
        if (**p == ')') {
            (*p)++;
            return head;
        }
        {
            mino_val_t *elem = read_form(p);
            if (elem == NULL && mino_last_error() != NULL) {
                return NULL;
            }
            if (elem == NULL) {
                /* EOF mid-list */
                set_error("unterminated list");
                return NULL;
            }
            {
                mino_val_t *cell = mino_cons(elem, mino_nil());
                if (tail == NULL) {
                    head = cell;
                } else {
                    tail->as.cons.cdr = cell;
                }
                tail = cell;
            }
        }
    }
}

static mino_val_t *read_vector_form(const char **p)
{
    /* Caller has positioned *p on the opening '['. */
    mino_val_t **buf = NULL;
    size_t       cap = 0;
    size_t       len = 0;
    mino_val_t  *result;
    (*p)++; /* skip '[' */
    for (;;) {
        skip_ws(p);
        if (**p == '\0') {
            free(buf);
            set_error("unterminated vector");
            return NULL;
        }
        if (**p == ']') {
            (*p)++;
            break;
        }
        {
            mino_val_t *elem = read_form(p);
            if (elem == NULL) {
                free(buf);
                if (mino_last_error() == NULL) {
                    set_error("unterminated vector");
                }
                return NULL;
            }
            if (len == cap) {
                cap = cap == 0 ? 8 : cap * 2;
                buf = (mino_val_t **)realloc(buf, cap * sizeof(*buf));
                if (buf == NULL) {
                    abort();
                }
            }
            buf[len++] = elem;
        }
    }
    result = mino_vector(buf, len);
    free(buf);
    return result;
}

static mino_val_t *read_map_form(const char **p)
{
    /* Caller has positioned *p on the opening '{'. Elements alternate as
     * key, value, key, value. An odd count is a parse error. */
    mino_val_t **kbuf = NULL;
    mino_val_t **vbuf = NULL;
    size_t       cap  = 0;
    size_t       len  = 0;
    mino_val_t  *result;
    (*p)++; /* skip '{' */
    for (;;) {
        mino_val_t *key;
        mino_val_t *val;
        skip_ws(p);
        if (**p == '\0') {
            free(kbuf); free(vbuf);
            set_error("unterminated map");
            return NULL;
        }
        if (**p == '}') {
            (*p)++;
            break;
        }
        key = read_form(p);
        if (key == NULL) {
            free(kbuf); free(vbuf);
            if (mino_last_error() == NULL) {
                set_error("unterminated map");
            }
            return NULL;
        }
        skip_ws(p);
        if (**p == '}' || **p == '\0') {
            free(kbuf); free(vbuf);
            set_error("map literal has odd number of forms");
            return NULL;
        }
        val = read_form(p);
        if (val == NULL) {
            free(kbuf); free(vbuf);
            if (mino_last_error() == NULL) {
                set_error("unterminated map");
            }
            return NULL;
        }
        if (len == cap) {
            cap = cap == 0 ? 8 : cap * 2;
            kbuf = (mino_val_t **)realloc(kbuf, cap * sizeof(*kbuf));
            vbuf = (mino_val_t **)realloc(vbuf, cap * sizeof(*vbuf));
            if (kbuf == NULL || vbuf == NULL) {
                abort();
            }
        }
        kbuf[len] = key;
        vbuf[len] = val;
        len++;
    }
    result = mino_map(kbuf, vbuf, len);
    free(kbuf); free(vbuf);
    return result;
}

static mino_val_t *read_atom(const char **p)
{
    const char *start = *p;
    size_t len = 0;
    while (!is_terminator((*p)[len])) {
        len++;
    }
    *p += len;

    if (len >= 2 && start[0] == ':') {
        return mino_keyword_n(start + 1, len - 1);
    }
    if (len == 1 && start[0] == ':') {
        set_error("keyword missing name");
        return NULL;
    }

    if (len == 3 && memcmp(start, "nil", 3) == 0) {
        return mino_nil();
    }
    if (len == 4 && memcmp(start, "true", 4) == 0) {
        return mino_true();
    }
    if (len == 5 && memcmp(start, "false", 5) == 0) {
        return mino_false();
    }

    /* Try numeric. */
    {
        char buf[64];
        char *endp = NULL;
        if (len < sizeof(buf)) {
            int has_dot_or_exp = 0;
            int looks_numeric = 1;
            size_t i = 0;
            size_t scan_start = 0;
            memcpy(buf, start, len);
            buf[len] = '\0';
            if (buf[0] == '+' || buf[0] == '-') {
                scan_start = 1;
            }
            if (scan_start == len) {
                looks_numeric = 0;
            }
            for (i = scan_start; i < len; i++) {
                char c = buf[i];
                if (c == '.' || c == 'e' || c == 'E') {
                    has_dot_or_exp = 1;
                } else if (!isdigit((unsigned char)c)) {
                    looks_numeric = 0;
                    break;
                }
            }
            if (looks_numeric) {
                if (has_dot_or_exp) {
                    double d = strtod(buf, &endp);
                    if (endp == buf + len) {
                        return mino_float(d);
                    }
                } else {
                    long long n = strtoll(buf, &endp, 10);
                    if (endp == buf + len) {
                        return mino_int(n);
                    }
                }
            }
        }
    }

    return mino_symbol_n(start, len);
}

static mino_val_t *read_form(const char **p)
{
    skip_ws(p);
    if (**p == '\0') {
        return NULL;
    }
    if (**p == '(') {
        return read_list_form(p);
    }
    if (**p == ')') {
        set_error("unexpected ')'");
        return NULL;
    }
    if (**p == '[') {
        return read_vector_form(p);
    }
    if (**p == ']') {
        set_error("unexpected ']'");
        return NULL;
    }
    if (**p == '{') {
        return read_map_form(p);
    }
    if (**p == '}') {
        set_error("unexpected '}'");
        return NULL;
    }
    if (**p == '"') {
        return read_string_form(p);
    }
    if (**p == '\'') {
        (*p)++;
        {
            mino_val_t *quoted = read_form(p);
            if (quoted == NULL) {
                if (mino_last_error() == NULL) {
                    set_error("expected form after quote");
                }
                return NULL;
            }
            return mino_cons(mino_symbol("quote"),
                             mino_cons(quoted, mino_nil()));
        }
    }
    if (**p == '`') {
        (*p)++;
        {
            mino_val_t *qq = read_form(p);
            if (qq == NULL) {
                if (mino_last_error() == NULL) {
                    set_error("expected form after `");
                }
                return NULL;
            }
            return mino_cons(mino_symbol("quasiquote"),
                             mino_cons(qq, mino_nil()));
        }
    }
    if (**p == '~') {
        const char *name = "unquote";
        (*p)++;
        if (**p == '@') {
            name = "unquote-splicing";
            (*p)++;
        }
        {
            mino_val_t *uq = read_form(p);
            if (uq == NULL) {
                if (mino_last_error() == NULL) {
                    set_error("expected form after ~");
                }
                return NULL;
            }
            return mino_cons(mino_symbol(name),
                             mino_cons(uq, mino_nil()));
        }
    }
    return read_atom(p);
}

mino_val_t *mino_read(const char *src, const char **end)
{
    const char *p = src;
    mino_val_t *v;
    clear_error();
    v = read_form(&p);
    if (end != NULL) {
        *end = p;
    }
    return v;
}

/* ------------------------------------------------------------------------- */
/* Equality                                                                  */
/* ------------------------------------------------------------------------- */

int mino_eq(const mino_val_t *a, const mino_val_t *b)
{
    if (a == b) {
        return 1;
    }
    if (a == NULL || b == NULL) {
        return 0;
    }
    if (a->type != b->type) {
        /*
         * Cross-type numeric equality: int and float compare by value.
         */
        if (a->type == MINO_INT && b->type == MINO_FLOAT) {
            return (double)a->as.i == b->as.f;
        }
        if (a->type == MINO_FLOAT && b->type == MINO_INT) {
            return a->as.f == (double)b->as.i;
        }
        return 0;
    }
    switch (a->type) {
    case MINO_NIL:
        return 1;
    case MINO_BOOL:
        return a->as.b == b->as.b;
    case MINO_INT:
        return a->as.i == b->as.i;
    case MINO_FLOAT:
        return a->as.f == b->as.f;
    case MINO_STRING:
    case MINO_SYMBOL:
    case MINO_KEYWORD:
        return a->as.s.len == b->as.s.len
            && memcmp(a->as.s.data, b->as.s.data, a->as.s.len) == 0;
    case MINO_CONS:
        return mino_eq(a->as.cons.car, b->as.cons.car)
            && mino_eq(a->as.cons.cdr, b->as.cons.cdr);
    case MINO_VECTOR: {
        size_t i;
        if (a->as.vec.len != b->as.vec.len) {
            return 0;
        }
        for (i = 0; i < a->as.vec.len; i++) {
            if (!mino_eq(vec_nth(a, i), vec_nth(b, i))) {
                return 0;
            }
        }
        return 1;
    }
    case MINO_MAP: {
        /* Map equality ignores iteration order: same key set with the same
         * values, regardless of when each was inserted. */
        size_t i;
        if (a->as.map.len != b->as.map.len) {
            return 0;
        }
        for (i = 0; i < a->as.map.len; i++) {
            mino_val_t *key = vec_nth(a->as.map.key_order, i);
            mino_val_t *bv  = map_get_val(b, key);
            mino_val_t *av  = map_get_val(a, key);
            if (bv == NULL) {
                return 0;
            }
            if (!mino_eq(av, bv)) {
                return 0;
            }
        }
        return 1;
    }
    case MINO_PRIM:
        return a->as.prim.fn == b->as.prim.fn;
    case MINO_FN:
    case MINO_MACRO:
        /* Callables compare by identity. Structural equality on bodies and
         * captured environments is neither cheap nor especially meaningful. */
        return a == b;
    case MINO_RECUR:
        return a == b;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Environment                                                               */
/* ------------------------------------------------------------------------- */

/*
 * Environment: a chain of frames. Each frame is a flat (name, value) array
 * with linear search. The root frame has parent == NULL and holds globals;
 * child frames are created by let, fn application, and loop. Lookup walks
 * parents; binding always writes to the current frame so that let and fn
 * parameters shadow rather than mutate outer bindings.
 *
 * Frames created for locals are currently never freed — v0.7 introduces a
 * tracing collector that owns environment lifetimes. Only the root frame
 * passed to mino_env_free is reclaimed.
 */

typedef struct {
    char       *name;
    mino_val_t *val;
} env_binding_t;

struct mino_env {
    env_binding_t *bindings;
    size_t         len;
    size_t         cap;
    mino_env_t    *parent;
};

static mino_env_t *env_alloc(mino_env_t *parent)
{
    mino_env_t *env = (mino_env_t *)calloc(1, sizeof(*env));
    if (env == NULL) {
        abort();
    }
    env->parent = parent;
    return env;
}

mino_env_t *mino_env_new(void)
{
    return env_alloc(NULL);
}

static mino_env_t *env_child(mino_env_t *parent)
{
    return env_alloc(parent);
}

void mino_env_free(mino_env_t *env)
{
    size_t i;
    if (env == NULL) {
        return;
    }
    for (i = 0; i < env->len; i++) {
        free(env->bindings[i].name);
    }
    free(env->bindings);
    free(env);
}

static env_binding_t *env_find_here(mino_env_t *env, const char *name)
{
    size_t i;
    for (i = 0; i < env->len; i++) {
        if (strcmp(env->bindings[i].name, name) == 0) {
            return &env->bindings[i];
        }
    }
    return NULL;
}

static void env_bind(mino_env_t *env, const char *name, mino_val_t *val)
{
    env_binding_t *b = env_find_here(env, name);
    if (b != NULL) {
        b->val = val;
        return;
    }
    if (env->len == env->cap) {
        size_t new_cap = env->cap == 0 ? 16 : env->cap * 2;
        env_binding_t *nb = (env_binding_t *)realloc(
            env->bindings, new_cap * sizeof(*nb));
        if (nb == NULL) {
            abort();
        }
        env->bindings = nb;
        env->cap = new_cap;
    }
    env->bindings[env->len].name = dup_n(name, strlen(name));
    env->bindings[env->len].val  = val;
    env->len++;
}

static mino_env_t *env_root(mino_env_t *env)
{
    while (env->parent != NULL) {
        env = env->parent;
    }
    return env;
}

void mino_env_set(mino_env_t *env, const char *name, mino_val_t *val)
{
    env_bind(env, name, val);
}

mino_val_t *mino_env_get(mino_env_t *env, const char *name)
{
    while (env != NULL) {
        env_binding_t *b = env_find_here(env, name);
        if (b != NULL) {
            return b->val;
        }
        env = env->parent;
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Evaluator                                                                 */
/* ------------------------------------------------------------------------- */

static int sym_eq(const mino_val_t *v, const char *s)
{
    size_t n;
    if (v == NULL || v->type != MINO_SYMBOL) {
        return 0;
    }
    n = strlen(s);
    return v->as.s.len == n && memcmp(v->as.s.data, s, n) == 0;
}

static mino_val_t *eval(mino_val_t *form, mino_env_t *env);
static mino_val_t *eval_value(mino_val_t *form, mino_env_t *env);
static mino_val_t *apply_callable(mino_val_t *fn, mino_val_t *args,
                                  mino_env_t *env);

/*
 * macroexpand1: if `form` is a call whose head resolves to a macro in env,
 * expand it once and return the new form. If not a macro call, return the
 * input unchanged and set *expanded = 0.
 */
static mino_val_t *macroexpand1(mino_val_t *form, mino_env_t *env,
                                 int *expanded)
{
    char        buf[256];
    size_t      n;
    mino_val_t *head;
    mino_val_t *mac;
    *expanded = 0;
    if (!mino_is_cons(form)) {
        return form;
    }
    head = form->as.cons.car;
    if (head == NULL || head->type != MINO_SYMBOL) {
        return form;
    }
    n = head->as.s.len;
    if (n >= sizeof(buf)) {
        return form;
    }
    memcpy(buf, head->as.s.data, n);
    buf[n] = '\0';
    mac = mino_env_get(env, buf);
    if (mac == NULL || mac->type != MINO_MACRO) {
        return form;
    }
    *expanded = 1;
    return apply_callable(mac, form->as.cons.cdr, env);
}

/* Expand repeatedly until `form` is no longer a macro call at the top. */
static mino_val_t *macroexpand_all(mino_val_t *form, mino_env_t *env)
{
    for (;;) {
        int         expanded = 0;
        mino_val_t *next     = macroexpand1(form, env, &expanded);
        if (next == NULL) {
            return NULL;
        }
        if (!expanded) {
            return form;
        }
        form = next;
    }
}

/*
 * quasiquote_expand: walk `form` as a template. Sublists, subvectors, and
 * submaps are recursed into; (unquote x) evaluates x and uses its value;
 * (unquote-splicing x) evaluates x (expected to yield a list) and splices
 * its elements into the enclosing list.
 */
static mino_val_t *quasiquote_expand(mino_val_t *form, mino_env_t *env)
{
    if (form != NULL && form->type == MINO_VECTOR) {
        size_t       nn  = form->as.vec.len;
        mino_val_t **tmp;
        mino_val_t  *result;
        size_t       i;
        if (nn == 0) { return form; }
        tmp = (mino_val_t **)malloc(nn * sizeof(*tmp));
        if (tmp == NULL) { abort(); }
        for (i = 0; i < nn; i++) {
            mino_val_t *e = quasiquote_expand(vec_nth(form, i), env);
            if (e == NULL) { free(tmp); return NULL; }
            tmp[i] = e;
        }
        result = mino_vector(tmp, nn);
        free(tmp);
        return result;
    }
    if (form != NULL && form->type == MINO_MAP) {
        size_t        nn = form->as.map.len;
        mino_val_t  **ks;
        mino_val_t  **vs;
        mino_val_t   *result;
        size_t        i;
        if (nn == 0) { return form; }
        ks = (mino_val_t **)malloc(nn * sizeof(*ks));
        vs = (mino_val_t **)malloc(nn * sizeof(*vs));
        if (ks == NULL || vs == NULL) { abort(); }
        for (i = 0; i < nn; i++) {
            mino_val_t *key = vec_nth(form->as.map.key_order, i);
            mino_val_t *val = map_get_val(form, key);
            mino_val_t *kk  = quasiquote_expand(key, env);
            mino_val_t *vv;
            if (kk == NULL) { free(ks); free(vs); return NULL; }
            vv = quasiquote_expand(val, env);
            if (vv == NULL) { free(ks); free(vs); return NULL; }
            ks[i] = kk; vs[i] = vv;
        }
        result = mino_map(ks, vs, nn);
        free(ks); free(vs);
        return result;
    }
    if (!mino_is_cons(form)) {
        return form;
    }
    {
        mino_val_t *head = form->as.cons.car;
        if (sym_eq(head, "unquote")) {
            mino_val_t *arg = form->as.cons.cdr;
            if (!mino_is_cons(arg)) {
                set_error("unquote requires one argument");
                return NULL;
            }
            return eval_value(arg->as.cons.car, env);
        }
        if (sym_eq(head, "unquote-splicing")) {
            set_error("unquote-splicing must appear inside a list");
            return NULL;
        }
    }
    {
        mino_val_t *out  = mino_nil();
        mino_val_t *tail = NULL;
        mino_val_t *p    = form;
        while (mino_is_cons(p)) {
            mino_val_t *elem = p->as.cons.car;
            if (mino_is_cons(elem)
                && sym_eq(elem->as.cons.car, "unquote-splicing")) {
                mino_val_t *arg = elem->as.cons.cdr;
                mino_val_t *spliced;
                mino_val_t *sp;
                if (!mino_is_cons(arg)) {
                    set_error("unquote-splicing requires one argument");
                    return NULL;
                }
                spliced = eval_value(arg->as.cons.car, env);
                if (spliced == NULL) { return NULL; }
                sp = spliced;
                while (mino_is_cons(sp)) {
                    mino_val_t *cell = mino_cons(sp->as.cons.car, mino_nil());
                    if (tail == NULL) { out = cell; } else { tail->as.cons.cdr = cell; }
                    tail = cell;
                    sp = sp->as.cons.cdr;
                }
            } else {
                mino_val_t *expanded = quasiquote_expand(elem, env);
                mino_val_t *cell;
                if (expanded == NULL) { return NULL; }
                cell = mino_cons(expanded, mino_nil());
                if (tail == NULL) { out = cell; } else { tail->as.cons.cdr = cell; }
                tail = cell;
            }
            p = p->as.cons.cdr;
        }
        return out;
    }
}

/*
 * Evaluate `form` for its value. Any MINO_RECUR escaping here is a
 * non-tail recur and is rejected. Use plain eval() in positions where
 * a recur is legitimately in tail position (if branches, implicit-do
 * trailing expression, fn/loop body through the trampoline).
 */
static mino_val_t *eval_value(mino_val_t *form, mino_env_t *env)
{
    mino_val_t *v = eval(form, env);
    if (v == NULL) {
        return NULL;
    }
    if (v->type == MINO_RECUR) {
        set_error("recur must be in tail position");
        return NULL;
    }
    return v;
}

static mino_val_t *eval_implicit_do(mino_val_t *body, mino_env_t *env)
{
    if (!mino_is_cons(body)) {
        return mino_nil();
    }
    for (;;) {
        mino_val_t *rest = body->as.cons.cdr;
        if (!mino_is_cons(rest)) {
            /* Last expression: tail position, propagate recur. */
            return eval(body->as.cons.car, env);
        }
        if (eval_value(body->as.cons.car, env) == NULL) {
            return NULL;
        }
        body = rest;
    }
}

static mino_val_t *eval_args(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *head = mino_nil();
    mino_val_t *tail = NULL;
    while (mino_is_cons(args)) {
        mino_val_t *v = eval_value(args->as.cons.car, env);
        mino_val_t *cell;
        if (v == NULL) {
            return NULL;
        }
        cell = mino_cons(v, mino_nil());
        if (tail == NULL) {
            head = cell;
        } else {
            tail->as.cons.cdr = cell;
        }
        tail = cell;
        args = args->as.cons.cdr;
    }
    return head;
}

/*
 * Bind a list of parameter symbols to a list of values in `env`.
 * Returns 1 on success, 0 on arity mismatch or over-long name (with error set).
 */
static int bind_params(mino_env_t *env, mino_val_t *params, mino_val_t *args,
                       const char *ctx)
{
    while (mino_is_cons(params)) {
        mino_val_t *name = params->as.cons.car;
        char        buf[256];
        size_t      n;
        /* `&` marks a rest-parameter: the next symbol binds to the remainder
         * of args as a list (possibly empty). */
        if (sym_eq(name, "&")) {
            mino_val_t *rest_param;
            params = params->as.cons.cdr;
            if (!mino_is_cons(params)
                || params->as.cons.car == NULL
                || params->as.cons.car->type != MINO_SYMBOL) {
                set_error("& must be followed by a single parameter name");
                return 0;
            }
            rest_param = params->as.cons.car;
            if (mino_is_cons(params->as.cons.cdr)) {
                set_error("& parameter must be last");
                return 0;
            }
            n = rest_param->as.s.len;
            if (n >= sizeof(buf)) {
                set_error("parameter name too long");
                return 0;
            }
            memcpy(buf, rest_param->as.s.data, n);
            buf[n] = '\0';
            env_bind(env, buf, args);  /* args is the remainder (may be nil) */
            return 1;
        }
        if (!mino_is_cons(args)) {
            char msg[96];
            snprintf(msg, sizeof(msg), "%s arity mismatch", ctx);
            set_error(msg);
            return 0;
        }
        n = name->as.s.len;
        if (n >= sizeof(buf)) {
            set_error("parameter name too long");
            return 0;
        }
        memcpy(buf, name->as.s.data, n);
        buf[n] = '\0';
        env_bind(env, buf, args->as.cons.car);
        params = params->as.cons.cdr;
        args   = args->as.cons.cdr;
    }
    if (mino_is_cons(args)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s arity mismatch", ctx);
        set_error(msg);
        return 0;
    }
    return 1;
}

static mino_val_t *eval(mino_val_t *form, mino_env_t *env)
{
    if (form == NULL) {
        return mino_nil();
    }
    switch (form->type) {
    case MINO_NIL:
    case MINO_BOOL:
    case MINO_INT:
    case MINO_FLOAT:
    case MINO_STRING:
    case MINO_KEYWORD:
    case MINO_PRIM:
    case MINO_FN:
    case MINO_MACRO:
    case MINO_RECUR:
        return form;
    case MINO_SYMBOL: {
        char buf[256];
        size_t n = form->as.s.len;
        mino_val_t *v;
        if (n >= sizeof(buf)) {
            set_error("symbol name too long");
            return NULL;
        }
        memcpy(buf, form->as.s.data, n);
        buf[n] = '\0';
        v = mino_env_get(env, buf);
        if (v == NULL) {
            char msg[300];
            snprintf(msg, sizeof(msg), "unbound symbol: %s", buf);
            set_error(msg);
            return NULL;
        }
        return v;
    }
    case MINO_VECTOR: {
        /* Vector literals evaluate each element in order, producing a new
         * vector whose shape matches the source. */
        size_t i;
        size_t n = form->as.vec.len;
        mino_val_t **tmp;
        mino_val_t  *result;
        if (n == 0) {
            return form;
        }
        tmp = (mino_val_t **)malloc(n * sizeof(*tmp));
        if (tmp == NULL) {
            abort();
        }
        for (i = 0; i < n; i++) {
            mino_val_t *ev = eval_value(vec_nth(form, i), env);
            if (ev == NULL) {
                free(tmp);
                return NULL;
            }
            tmp[i] = ev;
        }
        result = mino_vector(tmp, n);
        free(tmp);
        return result;
    }
    case MINO_MAP: {
        /* Map literals evaluate keys and values in read order; the
         * constructor handles duplicate-key resolution. */
        size_t i;
        size_t n = form->as.map.len;
        mino_val_t **ks;
        mino_val_t **vs;
        mino_val_t  *result;
        if (n == 0) {
            return form;
        }
        ks = (mino_val_t **)malloc(n * sizeof(*ks));
        vs = (mino_val_t **)malloc(n * sizeof(*vs));
        if (ks == NULL || vs == NULL) {
            abort();
        }
        for (i = 0; i < n; i++) {
            mino_val_t *form_key = vec_nth(form->as.map.key_order, i);
            mino_val_t *form_val = map_get_val(form, form_key);
            mino_val_t *k = eval_value(form_key, env);
            mino_val_t *v;
            if (k == NULL) { free(ks); free(vs); return NULL; }
            v = eval_value(form_val, env);
            if (v == NULL) { free(ks); free(vs); return NULL; }
            ks[i] = k;
            vs[i] = v;
        }
        result = mino_map(ks, vs, n);
        free(ks); free(vs);
        return result;
    }
    case MINO_CONS: {
        mino_val_t *head = form->as.cons.car;
        mino_val_t *args = form->as.cons.cdr;

        /* Special forms. */
        if (sym_eq(head, "quote")) {
            if (!mino_is_cons(args)) {
                set_error("quote requires one argument");
                return NULL;
            }
            return args->as.cons.car;
        }
        if (sym_eq(head, "quasiquote")) {
            if (!mino_is_cons(args)) {
                set_error("quasiquote requires one argument");
                return NULL;
            }
            return quasiquote_expand(args->as.cons.car, env);
        }
        if (sym_eq(head, "unquote")
            || sym_eq(head, "unquote-splicing")) {
            set_error("unquote outside of quasiquote");
            return NULL;
        }
        if (sym_eq(head, "defmacro")) {
            mino_val_t *name_form;
            mino_val_t *params;
            mino_val_t *body;
            mino_val_t *mac;
            mino_val_t *p;
            char        buf[256];
            size_t      n;
            if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
                set_error("defmacro requires a name, parameters, and body");
                return NULL;
            }
            name_form = args->as.cons.car;
            params    = args->as.cons.cdr->as.cons.car;
            body      = args->as.cons.cdr->as.cons.cdr;
            if (name_form == NULL || name_form->type != MINO_SYMBOL) {
                set_error("defmacro name must be a symbol");
                return NULL;
            }
            if (!mino_is_cons(params) && !mino_is_nil(params)) {
                set_error("defmacro parameter list must be a list");
                return NULL;
            }
            for (p = params; mino_is_cons(p); p = p->as.cons.cdr) {
                mino_val_t *pn = p->as.cons.car;
                if (pn == NULL || pn->type != MINO_SYMBOL) {
                    set_error("defmacro parameter must be a symbol");
                    return NULL;
                }
            }
            mac = alloc_val(MINO_MACRO);
            mac->as.fn.params = params;
            mac->as.fn.body   = body;
            mac->as.fn.env    = env;
            n = name_form->as.s.len;
            if (n >= sizeof(buf)) {
                set_error("defmacro name too long");
                return NULL;
            }
            memcpy(buf, name_form->as.s.data, n);
            buf[n] = '\0';
            env_bind(env_root(env), buf, mac);
            return mac;
        }
        if (sym_eq(head, "def")) {
            mino_val_t *name_form;
            mino_val_t *value_form;
            mino_val_t *value;
            char buf[256];
            size_t n;
            if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
                set_error("def requires a name and a value");
                return NULL;
            }
            name_form  = args->as.cons.car;
            value_form = args->as.cons.cdr->as.cons.car;
            if (name_form == NULL || name_form->type != MINO_SYMBOL) {
                set_error("def name must be a symbol");
                return NULL;
            }
            n = name_form->as.s.len;
            if (n >= sizeof(buf)) {
                set_error("def name too long");
                return NULL;
            }
            memcpy(buf, name_form->as.s.data, n);
            buf[n] = '\0';
            value = eval_value(value_form, env);
            if (value == NULL) {
                return NULL;
            }
            env_bind(env_root(env), buf, value);
            return value;
        }
        if (sym_eq(head, "if")) {
            mino_val_t *cond_form;
            mino_val_t *then_form;
            mino_val_t *else_form = mino_nil();
            mino_val_t *cond;
            if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
                set_error("if requires a condition and a then-branch");
                return NULL;
            }
            cond_form = args->as.cons.car;
            then_form = args->as.cons.cdr->as.cons.car;
            if (mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
                else_form = args->as.cons.cdr->as.cons.cdr->as.cons.car;
            }
            cond = eval_value(cond_form, env);
            if (cond == NULL) {
                return NULL;
            }
            /* Branch is tail position: propagate recur up to trampoline. */
            return eval(mino_is_truthy(cond) ? then_form : else_form, env);
        }
        if (sym_eq(head, "do")) {
            return eval_implicit_do(args, env);
        }
        if (sym_eq(head, "let")) {
            mino_val_t *bindings;
            mino_val_t *body;
            mino_env_t *local;
            if (!mino_is_cons(args)) {
                set_error("let requires a binding list and body");
                return NULL;
            }
            bindings = args->as.cons.car;
            body     = args->as.cons.cdr;
            if (!mino_is_cons(bindings) && !mino_is_nil(bindings)) {
                set_error("let bindings must be a list");
                return NULL;
            }
            local = env_child(env);
            while (mino_is_cons(bindings)) {
                mino_val_t *name_form = bindings->as.cons.car;
                mino_val_t *rest_pair = bindings->as.cons.cdr;
                mino_val_t *val;
                char        buf[256];
                size_t      n;
                if (name_form == NULL || name_form->type != MINO_SYMBOL) {
                    set_error("let binding name must be a symbol");
                    return NULL;
                }
                if (!mino_is_cons(rest_pair)) {
                    set_error("let binding missing value");
                    return NULL;
                }
                n = name_form->as.s.len;
                if (n >= sizeof(buf)) {
                    set_error("let name too long");
                    return NULL;
                }
                memcpy(buf, name_form->as.s.data, n);
                buf[n] = '\0';
                val = eval_value(rest_pair->as.cons.car, local);
                if (val == NULL) {
                    return NULL;
                }
                env_bind(local, buf, val);
                bindings = rest_pair->as.cons.cdr;
            }
            return eval_implicit_do(body, local);
        }
        if (sym_eq(head, "fn")) {
            mino_val_t *params;
            mino_val_t *body;
            mino_val_t *p;
            if (!mino_is_cons(args)) {
                set_error("fn requires a parameter list");
                return NULL;
            }
            params = args->as.cons.car;
            body   = args->as.cons.cdr;
            if (!mino_is_cons(params) && !mino_is_nil(params)) {
                set_error("fn parameter list must be a list");
                return NULL;
            }
            for (p = params; mino_is_cons(p); p = p->as.cons.cdr) {
                mino_val_t *name = p->as.cons.car;
                if (name == NULL || name->type != MINO_SYMBOL) {
                    set_error("fn parameter must be a symbol");
                    return NULL;
                }
            }
            return make_fn(params, body, env);
        }
        if (sym_eq(head, "recur")) {
            mino_val_t *evaled = eval_args(args, env);
            mino_val_t *r;
            if (evaled == NULL && mino_last_error() != NULL) {
                return NULL;
            }
            r = alloc_val(MINO_RECUR);
            r->as.recur.args = evaled;
            return r;
        }
        if (sym_eq(head, "loop")) {
            mino_val_t *bindings;
            mino_val_t *body;
            mino_val_t *params      = mino_nil();
            mino_val_t *params_tail = NULL;
            mino_env_t *local;
            if (!mino_is_cons(args)) {
                set_error("loop requires a binding list and body");
                return NULL;
            }
            bindings = args->as.cons.car;
            body     = args->as.cons.cdr;
            if (!mino_is_cons(bindings) && !mino_is_nil(bindings)) {
                set_error("loop bindings must be a list");
                return NULL;
            }
            local = env_child(env);
            while (mino_is_cons(bindings)) {
                mino_val_t *name_form = bindings->as.cons.car;
                mino_val_t *rest_pair = bindings->as.cons.cdr;
                mino_val_t *val;
                char        buf[256];
                size_t      n;
                mino_val_t *cell;
                if (name_form == NULL || name_form->type != MINO_SYMBOL) {
                    set_error("loop binding name must be a symbol");
                    return NULL;
                }
                if (!mino_is_cons(rest_pair)) {
                    set_error("loop binding missing value");
                    return NULL;
                }
                n = name_form->as.s.len;
                if (n >= sizeof(buf)) {
                    set_error("loop name too long");
                    return NULL;
                }
                memcpy(buf, name_form->as.s.data, n);
                buf[n] = '\0';
                val = eval_value(rest_pair->as.cons.car, local);
                if (val == NULL) {
                    return NULL;
                }
                env_bind(local, buf, val);
                cell = mino_cons(name_form, mino_nil());
                if (params_tail == NULL) {
                    params = cell;
                } else {
                    params_tail->as.cons.cdr = cell;
                }
                params_tail = cell;
                bindings = rest_pair->as.cons.cdr;
            }
            for (;;) {
                mino_val_t *result = eval_implicit_do(body, local);
                if (result == NULL) {
                    return NULL;
                }
                if (result->type != MINO_RECUR) {
                    return result;
                }
                if (!bind_params(local, params, result->as.recur.args,
                                 "recur")) {
                    return NULL;
                }
            }
        }

        /* Function or macro application. */
        {
            mino_val_t *fn = eval_value(head, env);
            mino_val_t *evaled;
            if (fn == NULL) {
                return NULL;
            }
            if (fn->type == MINO_MACRO) {
                /* Expand with unevaluated args; re-eval the resulting form
                 * in the caller's environment. */
                mino_val_t *expanded = apply_callable(fn, args, env);
                if (expanded == NULL) {
                    return NULL;
                }
                return eval(expanded, env);
            }
            if (fn->type != MINO_PRIM && fn->type != MINO_FN) {
                set_error("not a function");
                return NULL;
            }
            evaled = eval_args(args, env);
            if (evaled == NULL && mino_last_error() != NULL) {
                return NULL;
            }
            return apply_callable(fn, evaled, env);
        }
    }
    }
    set_error("eval: unknown value type");
    return NULL;
}

/*
 * Invoke `fn` with an already-evaluated argument list. Used both by the
 * evaluator's function-call path and by primitives (e.g. update) that
 * need to call back into user-defined code.
 */
static mino_val_t *apply_callable(mino_val_t *fn, mino_val_t *args,
                                  mino_env_t *env)
{
    if (fn == NULL) {
        set_error("cannot apply null");
        return NULL;
    }
    if (fn->type == MINO_PRIM) {
        return fn->as.prim.fn(args, env);
    }
    if (fn->type == MINO_FN || fn->type == MINO_MACRO) {
        const char *tag       = fn->type == MINO_MACRO ? "macro" : "fn";
        mino_env_t *local     = env_child(fn->as.fn.env);
        mino_val_t *call_args = args;
        for (;;) {
            mino_val_t *result;
            if (!bind_params(local, fn->as.fn.params, call_args, tag)) {
                return NULL;
            }
            result = eval_implicit_do(fn->as.fn.body, local);
            if (result == NULL) {
                return NULL;
            }
            if (result->type != MINO_RECUR) {
                return result;
            }
            call_args = result->as.recur.args;
        }
    }
    set_error("not a function");
    return NULL;
}

mino_val_t *mino_eval(mino_val_t *form, mino_env_t *env)
{
    mino_val_t *v = eval(form, env);
    if (v != NULL && v->type == MINO_RECUR) {
        set_error("recur must be in tail position");
        return NULL;
    }
    return v;
}

/* ------------------------------------------------------------------------- */
/* Core primitives                                                           */
/* ------------------------------------------------------------------------- */

/*
 * Numeric coercion: if any argument is a float, the result is a float.
 * Otherwise integer arithmetic is used end-to-end.
 */

static int args_have_float(mino_val_t *args)
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

static int as_double(const mino_val_t *v, double *out)
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

static int as_long(const mino_val_t *v, long long *out)
{
    if (v == NULL || v->type != MINO_INT) {
        return 0;
    }
    *out = v->as.i;
    return 1;
}

static mino_val_t *prim_add(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (args_have_float(args)) {
        double acc = 0.0;
        while (mino_is_cons(args)) {
            double x;
            if (!as_double(args->as.cons.car, &x)) {
                set_error("+ expects numbers");
                return NULL;
            }
            acc += x;
            args = args->as.cons.cdr;
        }
        return mino_float(acc);
    } else {
        long long acc = 0;
        while (mino_is_cons(args)) {
            long long x;
            if (!as_long(args->as.cons.car, &x)) {
                set_error("+ expects numbers");
                return NULL;
            }
            acc += x;
            args = args->as.cons.cdr;
        }
        return mino_int(acc);
    }
}

static mino_val_t *prim_sub(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args)) {
        set_error("- requires at least one argument");
        return NULL;
    }
    if (args_have_float(args)) {
        double acc;
        if (!as_double(args->as.cons.car, &acc)) {
            set_error("- expects numbers");
            return NULL;
        }
        args = args->as.cons.cdr;
        if (!mino_is_cons(args)) {
            return mino_float(-acc);
        }
        while (mino_is_cons(args)) {
            double x;
            if (!as_double(args->as.cons.car, &x)) {
                set_error("- expects numbers");
                return NULL;
            }
            acc -= x;
            args = args->as.cons.cdr;
        }
        return mino_float(acc);
    } else {
        long long acc;
        if (!as_long(args->as.cons.car, &acc)) {
            set_error("- expects numbers");
            return NULL;
        }
        args = args->as.cons.cdr;
        if (!mino_is_cons(args)) {
            return mino_int(-acc);
        }
        while (mino_is_cons(args)) {
            long long x;
            if (!as_long(args->as.cons.car, &x)) {
                set_error("- expects numbers");
                return NULL;
            }
            acc -= x;
            args = args->as.cons.cdr;
        }
        return mino_int(acc);
    }
}

static mino_val_t *prim_mul(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (args_have_float(args)) {
        double acc = 1.0;
        while (mino_is_cons(args)) {
            double x;
            if (!as_double(args->as.cons.car, &x)) {
                set_error("* expects numbers");
                return NULL;
            }
            acc *= x;
            args = args->as.cons.cdr;
        }
        return mino_float(acc);
    } else {
        long long acc = 1;
        while (mino_is_cons(args)) {
            long long x;
            if (!as_long(args->as.cons.car, &x)) {
                set_error("* expects numbers");
                return NULL;
            }
            acc *= x;
            args = args->as.cons.cdr;
        }
        return mino_int(acc);
    }
}

static mino_val_t *prim_div(mino_val_t *args, mino_env_t *env)
{
    /* Division always yields a float result for now. */
    double acc;
    (void)env;
    if (!mino_is_cons(args)) {
        set_error("/ requires at least one argument");
        return NULL;
    }
    if (!as_double(args->as.cons.car, &acc)) {
        set_error("/ expects numbers");
        return NULL;
    }
    args = args->as.cons.cdr;
    if (!mino_is_cons(args)) {
        if (acc == 0.0) {
            set_error("division by zero");
            return NULL;
        }
        return mino_float(1.0 / acc);
    }
    while (mino_is_cons(args)) {
        double x;
        if (!as_double(args->as.cons.car, &x)) {
            set_error("/ expects numbers");
            return NULL;
        }
        if (x == 0.0) {
            set_error("division by zero");
            return NULL;
        }
        acc /= x;
        args = args->as.cons.cdr;
    }
    return mino_float(acc);
}

static mino_val_t *prim_eq(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args)) {
        return mino_true();
    }
    {
        mino_val_t *first = args->as.cons.car;
        args = args->as.cons.cdr;
        while (mino_is_cons(args)) {
            if (!mino_eq(first, args->as.cons.car)) {
                return mino_false();
            }
            args = args->as.cons.cdr;
        }
    }
    return mino_true();
}

/*
 * Chained numeric comparison. `op` selects the relation:
 *   0: <    1: <=    2: >    3: >=
 * Returns true if each successive pair satisfies the relation (and
 * trivially true on zero or one argument).
 */
static mino_val_t *compare_chain(mino_val_t *args, const char *name, int op)
{
    double prev;
    if (!mino_is_cons(args)) {
        return mino_true();
    }
    if (!as_double(args->as.cons.car, &prev)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%s expects numbers", name);
        set_error(msg);
        return NULL;
    }
    args = args->as.cons.cdr;
    while (mino_is_cons(args)) {
        double x;
        int    ok;
        if (!as_double(args->as.cons.car, &x)) {
            char msg[64];
            snprintf(msg, sizeof(msg), "%s expects numbers", name);
            set_error(msg);
            return NULL;
        }
        switch (op) {
        case 0:  ok = prev <  x; break;
        case 1:  ok = prev <= x; break;
        case 2:  ok = prev >  x; break;
        default: ok = prev >= x; break;
        }
        if (!ok) {
            return mino_false();
        }
        prev = x;
        args = args->as.cons.cdr;
    }
    return mino_true();
}

static mino_val_t *prim_lt(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    return compare_chain(args, "<", 0);
}

static mino_val_t *prim_le(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    return compare_chain(args, "<=", 1);
}

static mino_val_t *prim_gt(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    return compare_chain(args, ">", 2);
}

static mino_val_t *prim_ge(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    return compare_chain(args, ">=", 3);
}

static mino_val_t *prim_car(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args)) {
        set_error("car requires one argument");
        return NULL;
    }
    return mino_car(args->as.cons.car);
}

static mino_val_t *prim_cdr(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args)) {
        set_error("cdr requires one argument");
        return NULL;
    }
    return mino_cdr(args->as.cons.car);
}

static mino_val_t *prim_cons(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error("cons requires two arguments");
        return NULL;
    }
    return mino_cons(args->as.cons.car, args->as.cons.cdr->as.cons.car);
}

static mino_val_t *prim_list(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    /* Args are already a list of evaluated values. */
    return args == NULL ? mino_nil() : args;
}

/* ------------------------------------------------------------------------- */
/* Collection primitives                                                     */
/*                                                                           */
/* All collection ops treat values as immutable: every operation that        */
/* "modifies" a collection returns a freshly allocated value. v0.3 uses      */
/* naïve array-backed representations; persistent tries arrive in v0.4/v0.5 */
/* without changing the public primitive contracts.                          */
/* ------------------------------------------------------------------------- */

static size_t list_length(mino_val_t *list)
{
    size_t n = 0;
    while (mino_is_cons(list)) {
        n++;
        list = list->as.cons.cdr;
    }
    return n;
}

static int arg_count(mino_val_t *args, size_t *out)
{
    *out = list_length(args);
    return 1;
}

static mino_val_t *prim_count(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("count requires one argument");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_int(0);
    }
    switch (coll->type) {
    case MINO_CONS:   return mino_int((long long)list_length(coll));
    case MINO_VECTOR: return mino_int((long long)coll->as.vec.len);
    case MINO_MAP:    return mino_int((long long)coll->as.map.len);
    case MINO_STRING: return mino_int((long long)coll->as.s.len);
    default:
        set_error("count: unsupported collection");
        return NULL;
    }
}

static mino_val_t *prim_vector(mino_val_t *args, mino_env_t *env)
{
    size_t n;
    size_t i;
    mino_val_t **tmp;
    mino_val_t *result;
    mino_val_t *p;
    (void)env;
    arg_count(args, &n);
    if (n == 0) {
        return mino_vector(NULL, 0);
    }
    tmp = (mino_val_t **)malloc(n * sizeof(*tmp));
    if (tmp == NULL) {
        abort();
    }
    p = args;
    for (i = 0; i < n; i++) {
        tmp[i] = p->as.cons.car;
        p = p->as.cons.cdr;
    }
    result = mino_vector(tmp, n);
    free(tmp);
    return result;
}

static mino_val_t *prim_hash_map(mino_val_t *args, mino_env_t *env)
{
    size_t n;
    size_t pairs;
    size_t i;
    mino_val_t **ks;
    mino_val_t **vs;
    mino_val_t *result;
    mino_val_t *p;
    (void)env;
    arg_count(args, &n);
    if (n % 2 != 0) {
        set_error("hash-map requires an even number of arguments");
        return NULL;
    }
    if (n == 0) {
        return mino_map(NULL, NULL, 0);
    }
    pairs = n / 2;
    ks = (mino_val_t **)malloc(pairs * sizeof(*ks));
    vs = (mino_val_t **)malloc(pairs * sizeof(*vs));
    if (ks == NULL || vs == NULL) {
        abort();
    }
    p = args;
    for (i = 0; i < pairs; i++) {
        ks[i] = p->as.cons.car;
        p = p->as.cons.cdr;
        vs[i] = p->as.cons.car;
        p = p->as.cons.cdr;
    }
    result = mino_map(ks, vs, pairs);
    free(ks); free(vs);
    return result;
}

static mino_val_t *prim_nth(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *idx_val;
    mino_val_t *def_val = NULL;
    size_t      n;
    long long   idx;
    (void)env;
    arg_count(args, &n);
    if (n != 2 && n != 3) {
        set_error("nth requires 2 or 3 arguments");
        return NULL;
    }
    coll    = args->as.cons.car;
    idx_val = args->as.cons.cdr->as.cons.car;
    if (n == 3) {
        def_val = args->as.cons.cdr->as.cons.cdr->as.cons.car;
    }
    if (idx_val == NULL || idx_val->type != MINO_INT) {
        set_error("nth index must be an integer");
        return NULL;
    }
    idx = idx_val->as.i;
    if (idx < 0) {
        if (def_val != NULL) return def_val;
        set_error("nth index out of range");
        return NULL;
    }
    if (coll == NULL || coll->type == MINO_NIL) {
        if (def_val != NULL) return def_val;
        set_error("nth index out of range");
        return NULL;
    }
    if (coll->type == MINO_VECTOR) {
        if ((size_t)idx >= coll->as.vec.len) {
            if (def_val != NULL) return def_val;
            set_error("nth index out of range");
            return NULL;
        }
        return vec_nth(coll, (size_t)idx);
    }
    if (coll->type == MINO_CONS) {
        mino_val_t *p = coll;
        long long   i;
        for (i = 0; i < idx; i++) {
            if (!mino_is_cons(p)) {
                if (def_val != NULL) return def_val;
                set_error("nth index out of range");
                return NULL;
            }
            p = p->as.cons.cdr;
        }
        if (!mino_is_cons(p)) {
            if (def_val != NULL) return def_val;
            set_error("nth index out of range");
            return NULL;
        }
        return p->as.cons.car;
    }
    set_error("nth: unsupported collection");
    return NULL;
}

static mino_val_t *prim_first(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("first requires one argument");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil();
    }
    if (coll->type == MINO_CONS) {
        return coll->as.cons.car;
    }
    if (coll->type == MINO_VECTOR) {
        if (coll->as.vec.len == 0) {
            return mino_nil();
        }
        return vec_nth(coll, 0);
    }
    set_error("first: unsupported collection");
    return NULL;
}

static mino_val_t *prim_rest(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("rest requires one argument");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil();
    }
    if (coll->type == MINO_CONS) {
        return coll->as.cons.cdr;
    }
    if (coll->type == MINO_VECTOR) {
        /* Rest of a vector is a list of the trailing elements. v0.11 will
         * promote this to a seq abstraction. */
        mino_val_t *head = mino_nil();
        mino_val_t *tail = NULL;
        size_t i;
        for (i = 1; i < coll->as.vec.len; i++) {
            mino_val_t *cell = mino_cons(vec_nth(coll, i), mino_nil());
            if (tail == NULL) {
                head = cell;
            } else {
                tail->as.cons.cdr = cell;
            }
            tail = cell;
        }
        return head;
    }
    set_error("rest: unsupported collection");
    return NULL;
}

/* Layer n k/v pairs onto an existing map, returning a new map value that
 * shares structure with `coll`. Nil is treated as an empty map. */
static mino_val_t *map_assoc_pairs(mino_val_t *coll, mino_val_t *p,
                                    size_t extra_pairs)
{
    mino_hamt_node_t *root;
    mino_val_t       *order;
    size_t            len_out;
    size_t            i;
    if (coll == NULL || coll->type == MINO_NIL) {
        root    = NULL;
        order   = mino_vector(NULL, 0);
        len_out = 0;
    } else {
        root    = coll->as.map.root;
        order   = coll->as.map.key_order;
        len_out = coll->as.map.len;
    }
    for (i = 0; i < extra_pairs; i++) {
        mino_val_t   *k = p->as.cons.car;
        mino_val_t   *v = p->as.cons.cdr->as.cons.car;
        hamt_entry_t *e = hamt_entry_new(k, v);
        uint32_t      h = hash_val(k);
        int           replaced = 0;
        root = hamt_assoc(root, e, h, 0u, &replaced);
        if (!replaced) {
            order = vec_conj1(order, k);
            len_out++;
        }
        p = p->as.cons.cdr->as.cons.cdr;
    }
    {
        mino_val_t *out = alloc_val(MINO_MAP);
        out->as.map.root      = root;
        out->as.map.key_order = order;
        out->as.map.len       = len_out;
        return out;
    }
}

static mino_val_t *prim_assoc(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    size_t      n;
    size_t      extra_pairs;
    size_t      i;
    mino_val_t *p;
    (void)env;
    arg_count(args, &n);
    if (n < 3 || (n - 1) % 2 != 0) {
        set_error("assoc requires a collection and an even number of k/v pairs");
        return NULL;
    }
    coll = args->as.cons.car;
    extra_pairs = (n - 1) / 2;
    if (coll != NULL && coll->type == MINO_VECTOR) {
        /* Vector assoc: each key must be an integer index in [0, len]; an
         * index == len is a one-past-end append. Apply pairs in order on
         * successively-derived vectors so each update shares structure with
         * its predecessor. */
        mino_val_t *acc = coll;
        p = args->as.cons.cdr;
        for (i = 0; i < extra_pairs; i++) {
            mino_val_t *k = p->as.cons.car;
            mino_val_t *v = p->as.cons.cdr->as.cons.car;
            long long   idx;
            if (k == NULL || k->type != MINO_INT) {
                set_error("assoc on vector requires integer indices");
                return NULL;
            }
            idx = k->as.i;
            if (idx < 0 || (size_t)idx > acc->as.vec.len) {
                set_error("assoc on vector: index out of range");
                return NULL;
            }
            acc = vec_assoc1(acc, (size_t)idx, v);
            p = p->as.cons.cdr->as.cons.cdr;
        }
        return acc;
    }
    if (coll == NULL || coll->type == MINO_NIL || coll->type == MINO_MAP) {
        return map_assoc_pairs(coll, args->as.cons.cdr, extra_pairs);
    }
    set_error("assoc: unsupported collection");
    return NULL;
}

static mino_val_t *prim_get(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *key;
    mino_val_t *def_val = mino_nil();
    size_t      n;
    (void)env;
    arg_count(args, &n);
    if (n != 2 && n != 3) {
        set_error("get requires 2 or 3 arguments");
        return NULL;
    }
    coll = args->as.cons.car;
    key  = args->as.cons.cdr->as.cons.car;
    if (n == 3) {
        def_val = args->as.cons.cdr->as.cons.cdr->as.cons.car;
    }
    if (coll == NULL || coll->type == MINO_NIL) {
        return def_val;
    }
    if (coll->type == MINO_MAP) {
        mino_val_t *v = map_get_val(coll, key);
        return v == NULL ? def_val : v;
    }
    if (coll->type == MINO_VECTOR) {
        long long idx;
        if (key == NULL || key->type != MINO_INT) {
            return def_val;
        }
        idx = key->as.i;
        if (idx < 0 || (size_t)idx >= coll->as.vec.len) {
            return def_val;
        }
        return vec_nth(coll, (size_t)idx);
    }
    return def_val;
}

static mino_val_t *prim_conj(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    size_t      n;
    mino_val_t *p;
    (void)env;
    arg_count(args, &n);
    if (n < 2) {
        set_error("conj requires a collection and at least one item");
        return NULL;
    }
    coll = args->as.cons.car;
    p    = args->as.cons.cdr;
    if (coll == NULL || coll->type == MINO_NIL || coll->type == MINO_CONS) {
        /* List/nil: prepend each item so (conj '(1 2) 3 4) => (4 3 1 2). */
        mino_val_t *out = (coll == NULL || coll->type == MINO_NIL)
            ? mino_nil() : coll;
        while (mino_is_cons(p)) {
            out = mino_cons(p->as.cons.car, out);
            p = p->as.cons.cdr;
        }
        return out;
    }
    if (coll->type == MINO_VECTOR) {
        size_t extra = n - 1;
        mino_val_t *acc = coll;
        size_t i;
        for (i = 0; i < extra; i++) {
            acc = vec_conj1(acc, p->as.cons.car);
            p = p->as.cons.cdr;
        }
        return acc;
    }
    if (coll->type == MINO_MAP) {
        /* Each added item must be a 2-element vector [k v]. Assoc each onto
         * the accumulator so successor maps share structure with the source. */
        size_t      extra = n - 1;
        mino_val_t *acc   = coll;
        size_t      i;
        for (i = 0; i < extra; i++) {
            mino_val_t *item = p->as.cons.car;
            mino_val_t *pair_args;
            if (item == NULL || item->type != MINO_VECTOR
                || item->as.vec.len != 2) {
                set_error("conj on map requires 2-element vectors");
                return NULL;
            }
            pair_args = mino_cons(vec_nth(item, 0),
                                   mino_cons(vec_nth(item, 1), mino_nil()));
            acc = map_assoc_pairs(acc, pair_args, 1);
            p = p->as.cons.cdr;
        }
        return acc;
    }
    set_error("conj: unsupported collection");
    return NULL;
}

static mino_val_t *prim_update(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *key;
    mino_val_t *fn;
    mino_val_t *old_val = mino_nil();
    mino_val_t *new_val;
    mino_val_t *call_args;
    size_t      n;
    (void)env;
    arg_count(args, &n);
    if (n != 3) {
        set_error("update requires a collection, key, and function");
        return NULL;
    }
    coll = args->as.cons.car;
    key  = args->as.cons.cdr->as.cons.car;
    fn   = args->as.cons.cdr->as.cons.cdr->as.cons.car;
    if (fn == NULL || (fn->type != MINO_PRIM && fn->type != MINO_FN)) {
        set_error("update: third argument must be a function");
        return NULL;
    }
    if (coll != NULL && coll->type == MINO_MAP) {
        mino_val_t *found = map_get_val(coll, key);
        if (found != NULL) {
            old_val = found;
        }
    } else if (coll != NULL && coll->type == MINO_VECTOR
               && key != NULL && key->type == MINO_INT) {
        long long idx = key->as.i;
        if (idx >= 0 && (size_t)idx < coll->as.vec.len) {
            old_val = vec_nth(coll, (size_t)idx);
        }
    } else if (coll == NULL || coll->type == MINO_NIL) {
        /* Update on nil behaves like update on an empty map. */
    } else {
        set_error("update: unsupported collection");
        return NULL;
    }
    call_args = mino_cons(old_val, mino_nil());
    new_val = apply_callable(fn, call_args, env);
    if (new_val == NULL) {
        return NULL;
    }
    {
        mino_val_t *assoc_args;
        assoc_args = mino_cons(
            coll == NULL ? mino_nil() : coll,
            mino_cons(key, mino_cons(new_val, mino_nil())));
        return prim_assoc(assoc_args, env);
    }
}

static mino_val_t *prim_keys(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *head = mino_nil();
    mino_val_t *tail = NULL;
    size_t i;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("keys requires one argument");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil();
    }
    if (coll->type != MINO_MAP) {
        set_error("keys: argument must be a map");
        return NULL;
    }
    for (i = 0; i < coll->as.map.len; i++) {
        mino_val_t *cell = mino_cons(vec_nth(coll->as.map.key_order, i),
                                      mino_nil());
        if (tail == NULL) {
            head = cell;
        } else {
            tail->as.cons.cdr = cell;
        }
        tail = cell;
    }
    return head;
}

static mino_val_t *prim_vals(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *head = mino_nil();
    mino_val_t *tail = NULL;
    size_t i;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("vals requires one argument");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil();
    }
    if (coll->type != MINO_MAP) {
        set_error("vals: argument must be a map");
        return NULL;
    }
    for (i = 0; i < coll->as.map.len; i++) {
        mino_val_t *key  = vec_nth(coll->as.map.key_order, i);
        mino_val_t *cell = mino_cons(map_get_val(coll, key), mino_nil());
        if (tail == NULL) {
            head = cell;
        } else {
            tail->as.cons.cdr = cell;
        }
        tail = cell;
    }
    return head;
}

static mino_val_t *prim_macroexpand_1(mino_val_t *args, mino_env_t *env)
{
    int expanded;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("macroexpand-1 requires one argument");
        return NULL;
    }
    return macroexpand1(args->as.cons.car, env, &expanded);
}

static mino_val_t *prim_macroexpand(mino_val_t *args, mino_env_t *env)
{
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("macroexpand requires one argument");
        return NULL;
    }
    return macroexpand_all(args->as.cons.car, env);
}

static long gensym_counter = 0;

static mino_val_t *prim_gensym(mino_val_t *args, mino_env_t *env)
{
    const char *prefix_src = "G__";
    size_t      prefix_len = 3;
    char        buf[256];
    size_t      nargs;
    (void)env;
    arg_count(args, &nargs);
    if (nargs > 1) {
        set_error("gensym takes 0 or 1 arguments");
        return NULL;
    }
    if (nargs == 1) {
        mino_val_t *p = args->as.cons.car;
        if (p == NULL || p->type != MINO_STRING) {
            set_error("gensym prefix must be a string");
            return NULL;
        }
        prefix_src = p->as.s.data;
        prefix_len = p->as.s.len;
        if (prefix_len >= sizeof(buf) - 32) {
            set_error("gensym prefix too long");
            return NULL;
        }
    }
    {
        int used;
        memcpy(buf, prefix_src, prefix_len);
        used = snprintf(buf + prefix_len, sizeof(buf) - prefix_len,
                        "%ld", ++gensym_counter);
        if (used < 0) {
            set_error("gensym formatting failed");
            return NULL;
        }
        return mino_symbol_n(buf, prefix_len + (size_t)used);
    }
}

void mino_install_core(mino_env_t *env)
{
    mino_env_set(env, "+",        mino_prim("+",        prim_add));
    mino_env_set(env, "-",        mino_prim("-",        prim_sub));
    mino_env_set(env, "*",        mino_prim("*",        prim_mul));
    mino_env_set(env, "/",        mino_prim("/",        prim_div));
    mino_env_set(env, "=",        mino_prim("=",        prim_eq));
    mino_env_set(env, "<",        mino_prim("<",        prim_lt));
    mino_env_set(env, "<=",       mino_prim("<=",       prim_le));
    mino_env_set(env, ">",        mino_prim(">",        prim_gt));
    mino_env_set(env, ">=",       mino_prim(">=",       prim_ge));
    mino_env_set(env, "car",      mino_prim("car",      prim_car));
    mino_env_set(env, "cdr",      mino_prim("cdr",      prim_cdr));
    mino_env_set(env, "cons",     mino_prim("cons",     prim_cons));
    mino_env_set(env, "list",     mino_prim("list",     prim_list));
    mino_env_set(env, "count",    mino_prim("count",    prim_count));
    mino_env_set(env, "nth",      mino_prim("nth",      prim_nth));
    mino_env_set(env, "first",    mino_prim("first",    prim_first));
    mino_env_set(env, "rest",     mino_prim("rest",     prim_rest));
    mino_env_set(env, "vector",   mino_prim("vector",   prim_vector));
    mino_env_set(env, "hash-map", mino_prim("hash-map", prim_hash_map));
    mino_env_set(env, "assoc",    mino_prim("assoc",    prim_assoc));
    mino_env_set(env, "get",      mino_prim("get",      prim_get));
    mino_env_set(env, "conj",     mino_prim("conj",     prim_conj));
    mino_env_set(env, "update",   mino_prim("update",   prim_update));
    mino_env_set(env, "keys",     mino_prim("keys",     prim_keys));
    mino_env_set(env, "vals",     mino_prim("vals",     prim_vals));
    mino_env_set(env, "macroexpand-1",
                 mino_prim("macroexpand-1", prim_macroexpand_1));
    mino_env_set(env, "macroexpand",
                 mino_prim("macroexpand", prim_macroexpand));
    mino_env_set(env, "gensym",   mino_prim("gensym",   prim_gensym));
}
