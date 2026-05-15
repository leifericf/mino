/*
 * mino_internal.h -- private companion header to mino.h.
 *
 * Holds everything that is implementation-detail-but-needed-by-the-runtime:
 *
 *   - the body of struct mino_val (union arms, internal cache fields),
 *   - the pointer-tag encoding (MINO_TAG_*, MINO_MAKE_*, MINO_*_VAL),
 *   - opaque-to-embedders typedefs (mino_vec_node_t, mino_hamt_node_t,
 *     mino_rb_node_t, struct mino_bc_fn),
 *   - the host-array element-kind enum,
 *   - test-only fault-injection helpers,
 *   - chunked-seq and host-array constructors that are runtime-internal.
 *
 * Internal to the runtime. Embedders should only include mino.h. The
 * shape of struct mino_val, the tag scheme, and the helpers declared
 * here are NOT part of the public API contract and can change between
 * patch releases without notice.
 */

#ifndef MINO_INTERNAL_H
#define MINO_INTERNAL_H

#include "mino.h"

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
 * A mino_val_t* is either a real heap pointer to struct mino_val or an
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
    ((mino_val_t *)((uintptr_t)((uint64_t)(long long)(n) << MINO_TAG_BITS) \
                    | MINO_TAG_INT))

#define MINO_INT_VAL(v) (((long long)(intptr_t)(v)) >> MINO_TAG_BITS)

#define MINO_IS_BOOL(v) (MINO_TAG(v) == MINO_TAG_BOOL)
#define MINO_IS_NIL(v)  ((v) == NULL || MINO_TAG(v) == MINO_TAG_NIL)
#define MINO_IS_CHAR(v) (MINO_TAG(v) == MINO_TAG_CHAR)

#define MINO_TRUE_PAYLOAD  ((uintptr_t)0x1)
#define MINO_FALSE_PAYLOAD ((uintptr_t)0x0)
#define MINO_MAKE_BOOL(b) \
    ((mino_val_t *)(((b) ? MINO_TRUE_PAYLOAD : MINO_FALSE_PAYLOAD) << MINO_TAG_BITS \
                    | MINO_TAG_BOOL))
#define MINO_BOOL_VAL(v) \
    ((int)(((uintptr_t)(v) >> MINO_TAG_BITS) & 0x1))

#define MINO_MAKE_NIL ((mino_val_t *)MINO_TAG_NIL)

#define MINO_MAKE_CHAR(cp) \
    ((mino_val_t *)(((uintptr_t)(uint32_t)(cp) << MINO_TAG_BITS) | MINO_TAG_CHAR))
#define MINO_CHAR_VAL(v) \
    ((int)((uintptr_t)(v) >> MINO_TAG_BITS))

/* ------------------------------------------------------------------------- */
/* Host-array element kind                                                   */
/* ------------------------------------------------------------------------- */

/* JVM-style host array element kind. Used for printing and zero-fill
 * semantics on (int-array n) etc. Pure-object arrays nil-fill; the
 * primitive-element variants fill with their type's zero value. */
typedef enum {
    HOST_ARRAY_OBJECT = 0,
    HOST_ARRAY_INT,
    HOST_ARRAY_LONG,
    HOST_ARRAY_SHORT,
    HOST_ARRAY_BYTE,
    HOST_ARRAY_FLOAT,
    HOST_ARRAY_DOUBLE,
    HOST_ARRAY_CHAR,
    HOST_ARRAY_BOOLEAN
} host_array_kind_t;

/* ------------------------------------------------------------------------- */
/* Value struct body                                                         */
/* ------------------------------------------------------------------------- */

