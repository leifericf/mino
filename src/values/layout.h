/*
 * values/layout.h -- pointer-tagged value representation: tag scheme,
 * struct mino_val body, and the opaque forward typedefs for the
 * collection node types that mino_val embeds by pointer.
 *
 * Internal to the runtime. Embedders should only include mino.h. The
 * shape of struct mino_val, the tag scheme, and the helpers declared
 * here are NOT part of the public API contract and can change between
 * patch releases without notice.
 */

#ifndef VALUES_LAYOUT_H
#define VALUES_LAYOUT_H

#include "mino.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Forward typedefs for internal node types                                  */
/* ------------------------------------------------------------------------- */

typedef struct mino_vec_node  mino_vec_node_t;
typedef struct mino_hamt_node mino_hamt_node_t;
typedef struct mino_rb_node   mino_rb_node_t;
struct mino_bc_fn;            /* compiled-fn record */

/* ------------------------------------------------------------------------- */
/* Pointer-tagged value representation                                       */
/* ------------------------------------------------------------------------- */

/*
 * A mino_val* is either a real heap pointer to struct mino_val or an
 * inline tagged scalar. The three low bits encode which.
 *
 *   tag 000 -> heap pointer (alloc guarantees 8-byte alignment; every
 *              alloc site asserts this in debug builds).
 *   tag 001 -> inline 61-bit signed int; payload is bits 63..3.
 *              Range MINO_INT_MIN .. MINO_INT_MAX
 *              (-2^60 .. 2^60 - 1, ~= +/-1.15e18).
 *   tag 010 -> inline BOOL.
 *   tag 011 -> inline NIL.
 *   tag 100 -> inline CHAR.
 *   tag 101..111 -> reserved.
 *
 * Portability:
 *   - 64-bit hosts only. mino does not support 32-bit targets.
 *   - MINO_INT_VAL decode relies on arithmetic right shift of signed
 *     integers, which C99 6.5.7p5 leaves implementation-defined for
 *     negative operands. Every supported toolchain (clang, gcc, msvc
 *     on x86_64 and arm64) implements it as sign-preserving. A port
 *     to a target with logical right shift will need a sign-extending
 *     decode instead.
 */

#define MINO_TAG_BITS  3
#define MINO_TAG_MASK  ((uintptr_t)0x7)

#define MINO_TAG_PTR   ((uintptr_t)0x0)
#define MINO_TAG_INT   ((uintptr_t)0x1)
#define MINO_TAG_BOOL  ((uintptr_t)0x2)
#define MINO_TAG_NIL   ((uintptr_t)0x3)
#define MINO_TAG_CHAR  ((uintptr_t)0x4)

#define MINO_TAG(v)    ((uintptr_t)(v) & MINO_TAG_MASK)

#define MINO_IS_PTR(v) ((v) != NULL && MINO_TAG(v) == MINO_TAG_PTR)
#define MINO_IS_INT(v) (MINO_TAG(v) == MINO_TAG_INT)

/* 61-bit signed inline-int range. */
#define MINO_INT_MAX   ((long long)((1ULL << 60) - 1))
#define MINO_INT_MIN   (-((long long)(1LL << 60)))

#define MINO_MAKE_INT(n) \
    ((mino_val *)((uintptr_t)((uint64_t)(long long)(n) << MINO_TAG_BITS) \
                    | MINO_TAG_INT))

#define MINO_INT_VAL(v) (((long long)(intptr_t)(v)) >> MINO_TAG_BITS)

#define MINO_IS_BOOL(v) (MINO_TAG(v) == MINO_TAG_BOOL)
#define MINO_IS_NIL(v)  ((v) == NULL || MINO_TAG(v) == MINO_TAG_NIL)
#define MINO_IS_CHAR(v) (MINO_TAG(v) == MINO_TAG_CHAR)

#define MINO_TRUE_PAYLOAD  ((uintptr_t)0x1)
#define MINO_FALSE_PAYLOAD ((uintptr_t)0x0)
#define MINO_MAKE_BOOL(b) \
    ((mino_val *)(((b) ? MINO_TRUE_PAYLOAD : MINO_FALSE_PAYLOAD) << MINO_TAG_BITS \
                    | MINO_TAG_BOOL))
#define MINO_BOOL_VAL(v) \
    ((int)(((uintptr_t)(v) >> MINO_TAG_BITS) & 0x1))

#define MINO_MAKE_NIL ((mino_val *)MINO_TAG_NIL)

#define MINO_MAKE_CHAR(cp) \
    ((mino_val *)(((uintptr_t)(uint32_t)(cp) << MINO_TAG_BITS) | MINO_TAG_CHAR))
#define MINO_CHAR_VAL(v) \
    ((int)((uintptr_t)(v) >> MINO_TAG_BITS))

/* ------------------------------------------------------------------------- */
/* Lazy-sequence realization state machine                                   */
/* ------------------------------------------------------------------------- */

