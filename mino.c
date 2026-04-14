/*
 * mino.c — runtime implementation.
 *
 * Single-file amalgamation: this translation unit, paired with mino.h,
 * is the entire runtime. ANSI C, no external dependencies.
 */

#include "mino.h"

#include <ctype.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Allocation and garbage collection                                         */
/* ------------------------------------------------------------------------- */
/*
 * Every managed allocation carries a prepended gc_hdr_t and is threaded onto
 * a global singly-linked registry. All heap allocators in this file route
 * through gc_alloc_typed. A periodic stop-the-world mark-and-sweep walks the
 * registry: roots are traced, survivors marked, and the list is then filtered
 * in place to drop unmarked entries.
 *
 * Roots come from three sources:
 *   - every mino_env_t returned by mino_env_new, registered in root_envs;
 *   - the symbol and keyword intern tables, which pin every interned name;
 *   - a conservative scan of the C stack between the saved `gc_stack_bottom`
 *     (captured at the first mino_eval entry) and the collector's own frame.
 *     setjmp flushes registers onto that range so register-resident pointers
 *     are visible to the scan. Any aligned machine word whose value falls
 *     inside an allocated payload [p, p+size) pins that object.
 *
 * A collection runs when the gap between bytes allocated and bytes live at
 * the last sweep crosses a threshold, or on every allocation when the env
 * var MINO_GC_STRESS is set — the stress mode validates that no caller holds
 * unrooted pointers across any allocation site. `gc_depth` guards against
 * re-entrant collection during the collector's own bookkeeping allocations.
 */

enum {
    GC_T_RAW        = 1,  /* opaque bytes: strings, env binding arrays         */
    GC_T_VAL        = 2,  /* mino_val_t                                        */
    GC_T_ENV        = 3,  /* mino_env_t                                        */
    GC_T_VEC_NODE   = 4,  /* mino_vec_node_t (vector trie node; leaf flag set
                           *                  on leaves)                        */
    GC_T_HAMT_NODE  = 5,  /* mino_hamt_node_t                                  */
    GC_T_HAMT_ENTRY = 6,  /* hamt_entry_t                                      */
    GC_T_PTRARR     = 7,  /* void** slots inside a HAMT node                   */
    GC_T_VALARR     = 8   /* mino_val_t** scratch during collection building  */
};

typedef struct gc_hdr {
    unsigned char  type_tag;
    unsigned char  mark;
    size_t         size;      /* payload byte count                            */
    struct gc_hdr *next;      /* registry link (all allocated objects)         */
} gc_hdr_t;

static gc_hdr_t *gc_all          = NULL;
static size_t    gc_bytes_alloc  = 0;
static size_t    gc_bytes_live   = 0;           /* after last sweep            */
static size_t    gc_threshold    = 1u << 20;    /* 1 MiB default               */
static int       gc_stress       = -1;          /* -1 unset, 0 off, 1 on       */
static int       gc_depth        = 0;           /* re-entrancy guard           */
static void     *gc_stack_bottom = NULL;        /* set at first mino_eval      */

/* Execution limits (global; set via mino_set_limit). */
static size_t    limit_steps     = 0;           /* 0 = unlimited               */
static size_t    limit_heap      = 0;           /* 0 = unlimited               */
static size_t    eval_steps      = 0;           /* current step count          */
static int       limit_exceeded  = 0;           /* sticky flag for this eval   */

/* Exception handling: setjmp/longjmp stack for try/catch. */
#define MAX_TRY_DEPTH 64

typedef struct {
    jmp_buf     buf;
    mino_val_t *exception;
} try_frame_t;

static try_frame_t try_stack[MAX_TRY_DEPTH];
static int         try_depth = 0;

/* Module resolver (set via mino_set_resolver). */
static mino_resolve_fn module_resolver     = NULL;
static void           *module_resolver_ctx = NULL;

typedef struct {
    char       *name;
    mino_val_t *value;
} module_entry_t;

static module_entry_t *module_cache     = NULL;
static size_t          module_cache_len = 0;
static size_t          module_cache_cap = 0;

/* Metadata table: docstrings and source forms for def/defmacro bindings. */
typedef struct {
    char       *name;       /* binding name (malloc-owned) */
    char       *docstring;  /* docstring (malloc-owned, NULL if none) */
    mino_val_t *source;     /* source form (the whole def/defmacro form) */
} meta_entry_t;

static meta_entry_t *meta_table     = NULL;
static size_t        meta_table_len = 0;
static size_t        meta_table_cap = 0;

static meta_entry_t *meta_find(const char *name)
{
    size_t i;
    for (i = 0; i < meta_table_len; i++) {
        if (strcmp(meta_table[i].name, name) == 0) {
            return &meta_table[i];
        }
    }
    return NULL;
}

static void meta_set(const char *name, const char *doc, size_t doc_len,
                     mino_val_t *source)
{
    meta_entry_t *e = meta_find(name);
    if (e != NULL) {
        free(e->docstring);
        e->docstring = NULL;
        if (doc != NULL) {
            e->docstring = (char *)malloc(doc_len + 1);
            if (e->docstring != NULL) {
                memcpy(e->docstring, doc, doc_len);
                e->docstring[doc_len] = '\0';
            }
        }
        e->source = source;
        return;
    }
    if (meta_table_len == meta_table_cap) {
        size_t new_cap = meta_table_cap == 0 ? 32 : meta_table_cap * 2;
        meta_entry_t *ne = (meta_entry_t *)realloc(
            meta_table, new_cap * sizeof(*ne));
        if (ne == NULL) { return; }
        meta_table = ne;
        meta_table_cap = new_cap;
    }
    {
        size_t nlen = strlen(name);
        meta_table[meta_table_len].name = (char *)malloc(nlen + 1);
        if (meta_table[meta_table_len].name == NULL) { return; }
        memcpy(meta_table[meta_table_len].name, name, nlen + 1);
    }
    meta_table[meta_table_len].docstring = NULL;
    if (doc != NULL) {
        meta_table[meta_table_len].docstring = (char *)malloc(doc_len + 1);
        if (meta_table[meta_table_len].docstring != NULL) {
            memcpy(meta_table[meta_table_len].docstring, doc, doc_len);
            meta_table[meta_table_len].docstring[doc_len] = '\0';
        }
    }
    meta_table[meta_table_len].source = source;
    meta_table_len++;
}

static void gc_collect(void);

/* Record a stack address from a host-called entry point so the collector's
 * conservative scan covers the entire host-to-mino call chain. We keep the
 * maximum address (shallowest frame on a downward-growing stack). */
static void gc_note_host_frame(void *addr)
{
    if (gc_stack_bottom == NULL
        || (char *)addr > (char *)gc_stack_bottom) {
        gc_stack_bottom = addr;
    }
}

static void *gc_alloc_typed(unsigned char tag, size_t size)
{
    gc_hdr_t *h;
    if (gc_stress == -1) {
        const char *e = getenv("MINO_GC_STRESS");
        gc_stress = (e != NULL && e[0] != '\0' && e[0] != '0') ? 1 : 0;
    }
    if (gc_depth == 0 && gc_stack_bottom != NULL
        && (gc_stress || gc_bytes_alloc - gc_bytes_live > gc_threshold)) {
        gc_collect();
    }
    h = (gc_hdr_t *)calloc(1, sizeof(*h) + size);
    if (h == NULL) {
        abort();
    }
    h->type_tag      = tag;
    h->mark          = 0;
    h->size          = size;
    h->next          = gc_all;
    gc_all           = h;
    gc_bytes_alloc  += size;
    return (void *)(h + 1);
}

static mino_val_t *alloc_val(mino_type_t type)
{
    mino_val_t *v = (mino_val_t *)gc_alloc_typed(GC_T_VAL, sizeof(*v));
    v->type = type;
    return v;
}

static char *dup_n(const char *s, size_t len)
{
    char *out = (char *)gc_alloc_typed(GC_T_RAW, len + 1);
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
    unsigned char is_leaf;             /* 1 at level-0 leaves, 0 at branches */
    unsigned      count;               /* number of used slots */
    void         *slots[MINO_VEC_WIDTH]; /* mino_val_t* at leaves; mino_vec_node_t* at branches */
};

static mino_vec_node_t *vnode_new(unsigned count, int is_leaf)
{
    mino_vec_node_t *n = (mino_vec_node_t *)gc_alloc_typed(
        GC_T_VEC_NODE, sizeof(*n));
    n->is_leaf = (unsigned char)(is_leaf ? 1 : 0);
    n->count   = count;
    return n;
}

