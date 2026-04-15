/*
 * mino.c — runtime implementation.
 *
 * Single-file amalgamation: this translation unit, paired with mino.h,
 * is the entire runtime. ANSI C, no external dependencies.
 */

#include "mino.h"

#include <ctype.h>
#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
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

/* Exception handling: setjmp/longjmp stack for try/catch. */
#define MAX_TRY_DEPTH 64

typedef struct {
    jmp_buf     buf;
    mino_val_t *exception;
} try_frame_t;

/* Module cache entry. */
typedef struct {
    char       *name;
    mino_val_t *value;
} module_entry_t;

/* Metadata table entry: docstrings and source forms for def/defmacro. */
typedef struct {
    char       *name;       /* binding name (malloc-owned) */
    char       *docstring;  /* docstring (malloc-owned, NULL if none) */
    mino_val_t *source;     /* source form (the whole def/defmacro form) */
} meta_entry_t;

/* Intern table: flat array with linear scan for symbol/keyword dedup. */
typedef struct {
    mino_val_t **entries;
    size_t       len;
    size_t       cap;
} intern_table_t;

/* Call-stack frame for stack traces on error. */
#define MAX_CALL_DEPTH 256

typedef struct {
    const char *name;
    const char *file;
    int         line;
} call_frame_t;

/* GC root-environment registry node (malloc-owned, not GC-managed). */
typedef struct root_env {
    mino_env_t      *env;
    struct root_env *next;
} root_env_t;

/* Host-retained value ref (malloc-owned, not GC-managed). */
struct mino_ref {
    mino_val_t      *val;
    struct mino_ref *next;
    struct mino_ref *prev;
};

/* Dynamic binding frame: a stack of name-value pairs for (binding ...). */
typedef struct dyn_binding {
    const char          *name;
    mino_val_t          *val;
    struct dyn_binding  *next;
} dyn_binding_t;

typedef struct dyn_frame {
    dyn_binding_t       *bindings;
    struct dyn_frame    *prev;
} dyn_frame_t;

/* GC range: address span of one allocated payload for conservative scan. */
typedef struct {
    uintptr_t  start;  /* inclusive payload byte address */
    uintptr_t  end;    /* exclusive payload byte address */
    gc_hdr_t  *h;
} gc_range_t;

/* ------------------------------------------------------------------------- */
/* Runtime state                                                             */
/* ------------------------------------------------------------------------- */
/*
 * mino_state_t holds all mutable runtime state that was previously stored in
 * file-scoped static variables.  A default instance (g_state) provides the
 * same single-instance behaviour as before.  The macros below let existing
 * code use bare names (gc_all, try_depth, ...) which resolve to S_->field.
 */

struct mino_state {
    /* Garbage collection */
    gc_hdr_t       *gc_all;
    size_t          gc_bytes_alloc;
    size_t          gc_bytes_live;
    size_t          gc_threshold;
    int             gc_stress;
    int             gc_depth;
    void           *gc_stack_bottom;
    root_env_t     *gc_root_envs;
    gc_range_t     *gc_ranges;
    size_t          gc_ranges_len;
    size_t          gc_ranges_cap;

    /* Singletons */
    mino_val_t      nil_singleton;
    mino_val_t      true_singleton;
    mino_val_t      false_singleton;

    /* Intern tables */
    intern_table_t  sym_intern;
    intern_table_t  kw_intern;

    /* Execution limits */
    size_t          limit_steps;
    size_t          limit_heap;
    size_t          eval_steps;
    int             limit_exceeded;

    /* Exception handling */
    try_frame_t     try_stack[MAX_TRY_DEPTH];
    int             try_depth;

    /* Module system */
    mino_resolve_fn module_resolver;
    void           *module_resolver_ctx;
    module_entry_t *module_cache;
    size_t          module_cache_len;
    size_t          module_cache_cap;

    /* Metadata */
    meta_entry_t   *meta_table;
    size_t          meta_table_len;
    size_t          meta_table_cap;

    /* Printer */
    int             print_depth;

    /* Error reporting */
    char            error_buf[2048];
    call_frame_t    call_stack[MAX_CALL_DEPTH];
    int             call_depth;
    int             trace_added;

    /* Reader */
    const char     *reader_file;
    int             reader_line;

    /* Eval */
    const mino_val_t *eval_current_form;

    /* Random */
    int             rand_seeded;

    /* Sort comparator (used during merge sort) */
    mino_val_t     *sort_comp_fn;
    mino_env_t     *sort_comp_env;

    /* Gensym counter */
    long            gensym_counter;

    /* Host-retained value refs */
    mino_ref_t     *ref_roots;

    /* Dynamic bindings */
    dyn_frame_t    *dyn_stack;

    /* Interrupt flag (checked by the eval loop) */
    volatile int    interrupted;

    /* GC save stack: pinned values during eval to protect borrowed pointers
     * that the conservative stack scanner might miss (register-allocated). */
    mino_val_t     *gc_save[32];
    int             gc_save_len;
};

/* Default global state instance and current-state pointer. */
static mino_state_t g_state;
static int          g_state_ready;
static mino_state_t *S_ = &g_state;

static void state_init(mino_state_t *st)
{
    memset(st, 0, sizeof(*st));
    st->gc_threshold        = 1u << 20;
    st->gc_stress           = -1;
    st->nil_singleton.type  = MINO_NIL;
    st->true_singleton.type = MINO_BOOL;
    st->true_singleton.as.b = 1;
    st->false_singleton.type = MINO_BOOL;
    st->reader_line         = 1;
}

mino_state_t *mino_state_new(void)
{
    mino_state_t *st = (mino_state_t *)calloc(1, sizeof(*st));
    if (st == NULL) {
        abort();
    }
    state_init(st);
    return st;
}

void mino_state_free(mino_state_t *st)
{
    root_env_t *r;
    root_env_t *rnext;
    gc_hdr_t   *h;
    gc_hdr_t   *hnext;
    size_t      i;
    if (st == NULL) {
        return;
    }
    for (r = st->gc_root_envs; r != NULL; r = rnext) {
        rnext = r->next;
        free(r);
    }
    {
        mino_ref_t *ref = st->ref_roots;
        mino_ref_t *rnxt;
        while (ref != NULL) {
            rnxt = ref->next;
            free(ref);
            ref = rnxt;
        }
    }
    for (i = 0; i < st->module_cache_len; i++) {
        free(st->module_cache[i].name);
    }
    free(st->module_cache);
    for (i = 0; i < st->meta_table_len; i++) {
        free(st->meta_table[i].name);
        free(st->meta_table[i].docstring);
    }
    free(st->meta_table);
    free(st->sym_intern.entries);
    free(st->kw_intern.entries);
    free(st->gc_ranges);
    for (h = st->gc_all; h != NULL; h = hnext) {
        hnext = h->next;
        if (h->type_tag == GC_T_VAL) {
            mino_val_t *v = (mino_val_t *)(h + 1);
            if (v->type == MINO_HANDLE && v->as.handle.finalizer != NULL) {
                v->as.handle.finalizer(v->as.handle.ptr, v->as.handle.tag);
            }
        }
        free(h);
    }
    if (st != &g_state) {
        free(st);
    }
}

mino_state_t *mino_current_state(void)
{
    return S_;
}

mino_ref_t *mino_ref(mino_state_t *S, mino_val_t *val)
{
    mino_ref_t *r = (mino_ref_t *)calloc(1, sizeof(*r));
    if (r == NULL) {
        abort();
    }
    r->val  = val;
    r->prev = NULL;
    r->next = S->ref_roots;
    if (S->ref_roots != NULL) {
        S->ref_roots->prev = r;
    }
    S->ref_roots = r;
    return r;
}

mino_val_t *mino_deref(const mino_ref_t *ref)
{
    if (ref == NULL) {
        return NULL;
    }
    return ref->val;
}

void mino_unref(mino_state_t *S, mino_ref_t *ref)
{
    if (ref == NULL) {
        return;
    }
    if (ref->prev != NULL) {
        ref->prev->next = ref->next;
    } else {
        S->ref_roots = ref->next;
    }
    if (ref->next != NULL) {
        ref->next->prev = ref->prev;
    }
    free(ref);
}

/* ---- State field accessor macros ---------------------------------------- */
/* Existing code uses bare names which resolve to S_->field.                 */

#define gc_all              (S_->gc_all)
#define gc_bytes_alloc      (S_->gc_bytes_alloc)
#define gc_bytes_live       (S_->gc_bytes_live)
#define gc_threshold        (S_->gc_threshold)
#define gc_stress           (S_->gc_stress)
#define gc_depth            (S_->gc_depth)
#define gc_stack_bottom     (S_->gc_stack_bottom)
#define gc_root_envs        (S_->gc_root_envs)
#define gc_ranges           (S_->gc_ranges)
#define gc_ranges_len       (S_->gc_ranges_len)
#define gc_ranges_cap       (S_->gc_ranges_cap)
#define nil_singleton       (S_->nil_singleton)
#define true_singleton      (S_->true_singleton)
#define false_singleton     (S_->false_singleton)
#define sym_intern          (S_->sym_intern)
#define kw_intern           (S_->kw_intern)
#define limit_steps         (S_->limit_steps)
#define limit_heap          (S_->limit_heap)
#define eval_steps          (S_->eval_steps)
#define limit_exceeded      (S_->limit_exceeded)
#define try_stack           (S_->try_stack)
#define try_depth           (S_->try_depth)
#define module_resolver     (S_->module_resolver)
#define module_resolver_ctx (S_->module_resolver_ctx)
#define module_cache        (S_->module_cache)
#define module_cache_len    (S_->module_cache_len)
#define module_cache_cap    (S_->module_cache_cap)
#define meta_table          (S_->meta_table)
#define meta_table_len      (S_->meta_table_len)
#define meta_table_cap      (S_->meta_table_cap)
#define print_depth         (S_->print_depth)
#define error_buf           (S_->error_buf)
#define call_stack          (S_->call_stack)
#define call_depth          (S_->call_depth)
#define trace_added         (S_->trace_added)
#define reader_file         (S_->reader_file)
#define reader_line         (S_->reader_line)
#define eval_current_form   (S_->eval_current_form)
#define rand_seeded         (S_->rand_seeded)
#define sort_comp_fn        (S_->sort_comp_fn)
#define sort_comp_env       (S_->sort_comp_env)
#define gensym_counter      (S_->gensym_counter)
#define dyn_stack           (S_->dyn_stack)
#define interrupted         (S_->interrupted)
#define gc_save             (S_->gc_save)
#define gc_save_len         (S_->gc_save_len)

/* Pin a value on the GC save stack to protect borrowed pointers across
 * allocations.  The conservative stack scanner can miss locals that the
 * compiler has moved to registers or eliminated. */
#define gc_pin(v) \
    do { if (gc_save_len < 32) gc_save[gc_save_len++] = (v); } while (0)
#define gc_unpin(n) \
    do { gc_save_len -= (n); } while (0)

/* Look up a name in the dynamic binding stack.  Returns the value if
 * found, NULL otherwise. */
