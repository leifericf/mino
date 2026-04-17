/*
 * mino_internal.h -- shared internal types, macros, and declarations.
 *
 * Included by each translation unit of the mino runtime. Not part of the
 * public API; embedders should only use mino.h.
 */

#ifndef MINO_INTERNAL_H
#define MINO_INTERNAL_H

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
/* GC tag types                                                              */
/* ------------------------------------------------------------------------- */

enum {
    GC_T_RAW        = 1,
    GC_T_VAL        = 2,
    GC_T_ENV        = 3,
    GC_T_VEC_NODE   = 4,
    GC_T_HAMT_NODE  = 5,
    GC_T_HAMT_ENTRY = 6,
    GC_T_PTRARR     = 7,
    GC_T_VALARR     = 8
};

/* ------------------------------------------------------------------------- */
/* Internal type definitions                                                 */
/* ------------------------------------------------------------------------- */

typedef struct gc_hdr {
    unsigned char  type_tag;
    unsigned char  mark;
    size_t         size;
    struct gc_hdr *next;
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

/* Metadata table entry. */
typedef struct {
    char       *name;
    char       *docstring;
    mino_val_t *source;
} meta_entry_t;

/* Intern table. */
typedef struct {
    mino_val_t **entries;
    size_t       len;
    size_t       cap;
} intern_table_t;

/* Call-stack frame for stack traces. */
#define MAX_CALL_DEPTH 256

typedef struct {
    const char *name;
    const char *file;
    int         line;
} call_frame_t;

/* GC root-environment registry node (malloc-owned). */
typedef struct root_env {
    mino_env_t      *env;
    struct root_env *next;
} root_env_t;

/* Host-retained value ref (malloc-owned). */
struct mino_ref {
    mino_val_t      *val;
    struct mino_ref *next;
    struct mino_ref *prev;
};

/* Dynamic binding frame. */
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
    uintptr_t  start;
    uintptr_t  end;
    gc_hdr_t  *h;
} gc_range_t;

/* Environment binding. */
typedef struct {
    char       *name;
    mino_val_t *val;
} env_binding_t;

/* Full environment definition. */
struct mino_env {
    env_binding_t *bindings;
    size_t         len;
    size_t         cap;
    mino_env_t    *parent;
};

/* ------------------------------------------------------------------------- */
/* Runtime state                                                             */
/* ------------------------------------------------------------------------- */

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
    size_t          gc_ranges_valid;
    gc_range_t      gc_ranges_pending[8];
    size_t          gc_ranges_pending_len;

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

    /* Sort comparator */
    mino_val_t     *sort_comp_fn;
    mino_env_t     *sort_comp_env;

    /* Gensym counter */
    long            gensym_counter;

    /* Host-retained value refs */
    mino_ref_t     *ref_roots;

    /* Dynamic bindings */
    dyn_frame_t    *dyn_stack;

    /* Interrupt flag */
    volatile int    interrupted;

    /* GC save stack */
    mino_val_t     *gc_save[32];
    int             gc_save_len;

    /* Cached parsed core.mino forms (avoids re-parsing on second
     * mino_install_core call within the same state). */
    mino_val_t    **core_forms;
    size_t          core_forms_len;

    /* Fault injection: when fi_alloc_countdown > 0, decrement on each
     * gc_alloc_typed call; when it reaches zero, simulate OOM. */
    long            fi_alloc_countdown;
};

/* ------------------------------------------------------------------------- */
/* State field accessor macros                                               */
/* These macros require a local variable named `S` of type mino_state_t *.   */
/* ------------------------------------------------------------------------- */

#define gc_all              (S->gc_all)
#define gc_bytes_alloc      (S->gc_bytes_alloc)
#define gc_bytes_live       (S->gc_bytes_live)
#define gc_threshold        (S->gc_threshold)
#define gc_stress           (S->gc_stress)
#define gc_depth            (S->gc_depth)
#define gc_stack_bottom     (S->gc_stack_bottom)
#define gc_root_envs        (S->gc_root_envs)
#define gc_ranges           (S->gc_ranges)
#define gc_ranges_len       (S->gc_ranges_len)
#define gc_ranges_cap       (S->gc_ranges_cap)
#define gc_ranges_valid     (S->gc_ranges_valid)
#define gc_ranges_pending   (S->gc_ranges_pending)
#define gc_ranges_pending_len (S->gc_ranges_pending_len)
#define nil_singleton       (S->nil_singleton)
#define true_singleton      (S->true_singleton)
#define false_singleton     (S->false_singleton)
#define sym_intern          (S->sym_intern)
#define kw_intern           (S->kw_intern)
#define limit_steps         (S->limit_steps)
#define limit_heap          (S->limit_heap)
#define eval_steps          (S->eval_steps)
#define limit_exceeded      (S->limit_exceeded)
#define try_stack           (S->try_stack)
#define try_depth           (S->try_depth)
#define module_resolver     (S->module_resolver)
#define module_resolver_ctx (S->module_resolver_ctx)
#define module_cache        (S->module_cache)
#define module_cache_len    (S->module_cache_len)
#define module_cache_cap    (S->module_cache_cap)
#define meta_table          (S->meta_table)
#define meta_table_len      (S->meta_table_len)
#define meta_table_cap      (S->meta_table_cap)
#define print_depth         (S->print_depth)
#define error_buf           (S->error_buf)
#define call_stack          (S->call_stack)
#define call_depth          (S->call_depth)
#define trace_added         (S->trace_added)
#define reader_file         (S->reader_file)
#define reader_line         (S->reader_line)
#define eval_current_form   (S->eval_current_form)
#define rand_seeded         (S->rand_seeded)
#define sort_comp_fn        (S->sort_comp_fn)
#define sort_comp_env       (S->sort_comp_env)
#define gensym_counter      (S->gensym_counter)
#define dyn_stack           (S->dyn_stack)
#define interrupted         (S->interrupted)
#define gc_save             (S->gc_save)
#define gc_save_len         (S->gc_save_len)