static mino_vec_node_t *vnode_clone(const mino_vec_node_t *src)
{
    mino_vec_node_t *n = (mino_vec_node_t *)gc_alloc_typed(
        GC_T_VEC_NODE, sizeof(*n));
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
    n = vnode_new(1, 0);
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
            new_tail = vnode_new(1, 1);
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
    new_tail = vnode_new(1, 1);
    new_tail->slots[0] = item;
    trie_count = v->as.vec.len - v->as.vec.tail_len; /* before incorporation */
    new_shift  = v->as.vec.shift;
    if (v->as.vec.root == NULL) {
        /* Trie was empty: old tail becomes the leaf root. */
        new_root  = v->as.vec.tail;
        new_shift = 0;
    } else if (trie_count == ((size_t)1u << (v->as.vec.shift + MINO_VEC_B))) {
        /* Root is full at the current height: add a level. */
        mino_vec_node_t *grown = vnode_new(2, 0);
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
    /* Internal construction: suppress collection across the whole build so
     * intermediate layer arrays (plain malloc'd; not visible to the GC) stay
     * consistent. No user code runs inside; the next allocation after return
     * will re-enable periodic collection. */
    gc_depth++;
    trie_count = len - tail_len;
    tail = vnode_new(tail_len, 1);
    memcpy(tail->slots, items + trie_count, tail_len * sizeof(*items));
    if (trie_count == 0) {
        gc_depth--;
        return vec_assemble(NULL, tail, tail_len, 0u, len);
    }
    num_leaves = trie_count / MINO_VEC_WIDTH;
    layer = (mino_vec_node_t **)malloc(num_leaves * sizeof(*layer));
    if (layer == NULL) {
        abort();
    }
    for (i = 0; i < num_leaves; i++) {
        mino_vec_node_t *leaf = vnode_new(MINO_VEC_WIDTH, 1);
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
            node = vnode_new((unsigned)take, 0);
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
        gc_depth--;
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
    hamt_entry_t *e = (hamt_entry_t *)gc_alloc_typed(
        GC_T_HAMT_ENTRY, sizeof(*e));
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
    case MINO_SET: {
        /* XOR-fold of element hashes for order independence. */
        uint32_t acc = 0;
        size_t   n   = v->as.set.len;
        size_t   i;
        unsigned k;
        for (i = 0; i < n; i++) {
            mino_val_t *elem = vec_nth(v->as.set.key_order, i);
            acc ^= hash_val(elem);
        }
        h = fnv_mix(h, 0x0d);
        for (k = 0; k < 4; k++) {
            h = fnv_mix(h, (unsigned char)(acc & 0xFFu));
            acc >>= 8;
        }
        return h;
    }
    case MINO_HANDLE: {
        /* Handles compare by their host pointer, so we hash that. */
        uintptr_t p = (uintptr_t)v->as.handle.ptr;
        unsigned  i;
        h = fnv_mix(h, 0x0c);
        for (i = 0; i < sizeof(uintptr_t); i++) {
            h = fnv_mix(h, (unsigned char)(p & 0xFFu));
            p >>= 8;
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
    mino_hamt_node_t *n = (mino_hamt_node_t *)gc_alloc_typed(
        GC_T_HAMT_NODE, sizeof(*n));
    n->bitmap       = bitmap;
    n->subnode_mask = subnode_mask;
    n->slots        = slots;
    return n;
}

static mino_hamt_node_t *hamt_collision_node(uint32_t hash, void **slots,
                                              unsigned count)
{
    mino_hamt_node_t *n = (mino_hamt_node_t *)gc_alloc_typed(
        GC_T_HAMT_NODE, sizeof(*n));
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
        void **slots = (void **)gc_alloc_typed(GC_T_PTRARR, 2 * sizeof(*slots));
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
            void **slots = (void **)gc_alloc_typed(GC_T_PTRARR, sizeof(*slots));
            if (slots == NULL) { abort(); }
            slots[0] = child;
            return hamt_bitmap_node(1u << i1, 1u << i1, slots);
        } else {
            void **slots = (void **)gc_alloc_typed(GC_T_PTRARR, 2 * sizeof(*slots));
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
        void    **slots = (void **)gc_alloc_typed(GC_T_PTRARR, sizeof(*slots));
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
                    void **slots = (void **)gc_alloc_typed(GC_T_PTRARR, n->collision_count * sizeof(*slots));
                    unsigned k;
                    if (slots == NULL) { abort(); }
                    for (k = 0; k < n->collision_count; k++) { slots[k] = n->slots[k]; }
                    slots[j] = new_entry;
                    *replaced = 1;
                    return hamt_collision_node(h, slots, n->collision_count);
                }
            }
            {
                void **slots = (void **)gc_alloc_typed(GC_T_PTRARR, (n->collision_count + 1u) * sizeof(*slots));
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
                void            **slots = (void **)gc_alloc_typed(GC_T_PTRARR, sizeof(*slots));
                if (slots == NULL) { abort(); }
                slots[0] = sub;
                return hamt_bitmap_node(1u << ib, 1u << ib, slots);
            } else {
                void **slots = (void **)gc_alloc_typed(GC_T_PTRARR, 2 * sizeof(*slots));
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
            void **slots = (void **)gc_alloc_typed(GC_T_PTRARR, (pop + 1u) * sizeof(*slots));
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
            void **slots = (void **)gc_alloc_typed(GC_T_PTRARR, pop * sizeof(*slots));
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
                void **slots = (void **)gc_alloc_typed(GC_T_PTRARR, pop * sizeof(*slots));
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
                void            **slots = (void **)gc_alloc_typed(GC_T_PTRARR, pop * sizeof(*slots));
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

mino_val_t *mino_set(mino_val_t **items, size_t len)
{
    mino_val_t       *v        = alloc_val(MINO_SET);
    mino_hamt_node_t *root     = NULL;
    mino_val_t       *order    = mino_vector(NULL, 0);
    size_t            len_out  = 0;
    size_t            i;
    /* Sentinel value: all set entries map to true. */
    mino_val_t       *sentinel = mino_true();
    for (i = 0; i < len; i++) {
        hamt_entry_t *e        = hamt_entry_new(items[i], sentinel);
        uint32_t      h        = hash_val(items[i]);
        int           replaced = 0;
        root = hamt_assoc(root, e, h, 0u, &replaced);
        if (!replaced) {
            order = vec_conj1(order, items[i]);
            len_out++;
        }
    }
    v->as.set.root      = root;
    v->as.set.key_order = order;
    v->as.set.len       = len_out;
    return v;
}

mino_val_t *mino_prim(const char *name, mino_prim_fn fn)
{
    mino_val_t *v = alloc_val(MINO_PRIM);
    v->as.prim.name = name;
    v->as.prim.fn   = fn;
    return v;
}

mino_val_t *mino_handle(void *ptr, const char *tag)
{
    mino_val_t *v = alloc_val(MINO_HANDLE);
    v->as.handle.ptr = ptr;
    v->as.handle.tag = tag;
    return v;
}

int mino_is_handle(const mino_val_t *v)
{
    return v != NULL && v->type == MINO_HANDLE;
}

void *mino_handle_ptr(const mino_val_t *v)
{
    if (v == NULL || v->type != MINO_HANDLE) {
        return NULL;
    }
    return v->as.handle.ptr;
}

const char *mino_handle_tag(const mino_val_t *v)
{
    if (v == NULL || v->type != MINO_HANDLE) {
        return NULL;
    }
    return v->as.handle.tag;
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

int mino_to_int(const mino_val_t *v, long long *out)
{
    if (v == NULL || v->type != MINO_INT) {
        return 0;
    }
    if (out != NULL) {
        *out = v->as.i;
    }
    return 1;
}

int mino_to_float(const mino_val_t *v, double *out)
{
    if (v == NULL || v->type != MINO_FLOAT) {
        return 0;
    }
    if (out != NULL) {
        *out = v->as.f;
    }
    return 1;
}

int mino_to_string(const mino_val_t *v, const char **out, size_t *len)
{
    if (v == NULL || v->type != MINO_STRING) {
        return 0;
    }
    if (out != NULL) {
        *out = v->as.s.data;
    }
    if (len != NULL) {
        *len = v->as.s.len;
    }
    return 1;
}

int mino_to_bool(const mino_val_t *v)
{
    return mino_is_truthy(v);
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

/*
 * Cycle-safe print depth: when recursion exceeds this limit, the printer
 * emits #<...> instead of descending further. This prevents stack overflow
 * on deeply nested or self-referential structures (possible through mutable
 * cons tails in internal data, though the user-facing API is immutable).
 */
#define MINO_PRINT_DEPTH_MAX 128

static int print_depth = 0;

void mino_print_to(FILE *out, const mino_val_t *v)
{
    if (v == NULL || v->type == MINO_NIL) {
        fputs("nil", out);
        return;
    }
    if (print_depth >= MINO_PRINT_DEPTH_MAX) {
        fputs("#<...>", out);
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
        print_depth++;
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
        print_depth--;
        fputc(')', out);
        return;
    }
    case MINO_VECTOR: {
        size_t i;
        fputc('[', out);
        print_depth++;
        for (i = 0; i < v->as.vec.len; i++) {
            if (i > 0) {
                fputc(' ', out);
            }
            mino_print_to(out, vec_nth(v, i));
        }
        print_depth--;
        fputc(']', out);
        return;
    }
    case MINO_MAP: {
        size_t i;
        fputc('{', out);
        print_depth++;
        for (i = 0; i < v->as.map.len; i++) {
            mino_val_t *key = vec_nth(v->as.map.key_order, i);
            if (i > 0) {
                fputs(", ", out);
            }
            mino_print_to(out, key);
            fputc(' ', out);
            mino_print_to(out, map_get_val(v, key));
        }
        print_depth--;
        fputc('}', out);
        return;
    }
    case MINO_SET: {
        size_t i;
        fputs("#{", out);
        print_depth++;
        for (i = 0; i < v->as.set.len; i++) {
            if (i > 0) {
                fputc(' ', out);
            }
            mino_print_to(out, vec_nth(v->as.set.key_order, i));
        }
        print_depth--;
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
    case MINO_HANDLE:
        fputs("#<handle", out);
        if (v->as.handle.tag != NULL) {
            fputc(':', out);
            fputs(v->as.handle.tag, out);
        }
        fputc('>', out);
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

static char error_buf[2048] = { 0 };

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

/* Location-aware error: prepend file:line when the form has source info. */
static void set_error_at(const mino_val_t *form, const char *msg)
{
    if (form != NULL && form->type == MINO_CONS
        && form->as.cons.file != NULL && form->as.cons.line > 0) {
        char buf[2048];
        snprintf(buf, sizeof(buf), "%s:%d: %s",
                 form->as.cons.file, form->as.cons.line, msg);
        set_error(buf);
    } else {
        set_error(msg);
    }
}

/* Return a short human-readable label for a value's type. */
static const char *type_tag_str(const mino_val_t *v)
{
    if (v == NULL) return "nil";
    switch (v->type) {
    case MINO_NIL:     return "nil";
    case MINO_BOOL:    return "bool";
    case MINO_INT:     return "int";
    case MINO_FLOAT:   return "float";
    case MINO_STRING:  return "string";
    case MINO_SYMBOL:  return "symbol";
    case MINO_KEYWORD: return "keyword";
    case MINO_CONS:    return "list";
    case MINO_VECTOR:  return "vector";
    case MINO_MAP:     return "map";
    case MINO_SET:     return "set";
    case MINO_PRIM:    return "fn";
    case MINO_FN:      return "fn";
    case MINO_MACRO:   return "macro";
    case MINO_HANDLE:  return "handle";
    case MINO_RECUR:   return "recur";
    }
    return "unknown";
}

/* ------------------------------------------------------------------------- */
/* Call stack (for stack traces on error)                                     */
/* ------------------------------------------------------------------------- */

#define MAX_CALL_DEPTH 256

typedef struct {
    const char *name;
    const char *file;
    int         line;
} call_frame_t;

static call_frame_t call_stack[MAX_CALL_DEPTH];
static int          call_depth  = 0;
static int          trace_added = 0;  /* set after trace is appended */

static void push_frame(const char *name, const char *file, int line)
{
    if (call_depth < MAX_CALL_DEPTH) {
        call_stack[call_depth].name = name;
        call_stack[call_depth].file = file;
        call_stack[call_depth].line = line;
        call_depth++;
    }
}

static void pop_frame(void)
{
    if (call_depth > 0) {
        call_depth--;
    }
}

/* Append the current call stack to error_buf. Called once per error. */
static void append_trace(void)
{
    size_t pos;
    int    i;
    if (trace_added || call_depth == 0) {
        return;
    }
    trace_added = 1;
    pos = strlen(error_buf);
    for (i = call_depth - 1; i >= 0 && pos + 80 < sizeof(error_buf); i--) {
        pos += (size_t)snprintf(
            error_buf + pos, sizeof(error_buf) - pos, "\n  in %s",
            call_stack[i].name ? call_stack[i].name : "<fn>");
        if (call_stack[i].file != NULL && pos + 40 < sizeof(error_buf)) {
            pos += (size_t)snprintf(
                error_buf + pos, sizeof(error_buf) - pos, " (%s:%d)",
                call_stack[i].file, call_stack[i].line);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Reader                                                                    */
/* ------------------------------------------------------------------------- */

/*
 * Reader state: file name and line number for source-location tracking.
 * File names are interned so the const char* stored in cons cells remains
 * valid for the lifetime of the process.
 */
static const char *reader_file = NULL;
static int         reader_line = 1;

#define MAX_INTERNED_FILES 64

static const char *intern_filename(const char *name)
{
    static const char *files[MAX_INTERNED_FILES];
    static size_t      file_count = 0;
    size_t i;
    if (name == NULL) {
        return NULL;
    }
    for (i = 0; i < file_count; i++) {
        if (strcmp(files[i], name) == 0) {
            return files[i];
        }
    }
    if (file_count < MAX_INTERNED_FILES) {
        size_t len = strlen(name);
        char  *dup = (char *)malloc(len + 1);
        if (dup == NULL) {
            return name;
        }
        memcpy(dup, name, len + 1);
        files[file_count++] = dup;
        return dup;
    }
    return name;
}

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
        if (c == '\n') {
            reader_line++;
            (*p)++;
        } else if (is_ws(c)) {
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
        if (c == '\n') {
            reader_line++;
        }
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
    int         list_line = reader_line;
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
            int         elem_line = reader_line;
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
                cell->as.cons.file = reader_file;
                cell->as.cons.line = (tail == NULL) ? list_line : elem_line;
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
    /* Caller has positioned *p on the opening '['. `buf` accumulates the
     * partially-built element list and is tracked by the GC so intermediate
     * allocations inside nested read_form calls don't collect the entries
     * that have already been parsed. */
    mino_val_t **buf = NULL;
    size_t       cap = 0;
    size_t       len = 0;
    (*p)++; /* skip '[' */
    for (;;) {
        skip_ws(p);
        if (**p == '\0') {
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
                if (mino_last_error() == NULL) {
                    set_error("unterminated vector");
                }
                return NULL;
            }
            if (len == cap) {
                size_t       new_cap = cap == 0 ? 8 : cap * 2;
                mino_val_t **nb      = (mino_val_t **)gc_alloc_typed(
                    GC_T_VALARR, new_cap * sizeof(*nb));
                if (buf != NULL && len > 0) {
                    memcpy(nb, buf, len * sizeof(*nb));
                }
                buf = nb;
                cap = new_cap;
            }
            buf[len++] = elem;
        }
    }
    return mino_vector(buf, len);
}

static mino_val_t *read_map_form(const char **p)
{
    /* Caller has positioned *p on the opening '{'. Elements alternate as
     * key, value, key, value. An odd count is a parse error. The key and
     * value buffers are GC-tracked so parsed entries survive allocations
     * performed by later nested read_form calls. */
    mino_val_t **kbuf = NULL;
    mino_val_t **vbuf = NULL;
    size_t       cap  = 0;
    size_t       len  = 0;
    (*p)++; /* skip '{' */
    for (;;) {
        mino_val_t *key;
        mino_val_t *val;
        skip_ws(p);
        if (**p == '\0') {
            set_error("unterminated map");
            return NULL;
        }
        if (**p == '}') {
            (*p)++;
            break;
        }
        key = read_form(p);
        if (key == NULL) {
            if (mino_last_error() == NULL) {
                set_error("unterminated map");
            }
            return NULL;
        }
        skip_ws(p);
        if (**p == '}' || **p == '\0') {
            set_error("map literal has odd number of forms");
            return NULL;
        }
        val = read_form(p);
        if (val == NULL) {
            if (mino_last_error() == NULL) {
                set_error("unterminated map");
            }
            return NULL;
        }
        if (len == cap) {
            size_t       new_cap = cap == 0 ? 8 : cap * 2;
            mino_val_t **nk      = (mino_val_t **)gc_alloc_typed(
                GC_T_VALARR, new_cap * sizeof(*nk));
            mino_val_t **nv      = (mino_val_t **)gc_alloc_typed(
                GC_T_VALARR, new_cap * sizeof(*nv));
            if (kbuf != NULL && len > 0) {
                memcpy(nk, kbuf, len * sizeof(*nk));
                memcpy(nv, vbuf, len * sizeof(*nv));
            }
            kbuf = nk;
            vbuf = nv;
            cap  = new_cap;
        }
        kbuf[len] = key;
        vbuf[len] = val;
        len++;
    }
    return mino_map(kbuf, vbuf, len);
}

static mino_val_t *read_set_form(const char **p)
{
    /* Caller has positioned *p on the opening '{' after '#'. */
    mino_val_t **buf = NULL;
    size_t       cap = 0;
    size_t       len = 0;
    (*p)++; /* skip '{' */
    for (;;) {
        mino_val_t *elem;
        skip_ws(p);
        if (**p == '\0') {
            set_error("unterminated set");
            return NULL;
        }
        if (**p == '}') {
            (*p)++;
            break;
        }
        elem = read_form(p);
        if (elem == NULL) {
            if (mino_last_error() == NULL) {
                set_error("unterminated set");
            }
            return NULL;
        }
        if (len == cap) {
            size_t       new_cap = cap == 0 ? 8 : cap * 2;
            mino_val_t **nb      = (mino_val_t **)gc_alloc_typed(
                GC_T_VALARR, new_cap * sizeof(*nb));
            if (buf != NULL && len > 0) {
                memcpy(nb, buf, len * sizeof(*nb));
            }
            buf = nb;
            cap = new_cap;
        }
        buf[len++] = elem;
    }
    return mino_set(buf, len);
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
    if (**p == '#' && *(*p + 1) == '{') {
        (*p)++; /* skip '#', read_set_form will skip '{' */
        return read_set_form(p);
    }
    if (**p == '"') {
        return read_string_form(p);
    }
    if (**p == '\'') {
        int q_line = reader_line;
        (*p)++;
        {
            mino_val_t *quoted = read_form(p);
            mino_val_t *outer;
            if (quoted == NULL) {
                if (mino_last_error() == NULL) {
                    set_error("expected form after quote");
                }
                return NULL;
            }
            outer = mino_cons(mino_symbol("quote"),
                              mino_cons(quoted, mino_nil()));
            outer->as.cons.file = reader_file;
            outer->as.cons.line = q_line;
            return outer;
        }
    }
    if (**p == '`') {
        int q_line = reader_line;
        (*p)++;
        {
            mino_val_t *qq = read_form(p);
            mino_val_t *outer;
            if (qq == NULL) {
                if (mino_last_error() == NULL) {
                    set_error("expected form after `");
                }
                return NULL;
            }
            outer = mino_cons(mino_symbol("quasiquote"),
                              mino_cons(qq, mino_nil()));
            outer->as.cons.file = reader_file;
            outer->as.cons.line = q_line;
            return outer;
        }
    }
    if (**p == '~') {
        int         q_line = reader_line;
        const char *name = "unquote";
        (*p)++;
        if (**p == '@') {
            name = "unquote-splicing";
            (*p)++;
        }
        {
            mino_val_t *uq = read_form(p);
            mino_val_t *outer;
            if (uq == NULL) {
                if (mino_last_error() == NULL) {
                    set_error("expected form after ~");
                }
                return NULL;
            }
            outer = mino_cons(mino_symbol(name),
                              mino_cons(uq, mino_nil()));
            outer->as.cons.file = reader_file;
            outer->as.cons.line = q_line;
            return outer;
        }
    }
    return read_atom(p);
}

mino_val_t *mino_read(const char *src, const char **end)
{
    volatile char probe = 0;
    const char   *p = src;
    mino_val_t   *v;
    /* Record this frame as a host-level stack bottom so the collector's
     * conservative scan covers the reader's call chain in full. */
    gc_note_host_frame((void *)&probe);
    (void)probe;
    if (reader_file == NULL) {
        reader_file = intern_filename("<input>");
    }
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
    case MINO_SET: {
        /* Set equality: same elements regardless of insertion order. */
        size_t i;
        if (a->as.set.len != b->as.set.len) {
            return 0;
        }
        for (i = 0; i < a->as.set.len; i++) {
            mino_val_t *elem = vec_nth(a->as.set.key_order, i);
            uint32_t    h    = hash_val(elem);
            if (hamt_get(b->as.set.root, elem, h, 0u) == NULL) {
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
    case MINO_HANDLE:
        return a->as.handle.ptr == b->as.handle.ptr;
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
 * Envs and their binding arrays are GC-managed. `mino_env_new` registers
 * the returned env as a persistent root so the whole chain (and everything
 * reachable from its bindings) survives collection; `mino_env_free` unroots
 * it, letting the next sweep reclaim the frame and any closures that were
 * only reachable through it.
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

/* Registry of root environments returned by mino_env_new. Tracing starts
 * from every env on this list. The list node itself is ordinary malloc-
 * allocated and freed on mino_env_free; the env it references is GC-owned. */
typedef struct root_env {
    mino_env_t      *env;
    struct root_env *next;
} root_env_t;

static root_env_t *gc_root_envs = NULL;

static mino_env_t *env_alloc(mino_env_t *parent)
{
    mino_env_t *env = (mino_env_t *)gc_alloc_typed(GC_T_ENV, sizeof(*env));
    env->parent = parent;
    return env;
}

mino_env_t *mino_env_new(void)
{
    volatile char probe = 0;
    mino_env_t   *env;
    root_env_t   *r;
    /* Record the host's stack frame: this is typically the earliest point
     * the host calls into mino, so it fixes a generous stack bottom before
     * any allocator runs. */
    gc_note_host_frame((void *)&probe);
    (void)probe;
    env = env_alloc(NULL);
    r   = (root_env_t *)malloc(sizeof(*r));
    if (r == NULL) {
        abort();
    }
    r->env       = env;
    r->next      = gc_root_envs;
    gc_root_envs = r;
    return env;
}

static mino_env_t *env_child(mino_env_t *parent)
{
    return env_alloc(parent);
}

void mino_env_free(mino_env_t *env)
{
    /* Unroot the env. Its memory, along with any closures and bindings
     * reachable only through it, is reclaimed at the next collection. */
    root_env_t **pp = &gc_root_envs;
    if (env == NULL) {
        return;
    }
    while (*pp != NULL) {
        if ((*pp)->env == env) {
            root_env_t *dead = *pp;
            *pp = dead->next;
            free(dead);
            return;
        }
        pp = &(*pp)->next;
    }
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
        size_t         new_cap = env->cap == 0 ? 16 : env->cap * 2;
        env_binding_t *nb      = (env_binding_t *)gc_alloc_typed(
            GC_T_RAW, new_cap * sizeof(*nb));
        if (env->bindings != NULL && env->len > 0) {
            memcpy(nb, env->bindings, env->len * sizeof(*nb));
        }
        env->bindings = nb;
        env->cap      = new_cap;
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
/* Garbage collector: mark-and-sweep                                         */
/* ------------------------------------------------------------------------- */
/*
 * Marking is driven by three root sources: the registry of user-held envs
 * (`gc_root_envs`), the symbol and keyword intern tables, and a conservative
 * scan of the C stack between `gc_stack_bottom` (saved on the first
 * mino_eval call) and the collector's own frame.
 *
 * The marker traces each object according to its type tag, following every
 * owned pointer it knows about. Conservative stack scan treats every aligned
 * machine word as a candidate pointer and consults a sorted range index
 * built from the allocation registry to check, in O(log n), whether the
 * word falls inside any managed payload. Interior pointers (into the middle
 * of a payload) count; this keeps the scan correct under normal compiler
 * optimizations that may retain base+offset forms in registers.
 *
 * Sweep walks the registry in place, freeing every allocation whose mark
 * bit is clear and resetting the mark bit on survivors. The live-byte tally
 * becomes the next cycle's baseline; the threshold grows to 2× live so the
 * collector's amortized cost stays bounded under steady-state programs.
 */

typedef struct {
    uintptr_t  start;  /* inclusive payload byte address */
    uintptr_t  end;    /* exclusive payload byte address */
    gc_hdr_t  *h;
} gc_range_t;

static gc_range_t *gc_ranges     = NULL;
static size_t      gc_ranges_len = 0;
static size_t      gc_ranges_cap = 0;

static int gc_range_cmp(const void *a, const void *b)
{
    const gc_range_t *ra = (const gc_range_t *)a;
    const gc_range_t *rb = (const gc_range_t *)b;
    if (ra->start < rb->start) return -1;
    if (ra->start > rb->start) return 1;
    return 0;
}

static void gc_build_range_index(void)
{
    gc_hdr_t *h;
    size_t    n = 0;
    for (h = gc_all; h != NULL; h = h->next) {
        n++;
    }
    if (n > gc_ranges_cap) {
        size_t      new_cap = n * 2 + 16;
        gc_range_t *nr      = (gc_range_t *)realloc(
            gc_ranges, new_cap * sizeof(*nr));
        if (nr == NULL) {
            abort();
        }
        gc_ranges     = nr;
        gc_ranges_cap = new_cap;
    }
    gc_ranges_len = 0;
    for (h = gc_all; h != NULL; h = h->next) {
        gc_ranges[gc_ranges_len].start = (uintptr_t)(h + 1);
        gc_ranges[gc_ranges_len].end   = (uintptr_t)(h + 1) + h->size;
        gc_ranges[gc_ranges_len].h     = h;
        gc_ranges_len++;
    }
    qsort(gc_ranges, gc_ranges_len, sizeof(*gc_ranges), gc_range_cmp);
}

static gc_hdr_t *gc_find_header_for_ptr(const void *p)
{
    uintptr_t u  = (uintptr_t)p;
    size_t    lo = 0;
    size_t    hi = gc_ranges_len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (u < gc_ranges[mid].start) {
            hi = mid;
        } else if (u >= gc_ranges[mid].end) {
            lo = mid + 1;
        } else {
            return gc_ranges[mid].h;
        }
    }
    return NULL;
}

static void gc_mark_header(gc_hdr_t *h);

static void gc_mark_interior(const void *p)
{
    gc_hdr_t *h;
    if (p == NULL) {
        return;
    }
    h = gc_find_header_for_ptr(p);
    if (h != NULL) {
        gc_mark_header(h);
    }
}

static void gc_mark_val(mino_val_t *v)
{
    if (v == NULL) {
        return;
    }
    switch (v->type) {
    case MINO_STRING:
    case MINO_SYMBOL:
    case MINO_KEYWORD:
        gc_mark_interior(v->as.s.data);
        break;
    case MINO_CONS:
        gc_mark_interior(v->as.cons.car);
        gc_mark_interior(v->as.cons.cdr);
        break;
    case MINO_VECTOR:
        gc_mark_interior(v->as.vec.root);
        gc_mark_interior(v->as.vec.tail);
        break;
    case MINO_MAP:
        gc_mark_interior(v->as.map.root);
        gc_mark_interior(v->as.map.key_order);
        break;
    case MINO_SET:
        gc_mark_interior(v->as.set.root);
        gc_mark_interior(v->as.set.key_order);
        break;
    case MINO_FN:
    case MINO_MACRO:
        gc_mark_interior(v->as.fn.params);
        gc_mark_interior(v->as.fn.body);
        gc_mark_interior(v->as.fn.env);
        break;
    case MINO_RECUR:
        gc_mark_interior(v->as.recur.args);
        break;
    default:
        /* NIL, BOOL, INT, FLOAT, PRIM, HANDLE: no owned children. prim.name
         * and handle.tag are static/host-owned C strings. */
        break;
    }
}

static void gc_mark_env(mino_env_t *env)
{
    size_t i;
    if (env == NULL) {
        return;
    }
    gc_mark_interior(env->parent);
    if (env->bindings != NULL) {
        gc_mark_interior(env->bindings);
        for (i = 0; i < env->len; i++) {
            gc_mark_interior(env->bindings[i].name);
            gc_mark_interior(env->bindings[i].val);
        }
    }
}

static void gc_mark_vec_node(mino_vec_node_t *n)
{
    unsigned i;
    if (n == NULL) {
        return;
    }
    /* Leaf slots hold mino_val_t*; branch slots hold mino_vec_node_t*.
     * gc_mark_interior dispatches on the header's type tag either way. */
    for (i = 0; i < n->count; i++) {
        gc_mark_interior(n->slots[i]);
    }
}

static void gc_mark_hamt_node(mino_hamt_node_t *n)
{
    unsigned count;
    unsigned i;
    if (n == NULL) {
        return;
    }
    gc_mark_interior(n->slots);
    count = (n->collision_count > 0) ? n->collision_count
                                     : popcount32(n->bitmap);
    if (n->slots != NULL) {
        for (i = 0; i < count; i++) {
            gc_mark_interior(n->slots[i]);
        }
    }
}

static void gc_mark_hamt_entry(hamt_entry_t *e)
{
    if (e == NULL) {
        return;
    }
    gc_mark_interior(e->key);
    gc_mark_interior(e->val);
}

static void gc_mark_ptr_array(void **arr, size_t bytes)
{
    size_t n = bytes / sizeof(*arr);
    size_t i;
    if (arr == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        gc_mark_interior(arr[i]);
    }
}

static void gc_mark_header(gc_hdr_t *h)
{
    if (h == NULL || h->mark) {
        return;
    }
    h->mark = 1;
    switch (h->type_tag) {
    case GC_T_VAL:
        gc_mark_val((mino_val_t *)(h + 1));
        break;
    case GC_T_ENV:
        gc_mark_env((mino_env_t *)(h + 1));
        break;
    case GC_T_VEC_NODE:
        gc_mark_vec_node((mino_vec_node_t *)(h + 1));
        break;
    case GC_T_HAMT_NODE:
        gc_mark_hamt_node((mino_hamt_node_t *)(h + 1));
        break;
    case GC_T_HAMT_ENTRY:
        gc_mark_hamt_entry((hamt_entry_t *)(h + 1));
        break;
    case GC_T_VALARR:
    case GC_T_PTRARR:
        gc_mark_ptr_array((void **)(h + 1), h->size);
        break;
    case GC_T_RAW:
    default:
        /* Leaf allocation: no children. */
        break;
    }
}

static void gc_scan_stack(void)
{
    volatile char probe = 0;
    char         *lo;
    char         *hi;
    char         *cur;
    if (gc_stack_bottom == NULL) {
        return;
    }
    if ((char *)&probe < (char *)gc_stack_bottom) {
        lo = (char *)&probe;
        hi = (char *)gc_stack_bottom;
    } else {
        lo = (char *)gc_stack_bottom;
        hi = (char *)&probe;
    }
    while (((uintptr_t)lo % sizeof(void *)) != 0 && lo < hi) {
        lo++;
    }
    for (cur = lo; cur + sizeof(void *) <= hi; cur += sizeof(void *)) {
        void *word;
        memcpy(&word, cur, sizeof(word));
        gc_mark_interior(word);
    }
    (void)probe;
}

static void gc_mark_intern_table(const intern_table_t *tbl)
{
    size_t i;
    for (i = 0; i < tbl->len; i++) {
        gc_mark_interior(tbl->entries[i]);
    }
}

static void gc_mark_roots(void)
{
    root_env_t *r;
    int i;
    for (r = gc_root_envs; r != NULL; r = r->next) {
        gc_mark_interior(r->env);
    }
    gc_mark_intern_table(&sym_intern);
    gc_mark_intern_table(&kw_intern);
    /* Pin try/catch exception values and module cache results. */
    for (i = 0; i < try_depth; i++) {
        gc_mark_interior(try_stack[i].exception);
    }
    {
        size_t mi;
        for (mi = 0; mi < module_cache_len; mi++) {
            gc_mark_interior(module_cache[mi].value);
        }
    }
    /* Pin metadata source forms. */
    {
        size_t mi;
        for (mi = 0; mi < meta_table_len; mi++) {
            gc_mark_interior(meta_table[mi].source);
        }
    }
}

static void gc_sweep(void)
{
    gc_hdr_t **pp   = &gc_all;
    size_t     live = 0;
    while (*pp != NULL) {
        gc_hdr_t *h = *pp;
        if (h->mark) {
            h->mark = 0;
            live += h->size;
            pp = &h->next;
        } else {
            *pp = h->next;
            free(h);
        }
    }
    gc_bytes_live  = live;
    gc_bytes_alloc = live;
    /* Next cycle triggers after another threshold's worth of growth above
     * the live set; threshold grows to keep collection amortized. */
    if (live * 2 > gc_threshold) {
        gc_threshold = live * 2;
    }
}

static void gc_collect(void)
{
    jmp_buf jb;
    if (gc_depth > 0) {
        return;
    }
    gc_depth++;
    /* setjmp spills callee-saved registers into jb, which lives in this
     * stack frame. gc_scan_stack scans from a deeper frame up through ours,
     * so any pointer that was register-resident at entry is visible. */
    if (setjmp(jb) != 0) {
        /* We never longjmp; a nonzero return indicates a jump from
         * somewhere unexpected. Surface the bug. */
        abort();
    }
    (void)jb;
    gc_build_range_index();
    gc_mark_roots();
    gc_scan_stack();
    gc_sweep();
    gc_depth--;
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
        size_t       i;
        if (nn == 0) { return form; }
        /* GC_T_VALARR: scratch buffer whose slots the collector traces as
         * mino_val_t*, so partial fills survive allocation mid-loop. */
        tmp = (mino_val_t **)gc_alloc_typed(GC_T_VALARR, nn * sizeof(*tmp));
        for (i = 0; i < nn; i++) {
            mino_val_t *e = quasiquote_expand(vec_nth(form, i), env);
            if (e == NULL) { return NULL; }
            tmp[i] = e;
        }
        return mino_vector(tmp, nn);
    }
    if (form != NULL && form->type == MINO_MAP) {
        size_t        nn = form->as.map.len;
        mino_val_t  **ks;
        mino_val_t  **vs;
        size_t        i;
        if (nn == 0) { return form; }
        ks = (mino_val_t **)gc_alloc_typed(GC_T_VALARR, nn * sizeof(*ks));
        vs = (mino_val_t **)gc_alloc_typed(GC_T_VALARR, nn * sizeof(*vs));
        for (i = 0; i < nn; i++) {
            mino_val_t *key = vec_nth(form->as.map.key_order, i);
            mino_val_t *val = map_get_val(form, key);
            mino_val_t *kk  = quasiquote_expand(key, env);
            mino_val_t *vv;
            if (kk == NULL) { return NULL; }
            vv = quasiquote_expand(val, env);
            if (vv == NULL) { return NULL; }
            ks[i] = kk; vs[i] = vv;
        }
        return mino_map(ks, vs, nn);
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

/* Tracks the most recent cons-cell form being evaluated; used by
 * set_error_at to include source location in error messages. */
static const mino_val_t *eval_current_form = NULL;

static mino_val_t *eval(mino_val_t *form, mino_env_t *env)
{
    if (limit_exceeded) {
        return NULL;
    }
    if (limit_steps > 0 && ++eval_steps > limit_steps) {
        limit_exceeded = 1;
        set_error("step limit exceeded");
        return NULL;
    }
    if (limit_heap > 0 && gc_bytes_alloc > limit_heap) {
        limit_exceeded = 1;
        set_error("heap limit exceeded");
        return NULL;
    }
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
    case MINO_HANDLE:
    case MINO_RECUR:
        return form;
    case MINO_SYMBOL: {
        char buf[256];
        size_t n = form->as.s.len;
        mino_val_t *v;
        if (n >= sizeof(buf)) {
            set_error_at(eval_current_form, "symbol name too long");
            return NULL;
        }
        memcpy(buf, form->as.s.data, n);
        buf[n] = '\0';
        v = mino_env_get(env, buf);
        if (v == NULL) {
            char msg[300];
            snprintf(msg, sizeof(msg), "unbound symbol: %s", buf);
            set_error_at(eval_current_form, msg);
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
        if (n == 0) {
            return form;
        }
        tmp = (mino_val_t **)gc_alloc_typed(GC_T_VALARR, n * sizeof(*tmp));
        for (i = 0; i < n; i++) {
            mino_val_t *ev = eval_value(vec_nth(form, i), env);
            if (ev == NULL) {
                return NULL;
            }
            tmp[i] = ev;
        }
        return mino_vector(tmp, n);
    }
    case MINO_MAP: {
        /* Map literals evaluate keys and values in read order; the
         * constructor handles duplicate-key resolution. */
        size_t i;
        size_t n = form->as.map.len;
        mino_val_t **ks;
        mino_val_t **vs;
        if (n == 0) {
            return form;
        }
        ks = (mino_val_t **)gc_alloc_typed(GC_T_VALARR, n * sizeof(*ks));
        vs = (mino_val_t **)gc_alloc_typed(GC_T_VALARR, n * sizeof(*vs));
        for (i = 0; i < n; i++) {
            mino_val_t *form_key = vec_nth(form->as.map.key_order, i);
            mino_val_t *form_val = map_get_val(form, form_key);
            mino_val_t *k = eval_value(form_key, env);
            mino_val_t *v;
            if (k == NULL) { return NULL; }
            v = eval_value(form_val, env);
            if (v == NULL) { return NULL; }
            ks[i] = k;
            vs[i] = v;
        }
        return mino_map(ks, vs, n);
    }
    case MINO_SET: {
        /* Set literals evaluate each element in order. */
        size_t i;
        size_t n = form->as.set.len;
        mino_val_t **tmp;
        if (n == 0) {
            return form;
        }
        tmp = (mino_val_t **)gc_alloc_typed(GC_T_VALARR, n * sizeof(*tmp));
        for (i = 0; i < n; i++) {
            mino_val_t *ev = eval_value(vec_nth(form->as.set.key_order, i), env);
            if (ev == NULL) {
                return NULL;
            }
            tmp[i] = ev;
        }
        return mino_set(tmp, n);
    }
    case MINO_CONS: {
        mino_val_t *head = form->as.cons.car;
        mino_val_t *args = form->as.cons.cdr;
        eval_current_form = form;

        /* Special forms. */
        if (sym_eq(head, "quote")) {
            if (!mino_is_cons(args)) {
                set_error_at(form, "quote requires one argument");
                return NULL;
            }
            return args->as.cons.car;
        }
        if (sym_eq(head, "quasiquote")) {
            if (!mino_is_cons(args)) {
                set_error_at(form, "quasiquote requires one argument");
                return NULL;
            }
            return quasiquote_expand(args->as.cons.car, env);
        }
        if (sym_eq(head, "unquote")
            || sym_eq(head, "unquote-splicing")) {
            set_error_at(form, "unquote outside of quasiquote");
            return NULL;
        }
        if (sym_eq(head, "defmacro")) {
            mino_val_t *name_form;
            mino_val_t *params;
            mino_val_t *body;
            mino_val_t *mac;
            mino_val_t *p;
            const char *doc     = NULL;
            size_t      doc_len = 0;
            char        buf[256];
            size_t      n;
            if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
                set_error_at(form, "defmacro requires a name, parameters, and body");
                return NULL;
            }
            name_form = args->as.cons.car;
            if (name_form == NULL || name_form->type != MINO_SYMBOL) {
                set_error_at(form, "defmacro name must be a symbol");
                return NULL;
            }
            /* Optional docstring: (defmacro name "doc" (params) body) */
            {
                mino_val_t *after_name = args->as.cons.cdr;
                mino_val_t *maybe_doc  = after_name->as.cons.car;
                if (maybe_doc != NULL && maybe_doc->type == MINO_STRING
                    && mino_is_cons(after_name->as.cons.cdr)) {
                    doc     = maybe_doc->as.s.data;
                    doc_len = maybe_doc->as.s.len;
                    params  = after_name->as.cons.cdr->as.cons.car;
                    body    = after_name->as.cons.cdr->as.cons.cdr;
                } else {
                    params = after_name->as.cons.car;
                    body   = after_name->as.cons.cdr;
                }
            }
            if (!mino_is_cons(params) && !mino_is_nil(params)) {
                set_error_at(form, "defmacro parameter list must be a list");
                return NULL;
            }
            for (p = params; mino_is_cons(p); p = p->as.cons.cdr) {
                mino_val_t *pn = p->as.cons.car;
                if (pn == NULL || pn->type != MINO_SYMBOL) {
                    set_error_at(form, "defmacro parameter must be a symbol");
                    return NULL;
                }
            }
            mac = alloc_val(MINO_MACRO);
            mac->as.fn.params = params;
            mac->as.fn.body   = body;
            mac->as.fn.env    = env;
            n = name_form->as.s.len;
            if (n >= sizeof(buf)) {
                set_error_at(form, "defmacro name too long");
                return NULL;
            }
            memcpy(buf, name_form->as.s.data, n);
            buf[n] = '\0';
            env_bind(env_root(env), buf, mac);
            meta_set(buf, doc, doc_len, form);
            return mac;
        }
        if (sym_eq(head, "def")) {
            mino_val_t *name_form;
            mino_val_t *value_form;
            mino_val_t *value;
            const char *doc     = NULL;
            size_t      doc_len = 0;
            char buf[256];
            size_t n;
            if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
                set_error_at(form, "def requires a name and a value");
                return NULL;
            }
            name_form  = args->as.cons.car;
            if (name_form == NULL || name_form->type != MINO_SYMBOL) {
                set_error_at(form, "def name must be a symbol");
                return NULL;
            }
            /* Optional docstring: (def name "doc" value) */
            if (mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
                mino_val_t *maybe_doc = args->as.cons.cdr->as.cons.car;
                if (maybe_doc != NULL && maybe_doc->type == MINO_STRING) {
                    doc       = maybe_doc->as.s.data;
                    doc_len   = maybe_doc->as.s.len;
                    value_form = args->as.cons.cdr->as.cons.cdr->as.cons.car;
                } else {
                    value_form = args->as.cons.cdr->as.cons.car;
                }
            } else {
                value_form = args->as.cons.cdr->as.cons.car;
            }
            n = name_form->as.s.len;
            if (n >= sizeof(buf)) {
                set_error_at(form, "def name too long");
                return NULL;
            }
            memcpy(buf, name_form->as.s.data, n);
            buf[n] = '\0';
            value = eval_value(value_form, env);
            if (value == NULL) {
                return NULL;
            }
            env_bind(env_root(env), buf, value);
            meta_set(buf, doc, doc_len, form);
            return value;
        }
        if (sym_eq(head, "if")) {
            mino_val_t *cond_form;
            mino_val_t *then_form;
            mino_val_t *else_form = mino_nil();
            mino_val_t *cond;
            if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
                set_error_at(form, "if requires a condition and a then-branch");
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
                set_error_at(form, "let requires a binding list and body");
                return NULL;
            }
            bindings = args->as.cons.car;
            body     = args->as.cons.cdr;
            if (!mino_is_cons(bindings) && !mino_is_nil(bindings)) {
                set_error_at(form, "let bindings must be a list");
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
                    set_error_at(form, "let binding name must be a symbol");
                    return NULL;
                }
                if (!mino_is_cons(rest_pair)) {
                    set_error_at(form, "let binding missing value");
                    return NULL;
                }
                n = name_form->as.s.len;
                if (n >= sizeof(buf)) {
                    set_error_at(form, "let name too long");
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
                set_error_at(form, "fn requires a parameter list");
                return NULL;
            }
            params = args->as.cons.car;
            body   = args->as.cons.cdr;
            if (!mino_is_cons(params) && !mino_is_nil(params)) {
                set_error_at(form, "fn parameter list must be a list");
                return NULL;
            }
            for (p = params; mino_is_cons(p); p = p->as.cons.cdr) {
                mino_val_t *name = p->as.cons.car;
                if (name == NULL || name->type != MINO_SYMBOL) {
                    set_error_at(form, "fn parameter must be a symbol");
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
                set_error_at(form, "loop requires a binding list and body");
                return NULL;
            }
            bindings = args->as.cons.car;
            body     = args->as.cons.cdr;
            if (!mino_is_cons(bindings) && !mino_is_nil(bindings)) {
                set_error_at(form, "loop bindings must be a list");
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
                    set_error_at(form, "loop binding name must be a symbol");
                    return NULL;
                }
                if (!mino_is_cons(rest_pair)) {
                    set_error_at(form, "loop binding missing value");
                    return NULL;
                }
                n = name_form->as.s.len;
                if (n >= sizeof(buf)) {
                    set_error_at(form, "loop name too long");
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

        /*
         * try / catch: (try body (catch e handler...))
         * Script-level exceptions thrown by `throw` are caught; fatal
         * runtime errors (NULL returns without longjmp) propagate to host.
         */
        if (sym_eq(head, "try")) {
            mino_val_t *body_form;
            mino_val_t *catch_clause;
            mino_val_t *catch_var;
            mino_val_t *catch_body;
            char        var_buf[256];
            size_t      var_len;
            int         saved_try   = try_depth;
            int         saved_call  = call_depth;
            int         saved_trace = trace_added;
            if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
                set_error_at(form, "try requires a body and a (catch e ...) clause");
                return NULL;
            }
            body_form    = args->as.cons.car;
            catch_clause = args->as.cons.cdr->as.cons.car;
            /* Parse (catch e handler-body ...) */
            if (!mino_is_cons(catch_clause)
                || !sym_eq(catch_clause->as.cons.car, "catch")
                || !mino_is_cons(catch_clause->as.cons.cdr)) {
                set_error_at(form, "try: second form must be (catch e ...)");
                return NULL;
            }
            catch_var = catch_clause->as.cons.cdr->as.cons.car;
            if (catch_var == NULL || catch_var->type != MINO_SYMBOL) {
                set_error_at(form, "catch binding must be a symbol");
                return NULL;
            }
            var_len = catch_var->as.s.len;
            if (var_len >= sizeof(var_buf)) {
                set_error_at(form, "catch variable name too long");
                return NULL;
            }
            memcpy(var_buf, catch_var->as.s.data, var_len);
            var_buf[var_len] = '\0';
            catch_body = catch_clause->as.cons.cdr->as.cons.cdr;
            if (try_depth >= MAX_TRY_DEPTH) {
                set_error_at(form, "try nesting too deep");
                return NULL;
            }
            try_stack[try_depth].exception = NULL;
            if (setjmp(try_stack[try_depth].buf) == 0) {
                mino_val_t *result;
                try_depth++;
                result = eval(body_form, env);
                try_depth = saved_try;
                if (result == NULL) {
                    /* Fatal runtime error — propagate to host. */
                    return NULL;
                }
                return result;
            } else {
                /* longjmp'd from throw. */
                mino_val_t *ex = try_stack[saved_try].exception;
                mino_env_t *local;
                try_depth   = saved_try;
                call_depth  = saved_call;
                trace_added = saved_trace;
                clear_error();
                local = env_child(env);
                env_bind(local, var_buf, ex != NULL ? ex : mino_nil());
                return eval_implicit_do(catch_body, local);
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
                {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "not a function (got %s)",
                             type_tag_str(fn));
                    set_error_at(form, msg);
                }
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
        const char *file = NULL;
        int         line = 0;
        mino_val_t *result;
        if (eval_current_form != NULL
            && eval_current_form->type == MINO_CONS) {
            file = eval_current_form->as.cons.file;
            line = eval_current_form->as.cons.line;
        }
        push_frame(fn->as.prim.name, file, line);
        result = fn->as.prim.fn(args, env);
        if (result == NULL) {
            return NULL; /* leave frame for trace */
        }
        pop_frame();
        return result;
    }
    if (fn->type == MINO_FN || fn->type == MINO_MACRO) {
        const char *tag       = fn->type == MINO_MACRO ? "macro" : "fn";
        mino_env_t *local     = env_child(fn->as.fn.env);
        mino_val_t *call_args = args;
        const char *file      = NULL;
        int         line      = 0;
        mino_val_t *result;
        if (eval_current_form != NULL
            && eval_current_form->type == MINO_CONS) {
            file = eval_current_form->as.cons.file;
            line = eval_current_form->as.cons.line;
        }
        push_frame(tag, file, line);
        for (;;) {
            if (!bind_params(local, fn->as.fn.params, call_args, tag)) {
                return NULL; /* leave frame for trace */
            }
            result = eval_implicit_do(fn->as.fn.body, local);
            if (result == NULL) {
                return NULL; /* leave frame for trace */
            }
            if (result->type != MINO_RECUR) {
                pop_frame();
                return result;
            }
            call_args = result->as.recur.args;
        }
    }
    {
        char msg[128];
        snprintf(msg, sizeof(msg), "not a function (got %s)",
                 type_tag_str(fn));
        set_error(msg);
    }
    return NULL;
}

mino_val_t *mino_eval(mino_val_t *form, mino_env_t *env)
{
    volatile char probe = 0;
    mino_val_t   *v;
    gc_note_host_frame((void *)&probe);
    (void)probe;
    eval_steps     = 0;
    limit_exceeded = 0;
    trace_added    = 0;
    call_depth     = 0;
    v = eval(form, env);
    if (v == NULL) {
        append_trace();
        call_depth = 0;
        return NULL;
    }
    if (v->type == MINO_RECUR) {
        set_error("recur must be in tail position");
        call_depth = 0;
        return NULL;
    }
    call_depth = 0;
    return v;
}

mino_val_t *mino_eval_string(const char *src, mino_env_t *env)
{
    volatile char   probe = 0;
    mino_val_t     *last  = mino_nil();
    const char     *saved_file = reader_file;
    int             saved_line = reader_line;
    gc_note_host_frame((void *)&probe);
    (void)probe;
    eval_steps     = 0;
    limit_exceeded = 0;
    if (reader_file == NULL) {
        reader_file = intern_filename("<string>");
    }
    reader_line = 1;
    while (*src != '\0') {
        const char *end  = NULL;
        mino_val_t *form = mino_read(src, &end);
        if (form == NULL) {
            if (mino_last_error() != NULL) {
                reader_file = saved_file;
                reader_line = saved_line;
                return NULL;
            }
            break; /* EOF */
        }
        last = mino_eval(form, env);
        if (last == NULL) {
            reader_file = saved_file;
            reader_line = saved_line;
            return NULL;
        }
        src = end;
    }
    reader_file = saved_file;
    reader_line = saved_line;
    return last;
}

mino_val_t *mino_load_file(const char *path, mino_env_t *env)
{
    FILE  *f;
    char  *buf;
    long   sz;
    size_t rd;
    mino_val_t    *result;
    const char    *saved_file;
    if (path == NULL || env == NULL) {
        set_error("mino_load_file: NULL argument");
        return NULL;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        char msg[300];
        snprintf(msg, sizeof(msg), "cannot open file: %s", path);
        set_error(msg);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        set_error("cannot determine file size");
        return NULL;
    }
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(f);
        set_error("out of memory loading file");
        return NULL;
    }
    rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    saved_file  = reader_file;
    reader_file = intern_filename(path);
    result = mino_eval_string(buf, env);
    reader_file = saved_file;
    free(buf);
    return result;
}

mino_env_t *mino_new(void)
{
    mino_env_t *env = mino_env_new();
    mino_install_core(env);
    mino_install_io(env);
    return env;
}

void mino_register_fn(mino_env_t *env, const char *name, mino_prim_fn fn)
{
    mino_env_set(env, name, mino_prim(name, fn));
}

mino_val_t *mino_call(mino_val_t *fn, mino_val_t *args, mino_env_t *env)
{
    volatile char probe = 0;
    gc_note_host_frame((void *)&probe);
    (void)probe;
    return apply_callable(fn, args, env);
}

int mino_pcall(mino_val_t *fn, mino_val_t *args, mino_env_t *env,
               mino_val_t **out)
{
    mino_val_t *result = mino_call(fn, args, env);
    if (out != NULL) {
        *out = result;
    }
    return result == NULL ? -1 : 0;
}

void mino_set_limit(int kind, size_t value)
{
    switch (kind) {
    case MINO_LIMIT_STEPS: limit_steps = value; break;
    case MINO_LIMIT_HEAP:  limit_heap  = value; break;
    default: break;
    }
}

/* ------------------------------------------------------------------------- */
/* In-process REPL handle                                                    */
/* ------------------------------------------------------------------------- */

struct mino_repl {
    mino_env_t *env;
    char       *buf;
    size_t      len;
    size_t      cap;
};

mino_repl_t *mino_repl_new(mino_env_t *env)
{
    mino_repl_t *r = (mino_repl_t *)malloc(sizeof(*r));
    if (r == NULL) { return NULL; }
    r->env = env;
    r->buf = NULL;
    r->len = 0;
    r->cap = 0;
    return r;
}

static int repl_is_whitespace(const char *s)
{
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != ',') {
            return 0;
        }
    }
    return 1;
}

int mino_repl_feed(mino_repl_t *repl, const char *line, mino_val_t **out)
{
    size_t         add;
    const char    *cursor;
    const char    *end;
    mino_val_t    *form;
    mino_val_t    *result;

    if (out != NULL) { *out = NULL; }
    if (repl == NULL) { return MINO_REPL_ERROR; }

    /* Append the line to the buffer. */
    add = (line != NULL) ? strlen(line) : 0;
    if (repl->len + add + 1 > repl->cap) {
        size_t new_cap = repl->cap == 0 ? 256 : repl->cap;
        char  *nb;
        while (new_cap < repl->len + add + 1) { new_cap *= 2; }
        nb = (char *)realloc(repl->buf, new_cap);
        if (nb == NULL) {
            set_error("repl: out of memory");
            return MINO_REPL_ERROR;
        }
        repl->buf = nb;
        repl->cap = new_cap;
    }
    if (add > 0) {
        memcpy(repl->buf + repl->len, line, add);
    }
    repl->len += add;
    repl->buf[repl->len] = '\0';

    /* If buffer is only whitespace, need more input. */
    if (repl_is_whitespace(repl->buf)) {
        return MINO_REPL_MORE;
    }

    /* Try to read a form. */
    cursor = repl->buf;
    end    = repl->buf;
    form   = mino_read(cursor, &end);
    if (form == NULL) {
        const char *err = mino_last_error();
        if (err != NULL && strstr(err, "unterminated") != NULL) {
            return MINO_REPL_MORE;
        }
        /* Hard parse error — reset buffer. */
        repl->len = 0;
        repl->buf[0] = '\0';
        return MINO_REPL_ERROR;
    }

    /* Shift remaining bytes to the front. */
    {
        size_t consumed  = (size_t)(end - repl->buf);
        size_t remaining = repl->len - consumed;
        memmove(repl->buf, end, remaining + 1);
        repl->len = remaining;
    }

    /* Evaluate the form. */
    result = mino_eval(form, repl->env);
    if (result == NULL) {
        return MINO_REPL_ERROR;
    }
    if (out != NULL) { *out = result; }
    return MINO_REPL_OK;
}

void mino_repl_free(mino_repl_t *repl)
{
    if (repl == NULL) { return; }
    free(repl->buf);
    free(repl);
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

/* ------------------------------------------------------------------------- */
/* Collection primitives                                                     */
/*                                                                           */
/* All collection ops treat values as immutable: every operation that        */
/* "modifies" a collection returns a freshly allocated value. v0.3 uses      */
/* naïve array-backed representations; persistent tries arrive in v0.4/v0.5 */
/* without changing the public primitive contracts.                          */
/* ------------------------------------------------------------------------- */

static mino_val_t *set_conj1(const mino_val_t *s, mino_val_t *elem);
static mino_val_t *prim_str(mino_val_t *args, mino_env_t *env);

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
    case MINO_SET:    return mino_int((long long)coll->as.set.len);
    case MINO_STRING: return mino_int((long long)coll->as.s.len);
    default:
        {
            char msg[96];
            snprintf(msg, sizeof(msg), "count: expected a collection, got %s",
                     type_tag_str(coll));
            set_error(msg);
        }
        return NULL;
    }
}

static mino_val_t *prim_vector(mino_val_t *args, mino_env_t *env)
{
    size_t n;
    size_t i;
    mino_val_t **tmp;
    mino_val_t *p;
    (void)env;
    arg_count(args, &n);
    if (n == 0) {
        return mino_vector(NULL, 0);
    }
    /* GC_T_VALARR keeps partially-gathered pointers visible to the collector;
     * without this, the optimizer may drop the `args` parameter and the cons
     * cells holding the element values become unreachable mid-construction. */
    tmp = (mino_val_t **)gc_alloc_typed(GC_T_VALARR, n * sizeof(*tmp));
    p = args;
    for (i = 0; i < n; i++) {
        tmp[i] = p->as.cons.car;
        p = p->as.cons.cdr;
    }
    return mino_vector(tmp, n);
}

static mino_val_t *prim_hash_map(mino_val_t *args, mino_env_t *env)
{
    size_t n;
    size_t pairs;
    size_t i;
    mino_val_t **ks;
    mino_val_t **vs;
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
    ks = (mino_val_t **)gc_alloc_typed(GC_T_VALARR, pairs * sizeof(*ks));
    vs = (mino_val_t **)gc_alloc_typed(GC_T_VALARR, pairs * sizeof(*vs));
    p = args;
    for (i = 0; i < pairs; i++) {
        ks[i] = p->as.cons.car;
        p = p->as.cons.cdr;
        vs[i] = p->as.cons.car;
        p = p->as.cons.cdr;
    }
    return mino_map(ks, vs, pairs);
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
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "nth: expected a list, vector, or string, got %s",
                 type_tag_str(coll));
        set_error(msg);
    }
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
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "first: expected a list or vector, got %s",
                 type_tag_str(coll));
        set_error(msg);
    }
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
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "rest: expected a list or vector, got %s",
                 type_tag_str(coll));
        set_error(msg);
    }
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
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "assoc: expected a map or vector, got %s",
                 type_tag_str(coll));
        set_error(msg);
    }
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
    if (coll->type == MINO_SET) {
        uint32_t h = hash_val(key);
        mino_val_t *found = hamt_get(coll->as.set.root, key, h, 0u);
        return found != NULL ? key : def_val;
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
    if (coll->type == MINO_SET) {
        mino_val_t *acc = coll;
        while (mino_is_cons(p)) {
            acc = set_conj1(acc, p->as.cons.car);
            p = p->as.cons.cdr;
        }
        return acc;
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "conj: expected a list, vector, map, or set, got %s",
                 type_tag_str(coll));
        set_error(msg);
    }
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
        {
            char msg[96];
            snprintf(msg, sizeof(msg), "update: expected a map, got %s",
                     type_tag_str(coll));
            set_error(msg);
        }
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

/* Set helper: add one element to a set, returning a new set. */
static mino_val_t *set_conj1(const mino_val_t *s, mino_val_t *elem)
{
    mino_val_t       *v        = alloc_val(MINO_SET);
    mino_val_t       *sentinel = mino_true();
    hamt_entry_t     *e        = hamt_entry_new(elem, sentinel);
    uint32_t          h        = hash_val(elem);
    int               replaced = 0;
    mino_hamt_node_t *root     = hamt_assoc(s->as.set.root, e, h, 0u, &replaced);
    v->as.set.root      = root;
    if (replaced) {
        v->as.set.key_order = s->as.set.key_order;
        v->as.set.len       = s->as.set.len;
    } else {
        v->as.set.key_order = vec_conj1(s->as.set.key_order, elem);
        v->as.set.len       = s->as.set.len + 1;
    }
    return v;
}

static mino_val_t *prim_hash_set(mino_val_t *args, mino_env_t *env)
{
    size_t      n;
    size_t      i;
    mino_val_t **tmp;
    mino_val_t *p;
    (void)env;
    arg_count(args, &n);
    tmp = (mino_val_t **)gc_alloc_typed(GC_T_VALARR, (n > 0 ? n : 1) * sizeof(*tmp));
    p = args;
    for (i = 0; i < n; i++) {
        tmp[i] = p->as.cons.car;
        p = p->as.cons.cdr;
    }
    return mino_set(tmp, n);
}

static mino_val_t *prim_contains_p(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *key;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("contains? requires two arguments");
        return NULL;
    }
    coll = args->as.cons.car;
    key  = args->as.cons.cdr->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_false();
    }
    if (coll->type == MINO_MAP) {
        return map_get_val(coll, key) != NULL ? mino_true() : mino_false();
    }
    if (coll->type == MINO_SET) {
        uint32_t h = hash_val(key);
        return hamt_get(coll->as.set.root, key, h, 0u) != NULL
            ? mino_true() : mino_false();
    }
    if (coll->type == MINO_VECTOR) {
        /* For vectors, key is an index. */
        if (key != NULL && key->type == MINO_INT) {
            long long idx = key->as.i;
            return (idx >= 0 && (size_t)idx < coll->as.vec.len)
                ? mino_true() : mino_false();
        }
        return mino_false();
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "contains?: expected a map, set, or vector, got %s",
                 type_tag_str(coll));
        set_error(msg);
    }
    return NULL;
}

static mino_val_t *prim_disj(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *p;
    size_t      n;
    (void)env;
    arg_count(args, &n);
    if (n < 2) {
        set_error("disj requires a set and at least one key");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil();
    }
    if (coll->type != MINO_SET) {
        set_error("disj: first argument must be a set");
        return NULL;
    }
    /* Rebuild set excluding the specified elements. Not the most efficient
     * approach, but keeps the code simple and correct. */
    p = args->as.cons.cdr;
    while (mino_is_cons(p)) {
        mino_val_t *key = p->as.cons.car;
        uint32_t    h   = hash_val(key);
        if (hamt_get(coll->as.set.root, key, h, 0u) != NULL) {
            /* Element exists; rebuild without it. */
            mino_val_t *new_set = alloc_val(MINO_SET);
            mino_val_t *order   = mino_vector(NULL, 0);
            mino_hamt_node_t *root = NULL;
            size_t i;
            size_t new_len = 0;
            for (i = 0; i < coll->as.set.len; i++) {
                mino_val_t *elem = vec_nth(coll->as.set.key_order, i);
                if (!mino_eq(elem, key)) {
                    hamt_entry_t *e2 = hamt_entry_new(elem, mino_true());
                    uint32_t h2 = hash_val(elem);
                    int rep = 0;
                    root = hamt_assoc(root, e2, h2, 0u, &rep);
                    order = vec_conj1(order, elem);
                    new_len++;
                }
            }
            new_set->as.set.root      = root;
            new_set->as.set.key_order = order;
            new_set->as.set.len       = new_len;
            coll = new_set;
        }
        p = p->as.cons.cdr;
    }
    return coll;
}

/* ------------------------------------------------------------------------- */
/* Sequence primitives (strict — no lazy seqs)                               */
/* ------------------------------------------------------------------------- */

/*
 * Helper: build a freshly consed list from a collection. Works on lists,
 * vectors, maps (key-value vectors), and sets. Returns a (head, tail) pair
 * through pointers so the caller can append efficiently.
 */

/* Iterator abstraction over any sequential collection. */
typedef struct {
    const mino_val_t *coll;
    size_t            idx;       /* for vectors, maps, sets */
    const mino_val_t *cons_p;   /* for cons lists */
} seq_iter_t;

static void seq_iter_init(seq_iter_t *it, const mino_val_t *coll)
{
    it->coll  = coll;
    it->idx   = 0;
    it->cons_p = (coll != NULL && coll->type == MINO_CONS) ? coll : NULL;
}

static int seq_iter_done(const seq_iter_t *it)
{
    const mino_val_t *c = it->coll;
    if (c == NULL || c->type == MINO_NIL) return 1;
    switch (c->type) {
    case MINO_CONS:   return it->cons_p == NULL || it->cons_p->type != MINO_CONS;
    case MINO_VECTOR: return it->idx >= c->as.vec.len;
    case MINO_MAP:    return it->idx >= c->as.map.len;
    case MINO_SET:    return it->idx >= c->as.set.len;
    case MINO_STRING: return it->idx >= c->as.s.len;
    default:          return 1;
    }
}

static mino_val_t *seq_iter_val(const seq_iter_t *it)
{
    const mino_val_t *c = it->coll;
    switch (c->type) {
    case MINO_CONS:   return it->cons_p->as.cons.car;
    case MINO_VECTOR: return vec_nth(c, it->idx);
    case MINO_MAP: {
        /* Yield [key value] vectors for maps. */
        mino_val_t *key = vec_nth(c->as.map.key_order, it->idx);
        mino_val_t *val = map_get_val(c, key);
        mino_val_t *kv[2];
        kv[0] = key;
        kv[1] = val;
        return mino_vector(kv, 2);
    }
    case MINO_SET:    return vec_nth(c->as.set.key_order, it->idx);
    case MINO_STRING: return mino_string_n(c->as.s.data + it->idx, 1);
    default:          return mino_nil();
    }
}

static void seq_iter_next(seq_iter_t *it)
{
    if (it->coll != NULL && it->coll->type == MINO_CONS) {
        if (it->cons_p != NULL && it->cons_p->type == MINO_CONS) {
            it->cons_p = it->cons_p->as.cons.cdr;
        }
    } else {
        it->idx++;
    }
}

static mino_val_t *prim_map(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *fn;
    mino_val_t *coll;
    mino_val_t *head = mino_nil();
    mino_val_t *tail = NULL;
    seq_iter_t  it;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error("map requires a function and a collection");
        return NULL;
    }
    fn   = args->as.cons.car;
    coll = args->as.cons.cdr->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil();
    }
    seq_iter_init(&it, coll);
    while (!seq_iter_done(&it)) {
        mino_val_t *elem    = seq_iter_val(&it);
        mino_val_t *call_a  = mino_cons(elem, mino_nil());
        mino_val_t *result  = apply_callable(fn, call_a, env);
        mino_val_t *cell;
        if (result == NULL) return NULL;
        cell = mino_cons(result, mino_nil());
        if (tail == NULL) { head = cell; } else { tail->as.cons.cdr = cell; }
        tail = cell;
        seq_iter_next(&it);
    }
    return head;
}

