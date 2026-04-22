/*
 * mino_internal.h -- shared internal types, macros, and declarations.
 *
 * Included by each translation unit of the mino runtime. Not part of the
 * public API; embedders should only use mino.h.
 */

#ifndef MINO_INTERNAL_H
#define MINO_INTERNAL_H

#include "mino.h"
#include "diag.h"

#include <assert.h>
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
    GC_T_VALARR     = 8,
    GC_T_RB_NODE    = 9
};

/* ------------------------------------------------------------------------- */
/* Internal type definitions                                                 */
/* ------------------------------------------------------------------------- */

/* Generation tags for the two-generation collector. The OLD value is
 * set when a nursery survivor outlives the promotion age. gc_alloc_typed
 * stamps every new allocation GC_GEN_YOUNG. */
enum {
    GC_GEN_YOUNG = 0,
    GC_GEN_OLD   = 1
};

/* Collector phase -- currently three values. IDLE means no cycle is in
 * progress. MINOR is the young-only mark-and-sweep; it filters OLD
 * headers out of the mark frontier so tracing stays proportional to
 * young reachability. MAJOR covers the full-heap STW cycle; it is a
 * placeholder for the eventual MAJOR_MARK/MAJOR_SWEEP split of the
 * incremental collector. */
enum {
    GC_PHASE_IDLE  = 0,
    GC_PHASE_MINOR = 1,
    GC_PHASE_MAJOR = 2
};

/* Header layout on a 64-bit target: 1+1+1+1 bytes followed by 4 bytes
 * of padding, then 8-byte size and 8-byte next. Four single-byte fields
 * fit into the padding slot that existed for type_tag/mark alone; the
 * struct size is unchanged. */
typedef struct gc_hdr {
    unsigned char  type_tag;
    unsigned char  mark;
    unsigned char  gen;
    unsigned char  age;
    unsigned char  dirty;  /* remset membership bit; see gc_write_barrier */
    size_t         size;
    struct gc_hdr *next;
} gc_hdr_t;

/* Exception handling: setjmp/longjmp stack for try/catch. */
#define MAX_TRY_DEPTH 64

typedef struct {
    jmp_buf     buf;
    mino_val_t *exception;
} try_frame_t;

/* Red-black tree node for sorted maps/sets. */
struct mino_rb_node {
    mino_val_t     *key;
    mino_val_t     *val;    /* NULL sentinel for sorted sets */
    mino_rb_node_t *left;
    mino_rb_node_t *right;
    unsigned char   red;    /* 1 = red, 0 = black */
};

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

/* Intern table with hash index for O(1) lookup. */
typedef struct {
    mino_val_t **entries;
    size_t       len;
    size_t       cap;
    size_t      *ht_buckets;  /* open-addressing hash table: index into entries[] */
    size_t       ht_cap;      /* power of 2; SIZE_MAX marks empty slots */
} intern_table_t;

/* Call-stack frame for stack traces. */
#define MAX_CALL_DEPTH 256