static mino_val_t *dyn_lookup(const char *name)
{
    dyn_frame_t *f;
    dyn_binding_t *b;
    for (f = dyn_stack; f != NULL; f = f->prev) {
        for (b = f->bindings; b != NULL; b = b->next) {
            if (strcmp(b->name, name) == 0) return b->val;
        }
    }
    return NULL;
}

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
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
#endif
static void gc_note_host_frame(void *addr)
{
    if (gc_stack_bottom == NULL
        || (char *)addr > (char *)gc_stack_bottom) {
        gc_stack_bottom = addr;
    }
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

static void *gc_alloc_typed(unsigned char tag, size_t size)
{
    gc_hdr_t *h;
    if (!g_state_ready) {
        state_init(S_);
        g_state_ready = 1;
    }
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

mino_val_t *mino_nil(mino_state_t *S)
{
    S_ = S;
    return &nil_singleton;
}

mino_val_t *mino_true(mino_state_t *S)
{
    S_ = S;
    return &true_singleton;
}

mino_val_t *mino_false(mino_state_t *S)
{
    S_ = S;
    return &false_singleton;
}

/* ------------------------------------------------------------------------- */
/* Constructors                                                              */
/* ------------------------------------------------------------------------- */

mino_val_t *mino_int(mino_state_t *S, long long n)
{
    S_ = S;
    mino_val_t *v = alloc_val(MINO_INT);
    v->as.i = n;
    return v;
}

mino_val_t *mino_float(mino_state_t *S, double f)
{
    S_ = S;
    mino_val_t *v = alloc_val(MINO_FLOAT);
    v->as.f = f;
    return v;
}

mino_val_t *mino_string_n(mino_state_t *S, const char *s, size_t len)
{
    S_ = S;
    mino_val_t *v = alloc_val(MINO_STRING);
    v->as.s.data = dup_n(s, len);
    v->as.s.len  = len;
    return v;
}

mino_val_t *mino_string(mino_state_t *S, const char *s)
{
    S_ = S;
    return mino_string_n(S_, s, strlen(s));
}

/*
 * Symbols and keywords are interned through small process-wide tables so
 * that identity comparison is pointer-equal after lookup. The tables are
 * flat arrays with linear scan — adequate until the v0.5 HAMT arrives and
 * the collector reclaims names. Entries live for the life of the process.
 */

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

mino_val_t *mino_symbol_n(mino_state_t *S, const char *s, size_t len)
{
    S_ = S;
    return intern_lookup_or_create(&sym_intern, MINO_SYMBOL, s, len);
}

mino_val_t *mino_symbol(mino_state_t *S, const char *s)
{
    S_ = S;
    return mino_symbol_n(S_, s, strlen(s));
}

mino_val_t *mino_keyword_n(mino_state_t *S, const char *s, size_t len)
{
    S_ = S;
    return intern_lookup_or_create(&kw_intern, MINO_KEYWORD, s, len);
}

mino_val_t *mino_keyword(mino_state_t *S, const char *s)
{
    S_ = S;
    return mino_keyword_n(S_, s, strlen(s));
}

mino_val_t *mino_cons(mino_state_t *S, mino_val_t *car, mino_val_t *cdr)
{
    S_ = S;
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

mino_val_t *mino_vector(mino_state_t *S, mino_val_t **items, size_t len)
{
    S_ = S;
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
    case MINO_ATOM: {
        /* Atoms are identity-based (each atom is unique). */
        uintptr_t p = (uintptr_t)v;
        unsigned  i;
        h = fnv_mix(h, 0x0e);
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
mino_val_t *mino_map(mino_state_t *S, mino_val_t **keys, mino_val_t **vals, size_t len)
{
    S_ = S;
    mino_val_t       *v        = alloc_val(MINO_MAP);
    mino_hamt_node_t *root     = NULL;
    mino_val_t       *order    = mino_vector(S_, NULL, 0);
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

mino_val_t *mino_set(mino_state_t *S, mino_val_t **items, size_t len)
{
    S_ = S;
    mino_val_t       *v        = alloc_val(MINO_SET);
    mino_hamt_node_t *root     = NULL;
    mino_val_t       *order    = mino_vector(S_, NULL, 0);
    size_t            len_out  = 0;
    size_t            i;
    /* Sentinel value: all set entries map to true. */
    mino_val_t       *sentinel = mino_true(S_);
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

mino_val_t *mino_prim(mino_state_t *S, const char *name, mino_prim_fn fn)
{
    S_ = S;
    mino_val_t *v = alloc_val(MINO_PRIM);
    v->as.prim.name = name;
    v->as.prim.fn   = fn;
    return v;
}

mino_val_t *mino_handle(mino_state_t *S, void *ptr, const char *tag)
{
    S_ = S;
    mino_val_t *v = alloc_val(MINO_HANDLE);
    v->as.handle.ptr       = ptr;
    v->as.handle.tag       = tag;
    v->as.handle.finalizer = NULL;
    return v;
}

mino_val_t *mino_handle_ex(mino_state_t *S, void *ptr, const char *tag,
                           mino_finalizer_fn finalizer)
{
    S_ = S;
    mino_val_t *v = alloc_val(MINO_HANDLE);
    v->as.handle.ptr       = ptr;
    v->as.handle.tag       = tag;
    v->as.handle.finalizer = finalizer;
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

mino_val_t *mino_atom(mino_state_t *S, mino_val_t *val)
{
    S_ = S;
    mino_val_t *v = alloc_val(MINO_ATOM);
    v->as.atom.val = val;
    return v;
}

int mino_is_atom(const mino_val_t *v)
{
    return v != NULL && v->type == MINO_ATOM;
}

mino_val_t *mino_atom_deref(const mino_val_t *a)
{
    if (a == NULL || a->type != MINO_ATOM) return NULL;
    return a->as.atom.val;
}

void mino_atom_reset(mino_val_t *a, mino_val_t *val)
{
    if (a != NULL && a->type == MINO_ATOM) {
        a->as.atom.val = val;
    }
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
        return mino_nil(S_);
    }
    return v->as.cons.car;
}

mino_val_t *mino_cdr(const mino_val_t *v)
{
    if (!mino_is_cons(v)) {
        return mino_nil(S_);
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

/* Forward declaration: lazy_force is defined after the evaluator. */
static mino_val_t *eval_implicit_do(mino_val_t *body, mino_env_t *env);
static mino_val_t *lazy_force(mino_val_t *v);

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

void mino_print_to(mino_state_t *S, FILE *out, const mino_val_t *v)
{
    S_ = S;
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
            mino_print_to(S_, out, p->as.cons.car);
            p = p->as.cons.cdr;
            /* Force lazy tails so (cons x (lazy-seq ...)) prints as a list. */
            if (p != NULL && p->type == MINO_LAZY) {
                p = lazy_force((mino_val_t *)p);
            }
            if (p != NULL && p->type == MINO_CONS) {
                fputc(' ', out);
            } else if (p != NULL && p->type != MINO_NIL) {
                fputs(" . ", out);
                mino_print_to(S_, out, p);
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
            mino_print_to(S_, out, vec_nth(v, i));
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
            mino_print_to(S_, out, key);
            fputc(' ', out);
            mino_print_to(S_, out, map_get_val(v, key));
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
            mino_print_to(S_, out, vec_nth(v->as.set.key_order, i));
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
    case MINO_ATOM:
        fputs("#atom[", out);
        print_depth++;
        mino_print_to(S_, out, v->as.atom.val);
        print_depth--;
        fputc(']', out);
        return;
    case MINO_LAZY: {
        /* Force the lazy seq and print the realized value. */
        mino_val_t *forced = lazy_force((mino_val_t *)v);
        mino_print_to(S_, out, forced);
        return;
    }
    case MINO_RECUR:
        /* Internal sentinel; should not escape to user-visible output. */
        fputs("#<recur>", out);
        return;
    case MINO_TAIL_CALL:
        fputs("#<tail-call>", out);
        return;
    }
}

void mino_print(mino_state_t *S, const mino_val_t *v)
{
    S_ = S;
    mino_print_to(S_, stdout, v);
}

void mino_println(mino_state_t *S, const mino_val_t *v)
{
    S_ = S;
    mino_print_to(S_, stdout, v);
    fputc('\n', stdout);
}

/* ------------------------------------------------------------------------- */
/* Error reporting                                                           */
/* ------------------------------------------------------------------------- */

const char *mino_last_error(mino_state_t *S)
{
    S_ = S;
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
    case MINO_ATOM:    return "atom";
    case MINO_LAZY:    return "lazy-seq";
    case MINO_RECUR:     return "recur";
    case MINO_TAIL_CALL: return "tail-call";
    }
    return "unknown";
}

/* ------------------------------------------------------------------------- */
/* Call stack (for stack traces on error)                                     */
/* ------------------------------------------------------------------------- */

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
        || c == '`'  || c == '~' || c == '@'
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
        mino_val_t *v = mino_string_n(S_, buf, len);
        free(buf);
        return v;
    }
}

static mino_val_t *read_list_form(const char **p)
{
    /* Caller has positioned *p on the opening '('. */
    int         list_line = reader_line;
    mino_val_t *head = mino_nil(S_);
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
            if (elem == NULL && mino_last_error(S_) != NULL) {
                return NULL;
            }
            if (elem == NULL) {
                /* EOF mid-list */
                set_error("unterminated list");
                return NULL;
            }
            {
                mino_val_t *cell = mino_cons(S_, elem, mino_nil(S_));
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
                if (mino_last_error(S_) == NULL) {
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
    return mino_vector(S_, buf, len);
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
            if (mino_last_error(S_) == NULL) {
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
            if (mino_last_error(S_) == NULL) {
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
    return mino_map(S_, kbuf, vbuf, len);
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
            if (mino_last_error(S_) == NULL) {
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
    return mino_set(S_, buf, len);
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
        return mino_keyword_n(S_, start + 1, len - 1);
    }
    if (len == 1 && start[0] == ':') {
        set_error("keyword missing name");
        return NULL;
    }

    if (len == 3 && memcmp(start, "nil", 3) == 0) {
        return mino_nil(S_);
    }
    if (len == 4 && memcmp(start, "true", 4) == 0) {
        return mino_true(S_);
    }
    if (len == 5 && memcmp(start, "false", 5) == 0) {
        return mino_false(S_);
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
                        return mino_float(S_, d);
                    }
                } else {
                    long long n = strtoll(buf, &endp, 10);
                    if (endp == buf + len) {
                        return mino_int(S_, n);
                    }
                }
            }
        }
    }

    return mino_symbol_n(S_, start, len);
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
                if (mino_last_error(S_) == NULL) {
                    set_error("expected form after quote");
                }
                return NULL;
            }
            outer = mino_cons(S_, mino_symbol(S_, "quote"),
                              mino_cons(S_, quoted, mino_nil(S_)));
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
                if (mino_last_error(S_) == NULL) {
                    set_error("expected form after `");
                }
                return NULL;
            }
            outer = mino_cons(S_, mino_symbol(S_, "quasiquote"),
                              mino_cons(S_, qq, mino_nil(S_)));
            outer->as.cons.file = reader_file;
            outer->as.cons.line = q_line;
            return outer;
        }
    }
    if (**p == '@') {
        int q_line = reader_line;
        (*p)++;
        {
            mino_val_t *target = read_form(p);
            mino_val_t *outer;
            if (target == NULL) {
                if (mino_last_error(S_) == NULL) {
                    set_error("expected form after @");
                }
                return NULL;
            }
            outer = mino_cons(S_, mino_symbol(S_, "deref"),
                              mino_cons(S_, target, mino_nil(S_)));
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
                if (mino_last_error(S_) == NULL) {
                    set_error("expected form after ~");
                }
                return NULL;
            }
            outer = mino_cons(S_, mino_symbol(S_, name),
                              mino_cons(S_, uq, mino_nil(S_)));
            outer->as.cons.file = reader_file;
            outer->as.cons.line = q_line;
            return outer;
        }
    }
    return read_atom(p);
}

mino_val_t *mino_read(mino_state_t *S, const char *src, const char **end)
{
    S_ = S;
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
    /* Force lazy seqs before comparison. */
    if (a->type == MINO_LAZY) a = lazy_force((mino_val_t *)a);
    if (b->type == MINO_LAZY) b = lazy_force((mino_val_t *)b);
    if (a == NULL) a = mino_nil(S_);
    if (b == NULL) b = mino_nil(S_);
    if (a == b) return 1;
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
    case MINO_ATOM:
        return a == b;
    case MINO_LAZY:
        /* Should not reach here — lazy seqs are forced above. */
        return 0;
    case MINO_RECUR:
        return a == b;
    case MINO_TAIL_CALL:
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

static mino_env_t *env_alloc(mino_env_t *parent)
{
    mino_env_t *env = (mino_env_t *)gc_alloc_typed(GC_T_ENV, sizeof(*env));
    env->parent = parent;
    return env;
}

mino_env_t *mino_env_new(mino_state_t *S)
{
    volatile char probe = 0;
    S_ = S;
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

void mino_env_free(mino_state_t *S, mino_env_t *env)
{
    S_ = S;
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

mino_env_t *mino_env_clone(mino_state_t *S, mino_env_t *env)
{
    S_ = S;
    if (env == NULL) return NULL;

    /* Allocate a new root env and copy all bindings from the source. */
    mino_env_t *clone = mino_env_new(S);
    size_t i;
    for (i = 0; i < env->len; i++) {
        env_bind(clone, env->bindings[i].name, env->bindings[i].val);
    }
    return clone;
}

void mino_env_set(mino_state_t *S, mino_env_t *env, const char *name, mino_val_t *val)
{
    S_ = S;
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
    case MINO_ATOM:
        gc_mark_interior(v->as.atom.val);
        break;
    case MINO_LAZY:
        if (v->as.lazy.realized) {
            gc_mark_interior(v->as.lazy.cached);
        } else {
            gc_mark_interior(v->as.lazy.body);
            gc_mark_interior(v->as.lazy.env);
        }
        break;
    case MINO_RECUR:
        gc_mark_interior(v->as.recur.args);
        break;
    case MINO_TAIL_CALL:
        gc_mark_interior(v->as.tail_call.fn);
        gc_mark_interior(v->as.tail_call.args);
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
    /* Pin host-retained refs. */
    {
        mino_ref_t *ref;
        for (ref = S_->ref_roots; ref != NULL; ref = ref->next) {
            gc_mark_interior(ref->val);
        }
    }
    /* Pin dynamic binding values. */
    {
        dyn_frame_t *f;
        dyn_binding_t *b;
        for (f = dyn_stack; f != NULL; f = f->prev) {
            for (b = f->bindings; b != NULL; b = b->next) {
                gc_mark_interior(b->val);
            }
        }
    }
    /* Pin sort comparator if active. */
    gc_mark_interior(sort_comp_fn);
    /* Pin values on the GC save stack. */
    {
        int si;
        for (si = 0; si < gc_save_len; si++) {
            gc_mark_interior(gc_save[si]);
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
            /* Call finalizer for handles being collected. */
            if (h->type_tag == GC_T_VAL) {
                mino_val_t *v = (mino_val_t *)(h + 1);
                if (v->type == MINO_HANDLE && v->as.handle.finalizer != NULL) {
                    v->as.handle.finalizer(v->as.handle.ptr,
                                           v->as.handle.tag);
                }
            }
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

static mino_val_t *eval_impl(mino_val_t *form, mino_env_t *env, int tail);
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
        return mino_vector(S_, tmp, nn);
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
        return mino_map(S_, ks, vs, nn);
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
        mino_val_t *out  = mino_nil(S_);
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
                    mino_val_t *cell = mino_cons(S_, sp->as.cons.car, mino_nil(S_));
                    if (tail == NULL) { out = cell; } else { tail->as.cons.cdr = cell; }
                    tail = cell;
                    sp = sp->as.cons.cdr;
                }
            } else {
                mino_val_t *expanded = quasiquote_expand(elem, env);
                mino_val_t *cell;
                if (expanded == NULL) { return NULL; }
                cell = mino_cons(S_, expanded, mino_nil(S_));
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
    if (v->type == MINO_TAIL_CALL) {
        set_error("tail call in non-tail position");
        return NULL;
    }
    return v;
}

static mino_val_t *eval_implicit_do_impl(mino_val_t *body, mino_env_t *env,
                                         int tail)
{
    if (!mino_is_cons(body)) {
        return mino_nil(S_);
    }
    for (;;) {
        mino_val_t *rest = body->as.cons.cdr;
        if (!mino_is_cons(rest)) {
            /* Last expression: tail position, propagate recur/tail-call. */
            return eval_impl(body->as.cons.car, env, tail);
        }
        if (eval_value(body->as.cons.car, env) == NULL) {
            return NULL;
        }
        body = rest;
    }
}

static mino_val_t *eval_implicit_do(mino_val_t *body, mino_env_t *env)
{
    return eval_implicit_do_impl(body, env, 0);
}

/*
 * Force a lazy sequence: evaluate the body in the captured environment,
 * cache the result, and release the thunk for GC. Iteratively unwraps
 * nested lazy seqs to avoid stack overflow.
 */
static mino_val_t *lazy_force(mino_val_t *v)
{
    if (v->as.lazy.realized) {
        return v->as.lazy.cached;
    }
    {
        mino_val_t *result = eval_implicit_do(v->as.lazy.body, v->as.lazy.env);
        if (result == NULL) return NULL;
        /* Iteratively unwrap nested lazy seqs. */
        while (result != NULL && result->type == MINO_LAZY) {
            if (result->as.lazy.realized) {
                result = result->as.lazy.cached;
            } else {
                mino_val_t *inner = eval_implicit_do(
                    result->as.lazy.body, result->as.lazy.env);
                result->as.lazy.cached  = inner;
                result->as.lazy.realized = 1;
                result->as.lazy.body    = NULL;
                result->as.lazy.env     = NULL;
                result = inner;
                if (result == NULL) return NULL;
            }
        }
        v->as.lazy.cached   = result;
        v->as.lazy.realized = 1;
        v->as.lazy.body     = NULL;
        v->as.lazy.env      = NULL;
        return result;
    }
}

static mino_val_t *eval_args(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *head = mino_nil(S_);
    mino_val_t *tail = NULL;
    while (mino_is_cons(args)) {
        mino_val_t *v = eval_value(args->as.cons.car, env);
        mino_val_t *cell;
        if (v == NULL) {
            return NULL;
        }
        gc_pin(v);
        cell = mino_cons(S_, v, mino_nil(S_));
        gc_unpin(1);
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

static mino_val_t *eval_impl(mino_val_t *form, mino_env_t *env, int tail)
{
    if (limit_exceeded) {
        return NULL;
    }
    if (interrupted) {
        limit_exceeded = 1;
        set_error("interrupted");
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
        return mino_nil(S_);
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
    case MINO_ATOM:
    case MINO_LAZY:
    case MINO_RECUR:
    case MINO_TAIL_CALL:
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
        v = dyn_lookup(buf);
        if (v == NULL) v = mino_env_get(env, buf);
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
        return mino_vector(S_, tmp, n);
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
        return mino_map(S_, ks, vs, n);
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
        return mino_set(S_, tmp, n);
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
            gc_pin(mac);
            env_bind(env_root(env), buf, mac);
            gc_unpin(1);
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
            gc_pin(value);
            env_bind(env_root(env), buf, value);
            gc_unpin(1);
            meta_set(buf, doc, doc_len, form);
            return value;
        }
        if (sym_eq(head, "if")) {
            mino_val_t *cond_form;
            mino_val_t *then_form;
            mino_val_t *else_form = mino_nil(S_);
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
            /* Branch is tail position: propagate recur/tail-call. */
            return eval_impl(mino_is_truthy(cond) ? then_form : else_form,
                             env, tail);
        }
        if (sym_eq(head, "do")) {
            return eval_implicit_do_impl(args, env, tail);
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
                gc_pin(val);
                env_bind(local, buf, val);
                gc_unpin(1);
                bindings = rest_pair->as.cons.cdr;
            }
            return eval_implicit_do_impl(body, local, tail);
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
            if (evaled == NULL && mino_last_error(S_) != NULL) {
                return NULL;
            }
            r = alloc_val(MINO_RECUR);
            r->as.recur.args = evaled;
            return r;
        }
        if (sym_eq(head, "loop")) {
            mino_val_t *bindings;
            mino_val_t *body;
            mino_val_t *params      = mino_nil(S_);
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
                gc_pin(val);
                env_bind(local, buf, val);
                gc_unpin(1);
                cell = mino_cons(S_, name_form, mino_nil(S_));
                if (params_tail == NULL) {
                    params = cell;
                } else {
                    params_tail->as.cons.cdr = cell;
                }
                params_tail = cell;
                bindings = rest_pair->as.cons.cdr;
            }
            for (;;) {
                mino_val_t *result = eval_implicit_do_impl(body, local, tail);
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
                env_bind(local, var_buf, ex != NULL ? ex : mino_nil(S_));
                return eval_implicit_do(catch_body, local);
            }
        }

        /*
         * binding: (binding (name1 val1 name2 val2 ...) body...)
         * Pushes a dynamic binding frame, evaluates body forms, then pops
         * the frame (even on error, via setjmp/longjmp cleanup).
         */
        if (sym_eq(head, "binding")) {
            mino_val_t *pairs, *body, *result;
            dyn_frame_t frame;
            dyn_binding_t *bhead = NULL;
            if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.car)) {
                set_error_at(form, "binding requires a binding list and body");
                return NULL;
            }
            pairs = args->as.cons.car;
            body  = args->as.cons.cdr;
            /* Parse binding pairs. */
            while (pairs != NULL && pairs->type == MINO_CONS) {
                mino_val_t *sym_v, *val_form, *val;
                dyn_binding_t *b;
                char nbuf[256];
                size_t nlen;
                sym_v = pairs->as.cons.car;
                if (sym_v == NULL || sym_v->type != MINO_SYMBOL) {
                    set_error_at(form, "binding: names must be symbols");
                    /* Free any allocated bindings. */
                    while (bhead) { dyn_binding_t *n = bhead->next; free(bhead); bhead = n; }
                    return NULL;
                }
                nlen = sym_v->as.s.len;
                if (nlen >= sizeof(nbuf)) {
                    set_error_at(form, "binding: name too long");
                    while (bhead) { dyn_binding_t *n = bhead->next; free(bhead); bhead = n; }
                    return NULL;
                }
                memcpy(nbuf, sym_v->as.s.data, nlen);
                nbuf[nlen] = '\0';
                pairs = pairs->as.cons.cdr;
                if (pairs == NULL || pairs->type != MINO_CONS) {
                    set_error_at(form, "binding: odd number of forms in binding list");
                    while (bhead) { dyn_binding_t *n = bhead->next; free(bhead); bhead = n; }
                    return NULL;
                }
                val_form = pairs->as.cons.car;
                pairs    = pairs->as.cons.cdr;
                val = eval(val_form, env);
                if (val == NULL) {
                    while (bhead) { dyn_binding_t *n = bhead->next; free(bhead); bhead = n; }
                    return NULL;
                }
                b = (dyn_binding_t *)malloc(sizeof(*b));
                b->name = mino_symbol(S_, nbuf)->as.s.data; /* interned */
                b->val  = val;
                b->next = bhead;
                bhead   = b;
            }
            /* Push frame. */
            frame.bindings = bhead;
            frame.prev     = dyn_stack;
            dyn_stack      = &frame;
            result = eval_implicit_do(body, env);
            /* Pop frame. */
            dyn_stack = frame.prev;
            while (bhead) { dyn_binding_t *n = bhead->next; free(bhead); bhead = n; }
            return result;
        }

        if (sym_eq(head, "lazy-seq")) {
            mino_val_t *lz = alloc_val(MINO_LAZY);
            lz->as.lazy.body     = args;
            lz->as.lazy.env      = env;
            lz->as.lazy.cached   = NULL;
            lz->as.lazy.realized = 0;
            return lz;
        }

        /* Function or macro application. */
        {
            mino_val_t *fn = eval_value(head, env);
            mino_val_t *evaled;
            if (fn == NULL) {
                return NULL;
            }
            /* Pin fn: eval_args allocates, and the conservative stack
             * scanner may miss fn if the compiler keeps it in a register. */
            gc_pin(fn);
            if (fn->type == MINO_MACRO) {
                /* Expand with unevaluated args; re-eval the resulting form
                 * in the caller's environment. */
                mino_val_t *expanded = apply_callable(fn, args, env);
                gc_unpin(1);
                if (expanded == NULL) {
                    return NULL;
                }
                return eval_impl(expanded, env, tail);
            }
            if (fn->type != MINO_PRIM && fn->type != MINO_FN) {
                gc_unpin(1);
                {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "not a function (got %s)",
                             type_tag_str(fn));
                    set_error_at(form, msg);
                }
                return NULL;
            }
            evaled = eval_args(args, env);
            gc_unpin(1);
            if (evaled == NULL && mino_last_error(S_) != NULL) {
                return NULL;
            }
            /* Proper tail calls: in tail position, return a trampoline
             * sentinel instead of growing the C stack. */
            if (tail && fn->type == MINO_FN) {
                mino_val_t *tc = alloc_val(MINO_TAIL_CALL);
                tc->as.tail_call.fn   = fn;
                tc->as.tail_call.args = evaled;
                return tc;
            }
            return apply_callable(fn, evaled, env);
        }
    }
    }
    set_error("eval: unknown value type");
    return NULL;
}