struct mino_val {
    mino_type_t type;     /* value type tag */
    mino_val_t *meta;     /* metadata map (NULL when absent) */
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
        } s;
        struct {          /* MINO_CONS */
            mino_val_t *car;   /* first element */
            mino_val_t *cdr;   /* rest of the list */
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
            mino_val_t       *key_order; /* MINO_VECTOR of keys, insertion order */
            mino_val_t       *val_order; /* MINO_VECTOR of vals (flatmap), or NULL */
            size_t            len;       /* number of entries */
            uint32_t          cached_hash; /* hash_val of this map; 0 = uncomputed. */
        } map;
        struct {          /* MINO_SET: HAMT with sentinel values */
            mino_hamt_node_t *root;      /* HAMT root (NULL when len == 0) */
            mino_val_t       *key_order; /* MINO_VECTOR of elements */
            size_t            len;       /* number of elements */
            uint32_t          cached_hash; /* hash_val of this set; 0 = uncomputed. */
        } set;
        struct {          /* MINO_SORTED_MAP / MINO_SORTED_SET: red-black tree */
            mino_rb_node_t *root;       /* RB tree root (NULL when empty) */
            mino_val_t     *comparator; /* NULL = natural order, fn = custom */
            size_t          len;        /* number of entries */
        } sorted;
        struct {          /* MINO_PRIM */
            const char *name;  /* primitive name */
            mino_prim_fn  fn;  /* legacy cons-list ABI (NULL iff fn2 set) */
            mino_prim_fn2 fn2; /* argv ABI; non-NULL takes precedence */
        } prim;
        struct {          /* MINO_FN: user-defined closure */
            mino_val_t *params;
            mino_val_t *body;
            mino_env_t *env;
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
            mino_val_t *wraps_prim;
        } fn;
        struct {          /* MINO_HANDLE: opaque host pointer + tag */
            void       *ptr;
            const char *tag;
            void       (*finalizer)(void *ptr, const char *tag);
        } handle;
        struct {          /* MINO_ATOM: mutable reference cell */
            mino_val_t *val;
            mino_val_t *watches;
            mino_val_t *validator;
        } atom;
        struct {          /* MINO_VOLATILE: single-slot mutable cell */
            mino_val_t *val;
        } volatile_;
        struct {          /* MINO_LAZY: deferred sequence */
            mino_val_t *body;
            mino_env_t *env;
            mino_val_t *cached;
            mino_val_t *(*c_thunk)(struct mino_state *, mino_val_t *);
            int         realized;
        } lazy;
        struct {          /* MINO_CHUNK: fixed-cap value buffer */
            mino_val_t **vals;
            unsigned     cap;
            unsigned     len;
            int          sealed;
        } chunk;
        struct {          /* MINO_CHUNKED_CONS: chunk + offset + more */
            mino_val_t *chunk;
            mino_val_t *more;
            unsigned    off;
        } chunked_cons;
        struct {          /* MINO_RECUR */
            mino_val_t *args;
        } recur;
        struct {          /* MINO_TAIL_CALL */
            mino_val_t *fn;
            mino_val_t *args;
        } tail_call;
        struct {          /* MINO_REDUCED */
            mino_val_t *val;
        } reduced;
        struct {          /* MINO_VAR: first-class var */
            const char *ns;
            const char *sym;
            mino_val_t *root;
            int         dynamic;
            int         bound;
            int         is_private;
            mino_val_t *watches;
            mino_val_t *validator;
            unsigned    version;
        } var;
        struct {          /* MINO_TRANSIENT */
            mino_val_t *current;
            int         valid;
            /* Owner ID minted at creation by
             * S->transient_owner_next++; written into the owner field
             * of every owner-tagged vec/HAMT node so the transient's
             * *_bang mutators can recognise their own nodes and mutate
             * them in place. 0 means "no in-place editing tier", which
             * is the legacy wrapper behaviour. */
            uintptr_t   owner_id;
        } transient;
        struct {          /* MINO_BIGINT */
            void *mpz;
        } bigint;
        struct {          /* MINO_RATIO */
            mino_val_t *num;
            mino_val_t *denom;
        } ratio;
        struct {          /* MINO_BIGDEC */
            mino_val_t *unscaled;
            int         scale;
        } bigdec;
        struct {          /* MINO_TYPE: first-class record type */
            const char *ns;
            const char *name;
            mino_val_t *fields;
        } record_type;
        struct {          /* MINO_RECORD */
            mino_val_t  *type;
            mino_val_t **vals;
            mino_val_t  *ext;
        } record;
        struct {          /* MINO_FUTURE */
            struct mino_future *impl;
        } future;
        struct {          /* MINO_UUID */
            unsigned char bytes[16];
        } uuid;
        struct {          /* MINO_REGEX */
            mino_val_t *source;
        } regex;
        struct {          /* MINO_HOST_ARRAY */
            mino_val_t **vals;
            size_t       len;
            unsigned char element_kind; /* host_array_kind_t */
        } host_array;
        struct {          /* MINO_MAP_ENTRY */
            mino_val_t *k;
            mino_val_t *v;
        } map_entry;
        struct {          /* MINO_TX_REF */
            mino_val_t    *val;
            mino_val_t    *watches;
            mino_val_t    *validator;
            uint64_t       version;
            uint64_t       ref_id;
            mino_state_t  *owning_state;
        } tx_ref;
        struct {          /* MINO_AGENT */
            mino_val_t *val;
            mino_val_t *watches;
            mino_val_t *validator;
            mino_val_t *err;
            mino_val_t *err_handler;
            int         err_mode;
            int         in_flight;
            uint64_t    agent_id;
            mino_state_t *owning_state;
        } agent;
    } as;
};

/* ------------------------------------------------------------------------- */
/* Runtime-internal constructors                                             */
/* ------------------------------------------------------------------------- */

/* Create a host-style array of the given length, fill-initialized
 * according to the element kind. JVM-shape mimicry surface; not part
 * of the public embedding API. */
mino_val_t *mino_host_array_new(mino_state_t *S, size_t len,
                                host_array_kind_t kind);