static mino_val_t *prim_filter(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *pred;
    mino_val_t *coll;
    mino_val_t *head = mino_nil();
    mino_val_t *tail = NULL;
    seq_iter_t  it;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error("filter requires a predicate and a collection");
        return NULL;
    }
    pred = args->as.cons.car;
    coll = args->as.cons.cdr->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil();
    }
    seq_iter_init(&it, coll);
    while (!seq_iter_done(&it)) {
        mino_val_t *elem   = seq_iter_val(&it);
        mino_val_t *call_a = mino_cons(elem, mino_nil());
        mino_val_t *result = apply_callable(pred, call_a, env);
        if (result == NULL) return NULL;
        if (mino_is_truthy(result)) {
            mino_val_t *cell = mino_cons(elem, mino_nil());
            if (tail == NULL) { head = cell; } else { tail->as.cons.cdr = cell; }
            tail = cell;
        }
        seq_iter_next(&it);
    }
    return head;
}

static mino_val_t *prim_reduce(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *fn;
    mino_val_t *acc;
    mino_val_t *coll;
    seq_iter_t  it;
    size_t      n;
    arg_count(args, &n);
    if (n == 2) {
        /* (reduce f coll) — first element is the initial accumulator. */
        fn   = args->as.cons.car;
        coll = args->as.cons.cdr->as.cons.car;
        if (coll == NULL || coll->type == MINO_NIL) {
            /* (reduce f nil) → (f) */
            return apply_callable(fn, mino_nil(), env);
        }
        seq_iter_init(&it, coll);
        if (seq_iter_done(&it)) {
            return apply_callable(fn, mino_nil(), env);
        }
        acc = seq_iter_val(&it);
        seq_iter_next(&it);
    } else if (n == 3) {
        /* (reduce f init coll) */
        fn   = args->as.cons.car;
        acc  = args->as.cons.cdr->as.cons.car;
        coll = args->as.cons.cdr->as.cons.cdr->as.cons.car;
        if (coll == NULL || coll->type == MINO_NIL) {
            return acc;
        }
        seq_iter_init(&it, coll);
    } else {
        set_error("reduce requires 2 or 3 arguments");
        return NULL;
    }
    while (!seq_iter_done(&it)) {
        mino_val_t *elem   = seq_iter_val(&it);
        mino_val_t *call_a = mino_cons(acc, mino_cons(elem, mino_nil()));
        acc = apply_callable(fn, call_a, env);
        if (acc == NULL) return NULL;
        seq_iter_next(&it);
    }
    return acc;
}