static mino_val_t *eval(mino_val_t *form, mino_env_t *env)
{
    return eval_impl(form, env, 0);
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
            result = eval_implicit_do_impl(fn->as.fn.body, local, 1);
            if (result == NULL) {
                return NULL; /* leave frame for trace */
            }
            if (result->type == MINO_RECUR) {
                /* Self-recursion: rebind params and loop. */
                call_args = result->as.recur.args;
                continue;
            }
            if (result->type == MINO_TAIL_CALL) {
                /* Proper tail call: switch to the target function. */
                fn        = result->as.tail_call.fn;
                call_args = result->as.tail_call.args;
                local     = env_child(fn->as.fn.env);
                continue;
            }
            pop_frame();
            return result;
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

mino_val_t *mino_eval(mino_state_t *S, mino_val_t *form, mino_env_t *env)
{
    S_ = S;
    volatile char probe = 0;
    mino_val_t   *v;
    gc_note_host_frame((void *)&probe);
    (void)probe;
    eval_steps     = 0;
    limit_exceeded = 0;
    interrupted    = 0;
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
    if (v->type == MINO_TAIL_CALL) {
        set_error("tail call escaped to top level");
        call_depth = 0;
        return NULL;
    }
    call_depth = 0;
    return v;
}

mino_val_t *mino_eval_string(mino_state_t *S, const char *src, mino_env_t *env)
{
    S_ = S;
    volatile char   probe = 0;
    mino_val_t     *last  = mino_nil(S_);
    const char     *saved_file = reader_file;
    int             saved_line = reader_line;
    gc_note_host_frame((void *)&probe);
    (void)probe;
    eval_steps     = 0;
    limit_exceeded = 0;
    interrupted    = 0;
    if (reader_file == NULL) {
        reader_file = intern_filename("<string>");
    }
    reader_line = 1;
    while (*src != '\0') {
        const char *end  = NULL;
        mino_val_t *form = mino_read(S_, src, &end);
        if (form == NULL) {
            if (mino_last_error(S_) != NULL) {
                reader_file = saved_file;
                reader_line = saved_line;
                return NULL;
            }
            break; /* EOF */
        }
        last = mino_eval(S_, form, env);
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

mino_val_t *mino_load_file(mino_state_t *S, const char *path, mino_env_t *env)
{
    S_ = S;
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
    result = mino_eval_string(S_, buf, env);
    reader_file = saved_file;
    free(buf);
    return result;
}

mino_env_t *mino_new(mino_state_t *S)
{
    S_ = S;
    mino_env_t *env = mino_env_new(S_);
    mino_install_core(S_, env);
    mino_install_io(S_, env);
    return env;
}

void mino_register_fn(mino_state_t *S, mino_env_t *env, const char *name, mino_prim_fn fn)
{
    S_ = S;
    mino_env_set(S_, env, name, mino_prim(S_, name, fn));
}

mino_val_t *mino_call(mino_state_t *S, mino_val_t *fn, mino_val_t *args, mino_env_t *env)
{
    S_ = S;
    volatile char probe = 0;
    gc_note_host_frame((void *)&probe);
    (void)probe;
    return apply_callable(fn, args, env);
}

int mino_pcall(mino_state_t *S, mino_val_t *fn, mino_val_t *args, mino_env_t *env,
               mino_val_t **out)
{
    S_ = S;
    mino_val_t *result = mino_call(S_, fn, args, env);
    if (out != NULL) {
        *out = result;
    }
    return result == NULL ? -1 : 0;
}

void mino_set_limit(mino_state_t *S, int kind, size_t value)
{
    S_ = S;
    switch (kind) {
    case MINO_LIMIT_STEPS: limit_steps = value; break;
    case MINO_LIMIT_HEAP:  limit_heap  = value; break;
    default: break;
    }
}

void mino_interrupt(mino_state_t *S)
{
    /* Write directly to avoid S_ (may be in use by another thread). */
#undef interrupted
    S->interrupted = 1;
#define interrupted (S_->interrupted)
}

/* ------------------------------------------------------------------------- */
/* In-process REPL handle                                                    */
/* ------------------------------------------------------------------------- */

struct mino_repl {
    mino_state_t *state;
    mino_env_t   *env;
    char         *buf;
    size_t        len;
    size_t        cap;
};

mino_repl_t *mino_repl_new(mino_state_t *S, mino_env_t *env)
{
    S_ = S;
    mino_repl_t *r = (mino_repl_t *)malloc(sizeof(*r));
    if (r == NULL) { return NULL; }
    r->state = S;
    r->env   = env;
    r->buf   = NULL;
    r->len   = 0;
    r->cap   = 0;
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
    S_ = repl->state;

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
    form   = mino_read(S_, cursor, &end);
    if (form == NULL) {
        const char *err = mino_last_error(S_);
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
    result = mino_eval(S_, form, repl->env);
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
        return mino_float(S_, acc);
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
        return mino_int(S_, acc);
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
            return mino_float(S_, -acc);
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
        return mino_float(S_, acc);
    } else {
        long long acc;
        if (!as_long(args->as.cons.car, &acc)) {
            set_error("- expects numbers");
            return NULL;
        }
        args = args->as.cons.cdr;
        if (!mino_is_cons(args)) {
            return mino_int(S_, -acc);
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
        return mino_int(S_, acc);
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
        return mino_float(S_, acc);
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
        return mino_int(S_, acc);
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
        return mino_float(S_, 1.0 / acc);
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
    return mino_float(S_, acc);
}

static mino_val_t *prim_mod(mino_val_t *args, mino_env_t *env)
{
    double a, b, r;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("mod requires two arguments");
        return NULL;
    }
    if (!as_double(args->as.cons.car, &a) ||
        !as_double(args->as.cons.cdr->as.cons.car, &b)) {
        set_error("mod expects numbers");
        return NULL;
    }
    if (b == 0.0) {
        set_error("mod: division by zero");
        return NULL;
    }
    r = fmod(a, b);
    /* Floored modulo: result has same sign as divisor. */
    if (r != 0.0 && ((r < 0.0) != (b < 0.0))) r += b;
    /* Return int if both args are ints. */
    if (args->as.cons.car->type == MINO_INT &&
        args->as.cons.cdr->as.cons.car->type == MINO_INT) {
        return mino_int(S_, (long long)r);
    }
    return mino_float(S_, r);
}

static mino_val_t *prim_rem(mino_val_t *args, mino_env_t *env)
{
    double a, b, r;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("rem requires two arguments");
        return NULL;
    }
    if (!as_double(args->as.cons.car, &a) ||
        !as_double(args->as.cons.cdr->as.cons.car, &b)) {
        set_error("rem expects numbers");
        return NULL;
    }
    if (b == 0.0) {
        set_error("rem: division by zero");
        return NULL;
    }
    r = fmod(a, b);
    if (args->as.cons.car->type == MINO_INT &&
        args->as.cons.cdr->as.cons.car->type == MINO_INT) {
        return mino_int(S_, (long long)r);
    }
    return mino_float(S_, r);
}

static mino_val_t *prim_quot(mino_val_t *args, mino_env_t *env)
{
    double a, b, q;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("quot requires two arguments");
        return NULL;
    }
    if (!as_double(args->as.cons.car, &a) ||
        !as_double(args->as.cons.cdr->as.cons.car, &b)) {
        set_error("quot expects numbers");
        return NULL;
    }
    if (b == 0.0) {
        set_error("quot: division by zero");
        return NULL;
    }
    q = a / b;
    q = q >= 0 ? floor(q) : ceil(q);
    if (args->as.cons.car->type == MINO_INT &&
        args->as.cons.cdr->as.cons.car->type == MINO_INT) {
        return mino_int(S_, (long long)q);
    }
    return mino_float(S_, q);
}

/* --- Math functions (thin wrappers around math.h) --- */

#define MATH_UNARY(cname, cfn, label)                                  \
    static mino_val_t *cname(mino_val_t *args, mino_env_t *env)        \
    {                                                                   \
        double x;                                                       \
        (void)env;                                                      \
        if (!mino_is_cons(args) ||                                      \
            mino_is_cons(args->as.cons.cdr)) {                          \
            set_error(label " requires one argument");                  \
            return NULL;                                                \
        }                                                               \
        if (!as_double(args->as.cons.car, &x)) {                       \
            set_error(label " expects a number");                       \
            return NULL;                                                \
        }                                                               \
        return mino_float(S_, cfn(x));                                      \
    }

MATH_UNARY(prim_math_floor, floor, "math-floor")
MATH_UNARY(prim_math_ceil,  ceil,  "math-ceil")
MATH_UNARY(prim_math_round, round, "math-round")
MATH_UNARY(prim_math_sqrt,  sqrt,  "math-sqrt")
MATH_UNARY(prim_math_log,   log,   "math-log")
MATH_UNARY(prim_math_exp,   exp,   "math-exp")
MATH_UNARY(prim_math_sin,   sin,   "math-sin")
MATH_UNARY(prim_math_cos,   cos,   "math-cos")
MATH_UNARY(prim_math_tan,   tan,   "math-tan")

#undef MATH_UNARY

static mino_val_t *prim_math_pow(mino_val_t *args, mino_env_t *env)
{
    double base, exponent;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("math-pow requires two arguments");
        return NULL;
    }
    if (!as_double(args->as.cons.car, &base) ||
        !as_double(args->as.cons.cdr->as.cons.car, &exponent)) {
        set_error("math-pow expects numbers");
        return NULL;
    }
    return mino_float(S_, pow(base, exponent));
}

static mino_val_t *prim_math_atan2(mino_val_t *args, mino_env_t *env)
{
    double y, x;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("math-atan2 requires two arguments");
        return NULL;
    }
    if (!as_double(args->as.cons.car, &y) ||
        !as_double(args->as.cons.cdr->as.cons.car, &x)) {
        set_error("math-atan2 expects numbers");
        return NULL;
    }
    return mino_float(S_, atan2(y, x));
}