mino_val_t *mino_host_array_from_coll(mino_state_t *S, mino_val_t *coll,
                                      host_array_kind_t kind);

/* Chunked-seq constructors. Internal: lazy.c and sequences.c use these
 * to build chunked seqs from sequences and ranges. */
mino_val_t *mino_chunk_buffer(mino_state_t *S, unsigned cap);
int         mino_chunk_append(mino_val_t *buf, mino_val_t *elem);
mino_val_t *mino_chunk_seal(mino_val_t *buf);
mino_val_t *mino_chunked_cons(mino_state_t *S, mino_val_t *chunk,
                              mino_val_t *more);
mino_val_t *mino_chunked_cons_advance(mino_state_t *S, const mino_val_t *cs);

/* Wraparound int constructor: identical to mino_int when the value
 * fits the 61-bit tag, but always boxes the overflow path as MINO_INT
 * (never auto-promotes to MINO_BIGINT, regardless of MINO_CAP_BIGNUM).
 * Used by the unchecked-* family in numeric.c so the documented
 * two's-complement-wrap semantics produce an int, not a bigint. */
mino_val_t *mino_int_wrap(mino_state_t *S, long long n);

/* ------------------------------------------------------------------------- */
/* Per-capability install functions (internal-only)                          */
/* ------------------------------------------------------------------------- */

/* Each function registers the C primitives for its capability and sets
 * the corresponding MINO_CAP_* bit on the state. These are dispatched
 * from the capability registry in runtime/capabilities.c; embedders use
 * mino_install(S, env, caps) and never call these directly. */

void mino_install_regex       (mino_state_t *S, mino_env_t *env);
void mino_install_bignum      (mino_state_t *S, mino_env_t *env);
void mino_install_multimethods(mino_state_t *S, mino_env_t *env);
void mino_install_protocols   (mino_state_t *S, mino_env_t *env);
void mino_install_transducers (mino_state_t *S, mino_env_t *env);
void mino_install_io          (mino_state_t *S, mino_env_t *env);
void mino_install_fs          (mino_state_t *S, mino_env_t *env);
void mino_install_proc        (mino_state_t *S, mino_env_t *env);
void mino_install_stm         (mino_state_t *S, mino_env_t *env);
void mino_install_agent       (mino_state_t *S, mino_env_t *env);
void mino_install_host        (mino_state_t *S, mino_env_t *env);
void mino_install_async       (mino_state_t *S, mino_env_t *env);

/* Bundled-stdlib registration hooks. Each registers the in-binary source
 * for its namespace via mino_register_bundled_lib so a subsequent
 * (require '[<ns>]) loads it from memory. Pairs that depend on each
 * other ship as a single hook (clojure.repl + clojure.stacktrace;
 * clojure.datafy + clojure.core.protocols; clojure.test +
 * clojure.test.check). */
void mino_install_clojure_string    (mino_state_t *S, mino_env_t *env);
void mino_install_clojure_set       (mino_state_t *S, mino_env_t *env);
void mino_install_clojure_walk      (mino_state_t *S, mino_env_t *env);
void mino_install_clojure_edn       (mino_state_t *S, mino_env_t *env);
void mino_install_clojure_pprint    (mino_state_t *S, mino_env_t *env);
void mino_install_clojure_zip       (mino_state_t *S, mino_env_t *env);
void mino_install_clojure_data      (mino_state_t *S, mino_env_t *env);
void mino_install_clojure_test      (mino_state_t *S, mino_env_t *env);
void mino_install_clojure_test_check(mino_state_t *S, mino_env_t *env);
void mino_install_clojure_repl      (mino_state_t *S, mino_env_t *env);
void mino_install_clojure_datafy    (mino_state_t *S, mino_env_t *env);
void mino_install_clojure_instant   (mino_state_t *S, mino_env_t *env);
void mino_install_clojure_spec      (mino_state_t *S, mino_env_t *env);
void mino_install_mino_tooling      (mino_state_t *S, mino_env_t *env);

/* Internal: evaluate core.clj on the floor env. Used by mino_install
 * after capability bits are set; idempotent. */
void mino_install_clojure_core(mino_state_t *S, mino_env_t *env);

/* ------------------------------------------------------------------------- */
/* Fault injection (test-only)                                               */
/* ------------------------------------------------------------------------- */

/* Schedule a simulated OOM after the next `n` GC-managed allocations. */
void mino_set_fail_alloc_at(mino_state_t *S, long n);
/* Schedule a simulated OOM after `n` raw (non-GC) allocations. */
void mino_set_fail_raw_at(mino_state_t *S, long n);
/* Check-and-decrement the raw fault-injection counter. */
int  mino_fi_should_fail_raw(mino_state_t *S);

#ifdef __cplusplus
}
#endif

#endif /* MINO_INTERNAL_H */