static mino_val_t *prim_take(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *head = mino_nil();
    mino_val_t *tail = NULL;
    long long   n_take;
    long long   i = 0;
    seq_iter_t  it;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error("take requires a count and a collection");
        return NULL;
    }
    if (args->as.cons.car == NULL || args->as.cons.car->type != MINO_INT) {
        set_error("take: first argument must be an integer");
        return NULL;
    }
    n_take = args->as.cons.car->as.i;
    coll   = args->as.cons.cdr->as.cons.car;
    if (n_take <= 0 || coll == NULL || coll->type == MINO_NIL) {
        return mino_nil();
    }
    seq_iter_init(&it, coll);
    while (!seq_iter_done(&it) && i < n_take) {
        mino_val_t *elem = seq_iter_val(&it);
        mino_val_t *cell = mino_cons(elem, mino_nil());
        if (tail == NULL) { head = cell; } else { tail->as.cons.cdr = cell; }
        tail = cell;
        seq_iter_next(&it);
        i++;
    }
    return head;
}

static mino_val_t *prim_drop(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *head = mino_nil();
    mino_val_t *tail = NULL;
    long long   n_drop;
    long long   i = 0;
    seq_iter_t  it;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error("drop requires a count and a collection");
        return NULL;
    }
    if (args->as.cons.car == NULL || args->as.cons.car->type != MINO_INT) {
        set_error("drop: first argument must be an integer");
        return NULL;
    }
    n_drop = args->as.cons.car->as.i;
    coll   = args->as.cons.cdr->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil();
    }
    seq_iter_init(&it, coll);
    while (!seq_iter_done(&it) && i < n_drop) {
        seq_iter_next(&it);
        i++;
    }
    while (!seq_iter_done(&it)) {
        mino_val_t *elem = seq_iter_val(&it);
        mino_val_t *cell = mino_cons(elem, mino_nil());
        if (tail == NULL) { head = cell; } else { tail->as.cons.cdr = cell; }
        tail = cell;
        seq_iter_next(&it);
    }
    return head;
}