static mino_val_t *prim_bit_and(mino_val_t *args, mino_env_t *env)
{
    long long a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("bit-and requires two arguments");
        return NULL;
    }
    if (!as_long(args->as.cons.car, &a) ||
        !as_long(args->as.cons.cdr->as.cons.car, &b)) {
        set_error("bit-and expects integers");
        return NULL;
    }
    return mino_int(S_, a & b);
}

static mino_val_t *prim_bit_or(mino_val_t *args, mino_env_t *env)
{
    long long a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("bit-or requires two arguments");
        return NULL;
    }
    if (!as_long(args->as.cons.car, &a) ||
        !as_long(args->as.cons.cdr->as.cons.car, &b)) {
        set_error("bit-or expects integers");
        return NULL;
    }
    return mino_int(S_, a | b);
}

static mino_val_t *prim_bit_xor(mino_val_t *args, mino_env_t *env)
{
    long long a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("bit-xor requires two arguments");
        return NULL;
    }
    if (!as_long(args->as.cons.car, &a) ||
        !as_long(args->as.cons.cdr->as.cons.car, &b)) {
        set_error("bit-xor expects integers");
        return NULL;
    }
    return mino_int(S_, a ^ b);
}

static mino_val_t *prim_bit_not(mino_val_t *args, mino_env_t *env)
{
    long long a;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("bit-not requires one argument");
        return NULL;
    }
    if (!as_long(args->as.cons.car, &a)) {
        set_error("bit-not expects an integer");
        return NULL;
    }
    return mino_int(S_, ~a);
}

static mino_val_t *prim_bit_shift_left(mino_val_t *args, mino_env_t *env)
{
    long long a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("bit-shift-left requires two arguments");
        return NULL;
    }
    if (!as_long(args->as.cons.car, &a) ||
        !as_long(args->as.cons.cdr->as.cons.car, &b)) {
        set_error("bit-shift-left expects integers");
        return NULL;
    }
    return mino_int(S_, a << b);
}

static mino_val_t *prim_bit_shift_right(mino_val_t *args, mino_env_t *env)
{
    long long a, b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("bit-shift-right requires two arguments");
        return NULL;
    }
    if (!as_long(args->as.cons.car, &a) ||
        !as_long(args->as.cons.cdr->as.cons.car, &b)) {
        set_error("bit-shift-right expects integers");
        return NULL;
    }
    return mino_int(S_, a >> b);
}

static mino_val_t *prim_int(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("int requires one argument");
        return NULL;
    }
    v = args->as.cons.car;
    if (v != NULL && v->type == MINO_INT) return v;
    if (v != NULL && v->type == MINO_FLOAT) return mino_int(S_, (long long)v->as.f);
    set_error("int: expected a number");
    return NULL;
}

static mino_val_t *prim_float(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("float requires one argument");
        return NULL;
    }
    v = args->as.cons.car;
    if (v != NULL && v->type == MINO_FLOAT) return v;
    if (v != NULL && v->type == MINO_INT) return mino_float(S_, (double)v->as.i);
    set_error("float: expected a number");
    return NULL;
}

/*
 * Helper: print a value to a string buffer using the standard printer.
 * Returns a mino string. Uses tmpfile() for ANSI C portability.
 */
static mino_val_t *print_to_string(const mino_val_t *v)
{
    FILE  *f = tmpfile();
    long   n;
    char  *buf;
    mino_val_t *result;
    if (f == NULL) {
        set_error("pr-str: tmpfile failed");
        return NULL;
    }
    mino_print_to(S_, f, v);
    n = ftell(f);
    if (n < 0) n = 0;
    rewind(f);
    buf = (char *)malloc((size_t)n + 1);
    if (buf == NULL) {
        fclose(f);
        set_error("out of memory");
        return NULL;
    }
    if (n > 0) {
        size_t rd = fread(buf, 1, (size_t)n, f);
        (void)rd;
    }
    buf[n] = '\0';
    fclose(f);
    result = mino_string_n(S_, buf, (size_t)n);
    free(buf);
    return result;
}

/*
 * (format fmt & args) — simple string formatting.
 * Directives: %s (str of arg), %d (integer), %f (float), %% (literal %).
 */
static mino_val_t *prim_format(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *fmt_val;
    const char *fmt;
    size_t      fmt_len;
    mino_val_t *arg_list;
    char  *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    size_t i;
    (void)env;
    if (!mino_is_cons(args)) {
        set_error("format requires at least a format string");
        return NULL;
    }
    fmt_val = args->as.cons.car;
    if (fmt_val == NULL || fmt_val->type != MINO_STRING) {
        set_error("format: first argument must be a string");
        return NULL;
    }
    fmt     = fmt_val->as.s.data;
    fmt_len = fmt_val->as.s.len;
    arg_list = args->as.cons.cdr;

#define FMT_ENSURE(extra) do { \
        size_t _need = len + (extra) + 1; \
        if (_need > cap) { \
            cap = cap == 0 ? 128 : cap; \
            while (cap < _need) cap *= 2; \
            buf = (char *)realloc(buf, cap); \
            if (buf == NULL) { set_error("out of memory"); return NULL; } \
        } \
    } while (0)

    for (i = 0; i < fmt_len; i++) {
        if (fmt[i] == '%' && i + 1 < fmt_len) {
            char spec = fmt[i + 1];
            i++;
            if (spec == '%') {
                FMT_ENSURE(1);
                buf[len++] = '%';
            } else if (spec == 's') {
                mino_val_t *a;
                if (!mino_is_cons(arg_list)) {
                    free(buf);
                    set_error("format: not enough arguments for format string");
                    return NULL;
                }
                a = arg_list->as.cons.car;
                arg_list = arg_list->as.cons.cdr;
                if (a != NULL && a->type == MINO_STRING) {
                    FMT_ENSURE(a->as.s.len);
                    memcpy(buf + len, a->as.s.data, a->as.s.len);
                    len += a->as.s.len;
                } else {
                    mino_val_t *s = print_to_string(a);
                    if (s == NULL) { free(buf); return NULL; }
                    FMT_ENSURE(s->as.s.len);
                    memcpy(buf + len, s->as.s.data, s->as.s.len);
                    len += s->as.s.len;
                }
            } else if (spec == 'd') {
                long long n;
                char tmp[32];
                int  tn;
                if (!mino_is_cons(arg_list)) {
                    free(buf);
                    set_error("format: not enough arguments for format string");
                    return NULL;
                }
                if (!as_long(arg_list->as.cons.car, &n)) {
                    double d;
                    if (as_double(arg_list->as.cons.car, &d)) {
                        n = (long long)d;
                    } else {
                        free(buf);
                        set_error("format: %d expects a number");
                        return NULL;
                    }
                }
                arg_list = arg_list->as.cons.cdr;
                tn = snprintf(tmp, sizeof(tmp), "%lld", n);
                FMT_ENSURE((size_t)tn);
                memcpy(buf + len, tmp, (size_t)tn);
                len += (size_t)tn;
            } else if (spec == 'f') {
                double d;
                char tmp[64];
                int  tn;
                if (!mino_is_cons(arg_list)) {
                    free(buf);
                    set_error("format: not enough arguments for format string");
                    return NULL;
                }
                if (!as_double(arg_list->as.cons.car, &d)) {
                    free(buf);
                    set_error("format: %f expects a number");
                    return NULL;
                }
                arg_list = arg_list->as.cons.cdr;
                tn = snprintf(tmp, sizeof(tmp), "%f", d);
                FMT_ENSURE((size_t)tn);
                memcpy(buf + len, tmp, (size_t)tn);
                len += (size_t)tn;
            } else {
                /* Unknown directive: emit literal. */
                FMT_ENSURE(2);
                buf[len++] = '%';
                buf[len++] = spec;
            }
        } else {
            FMT_ENSURE(1);
            buf[len++] = fmt[i];
        }
    }
#undef FMT_ENSURE
    {
        mino_val_t *result = mino_string_n(S_, buf != NULL ? buf : "", len);
        free(buf);
        return result;
    }
}

static mino_val_t *prim_read_string(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s;
    mino_val_t *result;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("read-string requires one string argument");
        return NULL;
    }
    s = args->as.cons.car;
    if (s == NULL || s->type != MINO_STRING) {
        set_error("read-string: argument must be a string");
        return NULL;
    }
    clear_error();
    result = mino_read(S_, s->as.s.data, NULL);
    if (result == NULL && mino_last_error(S_) != NULL) {
        /* Throw parse errors as catchable exceptions so user code can
         * handle them via try/catch. */
        mino_val_t *ex = mino_string(S_, mino_last_error(S_));
        if (try_depth > 0) {
            try_stack[try_depth - 1].exception = ex;
            longjmp(try_stack[try_depth - 1].buf, 1);
        }
        /* No enclosing try — propagate as fatal error. */
        {
            char msg[512];
            snprintf(msg, sizeof(msg), "unhandled exception: %.*s",
                     (int)ex->as.s.len, ex->as.s.data);
            set_error(msg);
        }
        return NULL;
    }
    return result != NULL ? result : mino_nil(S_);
}

static mino_val_t *prim_pr_str(mino_val_t *args, mino_env_t *env)
{
    char  *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    int    first = 1;
    (void)env;
    while (mino_is_cons(args)) {
        mino_val_t *printed = print_to_string(args->as.cons.car);
        size_t      need;
        if (printed == NULL) {
            free(buf);
            return NULL;
        }
        need = len + (!first ? 1 : 0) + printed->as.s.len + 1;
        if (need > cap) {
            cap = cap == 0 ? 128 : cap;
            while (cap < need) cap *= 2;
            buf = (char *)realloc(buf, cap);
            if (buf == NULL) { set_error("out of memory"); return NULL; }
        }
        if (!first) buf[len++] = ' ';
        memcpy(buf + len, printed->as.s.data, printed->as.s.len);
        len += printed->as.s.len;
        first = 0;
        args = args->as.cons.cdr;
    }
    {
        mino_val_t *result = mino_string_n(S_, buf != NULL ? buf : "", len);
        free(buf);
        return result;
    }
}

static mino_val_t *prim_char_at(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *s;
    long long   idx;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("char-at requires two arguments");
        return NULL;
    }
    s = args->as.cons.car;
    if (s == NULL || s->type != MINO_STRING) {
        set_error("char-at: first argument must be a string");
        return NULL;
    }
    if (!as_long(args->as.cons.cdr->as.cons.car, &idx)) {
        set_error("char-at: second argument must be an integer");
        return NULL;
    }
    if (idx < 0 || (size_t)idx >= s->as.s.len) {
        set_error("char-at: index out of range");
        return NULL;
    }
    return mino_string_n(S_, s->as.s.data + idx, 1);
}

static mino_val_t *prim_name(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("name requires one argument");
        return NULL;
    }
    v = args->as.cons.car;
    if (v == NULL || v->type == MINO_NIL) return mino_nil(S_);
    if (v->type == MINO_STRING)  return v;
    if (v->type == MINO_KEYWORD) return mino_string_n(S_, v->as.s.data, v->as.s.len);
    if (v->type == MINO_SYMBOL)  return mino_string_n(S_, v->as.s.data, v->as.s.len);
    set_error("name: expected a keyword, symbol, or string");
    return NULL;
}

/* (rand) — return a random float in [0.0, 1.0). */
static mino_val_t *prim_rand(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (mino_is_cons(args)) {
        set_error("rand takes no arguments");
        return NULL;
    }
    if (!rand_seeded) {
        srand((unsigned int)time(NULL));
        rand_seeded = 1;
    }
    return mino_float(S_, (double)rand() / ((double)RAND_MAX + 1.0));
}