/* Pre-tri-state code stored 0 or 1 in lazy.realized and tested it as a
 * boolean. Concurrent forcers can both observe 0 before either writes
 * 1, so both run the thunk and tear-publish cached. The realizing
 * sentinel narrows this to a CAS-claimed window: state moves 0 -> 2 ->
 * 1 (winner) while losers see 2 and wait. Readers that don't go
 * through lazy_force still test `realized == LAZY_REALIZED`, never
 * truthy, so a mid-realization slot is never misread as "done". */
#define LAZY_UNREALIZED 0
#define LAZY_REALIZED   1
#define LAZY_REALIZING  2

/* ------------------------------------------------------------------------- */
/* Value struct body                                                         */
/* ------------------------------------------------------------------------- */

struct mino_val {
    mino_type type;     /* value type tag */
    mino_val *meta;     /* metadata map (NULL when absent) */
    union {
        int b;            /* MINO_BOOL: 0 or 1 */
        long long i;      /* MINO_INT */
        double f;         /* MINO_FLOAT or MINO_FLOAT32 (32-bit value
                           * already narrowed via (double)(float)d at
                           * construction so equality / hash / print
                           * see the rounded value). */
        int ch;           /* MINO_CHAR: Unicode codepoint (0..0x10FFFF) */
        struct {          /* MINO_STRING, MINO_SYMBOL, MINO_KEYWORD */
            char *data;   /* byte content (NUL-terminated) */
            size_t len;   /* length in bytes (excluding NUL) */
            uint32_t hash;/* FNV-1a of (data, len); 0 means "not cached"
                           * — recompute on the spot. Populated by the
                           * intern path; non-interned strings leave it
                           * zero. Internal use only; do not set
                           * externally. */
            /* For MINO_SYMBOL / MINO_KEYWORD: length of the namespace
             * prefix (excluding the separating `/`). 0 means the value
             * has no namespace (e.g. `:foo`, `'bar`). When non-zero,
             * `data[ns_len] == '/'` and `data[ns_len+1..len-1]` is the
             * name. This is the only correct way to recover the
             * original (ns, name) split — the 2-arg keyword/symbol
             * constructor records the boundary explicitly so the
             * (keyword "a/b" "c") vs (keyword "a" "b/c") distinction
             * survives, matching JVM Clojure. For MINO_STRING this
             * field is always 0. */
            size_t ns_len;
        } s;
        struct {          /* MINO_CONS */
            mino_val *car;   /* first element */
            mino_val *cdr;   /* rest of the list */
            const char *file;  /* source file (NULL if unknown) */
            int         line;  /* source line (0 if unknown) */
            int         column;/* source column (0 if unknown) */
            int         not_list; /* set when this cell came from a
                                   * (cons x y) call: list? is false,
                                   * peek/pop throw. List literals from
                                   * the reader keep this 0. */
        } cons;
        struct {          /* MINO_VECTOR: persistent 32-way trie with tail */
            mino_vec_node_t *root;     /* trie spine (NULL when len <= 32) */
            mino_vec_node_t *tail;     /* partial leaf, 1..32 slots used */
            unsigned         tail_len; /* number of valid slots in tail */
            unsigned         shift;    /* height of root in multiples of 5 */
            size_t           len;      /* visible element count */
            size_t           offset;   /* first visible element (0 unless subvec) */
            size_t           blen;     /* backing total (len+offset when no nesting) */
            uint32_t         cached_hash; /* hash_val of this vec; 0 = uncomputed. */
        } vec;
        struct {          /* MINO_MAP: flatmap (small) or HAMT (large) */
            mino_hamt_node_t *root;      /* HAMT root, or NULL for flatmap/empty */
            mino_val       *key_order; /* MINO_VECTOR of keys, insertion order */
            mino_val       *val_order; /* MINO_VECTOR of vals (flatmap), or NULL */
            size_t            len;       /* number of entries */
            uint32_t          cached_hash; /* hash_val of this map; 0 = uncomputed. */
        } map;
        struct {          /* MINO_SET: HAMT with sentinel values */
            mino_hamt_node_t *root;      /* HAMT root (NULL when len == 0) */
            mino_val       *key_order; /* MINO_VECTOR of elements */
            size_t            len;       /* number of elements */
            uint32_t          cached_hash; /* hash_val of this set; 0 = uncomputed. */
        } set;
        struct {          /* MINO_SORTED_MAP / MINO_SORTED_SET: red-black tree */
            mino_rb_node_t *root;       /* RB tree root (NULL when empty) */
            mino_val     *comparator; /* NULL = natural order, fn = custom */
            size_t          len;        /* number of entries */
        } sorted;
        struct {          /* MINO_PRIM */
            const char *name;  /* primitive name */
            mino_prim_fn  fn;  /* legacy cons-list ABI (NULL iff fn2 set) */
            mino_prim_fn2 fn2; /* argv ABI; non-NULL takes precedence */
        } prim;
        struct {          /* MINO_FN: user-defined closure */
            mino_val *params;
            mino_val *body;
            mino_env *env;
            const char *defining_ns;
            int         shape;
            struct mino_bc_fn *bc;
            /* If non-NULL, this fn's only behavior is to invoke a
             * single primitive on its arguments -- e.g. `(fn [x] (inc
             * x))` or `(fn [x y] (+ x y))`. The pipeline fast lanes
             * dereference this to skip apply_callable and route
             * straight to the prim's specialised path. NULL means the
             * fn is opaque (custom body, multi-arity, captures, etc.)
             * and must use the regular call path. */
            mino_val *wraps_prim;
            /* Closure -> template back-pointer. Closures created by
             * OP_CLOSURE point at the template fn whose bc / clauses
             * they inherit; templates and plain fns leave this NULL.
             * Used by invoke_bc_fn_argv's fold-staleness recompile to
             * recompile once via the template instead of per-closure.
             * GC-traced through the MINO_FN walker. */
            mino_val *template_fn;
        } fn;
        struct {          /* MINO_HANDLE: opaque host pointer + tag */
            void       *ptr;
            const char *tag;
            void       (*finalizer)(void *ptr, const char *tag);
        } handle;
        struct {          /* MINO_ATOM: mutable reference cell */
            mino_val *val;
            mino_val *watches;
            mino_val *validator;
        } atom;
        struct {          /* MINO_VOLATILE: single-slot mutable cell */
            mino_val *val;
        } volatile_;
        struct {          /* MINO_LAZY: deferred sequence */
            mino_val *body;
            mino_env *env;
            mino_val *cached;
            mino_val *(*c_thunk)(struct mino_state *, mino_val *);
            /* Tri-state machine: 0=untouched, 2=a thread is computing
             * the thunk now, 1=cached holds the published value. CAS
             * 0 -> 2 claims the realization; only the claiming thread
             * later writes 1. Concurrent forcers spin (yield_lock +
             * sched_yield) while state==2, then read cached after
             * state observes 1. If the thunk throws, the realizer
             * resets state to 0 so a retry can re-run the thunk,
             * matching JVM Clojure's LazySeq semantics. */
            int         realized;
        } lazy;
        struct {          /* MINO_CHUNK: fixed-cap value buffer */
            mino_val **vals;
            unsigned     cap;
            unsigned     len;
            int          sealed;
        } chunk;
        struct {          /* MINO_CHUNKED_CONS: chunk + offset + more */
            mino_val *chunk;
            mino_val *more;
            unsigned    off;
        } chunked_cons;
        struct {          /* MINO_RECUR */
            mino_val *args;
        } recur;
        struct {          /* MINO_TAIL_CALL */
            mino_val *fn;
            mino_val *args;
        } tail_call;
        struct {          /* MINO_REDUCED */
            mino_val *val;
        } reduced;
        struct {          /* MINO_VAR: first-class var */
            const char *ns;
            const char *sym;
            mino_val *root;
            int         dynamic;
            int         bound;
            int         is_private;
            mino_val *watches;
            mino_val *validator;
            unsigned    version;
        } var;
        struct {          /* MINO_TRANSIENT */
            mino_val *current;
            int         valid;
            /* Owner ID minted at creation by
             * S->ns_vars.transient_owner_next++; written into the owner field
             * of every owner-tagged vec/HAMT node so the transient's
             * *_bang mutators can recognise their own nodes and mutate
             * them in place. 0 means "no in-place editing tier" -- the
             * wrapper-mode fallback that allocates a fresh persistent
             * value and swaps it in. */
            uintptr_t   owner_id;
        } transient;
        struct {          /* MINO_BIGINT */
            void *mpz;
        } bigint;
        struct {          /* MINO_RATIO */
            mino_val *num;
            mino_val *denom;
        } ratio;
        struct {          /* MINO_BIGDEC */
            mino_val *unscaled;
            int         scale;
        } bigdec;
        struct {          /* MINO_TYPE: first-class record type */
            const char *ns;
            const char *name;
            mino_val *fields;
        } record_type;
        struct {          /* MINO_RECORD */
            mino_val  *type;
            mino_val **vals;
            mino_val  *ext;
        } record;
        struct {          /* MINO_FUTURE */
            struct mino_future *impl;
        } future;
        struct {          /* MINO_UUID */
            unsigned char bytes[16];
        } uuid;
        struct {          /* MINO_REGEX */
            mino_val *source;
        } regex;
        struct {          /* MINO_HOST_ARRAY */
            mino_val **vals;
            size_t       len;
            unsigned char element_kind; /* host_array_kind_t */
        } host_array;
        struct {          /* MINO_MAP_ENTRY */
            mino_val *k;
            mino_val *v;
        } map_entry;
        struct {          /* MINO_TX_REF */
            mino_val    *val;
            mino_val    *watches;
            mino_val    *validator;
            uint64_t       version;
            uint64_t       ref_id;
            mino_state  *owning_state;
        } tx_ref;
        struct {          /* MINO_AGENT */
            mino_val *val;
            mino_val *watches;
            mino_val *validator;
            mino_val *err;
            mino_val *err_handler;
            int         err_mode;
            int         in_flight;
            uint64_t    agent_id;
            mino_state *owning_state;
        } agent;
        struct {          /* MINO_CHAN: clojure.core.async channel */
            struct mino_chan_impl *impl;
        } chan;
    } as;
};

#ifdef __cplusplus
}
#endif

#endif /* VALUES_LAYOUT_H */