static mino_val_t *prim_range(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *head = mino_nil();
    mino_val_t *tail = NULL;
    long long   start = 0, end_val, step = 1;
    long long   i;
    size_t      n;
    (void)env;
    arg_count(args, &n);
    if (n == 0) {
        set_error("range requires at least one argument");
        return NULL;
    }
    if (n == 1) {
        if (args->as.cons.car == NULL || args->as.cons.car->type != MINO_INT) {
            set_error("range: arguments must be integers");
            return NULL;
        }
        end_val = args->as.cons.car->as.i;
    } else if (n == 2) {
        if (args->as.cons.car == NULL || args->as.cons.car->type != MINO_INT
            || args->as.cons.cdr->as.cons.car == NULL
            || args->as.cons.cdr->as.cons.car->type != MINO_INT) {
            set_error("range: arguments must be integers");
            return NULL;
        }
        start   = args->as.cons.car->as.i;
        end_val = args->as.cons.cdr->as.cons.car->as.i;
    } else if (n == 3) {
        if (args->as.cons.car == NULL || args->as.cons.car->type != MINO_INT
            || args->as.cons.cdr->as.cons.car == NULL
            || args->as.cons.cdr->as.cons.car->type != MINO_INT
            || args->as.cons.cdr->as.cons.cdr->as.cons.car == NULL
            || args->as.cons.cdr->as.cons.cdr->as.cons.car->type != MINO_INT) {
            set_error("range: arguments must be integers");
            return NULL;
        }
        start   = args->as.cons.car->as.i;
        end_val = args->as.cons.cdr->as.cons.car->as.i;
        step    = args->as.cons.cdr->as.cons.cdr->as.cons.car->as.i;
    } else {
        set_error("range requires 1, 2, or 3 arguments");
        return NULL;
    }
    if (step == 0) {
        set_error("range: step cannot be zero");
        return NULL;
    }
    for (i = start; step > 0 ? i < end_val : i > end_val; i += step) {
        mino_val_t *cell = mino_cons(mino_int(i), mino_nil());
        if (tail == NULL) { head = cell; } else { tail->as.cons.cdr = cell; }
        tail = cell;
    }
    return head;
}