/* (eval form) — evaluate a form at runtime. */
static mino_val_t *prim_eval(mino_val_t *args, mino_env_t *env)
{
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("eval requires one argument");
        return NULL;
    }
    return eval(args->as.cons.car, env);
}

/* (symbol str) — create a symbol from a string. */
static mino_val_t *prim_symbol(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("symbol requires one string argument");
        return NULL;
    }
    v = args->as.cons.car;
    if (v == NULL || v->type != MINO_STRING) {
        set_error("symbol: argument must be a string");
        return NULL;
    }
    return mino_symbol_n(S_, v->as.s.data, v->as.s.len);
}

/* (keyword str) — create a keyword from a string. */
static mino_val_t *prim_keyword(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("keyword requires one string argument");
        return NULL;
    }
    v = args->as.cons.car;
    if (v == NULL || v->type != MINO_STRING) {
        set_error("keyword: argument must be a string");
        return NULL;
    }
    return mino_keyword_n(S_, v->as.s.data, v->as.s.len);
}

/* (hash val) — return the integer hash code of any value. */
static mino_val_t *prim_hash(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("hash requires one argument");
        return NULL;
    }
    return mino_int(S_, (long long)hash_val(args->as.cons.car));
}

/* (compare a b) — general comparison returning -1, 0, or 1.
 * Compares numbers, strings, keywords, symbols, and nil. */
static mino_val_t *prim_compare(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *a, *b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("compare requires two arguments");
        return NULL;
    }
    a = args->as.cons.car;
    b = args->as.cons.cdr->as.cons.car;
    /* nil sorts before everything */
    if ((a == NULL || (a->type == MINO_NIL)) &&
        (b == NULL || (b->type == MINO_NIL))) return mino_int(S_, 0);
    if (a == NULL || a->type == MINO_NIL) return mino_int(S_, -1);
    if (b == NULL || b->type == MINO_NIL) return mino_int(S_, 1);
    /* numbers */
    {
        double da, db;
        if (as_double(a, &da) && as_double(b, &db)) {
            return mino_int(S_, da < db ? -1 : da > db ? 1 : 0);
        }
    }
    /* strings, keywords, symbols — lexicographic */
    if ((a->type == MINO_STRING || a->type == MINO_KEYWORD ||
         a->type == MINO_SYMBOL) && a->type == b->type) {
        int cmp = strcmp(a->as.s.data, b->as.s.data);
        return mino_int(S_, cmp < 0 ? -1 : cmp > 0 ? 1 : 0);
    }
    set_error("compare: cannot compare values of different types");
    return NULL;
}

static mino_val_t *prim_eq(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args)) {
        return mino_true(S_);
    }
    {
        mino_val_t *first = args->as.cons.car;
        args = args->as.cons.cdr;
        while (mino_is_cons(args)) {
            if (!mino_eq(first, args->as.cons.car)) {
                return mino_false(S_);
            }
            args = args->as.cons.cdr;
        }
    }
    return mino_true(S_);
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
        return mino_true(S_);
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
            return mino_false(S_);
        }
        prev = x;
        args = args->as.cons.cdr;
    }
    return mino_true(S_);
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
    return mino_cons(S_, args->as.cons.car, args->as.cons.cdr->as.cons.car);
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
    while (list != NULL && list->type == MINO_LAZY) {
        list = lazy_force(list);
    }
    while (mino_is_cons(list)) {
        n++;
        list = list->as.cons.cdr;
        /* Force lazy tails. */
        while (list != NULL && list->type == MINO_LAZY) {
            list = lazy_force(list);
        }
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
        return mino_int(S_, 0);
    }
    switch (coll->type) {
    case MINO_CONS:   return mino_int(S_, (long long)list_length(coll));
    case MINO_VECTOR: return mino_int(S_, (long long)coll->as.vec.len);
    case MINO_MAP:    return mino_int(S_, (long long)coll->as.map.len);
    case MINO_SET:    return mino_int(S_, (long long)coll->as.set.len);
    case MINO_STRING: return mino_int(S_, (long long)coll->as.s.len);
    case MINO_LAZY: {
        /* Force the entire lazy seq and count it. */
        mino_val_t *forced = lazy_force(coll);
        if (forced == NULL) return NULL;
        if (forced->type == MINO_NIL) return mino_int(S_, 0);
        return mino_int(S_, (long long)list_length(forced));
    }
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
        return mino_vector(S_, NULL, 0);
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
    return mino_vector(S_, tmp, n);
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
        return mino_map(S_, NULL, NULL, 0);
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
    return mino_map(S_, ks, vs, pairs);
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
        return mino_nil(S_);
    }
    if (coll->type == MINO_CONS) {
        return coll->as.cons.car;
    }
    if (coll->type == MINO_VECTOR) {
        if (coll->as.vec.len == 0) {
            return mino_nil(S_);
        }
        return vec_nth(coll, 0);
    }
    if (coll->type == MINO_LAZY) {
        mino_val_t *s = lazy_force(coll);
        if (s == NULL) return NULL;
        if (s->type == MINO_NIL || s == NULL) return mino_nil(S_);
        if (s->type == MINO_CONS) return s->as.cons.car;
        return mino_nil(S_);
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
        return mino_nil(S_);
    }
    if (coll->type == MINO_CONS) {
        return coll->as.cons.cdr;
    }
    if (coll->type == MINO_VECTOR) {
        /* Rest of a vector is a list of the trailing elements. v0.11 will
         * promote this to a seq abstraction. */
        mino_val_t *head = mino_nil(S_);
        mino_val_t *tail = NULL;
        size_t i;
        for (i = 1; i < coll->as.vec.len; i++) {
            mino_val_t *cell = mino_cons(S_, vec_nth(coll, i), mino_nil(S_));
            if (tail == NULL) {
                head = cell;
            } else {
                tail->as.cons.cdr = cell;
            }
            tail = cell;
        }
        return head;
    }
    if (coll->type == MINO_LAZY) {
        mino_val_t *s = lazy_force(coll);
        if (s == NULL) return NULL;
        if (s->type == MINO_NIL || s == NULL) return mino_nil(S_);
        if (s->type == MINO_CONS) return s->as.cons.cdr;
        return mino_nil(S_);
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
        order   = mino_vector(S_, NULL, 0);
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
    mino_val_t *def_val = mino_nil(S_);
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
            ? mino_nil(S_) : coll;
        while (mino_is_cons(p)) {
            out = mino_cons(S_, p->as.cons.car, out);
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
            pair_args = mino_cons(S_, vec_nth(item, 0),
                                   mino_cons(S_, vec_nth(item, 1), mino_nil(S_)));
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

static mino_val_t *prim_keys(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *head = mino_nil(S_);
    mino_val_t *tail = NULL;
    size_t i;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("keys requires one argument");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S_);
    }
    if (coll->type != MINO_MAP) {
        set_error("keys: argument must be a map");
        return NULL;
    }
    for (i = 0; i < coll->as.map.len; i++) {
        mino_val_t *cell = mino_cons(S_, vec_nth(coll->as.map.key_order, i),
                                      mino_nil(S_));
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
    mino_val_t *head = mino_nil(S_);
    mino_val_t *tail = NULL;
    size_t i;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("vals requires one argument");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S_);
    }
    if (coll->type != MINO_MAP) {
        set_error("vals: argument must be a map");
        return NULL;
    }
    for (i = 0; i < coll->as.map.len; i++) {
        mino_val_t *key  = vec_nth(coll->as.map.key_order, i);
        mino_val_t *cell = mino_cons(S_, map_get_val(coll, key), mino_nil(S_));
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
    mino_val_t       *sentinel = mino_true(S_);
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
    return mino_set(S_, tmp, n);
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
        return mino_false(S_);
    }
    if (coll->type == MINO_MAP) {
        return map_get_val(coll, key) != NULL ? mino_true(S_) : mino_false(S_);
    }
    if (coll->type == MINO_SET) {
        uint32_t h = hash_val(key);
        return hamt_get(coll->as.set.root, key, h, 0u) != NULL
            ? mino_true(S_) : mino_false(S_);
    }
    if (coll->type == MINO_VECTOR) {
        /* For vectors, key is an index. */
        if (key != NULL && key->type == MINO_INT) {
            long long idx = key->as.i;
            return (idx >= 0 && (size_t)idx < coll->as.vec.len)
                ? mino_true(S_) : mino_false(S_);
        }
        return mino_false(S_);
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
        return mino_nil(S_);
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
            mino_val_t *order   = mino_vector(S_, NULL, 0);
            mino_hamt_node_t *root = NULL;
            size_t i;
            size_t new_len = 0;
            for (i = 0; i < coll->as.set.len; i++) {
                mino_val_t *elem = vec_nth(coll->as.set.key_order, i);
                if (!mino_eq(elem, key)) {
                    hamt_entry_t *e2 = hamt_entry_new(elem, mino_true(S_));
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

static mino_val_t *prim_dissoc(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *p;
    size_t      n;
    (void)env;
    arg_count(args, &n);
    if (n < 2) {
        set_error("dissoc requires a map and at least one key");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S_);
    }
    if (coll->type != MINO_MAP) {
        set_error("dissoc: first argument must be a map");
        return NULL;
    }
    p = args->as.cons.cdr;
    while (mino_is_cons(p)) {
        mino_val_t *key = p->as.cons.car;
        uint32_t    h   = hash_val(key);
        if (hamt_get(coll->as.map.root, key, h, 0u) != NULL) {
            mino_val_t *new_map = alloc_val(MINO_MAP);
            mino_val_t *order   = mino_vector(S_, NULL, 0);
            mino_hamt_node_t *root = NULL;
            size_t i;
            size_t new_len = 0;
            for (i = 0; i < coll->as.map.len; i++) {
                mino_val_t *k = vec_nth(coll->as.map.key_order, i);
                if (!mino_eq(k, key)) {
                    mino_val_t   *v  = map_get_val(coll, k);
                    hamt_entry_t *e2 = hamt_entry_new(k, v);
                    uint32_t      h2 = hash_val(k);
                    int rep = 0;
                    root = hamt_assoc(root, e2, h2, 0u, &rep);
                    order = vec_conj1(order, k);
                    new_len++;
                }
            }
            new_map->as.map.root      = root;
            new_map->as.map.key_order = order;
            new_map->as.map.len       = new_len;
            coll = new_map;
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
    /* Force lazy seqs so they behave as cons lists. */
    if (coll != NULL && coll->type == MINO_LAZY) {
        coll = lazy_force((mino_val_t *)coll);
    }
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
        return mino_vector(S_, kv, 2);
    }
    case MINO_SET:    return vec_nth(c->as.set.key_order, it->idx);
    case MINO_STRING: return mino_string_n(S_, c->as.s.data + it->idx, 1);
    default:          return mino_nil(S_);
    }
}

static void seq_iter_next(seq_iter_t *it)
{
    if (it->coll != NULL && it->coll->type == MINO_CONS) {
        if (it->cons_p != NULL && it->cons_p->type == MINO_CONS) {
            const mino_val_t *next = it->cons_p->as.cons.cdr;
            /* Force lazy tail if present. */
            if (next != NULL && next->type == MINO_LAZY) {
                next = lazy_force((mino_val_t *)next);
            }
            it->cons_p = next;
        }
    } else {
        it->idx++;
    }
}

/* (map, filter are now lazy in core.mino) */

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
            return apply_callable(fn, mino_nil(S_), env);
        }
        seq_iter_init(&it, coll);
        if (seq_iter_done(&it)) {
            return apply_callable(fn, mino_nil(S_), env);
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
        mino_val_t *call_a = mino_cons(S_, acc, mino_cons(S_, elem, mino_nil(S_)));
        acc = apply_callable(fn, call_a, env);
        if (acc == NULL) return NULL;
        seq_iter_next(&it);
    }
    return acc;
}

/* (take, drop, range, repeat, concat are now lazy in core.mino) */

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
        mino_val_t *out = mino_nil(S_);
        seq_iter_init(&it, from);
        while (!seq_iter_done(&it)) {
            out = mino_cons(S_, seq_iter_val(&it), out);
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
            pair_args = mino_cons(S_, vec_nth(item, 0),
                                   mino_cons(S_, vec_nth(item, 1), mino_nil(S_)));
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
            out = mino_cons(S_, seq_iter_val(&it), out);
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
        mino_val_t *head = mino_nil(S_);
        mino_val_t *tail2 = NULL;
        p = args->as.cons.cdr;
        /* Collect all but the last arg as individual args. */
        while (mino_is_cons(p) && mino_is_cons(p->as.cons.cdr)) {
            mino_val_t *cell = mino_cons(S_, p->as.cons.car, mino_nil(S_));
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
                mino_val_t *cell = mino_cons(S_, seq_iter_val(&it), mino_nil(S_));
                if (tail2 == NULL) { head = cell; } else { tail2->as.cons.cdr = cell; }
                tail2 = cell;
                seq_iter_next(&it);
            }
        }
        return apply_callable(fn, head, env);
    }
    /* (apply f coll) — convert coll to a cons arg list. */
    if (last == NULL || last->type == MINO_NIL) {
        return apply_callable(fn, mino_nil(S_), env);
    }
    if (last->type == MINO_CONS) {
        return apply_callable(fn, last, env);
    }
    /* Convert non-list collection to cons list. */
    {
        mino_val_t *head = mino_nil(S_);
        mino_val_t *tail2 = NULL;
        seq_iter_t it;
        seq_iter_init(&it, last);
        while (!seq_iter_done(&it)) {
            mino_val_t *cell = mino_cons(S_, seq_iter_val(&it), mino_nil(S_));
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
    mino_val_t *out = mino_nil(S_);
    seq_iter_t  it;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("reverse requires one argument");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S_);
    }
    seq_iter_init(&it, coll);
    while (!seq_iter_done(&it)) {
        out = mino_cons(S_, seq_iter_val(&it), out);
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

/* Sort comparator state: when sort_comp_fn is non-NULL, the merge sort
 * calls the user-supplied comparison function instead of val_compare. */

static int sort_compare(const mino_val_t *a, const mino_val_t *b)
{
    if (sort_comp_fn != NULL) {
        mino_val_t *call_args = mino_cons(S_, (mino_val_t *)a,
                                  mino_cons(S_, (mino_val_t *)b, mino_nil(S_)));
        mino_val_t *result = mino_call(S_, sort_comp_fn, call_args, sort_comp_env);
        if (result == NULL) return 0;
        /* Numeric result: use sign directly (compare-style) */
        if (result->type == MINO_INT) {
            return result->as.i < 0 ? -1 : result->as.i > 0 ? 1 : 0;
        }
        if (result->type == MINO_FLOAT) {
            return result->as.f < 0 ? -1 : result->as.f > 0 ? 1 : 0;
        }
        /* Boolean result: true means a < b, false means a >= b */
        return mino_is_truthy(result) ? -1 : 1;
    }
    return val_compare(a, b);
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
        if (sort_compare(tmp[i], arr[j]) <= 0) {
            arr[k++] = tmp[i++];
        } else {
            arr[k++] = arr[j++];
        }
    }
    while (i < mid) { arr[k++] = tmp[i++]; }
}