typedef struct {
    const char *name;
    const char *file;
    int         line;
    int         column;
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

/* Namespace alias entry. */
typedef struct {
    char *alias;
    char *full_name;
} ns_alias_t;

/* Var registry entry. */
typedef struct {
    const char *ns;      /* interned namespace */
    const char *name;    /* interned name */
    mino_val_t *var;     /* the MINO_VAR value */
} var_entry_t;

/* Host interop: capability registry types. */
enum {
    HOST_CTOR   = 0,
    HOST_METHOD = 1,
    HOST_STATIC = 2,
    HOST_GETTER = 3
};

typedef struct {
    const char  *name;    /* interned member name (NULL for ctor) */
    int          arity;   /* expected arg count (-1 = variadic) */
    int          kind;    /* HOST_CTOR, HOST_METHOD, HOST_STATIC, HOST_GETTER */
    mino_host_fn fn;
    void        *fn_ctx;
} host_member_t;

typedef struct {
    const char    *type_key;  /* interned type tag */
    host_member_t *members;   /* malloc-owned array */
    size_t         members_len;
    size_t         members_cap;
} host_type_t;

/* Full environment definition.
 * Large frames (>= ENV_HASH_THRESHOLD bindings) get a hash index for O(1)
 * lookup; small frames use linear scan (faster for typical let/fn sizes). */
#define ENV_HASH_THRESHOLD 32

/* Small-integer cache range. Must fit in the small_ints[] array (256 slots). */
#define MINO_SMALL_INT_LO (-128)
#define MINO_SMALL_INT_HI  127

struct mino_env {
    env_binding_t *bindings;
    size_t         len;
    size_t         cap;
    mino_env_t    *parent;
    size_t        *ht_buckets;  /* hash index: maps hash -> binding slot */
    size_t         ht_cap;      /* power of 2; SIZE_MAX = empty slot */
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
    size_t          gc_collections_minor;
    size_t          gc_collections_major;
    size_t          gc_total_freed;
    size_t          gc_total_ns;       /* cumulative ns spent in gc_major_collect */
    size_t          gc_max_ns;         /* largest single-collection ns */
    /* Generational bookkeeping. Maintained continuously on every
     * allocation, sweep, and promotion: gc_bytes_young + gc_bytes_old
     * equals gc_bytes_alloc. gc_old_baseline captures gc_bytes_old
     * right after the last major sweep; future major cycles trigger
     * when gc_bytes_old exceeds baseline by the growth tenths factor. */
    size_t          gc_bytes_young;
    size_t          gc_bytes_old;
    size_t          gc_old_baseline;
    /* Remembered set: every old-gen header that observed a store of a
     * young-gen pointer since the last minor or major cycle. The array
     * doubles as needed. Each member has gc_hdr_t::dirty = 1 while
     * present, so repeated stores to the same container are deduped. */
    gc_hdr_t      **gc_remset;
    size_t          gc_remset_len;
    size_t          gc_remset_cap;
    /* Collector tuning parameters. gc_nursery_bytes triggers a minor
     * collection when exceeded. gc_promotion_age is the number of
     * minor survivals before a young object flips to old. Both have
     * defaults from state_init; a future mino_gc_set_param lets a
     * host override them. */
    size_t          gc_nursery_bytes;
    unsigned        gc_promotion_age;
    int             gc_phase;
    gc_hdr_t      **gc_mark_stack;
    size_t          gc_mark_stack_len;
    size_t          gc_mark_stack_cap;
    gc_hdr_t       *gc_freelists[4];   /* per-size-class recycling */
    /* Cached [min, max) bounds of all managed allocations. Lets the
     * conservative stack scan reject non-pointer words before doing a
     * binary search through the range index. */
    uintptr_t       gc_heap_min;
    uintptr_t       gc_heap_max;

    /* Singletons */
    mino_val_t      nil_singleton;
    mino_val_t      true_singleton;
    mino_val_t      false_singleton;
    /* Trampoline sentinels reused across recur/tail-call to avoid
     * per-iteration allocation. Their args/fn fields are replaced in-place
     * and the containing eval loop consumes them before any other code
     * runs, so sharing one cell per kind is safe. */
    mino_val_t      recur_sentinel;
    mino_val_t      tail_call_sentinel;

    /* Small-integer cache: mino_int(S, n) returns the shared cell for
     * n in [MINO_SMALL_INT_LO, MINO_SMALL_INT_HI]. Arithmetic-heavy code
     * (fib, loops, reductions) produces many small-int results and
     * re-boxing them dominates allocation without this cache. */
    mino_val_t      small_ints[256];

    /* Intern tables */
    intern_table_t  sym_intern;
    intern_table_t  kw_intern;

    /* Cached interned special-form symbols for O(1) pointer-eq dispatch.
     * Populated lazily on first eval_impl call. */
    int             sf_initialized;
    mino_val_t     *sf_quote;
    mino_val_t     *sf_quasiquote;
    mino_val_t     *sf_unquote;
    mino_val_t     *sf_unquote_splicing;
    mino_val_t     *sf_defmacro;
    mino_val_t     *sf_declare;
    mino_val_t     *sf_ns;
    mino_val_t     *sf_var;
    mino_val_t     *sf_def;
    mino_val_t     *sf_if;
    mino_val_t     *sf_do;
    mino_val_t     *sf_let;
    mino_val_t     *sf_let_star;
    mino_val_t     *sf_fn;
    mino_val_t     *sf_fn_star;
    mino_val_t     *sf_recur;
    mino_val_t     *sf_loop;
    mino_val_t     *sf_loop_star;
    mino_val_t     *sf_try;
    mino_val_t     *sf_binding;
    mino_val_t     *sf_lazy_seq;
    mino_val_t     *sf_new;
    mino_val_t     *sf_when;
    mino_val_t     *sf_and;
    mino_val_t     *sf_or;

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
    mino_diag_t    *last_diag;      /* malloc-owned structured diagnostic */

    /* Reader */
    const char     *reader_file;
    int             reader_line;
    int             reader_col;
    const char     *reader_dialect;   /* "mino" */

    /* Source cache for diagnostic rendering. */
    #define MINO_SOURCE_CACHE_SIZE 4
    struct {
        const char *file;   /* interned filename */
        char       *text;   /* malloc-owned full source text */
        size_t      len;    /* length of text */
    } source_cache[4];

    /* Namespace */
    const char     *current_ns;       /* from (ns ...), default "user" */
    ns_alias_t     *ns_aliases;
    size_t          ns_alias_len;
    size_t          ns_alias_cap;

    /* Var registry */
    var_entry_t    *var_registry;
    size_t          var_registry_len;
    size_t          var_registry_cap;

    /* Host interop */
    int             interop_enabled;
    host_type_t    *host_types;
    size_t          host_types_len;
    size_t          host_types_cap;

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
    mino_val_t     *gc_save[64];
    int             gc_save_len;

    /* Cached parsed core.mino forms (avoids re-parsing on second
     * mino_install_core call within the same state). */
    mino_val_t    **core_forms;
    size_t          core_forms_len;

    /* Fault injection: when fi_alloc_countdown > 0, decrement on each
     * gc_alloc_typed call; when it reaches zero, simulate OOM. */
    long            fi_alloc_countdown;

    /* Fault injection for raw (non-GC) allocation paths such as clone
     * serialization and mailbox buffers. Same semantics as above. */
    long            fi_raw_countdown;

    /* Async scheduler run queue (sched_entry_t defined in async_scheduler.h). */
    struct sched_entry *async_run_head;
    struct sched_entry *async_run_tail;

    /* Async timer queue (timer_entry_t defined in async_timer.h). */
    struct timer_entry *async_timers;
};

/* GC pin/unpin macros.
 * Always increment gc_save_len so pin/unpin pairs stay balanced even
 * when the save array is full.  Only write the pointer when there is
 * space; beyond 64 the value is not pinned but the counter remains
 * correct, preventing underflow on the matching unpin.
 * Require a local variable named `S` of type mino_state_t *. */
#define GC_SAVE_MAX 64
#define gc_pin(v) \
    do { if (S->gc_save_len < GC_SAVE_MAX) S->gc_save[S->gc_save_len] = (v); \
         S->gc_save_len++; } while (0)
#define gc_unpin(n) \
    do { assert(S->gc_save_len >= (n)); \
         S->gc_save_len -= (n); } while (0)

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
/*                                                                           */
/* Ownership conventions used in this header:                                */
/*   GC-owned  — returned pointer is managed by the garbage collector.       */
/*               It survives until the next collection unless pinned or      */
/*               reachable from a rooted environment.  Callers that need     */
/*               a value to survive across allocation must gc_pin it.        */
/*   borrowed  — returned pointer aliases existing storage.  The caller      */
/*               must not free it and must not retain it past the next       */
/*               mutation of the owning container.                           */
/*   static    — returned pointer has program lifetime; never freed.         */
/*   malloc-owned — returned pointer must be freed by the caller or by a     */
/*               documented owner (e.g. dyn_binding_list_free).              */
/* Parameters marked "borrowed" are read-only and not retained.              */
/* Parameters marked "consumed" transfer ownership to the callee.            */
/* ------------------------------------------------------------------------- */

/* runtime_gc.c: allocation and collection driver.
 * All gc_alloc/alloc_val returns are GC-owned. */
void  *gc_alloc_typed(mino_state_t *S, unsigned char tag, size_t size);
mino_val_t *alloc_val(mino_state_t *S, mino_type_t type);     /* GC-owned */
char  *dup_n(mino_state_t *S, const char *s, size_t len);     /* GC-owned copy */
void   gc_major_collect(mino_state_t *S);
void   gc_minor_collect(mino_state_t *S);
void   gc_note_host_frame(mino_state_t *S, void *addr);

/* Free-list size class lookup. Returns -1 for variable-size allocations
 * that cannot be recycled. Shared between alloc (runtime_gc.c) and sweep
 * (runtime_gc_major.c). */
int    gc_freelist_class(size_t size);

/* Mark-stack primitives (runtime_gc.c). Mark the header live and push it
 * for tracing; interior-pointer variant resolves a heap pointer to its
 * header first and is safe on stale/stack words. gc_drain_mark_stack pops
 * until empty, tracing each header's outgoing references.
 * gc_mark_push filters OLD headers out when gc_phase == GC_PHASE_MINOR
 * so minor marking stays proportional to young reachability.
 * gc_trace_children unconditionally pushes every child pointer held in
 * h into the mark stack; minor uses it to trace remembered-set
 * entries even though their header is OLD. */
void gc_mark_push(mino_state_t *S, gc_hdr_t *h);
void gc_drain_mark_stack(mino_state_t *S);
void gc_trace_children(mino_state_t *S, gc_hdr_t *h);

/* runtime_gc_roots.c: range index over live headers plus root enumeration.
 * The range index backs gc_find_header_for_ptr, which resolves a raw
 * machine word to its owning header during conservative stack scans and
 * interior-pointer mark. gc_range_insert buffers new allocations; the
 * index is rebuilt once per collection. */
void      gc_build_range_index(mino_state_t *S);
void      gc_range_insert(mino_state_t *S, gc_hdr_t *h);
void      gc_range_compact(mino_state_t *S);
gc_hdr_t *gc_find_header_for_ptr(mino_state_t *S, const void *p);
void      gc_mark_roots(mino_state_t *S);
void      gc_scan_stack(mino_state_t *S);

/* runtime_gc_major.c: full-heap sweep driven by gc_major_collect. Frees every
 * allocation whose mark bit is clear and resets the mark bit on
 * survivors; updates gc_bytes_live and gc_threshold. */
void gc_sweep(mino_state_t *S);

/* runtime_gc_barrier.c: write barrier and remembered-set machinery.
 * Call BEFORE storing new_value into a field owned by container; the
 * barrier inspects the generation of each and, when the store creates
 * an old->young reference, appends container to the remembered set
 * (deduped via container->dirty). Both pointers are the PAYLOAD start
 * of a GC-allocated object (e.g. mino_val_t*, mino_env_t*, or a raw
 * buffer from gc_alloc_typed). Pass NULL for new_value when the field
 * is being cleared. */
void gc_write_barrier(mino_state_t *S, void *container, const void *new_value);

/* Clear every dirty bit and empty the remembered set. Called at the
 * end of every full cycle -- after a complete trace, the old-to-young
 * reference set is rebuilt by future barriers, not inherited. */
void gc_remset_reset(mino_state_t *S);

/* Monotonic wall-clock nanoseconds. Uses CLOCK_MONOTONIC on POSIX,
 * QueryPerformanceCounter on Windows, clock() as coarse fallback.
 * Shared between prim_nano_time and gc_major_collect timing. */
long long mino_monotonic_ns(void);

/* runtime_error.c: error reporting, call stack, metadata.
 * set_error/set_error_at copy msg into S->error_buf; msg is borrowed. */
void        set_error(mino_state_t *S, const char *msg);          /* msg: borrowed */
void        set_error_at(mino_state_t *S, const mino_val_t *form, /* form: borrowed */
                         const char *msg);                         /* msg: borrowed */
void        clear_error(mino_state_t *S);
void        set_diag(mino_state_t *S, mino_diag_t *d);           /* d: consumed */
void        source_cache_store(mino_state_t *S, const char *file,
                               const char *text, size_t len);
const char *source_cache_get_line(mino_state_t *S, const char *file,
                                  int line, size_t *out_len);
void        set_eval_diag(mino_state_t *S, const mino_val_t *form,
                          const char *kind, const char *code,
                          const char *msg);
const char *type_tag_str(const mino_val_t *v);                    /* static string */
void        push_frame(mino_state_t *S, const char *name,     /* name: borrowed */
                       const char *file, int line,            /* file: borrowed */
                       int column);
void        pop_frame(mino_state_t *S);
void        append_trace(mino_state_t *S);
meta_entry_t *meta_find(mino_state_t *S, const char *name);   /* borrowed into meta_table */
void meta_set(mino_state_t *S, const char *name,              /* name: borrowed (copied) */
              const char *doc, size_t doc_len,                 /* doc: borrowed (copied) */
              mino_val_t *source);                             /* source: GC-owned, retained */

/* runtime_env.c: environment and dynamic bindings.
 * Environments are GC-owned. Bindings within are borrowed views. */
mino_env_t    *env_alloc(mino_state_t *S, mino_env_t *parent); /* GC-owned */
env_binding_t *env_find_here(mino_env_t *env, const char *name); /* borrowed */
void           env_bind(mino_state_t *S, mino_env_t *env,
                        const char *name,                      /* borrowed (copied) */
                        mino_val_t *val);                      /* GC-owned, retained */
void           env_bind_sym(mino_state_t *S, mino_env_t *env,
                        mino_val_t *sym,                       /* interned symbol */
                        mino_val_t *val);                      /* GC-owned, retained */
mino_env_t    *env_child(mino_state_t *S, mino_env_t *parent); /* GC-owned */
mino_env_t    *env_root(mino_state_t *S, mino_env_t *env);     /* borrowed (walks up) */
mino_val_t    *dyn_lookup(mino_state_t *S, const char *name);  /* borrowed */
void           dyn_binding_list_free(dyn_binding_t *head);     /* frees malloc chain */

/* val.c: var constructor. */
mino_val_t    *mino_mk_var(mino_state_t *S, const char *ns, const char *name,
                           mino_val_t *root);

/* runtime_var.c: var registry helpers. */
mino_val_t    *var_intern(mino_state_t *S, const char *ns, const char *name);
void           var_set_root(mino_state_t *S, mino_val_t *var, mino_val_t *val);
mino_val_t    *var_find(mino_state_t *S, const char *ns, const char *name);

/* mino.c: evaluator core helpers.
 * All eval/expand functions return GC-owned values (NULL on error). */
int         sym_eq(const mino_val_t *v, const char *s);        /* pure */
mino_val_t *eval_value(mino_state_t *S, mino_val_t *form, mino_env_t *env);
mino_val_t *eval_implicit_do(mino_state_t *S, mino_val_t *body,
                             mino_env_t *env);
mino_val_t *eval_implicit_do_impl(mino_state_t *S, mino_val_t *body,
                                  mino_env_t *env, int tail);
mino_val_t *lazy_force(mino_state_t *S, mino_val_t *v);       /* mutates lazy cache */
mino_val_t *eval_args(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *macroexpand1(mino_state_t *S, mino_val_t *form, mino_env_t *env,
                         int *expanded);
mino_val_t *macroexpand_all(mino_state_t *S, mino_val_t *form,
                            mino_env_t *env);
mino_val_t *quasiquote_expand(mino_state_t *S, mino_val_t *form,
                              mino_env_t *env);

/* eval_special.c: dispatch, special forms, destructuring, apply.
 * All return GC-owned values (NULL on error). */
mino_val_t *eval_impl(mino_state_t *S, mino_val_t *form, mino_env_t *env,
                      int tail);
mino_val_t *eval(mino_state_t *S, mino_val_t *form, mino_env_t *env);
mino_val_t *apply_callable(mino_state_t *S, mino_val_t *fn, mino_val_t *args,
                           mino_env_t *env);

/* val.c: constructors and interning.
 * Interned values are GC-owned singletons (deduplicated by content). */
mino_val_t *intern_lookup_or_create(mino_state_t *S, intern_table_t *tbl,
                                    mino_type_t type,
                                    const char *s, size_t len);  /* GC-owned */
mino_val_t *make_fn(mino_state_t *S, mino_val_t *params, mino_val_t *body,
                    mino_env_t *env);                            /* GC-owned */

/* val.c: hashing (pure, no allocation) */
uint32_t hash_val(const mino_val_t *v);
uint32_t fnv_mix(uint32_t h, unsigned char b);
uint32_t fnv_bytes(uint32_t h, const unsigned char *p, size_t n);

/* val.c: equality (may force lazy seqs, triggering allocation) */
int mino_eq_force(mino_state_t *S, const mino_val_t *a, const mino_val_t *b);

/* vec.c: persistent vector operations.
 * vec_nth returns a borrowed pointer into existing trie storage.
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

/* map.c: HAMT operations.
 * hamt_get/map_get_val return borrowed pointers into existing HAMT nodes.
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

/* rbtree.c: persistent red-black tree operations for sorted map/set.
 * rb_get returns a borrowed pointer; rb_assoc/rb_dissoc return new trees. */
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

/* print.c */
void print_val(mino_state_t *S, FILE *out, const mino_val_t *v, int readably);

/* read.c */
const char *intern_filename(const char *name);                    /* static/interned */

/* clone.c: actor primitives (registered by prim.c install).
 * Standard primitive signature: args borrowed, return GC-owned. */
mino_val_t *prim_spawn(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_send_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env);
mino_val_t *prim_receive(mino_state_t *S, mino_val_t *args, mino_env_t *env);

/* GC marking (used by gc_major_collect and by root enumeration). */
void gc_mark_interior(mino_state_t *S, const void *p);

/* host_interop.c: capability registry lookup. */
host_type_t   *host_type_find(mino_state_t *S, const char *type_key);
host_member_t *host_member_find(host_type_t *t, const char *name,
                                int kind, int arity);

#endif /* MINO_INTERNAL_H */