static mino_val_t *prim_repeat(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *head = mino_nil();
    mino_val_t *tail = NULL;
    long long   count;
    mino_val_t *val;
    long long   i;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error("repeat requires a count and a value");
        return NULL;
    }
    if (args->as.cons.car == NULL || args->as.cons.car->type != MINO_INT) {
        set_error("repeat: first argument must be an integer");
        return NULL;
    }
    count = args->as.cons.car->as.i;
    val   = args->as.cons.cdr->as.cons.car;
    for (i = 0; i < count; i++) {
        mino_val_t *cell = mino_cons(val, mino_nil());
        if (tail == NULL) { head = cell; } else { tail->as.cons.cdr = cell; }
        tail = cell;
    }
    return head;
}

static mino_val_t *prim_concat(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *head = mino_nil();
    mino_val_t *tail = NULL;
    (void)env;
    while (mino_is_cons(args)) {
        mino_val_t *coll = args->as.cons.car;
        seq_iter_t  it;
        if (coll != NULL && coll->type != MINO_NIL) {
            seq_iter_init(&it, coll);
            while (!seq_iter_done(&it)) {
                mino_val_t *elem = seq_iter_val(&it);
                mino_val_t *cell = mino_cons(elem, mino_nil());
                if (tail == NULL) { head = cell; }
                else              { tail->as.cons.cdr = cell; }
                tail = cell;
                seq_iter_next(&it);
            }
        }
        args = args->as.cons.cdr;
    }
    return head;
}

static mino_val_t *prim_into(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *to;
    mino_val_t *from;
    seq_iter_t  it;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error("into requires two arguments");
        return NULL;
    }
    to   = args->as.cons.car;
    from = args->as.cons.cdr->as.cons.car;
    if (from == NULL || from->type == MINO_NIL) {
        return to;
    }
    /* Conj each element of `from` into `to`. The type of `to` determines
     * the conj semantics (vector appends, list prepends, map/set merges). */
    if (to == NULL || to->type == MINO_NIL) {
        /* Into nil: build a list. */
        mino_val_t *out = mino_nil();
        seq_iter_init(&it, from);
        while (!seq_iter_done(&it)) {
            out = mino_cons(seq_iter_val(&it), out);
            seq_iter_next(&it);
        }
        return out;
    }
    if (to->type == MINO_VECTOR) {
        mino_val_t *acc = to;
        seq_iter_init(&it, from);
        while (!seq_iter_done(&it)) {
            acc = vec_conj1(acc, seq_iter_val(&it));
            seq_iter_next(&it);
        }
        return acc;
    }
    if (to->type == MINO_MAP) {
        mino_val_t *acc = to;
        seq_iter_init(&it, from);
        while (!seq_iter_done(&it)) {
            mino_val_t *item = seq_iter_val(&it);
            mino_val_t *pair_args;
            if (item == NULL || item->type != MINO_VECTOR
                || item->as.vec.len != 2) {
                set_error("into map: each element must be a 2-element vector");
                return NULL;
            }
            pair_args = mino_cons(vec_nth(item, 0),
                                   mino_cons(vec_nth(item, 1), mino_nil()));
            acc = map_assoc_pairs(acc, pair_args, 1);
            seq_iter_next(&it);
        }
        return acc;
    }
    if (to->type == MINO_SET) {
        mino_val_t *acc = to;
        seq_iter_init(&it, from);
        while (!seq_iter_done(&it)) {
            acc = set_conj1(acc, seq_iter_val(&it));
            seq_iter_next(&it);
        }
        return acc;
    }
    if (to->type == MINO_CONS) {
        mino_val_t *out = to;
        seq_iter_init(&it, from);
        while (!seq_iter_done(&it)) {
            out = mino_cons(seq_iter_val(&it), out);
            seq_iter_next(&it);
        }
        return out;
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "into: expected a list, vector, map, or set as target, got %s",
                 type_tag_str(to));
        set_error(msg);
    }
    return NULL;
}

static mino_val_t *prim_apply(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *fn;
    mino_val_t *last;
    mino_val_t *call_args;
    mino_val_t *p;
    size_t      n;
    arg_count(args, &n);
    if (n < 2) {
        set_error("apply requires a function and arguments");
        return NULL;
    }
    fn = args->as.cons.car;
    if (n == 2) {
        /* (apply f coll) — spread coll as args. */
        last = args->as.cons.cdr->as.cons.car;
    } else {
        /* (apply f a b ... coll) — prepend individual args, then spread coll. */
        mino_val_t *head = mino_nil();
        mino_val_t *tail2 = NULL;
        p = args->as.cons.cdr;
        /* Collect all but the last arg as individual args. */
        while (mino_is_cons(p) && mino_is_cons(p->as.cons.cdr)) {
            mino_val_t *cell = mino_cons(p->as.cons.car, mino_nil());
            if (tail2 == NULL) { head = cell; } else { tail2->as.cons.cdr = cell; }
            tail2 = cell;
            p = p->as.cons.cdr;
        }
        last = p->as.cons.car; /* the final collection argument */
        /* Append elements from `last` collection. */
        if (last != NULL && last->type != MINO_NIL) {
            seq_iter_t it;
            seq_iter_init(&it, last);
            while (!seq_iter_done(&it)) {
                mino_val_t *cell = mino_cons(seq_iter_val(&it), mino_nil());
                if (tail2 == NULL) { head = cell; } else { tail2->as.cons.cdr = cell; }
                tail2 = cell;
                seq_iter_next(&it);
            }
        }
        return apply_callable(fn, head, env);
    }
    /* (apply f coll) — convert coll to a cons arg list. */
    if (last == NULL || last->type == MINO_NIL) {
        return apply_callable(fn, mino_nil(), env);
    }
    if (last->type == MINO_CONS) {
        return apply_callable(fn, last, env);
    }
    /* Convert non-list collection to cons list. */
    {
        mino_val_t *head = mino_nil();
        mino_val_t *tail2 = NULL;
        seq_iter_t it;
        seq_iter_init(&it, last);
        while (!seq_iter_done(&it)) {
            mino_val_t *cell = mino_cons(seq_iter_val(&it), mino_nil());
            if (tail2 == NULL) { head = cell; } else { tail2->as.cons.cdr = cell; }
            tail2 = cell;
            seq_iter_next(&it);
        }
        call_args = head;
    }
    return apply_callable(fn, call_args, env);
}

static mino_val_t *prim_reverse(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *out = mino_nil();
    seq_iter_t  it;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("reverse requires one argument");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil();
    }
    seq_iter_init(&it, coll);
    while (!seq_iter_done(&it)) {
        out = mino_cons(seq_iter_val(&it), out);
        seq_iter_next(&it);
    }
    return out;
}

static mino_val_t *prim_sort(mino_val_t *args, mino_env_t *env);

/* Simple comparison function for sorting: numbers by value, strings
 * lexicographically, other types by type tag then identity. */
static int val_compare(const mino_val_t *a, const mino_val_t *b)
{
    if (a == b) return 0;
    if (a == NULL || a->type == MINO_NIL) return -1;
    if (b == NULL || b->type == MINO_NIL) return 1;
    if (a->type == MINO_INT && b->type == MINO_INT) {
        return a->as.i < b->as.i ? -1 : a->as.i > b->as.i ? 1 : 0;
    }
    if (a->type == MINO_FLOAT && b->type == MINO_FLOAT) {
        return a->as.f < b->as.f ? -1 : a->as.f > b->as.f ? 1 : 0;
    }
    if (a->type == MINO_INT && b->type == MINO_FLOAT) {
        double da = (double)a->as.i;
        return da < b->as.f ? -1 : da > b->as.f ? 1 : 0;
    }
    if (a->type == MINO_FLOAT && b->type == MINO_INT) {
        double db = (double)b->as.i;
        return a->as.f < db ? -1 : a->as.f > db ? 1 : 0;
    }
    if ((a->type == MINO_STRING || a->type == MINO_SYMBOL || a->type == MINO_KEYWORD)
        && a->type == b->type) {
        size_t min_len = a->as.s.len < b->as.s.len ? a->as.s.len : b->as.s.len;
        int c = memcmp(a->as.s.data, b->as.s.data, min_len);
        if (c != 0) return c;
        return a->as.s.len < b->as.s.len ? -1 : a->as.s.len > b->as.s.len ? 1 : 0;
    }
    /* Fall back to type tag ordering. */
    return a->type < b->type ? -1 : a->type > b->type ? 1 : 0;
}

/* Merge sort for mino_val_t* arrays. */
static void merge_sort_vals(mino_val_t **arr, mino_val_t **tmp, size_t len)
{
    size_t mid, i, j, k;
    if (len <= 1) return;
    mid = len / 2;
    merge_sort_vals(arr, tmp, mid);
    merge_sort_vals(arr + mid, tmp, len - mid);
    memcpy(tmp, arr, mid * sizeof(*tmp));
    i = 0; j = mid; k = 0;
    while (i < mid && j < len) {
        if (val_compare(tmp[i], arr[j]) <= 0) {
            arr[k++] = tmp[i++];
        } else {
            arr[k++] = arr[j++];
        }
    }
    while (i < mid) { arr[k++] = tmp[i++]; }
}

static mino_val_t *prim_sort(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t **arr;
    mino_val_t **tmp;
    mino_val_t *head = mino_nil();
    mino_val_t *tail = NULL;
    size_t      n_items, i;
    seq_iter_t  it;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("sort requires one argument");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil();
    }
    /* Collect elements into an array. */
    n_items = 0;
    seq_iter_init(&it, coll);
    while (!seq_iter_done(&it)) { n_items++; seq_iter_next(&it); }
    if (n_items == 0) return mino_nil();
    arr = (mino_val_t **)gc_alloc_typed(GC_T_VALARR, n_items * sizeof(*arr));
    tmp = (mino_val_t **)gc_alloc_typed(GC_T_VALARR, n_items * sizeof(*tmp));
    i = 0;
    seq_iter_init(&it, coll);
    while (!seq_iter_done(&it)) { arr[i++] = seq_iter_val(&it); seq_iter_next(&it); }
    merge_sort_vals(arr, tmp, n_items);
    for (i = 0; i < n_items; i++) {
        mino_val_t *cell = mino_cons(arr[i], mino_nil());
        if (tail == NULL) { head = cell; } else { tail->as.cons.cdr = cell; }
        tail = cell;
    }
    return head;
}

/* ------------------------------------------------------------------------- */
/* String primitives                                                         */
/* ------------------------------------------------------------------------- */