/* (sort coll) or (sort comp coll) */
static mino_val_t *prim_sort(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    mino_val_t *comp = NULL;
    mino_val_t **arr;
    mino_val_t **tmp;
    mino_val_t *head = mino_nil(S_);
    mino_val_t *tail = NULL;
    size_t      n_items, i;
    seq_iter_t  it;
    if (!mino_is_cons(args)) {
        set_error("sort requires one or two arguments");
        return NULL;
    }
    if (mino_is_cons(args->as.cons.cdr) &&
        !mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        /* Two args: (sort comp coll) */
        comp = args->as.cons.car;
        coll = args->as.cons.cdr->as.cons.car;
    } else if (!mino_is_cons(args->as.cons.cdr)) {
        /* One arg: (sort coll) */
        coll = args->as.cons.car;
    } else {
        set_error("sort requires one or two arguments");
        return NULL;
    }
    if (coll == NULL || coll->type == MINO_NIL) {
        return mino_nil(S_);
    }
    /* Collect elements into an array. */
    n_items = 0;
    seq_iter_init(&it, coll);
    while (!seq_iter_done(&it)) { n_items++; seq_iter_next(&it); }
    if (n_items == 0) return mino_nil(S_);
    arr = (mino_val_t **)gc_alloc_typed(GC_T_VALARR, n_items * sizeof(*arr));
    tmp = (mino_val_t **)gc_alloc_typed(GC_T_VALARR, n_items * sizeof(*tmp));
    i = 0;
    seq_iter_init(&it, coll);
    while (!seq_iter_done(&it)) { arr[i++] = seq_iter_val(&it); seq_iter_next(&it); }
    sort_comp_fn  = comp;
    sort_comp_env = env;
    merge_sort_vals(arr, tmp, n_items);
    sort_comp_fn  = NULL;
    sort_comp_env = NULL;
    for (i = 0; i < n_items; i++) {
        mino_val_t *cell = mino_cons(S_, arr[i], mino_nil(S_));
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
    return mino_string_n(S_, s_val->as.s.data + start, (size_t)(end_idx - start));
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
            buf[i] = mino_string_n(S_, s + i, 1);
        }
        return mino_vector(S_, buf, slen);
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
            buf[len++] = mino_string_n(S_, p, (size_t)(found - p));
            p = found + sep_len;
        } else {
            buf[len++] = mino_string_n(S_, p, (size_t)(s + slen - p));
            break;
        }
    }
    return mino_vector(S_, buf, len);
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
        return mino_string(S_, "");
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
            mino_val_t *str_a = mino_cons(S_, elem, mino_nil(S_));
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
        mino_val_t *result = mino_string_n(S_, buf != NULL ? buf : "", buf_len);
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
    if (prefix->as.s.len > s->as.s.len) return mino_false(S_);
    return memcmp(s->as.s.data, prefix->as.s.data, prefix->as.s.len) == 0
        ? mino_true(S_) : mino_false(S_);
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
    if (suffix->as.s.len > s->as.s.len) return mino_false(S_);
    return memcmp(s->as.s.data + s->as.s.len - suffix->as.s.len,
                  suffix->as.s.data, suffix->as.s.len) == 0
        ? mino_true(S_) : mino_false(S_);
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
    if (sub->as.s.len == 0) return mino_true(S_);
    if (sub->as.s.len > s->as.s.len) return mino_false(S_);
    for (p = s->as.s.data; p + sub->as.s.len <= s->as.s.data + s->as.s.len; p++) {
        if (memcmp(p, sub->as.s.data, sub->as.s.len) == 0) {
            return mino_true(S_);
        }
    }
    return mino_false(S_);
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
        mino_val_t *result = mino_string_n(S_, buf, s->as.s.len);
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
        mino_val_t *result = mino_string_n(S_, buf, s->as.s.len);
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
    return mino_string_n(S_, start, (size_t)(end_ptr - start));
}

/* ------------------------------------------------------------------------- */
/* Utility primitives                                                        */
/* ------------------------------------------------------------------------- */

static mino_val_t *prim_type(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("type requires one argument");
        return NULL;
    }
    v = args->as.cons.car;
    if (v == NULL || v->type == MINO_NIL)  return mino_keyword(S_, "nil");
    switch (v->type) {
    case MINO_NIL:     return mino_keyword(S_, "nil");
    case MINO_BOOL:    return mino_keyword(S_, "bool");
    case MINO_INT:     return mino_keyword(S_, "int");
    case MINO_FLOAT:   return mino_keyword(S_, "float");
    case MINO_STRING:  return mino_keyword(S_, "string");
    case MINO_SYMBOL:  return mino_keyword(S_, "symbol");
    case MINO_KEYWORD: return mino_keyword(S_, "keyword");
    case MINO_CONS:    return mino_keyword(S_, "list");
    case MINO_VECTOR:  return mino_keyword(S_, "vector");
    case MINO_MAP:     return mino_keyword(S_, "map");
    case MINO_SET:     return mino_keyword(S_, "set");
    case MINO_PRIM:    return mino_keyword(S_, "fn");
    case MINO_FN:      return mino_keyword(S_, "fn");
    case MINO_MACRO:   return mino_keyword(S_, "macro");
    case MINO_HANDLE:  return mino_keyword(S_, "handle");
    case MINO_ATOM:    return mino_keyword(S_, "atom");
    case MINO_LAZY:    return mino_keyword(S_, "lazy-seq");
    case MINO_RECUR:     return mino_keyword(S_, "recur");
    case MINO_TAIL_CALL: return mino_keyword(S_, "tail-call");
    }
    return mino_keyword(S_, "unknown");
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
        mino_val_t *result = mino_string_n(S_, buf != NULL ? buf : "", len);
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
    return mino_nil(S_);
}

static mino_val_t *prim_prn(mino_val_t *args, mino_env_t *env)
{
    int first = 1;
    (void)env;
    while (mino_is_cons(args)) {
        if (!first) fputc(' ', stdout);
        mino_print(S_, args->as.cons.car);
        first = 0;
        args = args->as.cons.cdr;
    }
    fputc('\n', stdout);
    fflush(stdout);
    return mino_nil(S_);
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
        return mino_symbol_n(S_, buf, prefix_len + (size_t)used);
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
    result = mino_string_n(S_, buf, rd);
    free(buf);
    return result;
}

static mino_val_t *prim_spit(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *path_val;
    mino_val_t *content;
    const char *path;
    FILE       *f;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("spit requires two arguments");
        return NULL;
    }
    path_val = args->as.cons.car;
    content  = args->as.cons.cdr->as.cons.car;
    if (path_val == NULL || path_val->type != MINO_STRING) {
        set_error("spit: first argument must be a string path");
        return NULL;
    }
    path = path_val->as.s.data;
    f = fopen(path, "wb");
    if (f == NULL) {
        char msg[300];
        snprintf(msg, sizeof(msg), "spit: cannot open file: %s", path);
        set_error(msg);
        return NULL;
    }
    if (content != NULL && content->type == MINO_STRING) {
        fwrite(content->as.s.data, 1, content->as.s.len, f);
    } else {
        mino_print_to(S_, f, content);
    }
    fclose(f);
    return mino_nil(S_);
}

/* (exit code) — terminate the process with the given exit code.
 * Defaults to 0 if no argument is given. */
static mino_val_t *prim_exit(mino_val_t *args, mino_env_t *env)
{
    int code = 0;
    (void)env;
    if (mino_is_cons(args)) {
        mino_val_t *v = args->as.cons.car;
        if (v != NULL && v->type == MINO_INT) {
            code = (int)v->as.i;
        } else if (v != NULL && v->type == MINO_FLOAT) {
            code = (int)v->as.f;
        }
    }
    exit(code);
    return mino_nil(S_); /* unreachable */
}

/* --- Regex primitives (using bundled tiny-regex-c) --- */
#include "re.h"

/* (re-find pattern text) — find first match of pattern in text.
 * Returns the matched substring, or nil if no match. */
static mino_val_t *prim_re_find(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *pat_val, *text_val;
    int match_len = 0;
    int match_idx;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("re-find requires two arguments");
        return NULL;
    }
    pat_val  = args->as.cons.car;
    text_val = args->as.cons.cdr->as.cons.car;
    if (pat_val == NULL || pat_val->type != MINO_STRING) {
        set_error("re-find: first argument must be a pattern string");
        return NULL;
    }
    if (text_val == NULL || text_val->type != MINO_STRING) {
        set_error("re-find: second argument must be a string");
        return NULL;
    }
    match_idx = re_match(pat_val->as.s.data, text_val->as.s.data, &match_len);
    if (match_idx == -1) {
        return mino_nil(S_);
    }
    return mino_string_n(S_, text_val->as.s.data + match_idx, (size_t)match_len);
}

/* (re-matches pattern text) — true if the entire text matches pattern. */
static mino_val_t *prim_re_matches(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *pat_val, *text_val;
    int match_len = 0;
    int match_idx;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("re-matches requires two arguments");
        return NULL;
    }
    pat_val  = args->as.cons.car;
    text_val = args->as.cons.cdr->as.cons.car;
    if (pat_val == NULL || pat_val->type != MINO_STRING) {
        set_error("re-matches: first argument must be a pattern string");
        return NULL;
    }
    if (text_val == NULL || text_val->type != MINO_STRING) {
        set_error("re-matches: second argument must be a string");
        return NULL;
    }
    match_idx = re_match(pat_val->as.s.data, text_val->as.s.data, &match_len);
    if (match_idx == 0 && (size_t)match_len == text_val->as.s.len) {
        return text_val;
    }
    return mino_nil(S_);
}

/* (time-ms) — return process time in milliseconds as a float.
 * Uses ANSI C clock() for portability across all C99 platforms. */
static mino_val_t *prim_time_ms(mino_val_t *args, mino_env_t *env)
{
    (void)args;
    (void)env;
    if (mino_is_cons(args)) {
        set_error("time-ms takes no arguments");
        return NULL;
    }
    return mino_float(S_, (double)clock() / (double)CLOCKS_PER_SEC * 1000.0);
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
    result = mino_load_file(S_, path, env);
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
        return mino_string(S_, e->docstring);
    }
    return mino_nil(S_);
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
    return mino_nil(S_);
}

/* (apropos substring) — return a list of bound names containing substring. */
static mino_val_t *prim_apropos(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *pat_val;
    const char *pat;
    mino_val_t *head = mino_nil(S_);
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
                mino_val_t *sym  = mino_symbol(S_, e->bindings[i].name);
                mino_val_t *cell = mino_cons(S_, sym, mino_nil(S_));
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

void mino_set_resolver(mino_state_t *S, mino_resolve_fn fn, void *ctx)
{
    S_ = S;
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

static void install_core_mino(mino_env_t *env)
{
    const char *src        = core_mino_src;
    const char *saved_file = reader_file;
    int         saved_line = reader_line;
    reader_file = intern_filename("<core>");
    reader_line = 1;
    while (*src != '\0') {
        const char *end  = NULL;
        mino_val_t *form = mino_read(S_, src, &end);
        if (form == NULL) {
            if (mino_last_error(S_) != NULL) {
                /* Hardcoded source — a parse error here is a build-time bug. */
                fprintf(stderr, "core.mino parse error: %s\n", mino_last_error(S_));
                abort();
            }
            break;
        }
        if (mino_eval(S_, form, env) == NULL) {
            fprintf(stderr, "core.mino eval error: %s\n", mino_last_error(S_));
            abort();
        }
        src = end;
    }
    reader_file = saved_file;
    reader_line = saved_line;
}

/* --- Atom primitives --------------------------------------------------- */

/*
 * (seq coll) — coerce a collection to a sequence (cons chain).
 * Returns nil for empty collections. Forces lazy sequences.
 */
static mino_val_t *prim_seq(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *coll;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("seq requires one argument");
        return NULL;
    }
    coll = args->as.cons.car;
    if (coll == NULL || coll->type == MINO_NIL) return mino_nil(S_);
    if (coll->type == MINO_LAZY) {
        mino_val_t *forced = lazy_force(coll);
        if (forced == NULL) return NULL;
        if (forced->type == MINO_NIL) return mino_nil(S_);
        return forced;
    }
    if (coll->type == MINO_CONS) return coll;
    if (coll->type == MINO_VECTOR) {
        mino_val_t *head = mino_nil(S_);
        mino_val_t *tail = NULL;
        size_t i;
        if (coll->as.vec.len == 0) return mino_nil(S_);
        for (i = 0; i < coll->as.vec.len; i++) {
            mino_val_t *cell = mino_cons(S_, vec_nth(coll, i), mino_nil(S_));
            if (tail == NULL) head = cell;
            else tail->as.cons.cdr = cell;
            tail = cell;
        }
        return head;
    }
    if (coll->type == MINO_MAP) {
        mino_val_t *head = mino_nil(S_);
        mino_val_t *tail = NULL;
        size_t i;
        if (coll->as.map.len == 0) return mino_nil(S_);
        for (i = 0; i < coll->as.map.len; i++) {
            mino_val_t *key = vec_nth(coll->as.map.key_order, i);
            mino_val_t *val = map_get_val(coll, key);
            mino_val_t *kv[2];
            mino_val_t *cell;
            kv[0] = key; kv[1] = val;
            cell = mino_cons(S_, mino_vector(S_, kv, 2), mino_nil(S_));
            if (tail == NULL) head = cell;
            else tail->as.cons.cdr = cell;
            tail = cell;
        }
        return head;
    }
    if (coll->type == MINO_SET) {
        mino_val_t *head = mino_nil(S_);
        mino_val_t *tail = NULL;
        size_t i;
        if (coll->as.set.len == 0) return mino_nil(S_);
        for (i = 0; i < coll->as.set.len; i++) {
            mino_val_t *elem = vec_nth(coll->as.set.key_order, i);
            mino_val_t *cell = mino_cons(S_, elem, mino_nil(S_));
            if (tail == NULL) head = cell;
            else tail->as.cons.cdr = cell;
            tail = cell;
        }
        return head;
    }
    if (coll->type == MINO_STRING) {
        mino_val_t *head = mino_nil(S_);
        mino_val_t *tail = NULL;
        size_t i;
        if (coll->as.s.len == 0) return mino_nil(S_);
        for (i = 0; i < coll->as.s.len; i++) {
            mino_val_t *ch = mino_string_n(S_, coll->as.s.data + i, 1);
            mino_val_t *cell = mino_cons(S_, ch, mino_nil(S_));
            if (tail == NULL) head = cell;
            else tail->as.cons.cdr = cell;
            tail = cell;
        }
        return head;
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "seq: cannot coerce %s to a sequence",
                 type_tag_str(coll));
        set_error(msg);
    }
    return NULL;
}