/* GC pin/unpin macros. */
#define gc_pin(v) \
    do { if (gc_save_len < 32) gc_save[gc_save_len++] = (v); } while (0)
#define gc_unpin(n) \
    do { gc_save_len -= (n); } while (0)

/* ------------------------------------------------------------------------- */
/* Persistent vector constants                                               */
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
/* HAMT constants                                                            */
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
/* Shared function declarations (defined across translation units)           */
/* ------------------------------------------------------------------------- */

/* mino.c: allocation and GC */
void  *gc_alloc_typed(mino_state_t *S, unsigned char tag, size_t size);
mino_val_t *alloc_val(mino_state_t *S, mino_type_t type);
char  *dup_n(mino_state_t *S, const char *s, size_t len);
void   gc_collect(mino_state_t *S);
void   gc_note_host_frame(mino_state_t *S, void *addr);

/* mino.c: error reporting */
void        set_error(mino_state_t *S, const char *msg);
void        set_error_at(mino_state_t *S, const mino_val_t *form,
                         const char *msg);
void        clear_error(mino_state_t *S);
const char *type_tag_str(const mino_val_t *v);
void        push_frame(mino_state_t *S, const char *name, const char *file,
                       int line);
void        pop_frame(mino_state_t *S);

/* mino.c: metadata */
meta_entry_t *meta_find(mino_state_t *S, const char *name);
void meta_set(mino_state_t *S, const char *name, const char *doc,
              size_t doc_len, mino_val_t *source);

/* mino.c: environment */
env_binding_t *env_find_here(mino_env_t *env, const char *name);
void           env_bind(mino_state_t *S, mino_env_t *env, const char *name,
                        mino_val_t *val);
mino_env_t    *env_child(mino_state_t *S, mino_env_t *parent);
mino_env_t    *env_root(mino_state_t *S, mino_env_t *env);

/* mino.c: dynamic lookup */
mino_val_t *dyn_lookup(mino_state_t *S, const char *name);

/* mino.c: evaluator */
mino_val_t *eval_impl(mino_state_t *S, mino_val_t *form, mino_env_t *env,
                      int tail);
mino_val_t *eval(mino_state_t *S, mino_val_t *form, mino_env_t *env);
mino_val_t *eval_value(mino_state_t *S, mino_val_t *form, mino_env_t *env);
mino_val_t *eval_implicit_do(mino_state_t *S, mino_val_t *body,
                             mino_env_t *env);
mino_val_t *lazy_force(mino_state_t *S, mino_val_t *v);
mino_val_t *apply_callable(mino_state_t *S, mino_val_t *fn, mino_val_t *args,
                           mino_env_t *env);
int         sym_eq(const mino_val_t *v, const char *s);
mino_val_t *eval_args(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *macroexpand1(mino_state_t *S, mino_val_t *form, mino_env_t *env,
                         int *expanded);
mino_val_t *macroexpand_all(mino_state_t *S, mino_val_t *form,
                            mino_env_t *env);

/* val.c: constructors and interning */
mino_val_t *intern_lookup_or_create(mino_state_t *S, intern_table_t *tbl,
                                    mino_type_t type,
                                    const char *s, size_t len);
mino_val_t *make_fn(mino_state_t *S, mino_val_t *params, mino_val_t *body,
                    mino_env_t *env);

/* val.c: hashing */
uint32_t hash_val(const mino_val_t *v);
uint32_t fnv_mix(uint32_t h, unsigned char b);
uint32_t fnv_bytes(uint32_t h, const unsigned char *p, size_t n);

/* val.c: equality (forcing lazy seqs) */
int mino_eq_force(mino_state_t *S, const mino_val_t *a, const mino_val_t *b);

/* vec.c */
mino_val_t *vec_nth(const mino_val_t *v, size_t i);
mino_val_t *vec_conj1(mino_state_t *S, const mino_val_t *v, mino_val_t *item);
mino_val_t *vec_assoc1(mino_state_t *S, const mino_val_t *v, size_t i,
                       mino_val_t *item);
mino_val_t *vec_from_array(mino_state_t *S, mino_val_t **items, size_t len);

/* map.c */
unsigned     popcount32(uint32_t x);
hamt_entry_t *hamt_entry_new(mino_state_t *S, mino_val_t *key,
                             mino_val_t *val);
mino_hamt_node_t *hamt_assoc(mino_state_t *S, const mino_hamt_node_t *n,
                              hamt_entry_t *entry, uint32_t hash,
                              unsigned shift, int *replaced);
mino_val_t *hamt_get(const mino_hamt_node_t *n, const mino_val_t *key,
                     uint32_t hash, unsigned shift);
mino_val_t *map_get_val(const mino_val_t *m, const mino_val_t *key);

/* print.c */
void print_val(mino_state_t *S, FILE *out, const mino_val_t *v, int readably);

/* read.c */
const char *intern_filename(const char *name);

/* clone.c: actor primitives (registered by prim.c install) */
mino_val_t *prim_spawn(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_send_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_receive(mino_state_t *S, mino_val_t *args, mino_env_t *env);

/* GC marking (used by gc_collect) */
void gc_mark_interior(mino_state_t *S, const void *p);

#endif /* MINO_INTERNAL_H */