static mino_val_t *prim_subs(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s_val;
    long long   start, end_idx;
    size_t      n;
    (void)env;
    arg_count(args, &n);
    if (n != 2 && n != 3) {
        set_error("subs requires 2 or 3 arguments");
        return NULL;
    }
    s_val = args->as.cons.car;
    if (s_val == NULL || s_val->type != MINO_STRING) {
        set_error("subs: first argument must be a string");
        return NULL;
    }
    if (args->as.cons.cdr->as.cons.car == NULL
        || args->as.cons.cdr->as.cons.car->type != MINO_INT) {
        set_error("subs: start index must be an integer");
        return NULL;
    }
    start = args->as.cons.cdr->as.cons.car->as.i;
    if (n == 3) {
        if (args->as.cons.cdr->as.cons.cdr->as.cons.car == NULL
            || args->as.cons.cdr->as.cons.cdr->as.cons.car->type != MINO_INT) {
            set_error("subs: end index must be an integer");
            return NULL;
        }
        end_idx = args->as.cons.cdr->as.cons.cdr->as.cons.car->as.i;
    } else {
        end_idx = (long long)s_val->as.s.len;
    }
    if (start < 0 || end_idx < start || (size_t)end_idx > s_val->as.s.len) {
        set_error("subs: index out of range");
        return NULL;
    }
    return mino_string_n(s_val->as.s.data + start, (size_t)(end_idx - start));
}

static mino_val_t *prim_split(mino_val_t *args, mino_env_t *env)
{
    mino_val_t  *s_val;
    mino_val_t  *sep_val;
    const char  *s;
    size_t       slen;
    const char  *sep;
    size_t       sep_len;
    mino_val_t **buf = NULL;
    size_t       cap = 0, len = 0;
    const char  *p;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error("split requires a string and a separator");
        return NULL;
    }
    s_val   = args->as.cons.car;
    sep_val = args->as.cons.cdr->as.cons.car;
    if (s_val == NULL || s_val->type != MINO_STRING
        || sep_val == NULL || sep_val->type != MINO_STRING) {
        set_error("split: both arguments must be strings");
        return NULL;
    }
    s       = s_val->as.s.data;
    slen    = s_val->as.s.len;
    sep     = sep_val->as.s.data;
    sep_len = sep_val->as.s.len;
    p       = s;
    if (sep_len == 0) {
        /* Split into individual characters. */
        size_t i;
        buf = (mino_val_t **)gc_alloc_typed(GC_T_VALARR,
              (slen > 0 ? slen : 1) * sizeof(*buf));
        for (i = 0; i < slen; i++) {
            buf[i] = mino_string_n(s + i, 1);
        }
        return mino_vector(buf, slen);
    }
    while (p <= s + slen) {
        const char *found = NULL;
        const char *q;
        for (q = p; q + sep_len <= s + slen; q++) {
            if (memcmp(q, sep, sep_len) == 0) {
                found = q;
                break;
            }
        }
        if (len == cap) {
            size_t new_cap = cap == 0 ? 8 : cap * 2;
            mino_val_t **nb = (mino_val_t **)gc_alloc_typed(
                GC_T_VALARR, new_cap * sizeof(*nb));
            if (buf != NULL && len > 0) memcpy(nb, buf, len * sizeof(*nb));
            buf = nb;
            cap = new_cap;
        }
        if (found != NULL) {
            buf[len++] = mino_string_n(p, (size_t)(found - p));
            p = found + sep_len;
        } else {
            buf[len++] = mino_string_n(p, (size_t)(s + slen - p));
            break;
        }
    }
    return mino_vector(buf, len);
}

static mino_val_t *prim_join(mino_val_t *args, mino_env_t *env)
{
    mino_val_t  *sep_val;
    mino_val_t  *coll;
    const char  *sep = "";
    size_t       sep_len = 0;
    char        *buf = NULL;
    size_t       buf_len = 0, buf_cap = 0;
    int          first = 1;
    seq_iter_t   it;
    size_t       n;
    (void)env;
    arg_count(args, &n);
    if (n == 1) {
        /* (join coll) — no separator. */
        coll = args->as.cons.car;
    } else if (n == 2) {
        /* (join sep coll) */
        sep_val = args->as.cons.car;
        coll    = args->as.cons.cdr->as.cons.car;
        if (sep_val == NULL || sep_val->type != MINO_STRING) {
            set_error("join: separator must be a string");
            return NULL;
        }
        sep     = sep_val->as.s.data;
        sep_len = sep_val->as.s.len;
    } else {
        set_error("join requires 1 or 2 arguments");
        return NULL;
    }
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_string("");
    }
    seq_iter_init(&it, coll);
    while (!seq_iter_done(&it)) {
        mino_val_t *elem = seq_iter_val(&it);
        const char *part;
        size_t      part_len;
        size_t      need;
        if (elem == NULL || elem->type == MINO_NIL) {
            seq_iter_next(&it);
            continue;
        }
        if (elem->type == MINO_STRING) {
            part     = elem->as.s.data;
            part_len = elem->as.s.len;
        } else {
            /* Convert to string. */
            mino_val_t *str_a = mino_cons(elem, mino_nil());
            mino_val_t *str   = prim_str(str_a, env);
            if (str == NULL) return NULL;
            part     = str->as.s.data;
            part_len = str->as.s.len;
        }
        need = buf_len + (first ? 0 : sep_len) + part_len + 1;
        if (need > buf_cap) {
            buf_cap = buf_cap == 0 ? 128 : buf_cap;
            while (buf_cap < need) buf_cap *= 2;
            buf = (char *)realloc(buf, buf_cap);
            if (buf == NULL) { set_error("out of memory"); return NULL; }
        }
        if (!first && sep_len > 0) {
            memcpy(buf + buf_len, sep, sep_len);
            buf_len += sep_len;
        }
        memcpy(buf + buf_len, part, part_len);
        buf_len += part_len;
        first = 0;
        seq_iter_next(&it);
    }
    {
        mino_val_t *result = mino_string_n(buf != NULL ? buf : "", buf_len);
        free(buf);
        return result;
    }
}

static mino_val_t *prim_starts_with_p(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s, *prefix;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error("starts-with? requires two string arguments");
        return NULL;
    }
    s      = args->as.cons.car;
    prefix = args->as.cons.cdr->as.cons.car;
    if (s == NULL || s->type != MINO_STRING
        || prefix == NULL || prefix->type != MINO_STRING) {
        set_error("starts-with? requires two string arguments");
        return NULL;
    }
    if (prefix->as.s.len > s->as.s.len) return mino_false();
    return memcmp(s->as.s.data, prefix->as.s.data, prefix->as.s.len) == 0
        ? mino_true() : mino_false();
}

static mino_val_t *prim_ends_with_p(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s, *suffix;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error("ends-with? requires two string arguments");
        return NULL;
    }
    s      = args->as.cons.car;
    suffix = args->as.cons.cdr->as.cons.car;
    if (s == NULL || s->type != MINO_STRING
        || suffix == NULL || suffix->type != MINO_STRING) {
        set_error("ends-with? requires two string arguments");
        return NULL;
    }
    if (suffix->as.s.len > s->as.s.len) return mino_false();
    return memcmp(s->as.s.data + s->as.s.len - suffix->as.s.len,
                  suffix->as.s.data, suffix->as.s.len) == 0
        ? mino_true() : mino_false();
}

static mino_val_t *prim_includes_p(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s, *sub;
    const char *p;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error("includes? requires two string arguments");
        return NULL;
    }
    s   = args->as.cons.car;
    sub = args->as.cons.cdr->as.cons.car;
    if (s == NULL || s->type != MINO_STRING
        || sub == NULL || sub->type != MINO_STRING) {
        set_error("includes? requires two string arguments");
        return NULL;
    }
    if (sub->as.s.len == 0) return mino_true();
    if (sub->as.s.len > s->as.s.len) return mino_false();
    for (p = s->as.s.data; p + sub->as.s.len <= s->as.s.data + s->as.s.len; p++) {
        if (memcmp(p, sub->as.s.data, sub->as.s.len) == 0) {
            return mino_true();
        }
    }
    return mino_false();
}

static mino_val_t *prim_upper_case(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s;
    char       *buf;
    size_t      i;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("upper-case requires one string argument");
        return NULL;
    }
    s = args->as.cons.car;
    if (s == NULL || s->type != MINO_STRING) {
        set_error("upper-case requires one string argument");
        return NULL;
    }
    buf = (char *)malloc(s->as.s.len);
    if (buf == NULL && s->as.s.len > 0) {
        set_error("out of memory"); return NULL;
    }
    for (i = 0; i < s->as.s.len; i++) {
        buf[i] = (char)toupper((unsigned char)s->as.s.data[i]);
    }
    {
        mino_val_t *result = mino_string_n(buf, s->as.s.len);
        free(buf);
        return result;
    }
}

static mino_val_t *prim_lower_case(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s;
    char       *buf;
    size_t      i;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("lower-case requires one string argument");
        return NULL;
    }
    s = args->as.cons.car;
    if (s == NULL || s->type != MINO_STRING) {
        set_error("lower-case requires one string argument");
        return NULL;
    }
    buf = (char *)malloc(s->as.s.len);
    if (buf == NULL && s->as.s.len > 0) {
        set_error("out of memory"); return NULL;
    }
    for (i = 0; i < s->as.s.len; i++) {
        buf[i] = (char)tolower((unsigned char)s->as.s.data[i]);
    }
    {
        mino_val_t *result = mino_string_n(buf, s->as.s.len);
        free(buf);
        return result;
    }
}

static mino_val_t *prim_trim(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s;
    const char *start, *end_ptr;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("trim requires one string argument");
        return NULL;
    }
    s = args->as.cons.car;
    if (s == NULL || s->type != MINO_STRING) {
        set_error("trim requires one string argument");
        return NULL;
    }
    start   = s->as.s.data;
    end_ptr = s->as.s.data + s->as.s.len;
    while (start < end_ptr && isspace((unsigned char)*start)) start++;
    while (end_ptr > start && isspace((unsigned char)*(end_ptr - 1))) end_ptr--;
    return mino_string_n(start, (size_t)(end_ptr - start));
}

/* ------------------------------------------------------------------------- */
/* Utility primitives                                                        */
/* ------------------------------------------------------------------------- */

static mino_val_t *prim_some(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *pred;
    mino_val_t *coll;
    seq_iter_t  it;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error("some requires a predicate and a collection");
        return NULL;
    }
    pred = args->as.cons.car;
    coll = args->as.cons.cdr->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) return mino_nil();
    seq_iter_init(&it, coll);
    while (!seq_iter_done(&it)) {
        mino_val_t *elem   = seq_iter_val(&it);
        mino_val_t *call_a = mino_cons(elem, mino_nil());
        mino_val_t *result = apply_callable(pred, call_a, env);
        if (result == NULL) return NULL;
        if (mino_is_truthy(result)) return result;
        seq_iter_next(&it);
    }
    return mino_nil();
}

static mino_val_t *prim_every_p(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *pred;
    mino_val_t *coll;
    seq_iter_t  it;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error("every? requires a predicate and a collection");
        return NULL;
    }
    pred = args->as.cons.car;
    coll = args->as.cons.cdr->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) return mino_true();
    seq_iter_init(&it, coll);
    while (!seq_iter_done(&it)) {
        mino_val_t *elem   = seq_iter_val(&it);
        mino_val_t *call_a = mino_cons(elem, mino_nil());
        mino_val_t *result = apply_callable(pred, call_a, env);
        if (result == NULL) return NULL;
        if (!mino_is_truthy(result)) return mino_false();
        seq_iter_next(&it);
    }
    return mino_true();
}

static mino_val_t *prim_cons_p(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("cons? requires one argument");
        return NULL;
    }
    return mino_is_cons(args->as.cons.car) ? mino_true() : mino_false();
}

static mino_val_t *prim_nil_p(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("nil? requires one argument");
        return NULL;
    }
    return mino_is_nil(args->as.cons.car) ? mino_true() : mino_false();
}

/* Type predicate helpers: each takes one arg, returns true/false. */
#define DEFINE_TYPE_PRED(name, test)                                        \
    static mino_val_t *name(mino_val_t *args, mino_env_t *env) {           \
        (void)env;                                                         \
        if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {      \
            set_error(#name " requires one argument");                     \
            return NULL;                                                   \
        }                                                                  \
        return (test) ? mino_true() : mino_false();                        \
    }

DEFINE_TYPE_PRED(prim_string_p,  args->as.cons.car != NULL && args->as.cons.car->type == MINO_STRING)
DEFINE_TYPE_PRED(prim_number_p,  args->as.cons.car != NULL && (args->as.cons.car->type == MINO_INT || args->as.cons.car->type == MINO_FLOAT))
DEFINE_TYPE_PRED(prim_keyword_p, args->as.cons.car != NULL && args->as.cons.car->type == MINO_KEYWORD)
DEFINE_TYPE_PRED(prim_symbol_p,  args->as.cons.car != NULL && args->as.cons.car->type == MINO_SYMBOL)
DEFINE_TYPE_PRED(prim_vector_p,  args->as.cons.car != NULL && args->as.cons.car->type == MINO_VECTOR)
DEFINE_TYPE_PRED(prim_map_p,     args->as.cons.car != NULL && args->as.cons.car->type == MINO_MAP)
DEFINE_TYPE_PRED(prim_set_p,     args->as.cons.car != NULL && args->as.cons.car->type == MINO_SET)
DEFINE_TYPE_PRED(prim_fn_p,      args->as.cons.car != NULL && (args->as.cons.car->type == MINO_FN || args->as.cons.car->type == MINO_PRIM))

#undef DEFINE_TYPE_PRED

static mino_val_t *prim_type(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("type requires one argument");
        return NULL;
    }
    v = args->as.cons.car;
    if (v == NULL || v->type == MINO_NIL)  return mino_keyword("nil");
    switch (v->type) {
    case MINO_NIL:     return mino_keyword("nil");
    case MINO_BOOL:    return mino_keyword("bool");
    case MINO_INT:     return mino_keyword("int");
    case MINO_FLOAT:   return mino_keyword("float");
    case MINO_STRING:  return mino_keyword("string");
    case MINO_SYMBOL:  return mino_keyword("symbol");
    case MINO_KEYWORD: return mino_keyword("keyword");
    case MINO_CONS:    return mino_keyword("list");
    case MINO_VECTOR:  return mino_keyword("vector");
    case MINO_MAP:     return mino_keyword("map");
    case MINO_SET:     return mino_keyword("set");
    case MINO_PRIM:    return mino_keyword("fn");
    case MINO_FN:      return mino_keyword("fn");
    case MINO_MACRO:   return mino_keyword("macro");
    case MINO_HANDLE:  return mino_keyword("handle");
    case MINO_RECUR:   return mino_keyword("recur");
    }
    return mino_keyword("unknown");
}

/*
 * (str & args) — concatenate printed representations. Strings contribute
 * their raw content (no quotes); everything else uses the printer form.
 */