static mino_val_t *prim_realized_p(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("realized? requires one argument");
        return NULL;
    }
    v = args->as.cons.car;
    if (v != NULL && v->type == MINO_LAZY) {
        return v->as.lazy.realized ? mino_true(S_) : mino_false(S_);
    }
    /* Non-lazy values are always realized. */
    return mino_true(S_);
}

static mino_val_t *prim_atom(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("atom requires one argument");
        return NULL;
    }
    return mino_atom(S_, args->as.cons.car);
}

static mino_val_t *prim_deref(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *a;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("deref requires one argument");
        return NULL;
    }
    a = args->as.cons.car;
    if (a == NULL || a->type != MINO_ATOM) {
        set_error("deref: expected an atom");
        return NULL;
    }
    return a->as.atom.val;
}

static mino_val_t *prim_reset_bang(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *a, *val;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error("reset! requires two arguments");
        return NULL;
    }
    a   = args->as.cons.car;
    val = args->as.cons.cdr->as.cons.car;
    if (a == NULL || a->type != MINO_ATOM) {
        set_error("reset!: first argument must be an atom");
        return NULL;
    }
    a->as.atom.val = val;
    return val;
}

/* (swap! atom f & args) — applies (f current-val args...) and sets result. */
static mino_val_t *prim_swap_bang(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *a, *fn, *cur, *call_args, *extra, *tail, *result;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error("swap! requires at least 2 arguments: atom and function");
        return NULL;
    }
    a  = args->as.cons.car;
    fn = args->as.cons.cdr->as.cons.car;
    if (a == NULL || a->type != MINO_ATOM) {
        set_error("swap!: first argument must be an atom");
        return NULL;
    }
    cur = a->as.atom.val;
    /* Build arg list: (cur extra1 extra2 ...) */
    call_args = mino_nil(S_);
    /* Append extra args in reverse then prepend cur. */
    extra = args->as.cons.cdr->as.cons.cdr; /* rest after fn */
    if (extra != NULL && extra->type == MINO_CONS) {
        /* Collect extras into a list. */
        tail = mino_nil(S_);
        while (extra != NULL && extra->type == MINO_CONS) {
            tail = mino_cons(S_, extra->as.cons.car, tail);
            extra = extra->as.cons.cdr;
        }
        /* Reverse to get correct order. */
        call_args = mino_nil(S_);
        while (tail != NULL && tail->type == MINO_CONS) {
            call_args = mino_cons(S_, tail->as.cons.car, call_args);
            tail = tail->as.cons.cdr;
        }
    }
    call_args = mino_cons(S_, cur, call_args);
    result = mino_call(S_, fn, call_args, env);
    if (result == NULL) return NULL;
    a->as.atom.val = result;
    return result;
}

static mino_val_t *prim_atom_p(mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error("atom? requires one argument");
        return NULL;
    }
    return mino_is_atom(args->as.cons.car) ? mino_true(S_) : mino_false(S_);
}

/* ------------------------------------------------------------------------- */
/* Value cloning (cross-state transfer)                                      */
/* ------------------------------------------------------------------------- */

static mino_val_t *clone_val(mino_state_t *dst, const mino_val_t *v)
{
    if (v == NULL) return mino_nil(dst);

    switch (v->type) {
    case MINO_NIL:    return mino_nil(dst);
    case MINO_BOOL:   return v->as.b ? mino_true(dst) : mino_false(dst);
    case MINO_INT:    return mino_int(dst, v->as.i);
    case MINO_FLOAT:  return mino_float(dst, v->as.f);
    case MINO_STRING: return mino_string_n(dst, v->as.s.data, v->as.s.len);
    case MINO_SYMBOL: return mino_symbol_n(dst, v->as.s.data, v->as.s.len);
    case MINO_KEYWORD:return mino_keyword_n(dst, v->as.s.data, v->as.s.len);
    case MINO_CONS: {
        mino_val_t *car = clone_val(dst, v->as.cons.car);
        mino_val_t *cdr;
        mino_ref_t *rcar = mino_ref(dst, car);
        cdr = clone_val(dst, v->as.cons.cdr);
        car = mino_deref(rcar);
        mino_unref(dst, rcar);
        return mino_cons(dst, car, cdr);
    }
    case MINO_VECTOR: {
        size_t len = v->as.vec.len;
        size_t i;
        mino_val_t **items;
        mino_val_t *result;
        mino_ref_t **refs;
        if (len == 0) return mino_vector(dst, NULL, 0);
        items = (mino_val_t **)malloc(len * sizeof(*items));
        refs  = (mino_ref_t **)malloc(len * sizeof(*refs));
        if (items == NULL || refs == NULL) {
            free(items); free(refs);
            return NULL;
        }
        for (i = 0; i < len; i++) {
            items[i] = clone_val(dst, vec_nth(v, i));
            refs[i]  = mino_ref(dst, items[i]);
        }
        for (i = 0; i < len; i++) {
            items[i] = mino_deref(refs[i]);
        }
        result = mino_vector(dst, items, len);
        for (i = 0; i < len; i++) mino_unref(dst, refs[i]);
        free(items);
        free(refs);
        return result;
    }
    case MINO_MAP: {
        size_t len = v->as.map.len;
        size_t i;
        mino_val_t **keys, **vals;
        mino_ref_t **krefs, **vrefs;
        mino_val_t *result;
        if (len == 0) return mino_map(dst, NULL, NULL, 0);
        keys  = (mino_val_t **)malloc(len * sizeof(*keys));
        vals  = (mino_val_t **)malloc(len * sizeof(*vals));
        krefs = (mino_ref_t **)malloc(len * sizeof(*krefs));
        vrefs = (mino_ref_t **)malloc(len * sizeof(*vrefs));
        if (!keys || !vals || !krefs || !vrefs) {
            free(keys); free(vals); free(krefs); free(vrefs);
            return NULL;
        }
        for (i = 0; i < len; i++) {
            mino_val_t *src_key = vec_nth(v->as.map.key_order, i);
            mino_val_t *src_val = hamt_get(v->as.map.root, src_key,
                                           hash_val(src_key), 0);
            keys[i]  = clone_val(dst, src_key);
            krefs[i] = mino_ref(dst, keys[i]);
            vals[i]  = clone_val(dst, src_val);
            vrefs[i] = mino_ref(dst, vals[i]);
        }
        for (i = 0; i < len; i++) {
            keys[i] = mino_deref(krefs[i]);
            vals[i] = mino_deref(vrefs[i]);
        }
        result = mino_map(dst, keys, vals, len);
        for (i = 0; i < len; i++) {
            mino_unref(dst, krefs[i]);
            mino_unref(dst, vrefs[i]);
        }
        free(keys); free(vals); free(krefs); free(vrefs);
        return result;
    }
    case MINO_SET: {
        size_t len = v->as.set.len;
        size_t i;
        mino_val_t **items;
        mino_ref_t **refs;
        mino_val_t *result;
        if (len == 0) return mino_set(dst, NULL, 0);
        items = (mino_val_t **)malloc(len * sizeof(*items));
        refs  = (mino_ref_t **)malloc(len * sizeof(*refs));
        if (!items || !refs) { free(items); free(refs); return NULL; }
        for (i = 0; i < len; i++) {
            items[i] = clone_val(dst, vec_nth(v->as.set.key_order, i));
            refs[i]  = mino_ref(dst, items[i]);
        }
        for (i = 0; i < len; i++) {
            items[i] = mino_deref(refs[i]);
        }
        result = mino_set(dst, items, len);
        for (i = 0; i < len; i++) mino_unref(dst, refs[i]);
        free(items);
        free(refs);
        return result;
    }
    /* Non-transferable types. */
    case MINO_FN:
    case MINO_MACRO:
    case MINO_PRIM:
    case MINO_HANDLE:
    case MINO_ATOM:
    case MINO_LAZY:
    case MINO_RECUR:
    case MINO_TAIL_CALL:
        return NULL;
    }
    return NULL; /* unreachable */
}

mino_val_t *mino_clone(mino_state_t *dst, mino_state_t *src, mino_val_t *val)
{
    mino_val_t *result;
    mino_state_t *saved = S_;
    (void)src;
    result = clone_val(dst, val);
    S_ = saved;
    if (result == NULL && val != NULL) {
        S_ = dst;
        set_error("clone: value contains non-transferable types "
                  "(fn, macro, prim, handle, atom, or lazy-seq)");
        S_ = saved;
    }
    return result;
}

/* ------------------------------------------------------------------------- */
/* Mailbox (thread-safe value queue between states)                          */
/* ------------------------------------------------------------------------- */

#ifdef _WIN32
#include <windows.h>
typedef CRITICAL_SECTION mb_mutex_t;
#define MB_MUTEX_INIT(m)    InitializeCriticalSection(m)
#define MB_MUTEX_LOCK(m)    EnterCriticalSection(m)
#define MB_MUTEX_UNLOCK(m)  LeaveCriticalSection(m)
#define MB_MUTEX_DESTROY(m) DeleteCriticalSection(m)
#else
#include <pthread.h>
typedef pthread_mutex_t mb_mutex_t;
#define MB_MUTEX_INIT(m)    pthread_mutex_init(m, NULL)
#define MB_MUTEX_LOCK(m)    pthread_mutex_lock(m)
#define MB_MUTEX_UNLOCK(m)  pthread_mutex_unlock(m)
#define MB_MUTEX_DESTROY(m) pthread_mutex_destroy(m)
#endif

typedef struct mb_msg {
    char           *data;   /* serialized form (malloc'd) */
    size_t          len;
    struct mb_msg  *next;
} mb_msg_t;

struct mino_mailbox {
    mb_msg_t   *head;
    mb_msg_t   *tail;
    mb_mutex_t  lock;
};

mino_mailbox_t *mino_mailbox_new(void)
{
    mino_mailbox_t *mb = (mino_mailbox_t *)calloc(1, sizeof(*mb));
    if (mb == NULL) return NULL;
    MB_MUTEX_INIT(&mb->lock);
    return mb;
}

void mino_mailbox_free(mino_mailbox_t *mb)
{
    mb_msg_t *m, *next;
    if (mb == NULL) return;
    for (m = mb->head; m != NULL; m = next) {
        next = m->next;
        free(m->data);
        free(m);
    }
    MB_MUTEX_DESTROY(&mb->lock);
    free(mb);
}