static mino_val_t *prim_str(mino_val_t *args, mino_env_t *env)
{
    char  *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    (void)env;
    while (mino_is_cons(args)) {
        mino_val_t *a = args->as.cons.car;
        if (a != NULL && a->type == MINO_STRING) {
            /* Append raw string content without quotes. */
            size_t need = len + a->as.s.len + 1;
            if (need > cap) {
                cap = cap == 0 ? 128 : cap;
                while (cap < need) cap *= 2;
                buf = (char *)realloc(buf, cap);
                if (buf == NULL) { set_error("out of memory"); return NULL; }
            }
            memcpy(buf + len, a->as.s.data, a->as.s.len);
            len += a->as.s.len;
        } else if (a != NULL && a->type == MINO_NIL) {
            /* nil contributes nothing. */
        } else if (a == NULL) {
            /* NULL treated as nil. */
        } else {
            /* Print to a temp buffer using the standard printer. */
            char tmp[256];
            int  n;
            switch (a->type) {
            case MINO_BOOL:
                n = snprintf(tmp, sizeof(tmp), "%s", a->as.b ? "true" : "false");
                break;
            case MINO_INT:
                n = snprintf(tmp, sizeof(tmp), "%lld", a->as.i);
                break;
            case MINO_FLOAT: {
                char fb[64];
                int fn2 = snprintf(fb, sizeof(fb), "%g", a->as.f);
                int needs_dot = 1, k;
                for (k = 0; k < fn2; k++) {
                    if (fb[k] == '.' || fb[k] == 'e' || fb[k] == 'E'
                        || fb[k] == 'n' || fb[k] == 'i') {
                        needs_dot = 0; break;
                    }
                }
                if (needs_dot) {
                    fb[fn2] = '.'; fb[fn2+1] = '0'; fb[fn2+2] = '\0';
                    fn2 += 2;
                }
                n = fn2;
                memcpy(tmp, fb, (size_t)n + 1);
                break;
            }
            case MINO_SYMBOL:
            case MINO_KEYWORD: {
                size_t slen = a->as.s.len;
                int    off  = a->type == MINO_KEYWORD ? 1 : 0;
                if (off + slen + 1 > sizeof(tmp)) slen = sizeof(tmp) - off - 1;
                if (off) tmp[0] = ':';
                memcpy(tmp + off, a->as.s.data, slen);
                n = (int)(off + slen);
                tmp[n] = '\0';
                break;
            }
            default:
                n = snprintf(tmp, sizeof(tmp), "#<%s>",
                             a->type == MINO_PRIM ? "prim" :
                             a->type == MINO_FN   ? "fn" :
                             a->type == MINO_MACRO ? "macro" :
                             a->type == MINO_HANDLE ? "handle" : "?");
                break;
            }
            if (n > 0) {
                size_t need = len + (size_t)n + 1;
                if (need > cap) {
                    cap = cap == 0 ? 128 : cap;
                    while (cap < need) cap *= 2;
                    buf = (char *)realloc(buf, cap);
                    if (buf == NULL) { set_error("out of memory"); return NULL; }
                }
                memcpy(buf + len, tmp, (size_t)n);
                len += (size_t)n;
            }
        }
        args = args->as.cons.cdr;
    }
    {
        mino_val_t *result = mino_string_n(buf != NULL ? buf : "", len);
        free(buf);
        return result;
    }
}

static mino_val_t *prim_println(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *result = prim_str(args, env);
    if (result == NULL) return NULL;
    fwrite(result->as.s.data, 1, result->as.s.len, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    return mino_nil();
}

static mino_val_t *prim_prn(mino_val_t *args, mino_env_t *env)
{
    int first = 1;
    (void)env;
    while (mino_is_cons(args)) {
        if (!first) fputc(' ', stdout);
        mino_print(args->as.cons.car);
        first = 0;
        args = args->as.cons.cdr;
    }
    fputc('\n', stdout);
    fflush(stdout);
    return mino_nil();
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

/* (throw value) — raise a script exception. Caught by try/catch; if no
 * enclosing try, becomes a fatal runtime error. */
static mino_val_t *prim_throw(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *ex;
    (void)env;
    if (!mino_is_cons(args)) {
        set_error("throw requires one argument");
        return NULL;
    }
    ex = args->as.cons.car;
    if (try_depth <= 0) {
        /* No enclosing try — format as fatal error. */
        char msg[512];
        if (ex != NULL && ex->type == MINO_STRING) {
            snprintf(msg, sizeof(msg), "unhandled exception: %.*s",
                     (int)ex->as.s.len, ex->as.s.data);
        } else {
            snprintf(msg, sizeof(msg), "unhandled exception");
        }
        set_error(msg);
        return NULL;
    }
    try_stack[try_depth - 1].exception = ex;
    longjmp(try_stack[try_depth - 1].buf, 1);
    return NULL; /* unreachable */
}

/* (slurp path) — read a file's entire contents as a string. I/O capability;
 * only installed by mino_install_io, not mino_install_core. */
static mino_val_t *prim_slurp(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *path_val;
    const char *path;
    FILE       *f;
    long        sz;
    size_t      rd;
    char       *buf;
    mino_val_t *result;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("slurp requires one argument");
        return NULL;
    }
    path_val = args->as.cons.car;
    if (path_val == NULL || path_val->type != MINO_STRING) {
        set_error("slurp: argument must be a string");
        return NULL;
    }
    path = path_val->as.s.data;
    f = fopen(path, "rb");
    if (f == NULL) {
        char msg[300];
        snprintf(msg, sizeof(msg), "slurp: cannot open file: %s", path);
        set_error(msg);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        set_error("slurp: cannot determine file size");
        return NULL;
    }
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(f);
        set_error("slurp: out of memory");
        return NULL;
    }
    rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    result = mino_string_n(buf, rd);
    free(buf);
    return result;
}

/* (require name) — load a module by name using the host-supplied resolver.
 * Returns the cached value on subsequent calls with the same name. */
static mino_val_t *prim_require(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *name_val;
    const char *name;
    const char *path;
    size_t      i;
    mino_val_t *result;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("require requires one argument");
        return NULL;
    }
    name_val = args->as.cons.car;
    if (name_val == NULL || name_val->type != MINO_STRING) {
        set_error("require: argument must be a string");
        return NULL;
    }
    name = name_val->as.s.data;
    /* Check cache. */
    for (i = 0; i < module_cache_len; i++) {
        if (strcmp(module_cache[i].name, name) == 0) {
            return module_cache[i].value;
        }
    }
    /* Resolve. */
    if (module_resolver == NULL) {
        set_error("require: no module resolver configured");
        return NULL;
    }
    path = module_resolver(name, module_resolver_ctx);
    if (path == NULL) {
        char msg[300];
        snprintf(msg, sizeof(msg), "require: cannot resolve module: %s", name);
        set_error(msg);
        return NULL;
    }
    /* Load. */
    result = mino_load_file(path, env);
    if (result == NULL) {
        return NULL;
    }
    /* Cache. */
    if (module_cache_len == module_cache_cap) {
        size_t         new_cap = module_cache_cap == 0 ? 8 : module_cache_cap * 2;
        module_entry_t *nb     = (module_entry_t *)realloc(
            module_cache, new_cap * sizeof(*nb));
        if (nb == NULL) {
            set_error("require: out of memory");
            return NULL;
        }
        module_cache     = nb;
        module_cache_cap = new_cap;
    }
    {
        size_t nlen = strlen(name);
        char *dup   = (char *)malloc(nlen + 1);
        if (dup == NULL) {
            set_error("require: out of memory");
            return NULL;
        }
        memcpy(dup, name, nlen + 1);
        module_cache[module_cache_len].name  = dup;
        module_cache[module_cache_len].value = result;
        module_cache_len++;
    }
    return result;
}

/* (doc name) — print the docstring for a def/defmacro binding.
 * Returns the docstring as a string, or nil if no docstring. */
static mino_val_t *prim_doc(mino_val_t *args, mino_env_t *env)
{
    mino_val_t   *name_val;
    char          buf[256];
    size_t        n;
    meta_entry_t *e;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("doc requires one argument");
        return NULL;
    }
    name_val = args->as.cons.car;
    if (name_val == NULL || name_val->type != MINO_SYMBOL) {
        set_error("doc: argument must be a symbol");
        return NULL;
    }
    n = name_val->as.s.len;
    if (n >= sizeof(buf)) {
        set_error("doc: name too long");
        return NULL;
    }
    memcpy(buf, name_val->as.s.data, n);
    buf[n] = '\0';
    e = meta_find(buf);
    if (e != NULL && e->docstring != NULL) {
        return mino_string(e->docstring);
    }
    return mino_nil();
}

/* (source name) — return the source form of a def/defmacro binding. */
static mino_val_t *prim_source(mino_val_t *args, mino_env_t *env)
{
    mino_val_t   *name_val;
    char          buf[256];
    size_t        n;
    meta_entry_t *e;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("source requires one argument");
        return NULL;
    }
    name_val = args->as.cons.car;
    if (name_val == NULL || name_val->type != MINO_SYMBOL) {
        set_error("source: argument must be a symbol");
        return NULL;
    }
    n = name_val->as.s.len;
    if (n >= sizeof(buf)) {
        set_error("source: name too long");
        return NULL;
    }
    memcpy(buf, name_val->as.s.data, n);
    buf[n] = '\0';
    e = meta_find(buf);
    if (e != NULL && e->source != NULL) {
        return e->source;
    }
    return mino_nil();
}

/* (apropos substring) — return a list of bound names containing substring. */
static mino_val_t *prim_apropos(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *pat_val;
    const char *pat;
    mino_val_t *head = mino_nil();
    mino_val_t *tail = NULL;
    mino_env_t *e;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("apropos requires one argument");
        return NULL;
    }
    pat_val = args->as.cons.car;
    if (pat_val == NULL || pat_val->type != MINO_STRING) {
        set_error("apropos: argument must be a string");
        return NULL;
    }
    pat = pat_val->as.s.data;
    /* Walk every env frame from the given env up to root. */
    for (e = env; e != NULL; e = e->parent) {
        size_t i;
        for (i = 0; i < e->len; i++) {
            if (strstr(e->bindings[i].name, pat) != NULL) {
                mino_val_t *sym  = mino_symbol(e->bindings[i].name);
                mino_val_t *cell = mino_cons(sym, mino_nil());
                if (tail == NULL) {
                    head = cell;
                } else {
                    tail->as.cons.cdr = cell;
                }
                tail = cell;
            }
        }
    }
    return head;
}

void mino_set_resolver(mino_resolve_fn fn, void *ctx)
{
    module_resolver     = fn;
    module_resolver_ctx = ctx;
}

/*
 * Stdlib macros defined in mino itself. Each form is read + evaluated in
 * order against the installing env during mino_install_core, so downstream
 * code can depend on them as if they were primitives.
 *
 * Hygiene: macro writers introduce temporaries via (gensym) to avoid
 * capturing names from the caller's environment. 0.x makes no automatic
 * hygiene promise; gensym is the convention.
 */
#include "stdlib_mino.h"

static void install_stdlib_macros(mino_env_t *env)
{
    const char *src        = stdlib_mino_src;
    const char *saved_file = reader_file;
    int         saved_line = reader_line;
    reader_file = intern_filename("<stdlib>");
    reader_line = 1;
    while (*src != '\0') {
        const char *end  = NULL;
        mino_val_t *form = mino_read(src, &end);
        if (form == NULL) {
            if (mino_last_error() != NULL) {
                /* Hardcoded source — a parse error here is a build-time bug. */
                fprintf(stderr, "stdlib parse error: %s\n", mino_last_error());
                abort();
            }
            break;
        }
        if (mino_eval(form, env) == NULL) {
            fprintf(stderr, "stdlib eval error: %s\n", mino_last_error());
            abort();
        }
        src = end;
    }
    reader_file = saved_file;
    reader_line = saved_line;
}

void mino_install_core(mino_env_t *env)
{
    volatile char probe = 0;
    gc_note_host_frame((void *)&probe);
    (void)probe;
    mino_env_set(env, "+",        mino_prim("+",        prim_add));
    mino_env_set(env, "-",        mino_prim("-",        prim_sub));
    mino_env_set(env, "*",        mino_prim("*",        prim_mul));
    mino_env_set(env, "/",        mino_prim("/",        prim_div));
    mino_env_set(env, "=",        mino_prim("=",        prim_eq));
    mino_env_set(env, "<",        mino_prim("<",        prim_lt));
    mino_env_set(env, "car",      mino_prim("car",      prim_car));
    mino_env_set(env, "cdr",      mino_prim("cdr",      prim_cdr));
    mino_env_set(env, "cons",     mino_prim("cons",     prim_cons));
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
    mino_env_set(env, "cons?",    mino_prim("cons?",    prim_cons_p));
    mino_env_set(env, "nil?",     mino_prim("nil?",     prim_nil_p));
    mino_env_set(env, "string?",  mino_prim("string?",  prim_string_p));
    mino_env_set(env, "number?",  mino_prim("number?",  prim_number_p));
    mino_env_set(env, "keyword?", mino_prim("keyword?", prim_keyword_p));
    mino_env_set(env, "symbol?",  mino_prim("symbol?",  prim_symbol_p));
    mino_env_set(env, "vector?",  mino_prim("vector?",  prim_vector_p));
    mino_env_set(env, "map?",     mino_prim("map?",     prim_map_p));
    mino_env_set(env, "fn?",      mino_prim("fn?",      prim_fn_p));
    mino_env_set(env, "type",     mino_prim("type",     prim_type));
    mino_env_set(env, "str",      mino_prim("str",      prim_str));
    mino_env_set(env, "throw",    mino_prim("throw",    prim_throw));
    mino_env_set(env, "require",  mino_prim("require",  prim_require));
    mino_env_set(env, "doc",      mino_prim("doc",      prim_doc));
    mino_env_set(env, "source",   mino_prim("source",   prim_source));
    mino_env_set(env, "apropos",  mino_prim("apropos",  prim_apropos));
    /* set operations */
    mino_env_set(env, "hash-set", mino_prim("hash-set", prim_hash_set));
    mino_env_set(env, "set?",     mino_prim("set?",     prim_set_p));
    mino_env_set(env, "contains?",mino_prim("contains?",prim_contains_p));
    mino_env_set(env, "disj",     mino_prim("disj",     prim_disj));
    /* sequence operations */
    mino_env_set(env, "map",      mino_prim("map",      prim_map));
    mino_env_set(env, "filter",   mino_prim("filter",   prim_filter));
    mino_env_set(env, "reduce",   mino_prim("reduce",   prim_reduce));
    mino_env_set(env, "take",     mino_prim("take",     prim_take));
    mino_env_set(env, "drop",     mino_prim("drop",     prim_drop));
    mino_env_set(env, "range",    mino_prim("range",    prim_range));
    mino_env_set(env, "repeat",   mino_prim("repeat",   prim_repeat));
    mino_env_set(env, "concat",   mino_prim("concat",   prim_concat));
    mino_env_set(env, "into",     mino_prim("into",     prim_into));
    mino_env_set(env, "apply",    mino_prim("apply",    prim_apply));
    mino_env_set(env, "reverse",  mino_prim("reverse",  prim_reverse));
    mino_env_set(env, "sort",     mino_prim("sort",     prim_sort));
    /* string operations */
    mino_env_set(env, "subs",     mino_prim("subs",     prim_subs));
    mino_env_set(env, "split",    mino_prim("split",    prim_split));
    mino_env_set(env, "join",     mino_prim("join",     prim_join));
    mino_env_set(env, "starts-with?",
                 mino_prim("starts-with?", prim_starts_with_p));
    mino_env_set(env, "ends-with?",
                 mino_prim("ends-with?", prim_ends_with_p));
    mino_env_set(env, "includes?",
                 mino_prim("includes?", prim_includes_p));
    mino_env_set(env, "upper-case",
                 mino_prim("upper-case", prim_upper_case));
    mino_env_set(env, "lower-case",
                 mino_prim("lower-case", prim_lower_case));
    mino_env_set(env, "trim",     mino_prim("trim",     prim_trim));
    /* utility */
    mino_env_set(env, "some",     mino_prim("some",     prim_some));
    mino_env_set(env, "every?",   mino_prim("every?",   prim_every_p));
    install_stdlib_macros(env);
}

void mino_install_io(mino_env_t *env)
{
    mino_env_set(env, "println",  mino_prim("println",  prim_println));
    mino_env_set(env, "prn",      mino_prim("prn",      prim_prn));
    mino_env_set(env, "slurp",    mino_prim("slurp",    prim_slurp));
}