/* Serialize a value to a malloc'd string.  Returns NULL on failure. */
static char *val_serialize(mino_state_t *S, mino_val_t *val, size_t *out_len)
{
    FILE *f = tmpfile();
    long  n;
    char *buf;
    if (f == NULL) return NULL;
    mino_print_to(S, f, val);
    n = ftell(f);
    if (n < 0) n = 0;
    rewind(f);
    buf = (char *)malloc((size_t)n + 1);
    if (buf == NULL) { fclose(f); return NULL; }
    if (n > 0) {
        size_t got = fread(buf, 1, (size_t)n, f);
        (void)got;
    }
    buf[n] = '\0';
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

int mino_mailbox_send(mino_mailbox_t *mb, mino_state_t *S, mino_val_t *val)
{
    char   *data;
    size_t  len;
    mb_msg_t *msg;
    if (mb == NULL || val == NULL) return -1;
    data = val_serialize(S, val, &len);
    if (data == NULL) return -1;
    msg = (mb_msg_t *)calloc(1, sizeof(*msg));
    if (msg == NULL) { free(data); return -1; }
    msg->data = data;
    msg->len  = len;
    MB_MUTEX_LOCK(&mb->lock);
    if (mb->tail != NULL) {
        mb->tail->next = msg;
    } else {
        mb->head = msg;
    }
    mb->tail = msg;
    MB_MUTEX_UNLOCK(&mb->lock);
    return 0;
}

mino_val_t *mino_mailbox_recv(mino_mailbox_t *mb, mino_state_t *S)
{
    mb_msg_t   *msg;
    mino_val_t *result;
    if (mb == NULL) return NULL;
    MB_MUTEX_LOCK(&mb->lock);
    msg = mb->head;
    if (msg != NULL) {
        mb->head = msg->next;
        if (mb->head == NULL) mb->tail = NULL;
    }
    MB_MUTEX_UNLOCK(&mb->lock);
    if (msg == NULL) return NULL;
    result = mino_read(S, msg->data, NULL);
    free(msg->data);
    free(msg);
    return result;
}

/* ------------------------------------------------------------------------- */
/* Actor (state + env + mailbox bundle)                                      */
/* ------------------------------------------------------------------------- */

struct mino_actor {
    mino_state_t   *state;
    mino_env_t     *env;
    mino_mailbox_t *mailbox;
};

mino_actor_t *mino_actor_new(void)
{
    mino_actor_t *a = (mino_actor_t *)calloc(1, sizeof(*a));
    if (a == NULL) return NULL;
    a->state = mino_state_new();
    if (a->state == NULL) { free(a); return NULL; }
    a->env = mino_new(a->state);
    if (a->env == NULL) {
        mino_state_free(a->state);
        free(a);
        return NULL;
    }
    a->mailbox = mino_mailbox_new();
    if (a->mailbox == NULL) {
        mino_env_free(a->state, a->env);
        mino_state_free(a->state);
        free(a);
        return NULL;
    }
    return a;
}

mino_state_t *mino_actor_state(mino_actor_t *a)
{
    return a ? a->state : NULL;
}

mino_env_t *mino_actor_env(mino_actor_t *a)
{
    return a ? a->env : NULL;
}

mino_mailbox_t *mino_actor_mailbox(mino_actor_t *a)
{
    return a ? a->mailbox : NULL;
}

void mino_actor_send(mino_actor_t *a, mino_state_t *src, mino_val_t *val)
{
    if (a == NULL || val == NULL) return;
    mino_mailbox_send(a->mailbox, src, val);
}

mino_val_t *mino_actor_recv(mino_actor_t *a)
{
    if (a == NULL) return NULL;
    return mino_mailbox_recv(a->mailbox, a->state);
}

void mino_actor_free(mino_actor_t *a)
{
    if (a == NULL) return;
    mino_mailbox_free(a->mailbox);
    mino_env_free(a->state, a->env);
    mino_state_free(a->state);
    free(a);
}

/* Primitive: (send! actor val) — send a value to an actor's mailbox. */
static mino_val_t *prim_send_bang(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *handle, *val;
    mino_actor_t *a;
    (void)env;
    if (args == NULL || args->type != MINO_CONS ||
        args->as.cons.cdr == NULL || args->as.cons.cdr->type != MINO_CONS) {
        set_error("send! requires 2 arguments: actor and value");
        return NULL;
    }
    handle = args->as.cons.car;
    val    = args->as.cons.cdr->as.cons.car;
    if (handle == NULL || handle->type != MINO_HANDLE ||
        strcmp(handle->as.handle.tag, "actor") != 0) {
        set_error("send! first argument must be an actor handle");
        return NULL;
    }
    a = (mino_actor_t *)handle->as.handle.ptr;
    mino_actor_send(a, S_, val);
    return mino_nil(S_);
}

/* Primitive: (receive) — receive from the current state's actor mailbox.
 * The host must set up the "self" binding before ticking the actor.
 * Returns nil if the mailbox is empty. */
static mino_val_t *prim_receive(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *self;
    mino_actor_t *a;
    mino_val_t *msg;
    (void)args;
    self = mino_env_get(env, "*self*");
    if (self == NULL || self->type != MINO_HANDLE ||
        strcmp(self->as.handle.tag, "actor") != 0) {
        set_error("receive: no actor context (*self* not bound)");
        return NULL;
    }
    a = (mino_actor_t *)self->as.handle.ptr;
    msg = mino_actor_recv(a);
    return msg != NULL ? msg : mino_nil(S_);
}

/* Primitive: (spawn src) — create a new actor, eval src string in it,
 * return a handle. The src typically defines a handler function. */
static mino_val_t *prim_spawn(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *src_val;
    const char *src;
    mino_actor_t *a;
    mino_val_t *self_handle;
    char buf[256];
    (void)env;
    if (args == NULL || args->type != MINO_CONS) {
        set_error("spawn requires 1 argument: init source string");
        return NULL;
    }
    src_val = args->as.cons.car;
    if (src_val == NULL || src_val->type != MINO_STRING) {
        set_error("spawn argument must be a string");
        return NULL;
    }
    src = src_val->as.s.data;
    a = mino_actor_new();
    if (a == NULL) {
        set_error("spawn: failed to create actor");
        return NULL;
    }
    /* Install I/O in the actor so it can print etc. */
    mino_install_io(a->state, a->env);
    /* Create a handle for this actor in the actor's own state and bind *self*. */
    self_handle = mino_handle(a->state, a, "actor");
    mino_env_set(a->state, a->env, "*self*", self_handle);
    /* Evaluate the init source in the actor's state. */
    if (mino_eval_string(a->state, src, a->env) == NULL) {
        const char *err = mino_last_error(a->state);
        snprintf(buf, sizeof(buf),
                 "spawn: init eval failed: %s", err ? err : "unknown error");
        set_error(buf);
        mino_actor_free(a);
        return NULL;
    }
    /* Return a handle in the caller's state. */
    return mino_handle(S_, a, "actor");
}

void mino_install_core(mino_state_t *S, mino_env_t *env)
{
    S_ = S;
    volatile char probe = 0;
    gc_note_host_frame((void *)&probe);
    (void)probe;
    mino_env_set(S_, env, "+",        mino_prim(S_, "+",        prim_add));
    mino_env_set(S_, env, "-",        mino_prim(S_, "-",        prim_sub));
    mino_env_set(S_, env, "*",        mino_prim(S_, "*",        prim_mul));
    mino_env_set(S_, env, "/",        mino_prim(S_, "/",        prim_div));
    mino_env_set(S_, env, "=",        mino_prim(S_, "=",        prim_eq));
    mino_env_set(S_, env, "<",        mino_prim(S_, "<",        prim_lt));
    mino_env_set(S_, env, "mod",      mino_prim(S_, "mod",      prim_mod));
    mino_env_set(S_, env, "rem",      mino_prim(S_, "rem",      prim_rem));
    mino_env_set(S_, env, "quot",     mino_prim(S_, "quot",     prim_quot));
    /* math */
    mino_env_set(S_, env, "math-floor", mino_prim(S_, "math-floor", prim_math_floor));
    mino_env_set(S_, env, "math-ceil",  mino_prim(S_, "math-ceil",  prim_math_ceil));
    mino_env_set(S_, env, "math-round", mino_prim(S_, "math-round", prim_math_round));
    mino_env_set(S_, env, "math-sqrt",  mino_prim(S_, "math-sqrt",  prim_math_sqrt));
    mino_env_set(S_, env, "math-pow",   mino_prim(S_, "math-pow",   prim_math_pow));
    mino_env_set(S_, env, "math-log",   mino_prim(S_, "math-log",   prim_math_log));
    mino_env_set(S_, env, "math-exp",   mino_prim(S_, "math-exp",   prim_math_exp));
    mino_env_set(S_, env, "math-sin",   mino_prim(S_, "math-sin",   prim_math_sin));
    mino_env_set(S_, env, "math-cos",   mino_prim(S_, "math-cos",   prim_math_cos));
    mino_env_set(S_, env, "math-tan",   mino_prim(S_, "math-tan",   prim_math_tan));
    mino_env_set(S_, env, "math-atan2", mino_prim(S_, "math-atan2", prim_math_atan2));
    mino_env_set(S_, env, "math-pi",    mino_float(S_, 3.14159265358979323846));
    mino_env_set(S_, env, "bit-and", mino_prim(S_, "bit-and", prim_bit_and));
    mino_env_set(S_, env, "bit-or",  mino_prim(S_, "bit-or",  prim_bit_or));
    mino_env_set(S_, env, "bit-xor", mino_prim(S_, "bit-xor", prim_bit_xor));
    mino_env_set(S_, env, "bit-not", mino_prim(S_, "bit-not", prim_bit_not));
    mino_env_set(S_, env, "bit-shift-left",
                 mino_prim(S_, "bit-shift-left", prim_bit_shift_left));
    mino_env_set(S_, env, "bit-shift-right",
                 mino_prim(S_, "bit-shift-right", prim_bit_shift_right));
    mino_env_set(S_, env, "car",      mino_prim(S_, "car",      prim_car));
    mino_env_set(S_, env, "cdr",      mino_prim(S_, "cdr",      prim_cdr));
    mino_env_set(S_, env, "cons",     mino_prim(S_, "cons",     prim_cons));
    mino_env_set(S_, env, "count",    mino_prim(S_, "count",    prim_count));
    mino_env_set(S_, env, "nth",      mino_prim(S_, "nth",      prim_nth));
    mino_env_set(S_, env, "first",    mino_prim(S_, "first",    prim_first));
    mino_env_set(S_, env, "rest",     mino_prim(S_, "rest",     prim_rest));
    mino_env_set(S_, env, "vector",   mino_prim(S_, "vector",   prim_vector));
    mino_env_set(S_, env, "hash-map", mino_prim(S_, "hash-map", prim_hash_map));
    mino_env_set(S_, env, "assoc",    mino_prim(S_, "assoc",    prim_assoc));
    mino_env_set(S_, env, "get",      mino_prim(S_, "get",      prim_get));
    mino_env_set(S_, env, "conj",     mino_prim(S_, "conj",     prim_conj));
    mino_env_set(S_, env, "keys",     mino_prim(S_, "keys",     prim_keys));
    mino_env_set(S_, env, "vals",     mino_prim(S_, "vals",     prim_vals));
    mino_env_set(S_, env, "macroexpand-1",
                 mino_prim(S_, "macroexpand-1", prim_macroexpand_1));
    mino_env_set(S_, env, "macroexpand",
                 mino_prim(S_, "macroexpand", prim_macroexpand));
    mino_env_set(S_, env, "gensym",   mino_prim(S_, "gensym",   prim_gensym));
    mino_env_set(S_, env, "type",     mino_prim(S_, "type",     prim_type));
    mino_env_set(S_, env, "name",     mino_prim(S_, "name",     prim_name));
    mino_env_set(S_, env, "rand",     mino_prim(S_, "rand",     prim_rand));
    /* regex */
    mino_env_set(S_, env, "re-find",    mino_prim(S_, "re-find",    prim_re_find));
    mino_env_set(S_, env, "re-matches", mino_prim(S_, "re-matches", prim_re_matches));
    mino_env_set(S_, env, "eval",     mino_prim(S_, "eval",     prim_eval));
    mino_env_set(S_, env, "symbol",   mino_prim(S_, "symbol",   prim_symbol));
    mino_env_set(S_, env, "keyword",  mino_prim(S_, "keyword",  prim_keyword));
    mino_env_set(S_, env, "hash",     mino_prim(S_, "hash",     prim_hash));
    mino_env_set(S_, env, "compare",  mino_prim(S_, "compare",  prim_compare));
    mino_env_set(S_, env, "int",      mino_prim(S_, "int",      prim_int));
    mino_env_set(S_, env, "float",    mino_prim(S_, "float",    prim_float));
    mino_env_set(S_, env, "str",      mino_prim(S_, "str",      prim_str));
    mino_env_set(S_, env, "pr-str",   mino_prim(S_, "pr-str",   prim_pr_str));
    mino_env_set(S_, env, "read-string",
                 mino_prim(S_, "read-string", prim_read_string));
    mino_env_set(S_, env, "format",   mino_prim(S_, "format",   prim_format));
    mino_env_set(S_, env, "throw",    mino_prim(S_, "throw",    prim_throw));
    mino_env_set(S_, env, "require",  mino_prim(S_, "require",  prim_require));
    mino_env_set(S_, env, "doc",      mino_prim(S_, "doc",      prim_doc));
    mino_env_set(S_, env, "source",   mino_prim(S_, "source",   prim_source));
    mino_env_set(S_, env, "apropos",  mino_prim(S_, "apropos",  prim_apropos));
    /* set operations */
    mino_env_set(S_, env, "hash-set", mino_prim(S_, "hash-set", prim_hash_set));
    mino_env_set(S_, env, "contains?",mino_prim(S_, "contains?",prim_contains_p));
    mino_env_set(S_, env, "disj",     mino_prim(S_, "disj",     prim_disj));
    mino_env_set(S_, env, "dissoc",   mino_prim(S_, "dissoc",   prim_dissoc));
    /* sequence operations (map, filter, take, drop, range, repeat,
       concat are now lazy in core.mino) */
    mino_env_set(S_, env, "reduce",   mino_prim(S_, "reduce",   prim_reduce));
    mino_env_set(S_, env, "into",     mino_prim(S_, "into",     prim_into));
    mino_env_set(S_, env, "apply",    mino_prim(S_, "apply",    prim_apply));
    mino_env_set(S_, env, "reverse",  mino_prim(S_, "reverse",  prim_reverse));
    mino_env_set(S_, env, "sort",     mino_prim(S_, "sort",     prim_sort));
    /* string operations */
    mino_env_set(S_, env, "subs",     mino_prim(S_, "subs",     prim_subs));
    mino_env_set(S_, env, "split",    mino_prim(S_, "split",    prim_split));
    mino_env_set(S_, env, "join",     mino_prim(S_, "join",     prim_join));
    mino_env_set(S_, env, "starts-with?",
                 mino_prim(S_, "starts-with?", prim_starts_with_p));
    mino_env_set(S_, env, "ends-with?",
                 mino_prim(S_, "ends-with?", prim_ends_with_p));
    mino_env_set(S_, env, "includes?",
                 mino_prim(S_, "includes?", prim_includes_p));
    mino_env_set(S_, env, "upper-case",
                 mino_prim(S_, "upper-case", prim_upper_case));
    mino_env_set(S_, env, "lower-case",
                 mino_prim(S_, "lower-case", prim_lower_case));
    mino_env_set(S_, env, "trim",     mino_prim(S_, "trim",     prim_trim));
    mino_env_set(S_, env, "char-at",  mino_prim(S_, "char-at",  prim_char_at));
    /* (some and every? are now in core.mino) */
    /* sequences */
    mino_env_set(S_, env, "seq",       mino_prim(S_, "seq",       prim_seq));
    mino_env_set(S_, env, "realized?", mino_prim(S_, "realized?", prim_realized_p));
    /* atoms */
    mino_env_set(S_, env, "atom",     mino_prim(S_, "atom",     prim_atom));
    mino_env_set(S_, env, "deref",    mino_prim(S_, "deref",    prim_deref));
    mino_env_set(S_, env, "reset!",   mino_prim(S_, "reset!",   prim_reset_bang));
    mino_env_set(S_, env, "swap!",    mino_prim(S_, "swap!",    prim_swap_bang));
    mino_env_set(S_, env, "atom?",    mino_prim(S_, "atom?",    prim_atom_p));
    /* actors */
    mino_env_set(S_, env, "spawn",    mino_prim(S_, "spawn",    prim_spawn));
    mino_env_set(S_, env, "send!",    mino_prim(S_, "send!",    prim_send_bang));
    mino_env_set(S_, env, "receive",  mino_prim(S_, "receive",  prim_receive));
    install_core_mino(env);
}

void mino_install_io(mino_state_t *S, mino_env_t *env)
{
    S_ = S;
    mino_env_set(S_, env, "println",  mino_prim(S_, "println",  prim_println));
    mino_env_set(S_, env, "prn",      mino_prim(S_, "prn",      prim_prn));
    mino_env_set(S_, env, "slurp",    mino_prim(S_, "slurp",    prim_slurp));
    mino_env_set(S_, env, "spit",     mino_prim(S_, "spit",     prim_spit));
    mino_env_set(S_, env, "exit",     mino_prim(S_, "exit",     prim_exit));
    mino_env_set(S_, env, "time-ms",  mino_prim(S_, "time-ms",  prim_time_ms));
}
