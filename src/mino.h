/*
 * mino.h - public C API for the mino runtime.
 *
 * UNSTABLE until v1.0.0. Symbol names, types, and semantics may change.
 */

#ifndef MINO_H
#define MINO_H

#include <stddef.h>

/* ------------------------------------------------------------------------- */
/* Version                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Compile-time version constants, bumped by the release process. Host code
 * can check these at preprocessor time to guard against embedding against an
 * unexpected runtime:
 *
 *   #if MINO_VERSION_MAJOR == 0 && MINO_VERSION_MINOR < 48
 *   # error "embedded mino is too old; need >= 0.48"
 *   #endif
 *
 * The linked-in version (which may differ if the header was updated without
 * rebuilding the runtime) is available at runtime via mino_version_string().
 */
#define MINO_VERSION_MAJOR 0
#define MINO_VERSION_MINOR 85
#define MINO_VERSION_PATCH 0

/*
 * Human-readable version string of the *linked* runtime, e.g. "0.48.0".
 * The pointer is to a static string with program lifetime; callers must not
 * free it.
 */
const char *mino_version_string(void);

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Value types                                                               */
/* ------------------------------------------------------------------------- */

typedef enum {
    MINO_NIL,
    MINO_BOOL,
    MINO_INT,
    MINO_FLOAT,
    MINO_CHAR,       /* Unicode scalar value (codepoint) */
    MINO_STRING,
    MINO_SYMBOL,
    MINO_KEYWORD,
    MINO_CONS,
    MINO_VECTOR,
    MINO_MAP,
    MINO_SET,        /* persistent set: HAMT of keys with sentinel values */
    MINO_SORTED_MAP, /* persistent sorted map: red-black tree */
    MINO_SORTED_SET, /* persistent sorted set: red-black tree */
    MINO_PRIM,
    MINO_FN,
    MINO_MACRO,   /* user-defined macro (shares the fn struct layout) */
    MINO_HANDLE,  /* opaque host object: pointer + type tag string */
    MINO_ATOM,    /* mutable reference cell: wraps a single value */
    MINO_LAZY,    /* lazy sequence: thunk body + env, cached on first force */
    MINO_RECUR,   /* internal tail-call trampoline sentinel */
    MINO_TAIL_CALL, /* proper tail call: carries {fn, args} for trampoline */
    MINO_REDUCED, /* early termination wrapper for reduce */
    MINO_VAR,     /* first-class var: ns + name + root binding */
    MINO_TRANSIENT, /* mutable staging wrapper for batch mutation of a
                     * persistent vector, map, or set. Embedders call
                     * mino_transient / mino_persistent and the
                     * *_bang accessors below. */
    MINO_BIGINT,    /* arbitrary-precision signed integer. Backed by the
                     * vendored imath library (src/vendor/imath.c). The
                     * mpz_t struct is embedded in the value cell; the
                     * digit storage is owned by the cell and freed
                     * during GC sweep. See also `1N` literals and the
                     * `bigint` / `biginteger` constructors. */
    MINO_RATIO,     /* arbitrary-precision rational number, stored as
                     * a numerator/denominator pair of bigints. The
                     * representation is always reduced (gcd = 1) and
                     * the denominator is always positive (sign lives
                     * on the numerator). See also `1/2` literals and
                     * the `numerator` / `denominator` primitives. */
    MINO_BIGDEC,    /* arbitrary-precision decimal, stored as an
                     * unscaled bigint plus a non-negative integer
                     * scale: value = unscaled * 10^-scale. Constructed
                     * from `1.5M` / `1M` literals or via `(bigdec x)`. */
    MINO_TYPE,      /* first-class record type. Carries identity (ptr),
                     * defining namespace, unqualified name, and an
                     * ordered vector of declared field-name keywords.
                     * Method tables are not stored here; protocol
                     * dispatch lives in the protocol's namespace,
                     * keyed by what (type x) returns. Construct via
                     * mino_defrecord. */
    MINO_RECORD     /* record value: pointer to its MINO_TYPE plus a
                     * malloc-owned array of field-value slots and an
                     * optional MINO_MAP for extension keys. Storage
                     * is field slots, not a backing map; map-iso
                     * behaviour (get, assoc, seq, count, ...) is a
                     * contract layered on top via primitive dispatch
                     * on this tag. Construct via mino_record. */
} mino_type_t;

typedef struct mino_val   mino_val_t;
typedef struct mino_env   mino_env_t;
typedef struct mino_state mino_state_t;
typedef struct mino_ref   mino_ref_t;
typedef struct mino_vec_node  mino_vec_node_t;   /* opaque; see mino.c */
typedef struct mino_hamt_node mino_hamt_node_t;  /* opaque; see mino.c */
typedef struct mino_rb_node   mino_rb_node_t;    /* opaque; see rbtree.c */

typedef mino_val_t *(*mino_prim_fn)(mino_state_t *S, mino_val_t *args,
                                    mino_env_t *env);
typedef void (*mino_finalizer_fn)(void *ptr, const char *tag);

/* Host interop callback. target is the handle (NULL for ctor/static),
 * args is a cons list of evaluated arguments. */
typedef mino_val_t *(*mino_host_fn)(mino_state_t *S, mino_val_t *target,
                                    mino_val_t *args, void *ctx);

struct mino_val {
    mino_type_t type;     /* value type tag */
    mino_val_t *meta;     /* metadata map (NULL when absent) */
    union {
        int b;            /* MINO_BOOL: 0 or 1 */
        long long i;      /* MINO_INT */
        double f;         /* MINO_FLOAT */
        int ch;           /* MINO_CHAR: Unicode codepoint (0..0x10FFFF) */
        struct {          /* MINO_STRING, MINO_SYMBOL, MINO_KEYWORD */
            char *data;   /* byte content (NUL-terminated) */
            size_t len;   /* length in bytes (excluding NUL) */
        } s;
        struct {          /* MINO_CONS */
            mino_val_t *car;   /* first element */
            mino_val_t *cdr;   /* rest of the list */
            const char *file;  /* source file (NULL if unknown) */
            int         line;  /* source line (0 if unknown) */
            int         column;/* source column (0 if unknown) */
        } cons;
        struct {          /* MINO_VECTOR: persistent 32-way trie with tail */
            mino_vec_node_t *root;     /* trie spine (NULL when len <= 32) */
            mino_vec_node_t *tail;     /* partial leaf, 1..32 slots used */
            unsigned         tail_len; /* number of valid slots in tail */
            unsigned         shift;    /* height of root in multiples of 5 */
            size_t           len;      /* visible element count */
            size_t           offset;   /* first visible element (0 unless subvec) */
            size_t           blen;     /* backing total (len+offset when no nesting) */
        } vec;
        struct {          /* MINO_MAP: HAMT keyed by hash(key) */
            mino_hamt_node_t *root;      /* HAMT root (NULL when len == 0) */
            mino_val_t       *key_order; /* MINO_VECTOR of keys, insertion order */
            size_t            len;       /* number of entries */
        } map;
        struct {          /* MINO_SET: HAMT with sentinel values */
            mino_hamt_node_t *root;      /* HAMT root (NULL when len == 0) */
            mino_val_t       *key_order; /* MINO_VECTOR of elements */
            size_t            len;       /* number of elements */
        } set;
        struct {          /* MINO_SORTED_MAP / MINO_SORTED_SET: red-black tree */
            mino_rb_node_t *root;       /* RB tree root (NULL when empty) */
            mino_val_t     *comparator; /* NULL = natural order, fn = custom */
            size_t          len;        /* number of entries */
        } sorted;
        struct {          /* MINO_PRIM */
            const char *name;  /* primitive name */
            mino_prim_fn fn;   /* C function pointer */
        } prim;
        struct {          /* MINO_FN: user-defined closure */
            mino_val_t *params; /* parameter list or vector */
            mino_val_t *body;   /* body forms */
            mino_env_t *env;    /* captured lexical environment */
            const char *defining_ns; /* ns at creation time (interned), or NULL */
        } fn;
        struct {          /* MINO_HANDLE: opaque host pointer + tag */
            void       *ptr;   /* host-owned pointer */
            const char *tag;   /* type tag (static or interned) */
            void       (*finalizer)(void *ptr, const char *tag);
                               /* called on GC (NULL if none) */
        } handle;
        struct {          /* MINO_ATOM: mutable reference cell */
            mino_val_t *val;       /* current value */
            mino_val_t *watches;   /* key -> callback fn, or NULL */
            mino_val_t *validator; /* validation fn or NULL */
        } atom;
        struct {          /* MINO_LAZY: deferred sequence */
            mino_val_t *body;     /* unevaluated form list (NULL after force) */
            mino_env_t *env;      /* captured environment (NULL after force) */
            mino_val_t *cached;   /* realized cons/nil (valid after force) */
            mino_val_t *(*c_thunk)(struct mino_state *, mino_val_t *);
                                  /* optional C thunk; body is context if set */
            int         realized; /* 0 = pending, 1 = forced */
        } lazy;
        struct {          /* MINO_RECUR: carries rebind args for trampoline */
            mino_val_t *args;  /* argument list for next iteration */
        } recur;
        struct {          /* MINO_TAIL_CALL: proper tail call trampoline */
            mino_val_t *fn;    /* function to call */
            mino_val_t *args;  /* argument list */
        } tail_call;
        struct {          /* MINO_REDUCED: early termination wrapper */
            mino_val_t *val;   /* wrapped value */
        } reduced;
        struct {          /* MINO_VAR: first-class var */
            const char *ns;        /* namespace (interned) */
            const char *sym;       /* name (interned) */
            mino_val_t *root;      /* root binding value */
            int         dynamic;   /* 1 if ^:dynamic */
            int         bound;     /* 0 if (def x) with no init; 1 once bound */
            int         is_private; /* 1 if ^:private */
        } var;
        struct {          /* MINO_TRANSIENT: batch-mutation wrapper */
            mino_val_t *current; /* current persistent value (vec/map/set) */
            int         valid;   /* 0 after persistent!, 1 while mutable */
        } transient;
        struct {          /* MINO_BIGINT: arbitrary-precision integer */
            void *mpz;    /* opaque mp_int; malloc-owned, freed at sweep */
        } bigint;
        struct {          /* MINO_RATIO: numerator/denominator bigints */
            mino_val_t *num;   /* MINO_BIGINT */
            mino_val_t *denom; /* MINO_BIGINT, always positive */
        } ratio;
        struct {          /* MINO_BIGDEC: unscaled bigint + decimal scale */
            mino_val_t *unscaled; /* MINO_BIGINT */
            int         scale;    /* value = unscaled * 10^-scale */
        } bigdec;
        struct {          /* MINO_TYPE: first-class record type */
            const char *ns;      /* defining namespace (interned) */
            const char *name;    /* unqualified type name (interned) */
            mino_val_t *fields;  /* MINO_VECTOR of field-name keywords */
        } record_type;
        struct {          /* MINO_RECORD: record value */
            mino_val_t  *type;   /* MINO_TYPE (never NULL) */
            mino_val_t **vals;   /* malloc-owned; len == type's field count */
            mino_val_t  *ext;    /* MINO_MAP for ext keys, or NULL */
        } record;
    } as;
};

/* ------------------------------------------------------------------------- */
/* Constructors                                                              */
/* ------------------------------------------------------------------------- */

/* Return the singleton nil value. */
mino_val_t *mino_nil(mino_state_t *S);

/* Return the singleton true value. */
mino_val_t *mino_true(mino_state_t *S);

/* Return the singleton false value. */
mino_val_t *mino_false(mino_state_t *S);

/* Create an integer value. */
mino_val_t *mino_int(mino_state_t *S, long long n);

/* Create a floating-point value. */
mino_val_t *mino_float(mino_state_t *S, double f);

/* Create a bigint from a signed long long. */
mino_val_t *mino_bigint_from_ll(mino_state_t *S, long long n);

/* Create a bigint by parsing a base-10 numeric string (optional leading
 * '+' or '-'). Returns NULL on parse failure. */
mino_val_t *mino_bigint_from_string(mino_state_t *S, const char *s);

/* Create a rational from numerator and denominator long longs. The result
 * is reduced (gcd = 1) and normalised so the denominator is positive.
 * If denom is zero, throws division-by-zero. If the result is integer,
 * returns a MINO_INT or MINO_BIGINT instead of a MINO_RATIO. */
mino_val_t *mino_ratio_from_ll(mino_state_t *S, long long num, long long denom);

/* Create a bigdec from a base-10 numeric string. Returns NULL on parse
 * failure. Recognises optional sign, fractional part, and 'e'-prefixed
 * exponent; the trailing 'M' suffix is optional in this entry point. */
mino_val_t *mino_bigdec_from_string(mino_state_t *S, const char *s);

/* Create a character value from a Unicode codepoint (0..0x10FFFF). */
mino_val_t *mino_char(mino_state_t *S, int codepoint);

/* Create a string from a NUL-terminated C string. The data is copied. */
mino_val_t *mino_string(mino_state_t *S, const char *s);

/* Create a string from a buffer of length len. The data is copied. */
mino_val_t *mino_string_n(mino_state_t *S, const char *s, size_t len);

/* Intern a symbol from a NUL-terminated C string. */
mino_val_t *mino_symbol(mino_state_t *S, const char *s);

/* Intern a symbol from a buffer of length len. */
mino_val_t *mino_symbol_n(mino_state_t *S, const char *s, size_t len);

/* Intern a keyword from a NUL-terminated C string (without the leading :). */
mino_val_t *mino_keyword(mino_state_t *S, const char *s);

/* Intern a keyword from a buffer of length len (without the leading :). */
mino_val_t *mino_keyword_n(mino_state_t *S, const char *s, size_t len);

/* Create a cons cell (list node) with the given car and cdr. */
mino_val_t *mino_cons(mino_state_t *S, mino_val_t *car, mino_val_t *cdr);

/* Create a persistent vector from a C array of values. */
mino_val_t *mino_vector(mino_state_t *S, mino_val_t **items, size_t len);

/* Create a persistent hash map from parallel key and value arrays. */
mino_val_t *mino_map(mino_state_t *S, mino_val_t **keys, mino_val_t **vals,
                     size_t len);

/* Create a persistent hash set from a C array of values. */
mino_val_t *mino_set(mino_state_t *S, mino_val_t **items, size_t len);

/* Create a primitive function value from a C function pointer. */
mino_val_t *mino_prim(mino_state_t *S, const char *name, mino_prim_fn fn);

/* Wrap a host pointer as an opaque handle with a type tag. */
mino_val_t *mino_handle(mino_state_t *S, void *ptr, const char *tag);

/* Wrap a host pointer with a type tag and a finalizer called on GC. */
mino_val_t *mino_handle_ex(mino_state_t *S, void *ptr, const char *tag,
                           mino_finalizer_fn finalizer);

/* Create a mutable atom initialized with val. */
mino_val_t *mino_atom(mino_state_t *S, mino_val_t *val);

/* Return 1 if v is a handle, 0 otherwise. */
int         mino_is_handle(const mino_val_t *v);

/* Return the host pointer from a handle, or NULL if v is not a handle. */
void       *mino_handle_ptr(const mino_val_t *v);

/* Return the type tag from a handle, or NULL if v is not a handle. */
const char *mino_handle_tag(const mino_val_t *v);

/* Return 1 if v is an atom, 0 otherwise. */
int         mino_is_atom(const mino_val_t *v);

/* Return the current value of an atom. */
mino_val_t *mino_atom_deref(const mino_val_t *a);

/* Set the value of an atom. */
void        mino_atom_reset(mino_val_t *a, mino_val_t *val);

/* ------------------------------------------------------------------------- */
/* Record types                                                              */
/* ------------------------------------------------------------------------- */

/*
 * Define a record type. Returns the MINO_TYPE value. The pointer is
 * stable for the life of the state (record types are pinned), so type
 * identity is pointer equality.
 *
 * Idempotent: subsequent calls with the same ns/name return the
 * existing type so re-loading scripts is safe. Field shape on a
 * re-call is ignored (the existing type wins) — F.4 macros validate
 * shape changes at the script layer when defrecord re-evaluates.
 *
 * The strings are copied into the state's intern table on first use,
 * so callers may pass stack-allocated names. Field names are
 * converted to keywords and stored in declared order.
 *
 * Mirrors the Clojure `defrecord` macro name to make the C/script
 * analogy obvious to a C/C++/Rust embedder.
 */
mino_val_t *mino_defrecord(mino_state_t *S,
                           const char *ns,
                           const char *name,
                           const char *const *field_names,
                           size_t n_fields);

/* Return 1 if v is a record type (MINO_TYPE), 0 otherwise. */
int mino_is_record_type(const mino_val_t *v);

/*
 * Build a record. type must be a MINO_TYPE returned by
 * mino_defrecord; n_vals must equal the type's declared field count;
 * vals[i] gives the value for the i-th declared field. The vals
 * array is copied into a fresh malloc-owned slot vector owned by the
 * returned record. Returns NULL on type mismatch or arity mismatch
 * (the caller should treat NULL as a runtime error).
 *
 * The new record has no extension keys; assoc with an undeclared key
 * lazily allocates the ext map.
 */
mino_val_t *mino_record(mino_state_t *S, mino_val_t *type,
                        mino_val_t **vals, size_t n_vals);

/*
 * Read a declared field by name. Returns the field value (borrowed)
 * or NULL if name is not a declared field of record's type.
 * Extension keys are not read by this entry point; embedders can
 * reach them via the script-level (get r :ext-key) path.
 */
mino_val_t *mino_record_field(const mino_val_t *record, const char *name);

/* Return 1 if v is a record (MINO_RECORD), 0 otherwise. */
int mino_is_record(const mino_val_t *v);

/* ------------------------------------------------------------------------- */
/* Transient (batch-mutation) API                                            */
/* ------------------------------------------------------------------------- */

/*
 * Transients give embedders an efficient batch-mutation path for
 * building vectors, maps, and sets without allocating a new
 * persistent value per step. After calling mino_persistent(t) the
 * transient is sealed; calling any *_bang on it returns NULL and sets
 * a runtime error.
 *
 * Current implementation wraps persistent ops rather than mutating
 * the underlying trie nodes in place. The public API shape is
 * stable; in-place fast-paths may replace the wrapper in a later
 * release without ABI change.
 */

/* Wrap a persistent vector, map, or set in a new transient. Throws
 * on other types. The returned value is reusable until
 * mino_persistent is called on it. */
mino_val_t *mino_transient(mino_state_t *S, mino_val_t *coll);

/* Extract the current persistent value and invalidate the transient.
 * Further *_bang calls on t will throw. */
mino_val_t *mino_persistent(mino_state_t *S, mino_val_t *t);

/* Transient mutators. Each returns t on success (after updating its
 * inner persistent value) and NULL on a classified error. The caller
 * must use the returned value instead of retaining its own handle to
 * the transient across the call, matching Clojure's convention. */
mino_val_t *mino_assoc_bang(mino_state_t *S, mino_val_t *t,
                            mino_val_t *key, mino_val_t *val);
mino_val_t *mino_conj_bang(mino_state_t *S, mino_val_t *t,
                           mino_val_t *val);
mino_val_t *mino_dissoc_bang(mino_state_t *S, mino_val_t *t,
                             mino_val_t *key);
mino_val_t *mino_disj_bang(mino_state_t *S, mino_val_t *t,
                           mino_val_t *key);
mino_val_t *mino_pop_bang(mino_state_t *S, mino_val_t *t);

/* Transient predicates and accessors. */
int         mino_is_transient(const mino_val_t *v);
size_t      mino_transient_count(const mino_val_t *t);

/* ------------------------------------------------------------------------- */
/* Predicates and accessors                                                  */
/* ------------------------------------------------------------------------- */

/* Return 1 if v is nil, 0 otherwise. */
int mino_is_nil(const mino_val_t *v);

/* Return 1 if v is truthy (everything except nil and false). */
int mino_is_truthy(const mino_val_t *v);

/* Return 1 if v is a cons cell (list node), 0 otherwise. */
int mino_is_cons(const mino_val_t *v);

/* Structural equality. Returns 1 if a and b are equal, 0 otherwise. */
int mino_eq(const mino_val_t *a, const mino_val_t *b);

/* Return the first element of a cons cell, or NULL. */
mino_val_t *mino_car(const mino_val_t *v);

/* Return the rest of a cons cell, or NULL. */
mino_val_t *mino_cdr(const mino_val_t *v);

/* Return the number of cons cells in a list. */
size_t mino_length(const mino_val_t *list);

/* Type-safe C extraction. Each returns 1 on success, 0 on type mismatch.
 * mino_to_bool uses truthiness (only nil and false are falsey). */
int mino_to_int(const mino_val_t *v, long long *out);
int mino_to_float(const mino_val_t *v, double *out);
int mino_to_string(const mino_val_t *v, const char **out, size_t *len);
int mino_to_bool(const mino_val_t *v);

/* ------------------------------------------------------------------------- */
/* Printer                                                                   */
/* ------------------------------------------------------------------------- */

#include <stdio.h>

/* Print a value to stdout in readable form. */
void mino_print(mino_state_t *S, const mino_val_t *v);

/* Print a value to stdout followed by a newline. */
void mino_println(mino_state_t *S, const mino_val_t *v);

/* Print a value to the given FILE stream. */
void mino_print_to(mino_state_t *S, FILE *out, const mino_val_t *v);

/* ------------------------------------------------------------------------- */
/* Reader                                                                    */
/* ------------------------------------------------------------------------- */

/*
 * Read one form from `src`. On success returns a value and writes a pointer
 * just past the consumed input to `*end` (when `end` is non-NULL). On EOF
 * (only whitespace / comments remaining) returns NULL with `*end` advanced
 * past the trailing whitespace. On parse error returns NULL and writes a
 * human-readable message via `mino_last_error()`.
 */
mino_val_t *mino_read(mino_state_t *S, const char *src, const char **end);

/* Return the last error message, or NULL if no error occurred. */
const char *mino_last_error(mino_state_t *S);

/* Opaque structured diagnostic type. Use mino_last_diag() to retrieve. */
typedef struct mino_diag mino_diag_t;

/* Return the last structured diagnostic, or NULL if no error occurred.
 * The returned pointer is valid until the next error or clear_error. */
const mino_diag_t *mino_last_diag(mino_state_t *S);

/* Return the last error as a mino map with :mino/kind, :mino/code, etc.
 * Returns nil if no error occurred. The value is GC-owned and cached. */
mino_val_t *mino_last_error_map(mino_state_t *S);

/* Diagnostic rendering modes. */
#define MINO_DIAG_RENDER_COMPACT 0
#define MINO_DIAG_RENDER_PRETTY  1

/* Render a diagnostic into buf. Returns bytes written (excl NUL).
 * mode is one of MINO_DIAG_RENDER_COMPACT or _PRETTY. */
int mino_render_diag(mino_state_t *S, const mino_diag_t *d,
                     int mode, char *buf, size_t n);

/* ------------------------------------------------------------------------- */
/* Runtime state                                                             */
/* ------------------------------------------------------------------------- */

/*
 * Create a new isolated runtime state. Each state owns its own GC, intern
 * tables, module cache, and singletons. Multiple states may coexist in the
 * same process; they share no mutable data.
 *
 * Fatal: aborts on allocation failure (no state exists to report through).
 */
mino_state_t *mino_state_new(void);

/*
 * Free a runtime state and all resources owned by it. All GC-managed objects,
 * intern tables, module caches, and metadata are released. Environments
 * created within this state become invalid.
 */
void mino_state_free(mino_state_t *S);

/* ------------------------------------------------------------------------- */
/* Environment and evaluator                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Allocate a fresh root environment and register it with the collector so
 * every value reachable through it survives collection. The runtime holds
 * the returned env weakly: mino_env_free unregisters it and lets the next
 * sweep reclaim the frame and any closures that were reachable only from
 * within it. The host does not free any mino-owned pointers directly.
 *
 * Returns NULL on allocation failure (check mino_last_error).
 */
mino_env_t *mino_env_new(mino_state_t *S);
/* Free an environment and unregister it from the collector. */
void        mino_env_free(mino_state_t *S, mino_env_t *env);

/*
 * Clone an environment: allocate a new root environment and copy all
 * bindings from the source. Values are shared (not deep-copied), so
 * the clone is only meaningful within the same state. Useful for
 * snapshotting a session before independent evaluation in each copy.
 */
mino_env_t *mino_env_clone(mino_state_t *S, mino_env_t *env);

/*
 * Convenience: allocate a new env and install all bindings in one call.
 * Equivalent to mino_env_new() followed by mino_install_core() and
 * mino_install_io().  For a sandboxed environment without I/O, call
 * mino_env_new() and mino_install_core() directly.
 */
mino_env_t *mino_new(mino_state_t *S);

/* Define or replace a binding in `env`. */
void        mino_env_set(mino_state_t *S, mino_env_t *env, const char *name,
                         mino_val_t *val);
/* Look up `name`. Returns NULL if unbound. */
mino_val_t *mino_env_get(mino_env_t *env, const char *name);

/*
 * Install all core primitive bindings and the standard library into `env`.
 * This includes arithmetic, comparison, collections, sequences, predicates,
 * strings, reflection, and the `core.clj` stdlib. Does not install I/O
 * primitives; call `mino_install_io` separately to opt in.
 */
void        mino_install_core(mino_state_t *S, mino_env_t *env);

/*
 * Install I/O primitives: println, prn, slurp, spit, exit. These are kept
 * separate from mino_install_core so that sandboxed environments start with
 * no I/O capability; the host opts in by calling this function.
 */
void        mino_install_io(mino_state_t *S, mino_env_t *env);

/*
 * Install filesystem primitives: file-exists?, directory?, mkdir-p, rm-rf.
 *
 * Separate from I/O because these grant directory creation and deletion
 * capabilities.  The host opts in by calling this function.
 */
void        mino_install_fs(mino_state_t *S, mino_env_t *env);

/*
 * Install process execution primitives: sh, sh!.
 *
 * These allow spawning external processes via popen.  Separate from
 * core/io because process execution is a powerful capability that
 * sandboxed environments may wish to withhold.
 */
void        mino_install_proc(mino_state_t *S, mino_env_t *env);

/*
 * Evaluate one form. Returns NULL on error and writes a message via
 * mino_last_error(). Returns mino_nil() for an explicit nil result.
 *
 * A top-level try frame catches out-of-memory conditions and unhandled
 * exceptions during evaluation, surfacing them as NULL + error message
 * rather than aborting the process. The state remains usable after an
 * OOM return.
 */
mino_val_t *mino_eval(mino_state_t *S, mino_val_t *form, mino_env_t *env);

/*
 * Read and evaluate all forms in `src`. Returns the value of the last
 * form, or NULL on error. An empty string returns mino_nil().
 *
 * Installs its own try frame: OOM during read or eval returns NULL
 * with an error message rather than aborting.
 */
mino_val_t *mino_eval_string(mino_state_t *S, const char *src,
                             mino_env_t *env);

/*
 * Read a file at `path` and evaluate all forms. Returns the value of the
 * last form, or NULL on error (file I/O failures and parse/eval errors).
 */
mino_val_t *mino_load_file(mino_state_t *S, const char *path,
                           mino_env_t *env);

/*
 * Shorthand: bind a C function as a primitive in `env`.
 * Equivalent to mino_env_set(S, env, name, mino_prim(S, name, fn)).
 */
void mino_register_fn(mino_state_t *S, mino_env_t *env, const char *name,
                      mino_prim_fn fn);

/*
 * Call a callable value (fn, macro, prim) with an argument list.
 * Returns the result, or NULL on error (via mino_last_error).
 */
mino_val_t *mino_call(mino_state_t *S, mino_val_t *fn, mino_val_t *args,
                      mino_env_t *env);

/*
 * Protected call: same as mino_call but returns 0 on success (writing the
 * result to *out) or -1 on error. The error message is available via
 * mino_last_error(). *out is set to NULL on error.
 */
int mino_pcall(mino_state_t *S, mino_val_t *fn, mino_val_t *args,
               mino_env_t *env, mino_val_t **out);

/* ------------------------------------------------------------------------- */
/* Exceptions                                                                */
/* ------------------------------------------------------------------------- */

/*
 * Raise a mino exception carrying `ex` as the payload. When called from a
 * C primitive that is executing inside a mino (try ... (catch ...)) frame,
 * the payload is delivered to the matching catch binding. When called
 * outside any try frame, the exception surfaces as a classified error
 * through mino_last_error (kind "user", code "MUS001") and the function
 * returns NULL.
 *
 * Inside a try frame this call does NOT return; control transfers to the
 * try handler via longjmp. Callers should treat it as "returns NULL" for
 * typing purposes and not rely on any code after the call:
 *
 *   return mino_throw(S, mino_keyword(S, "bad-arg"));
 *
 * The payload can be any mino value: a keyword, a string, or an
 * information-carrying map built via mino_map(). This is the C-side
 * analogue of the (throw ex) mino primitive.
 */
mino_val_t *mino_throw(mino_state_t *S, mino_val_t *ex);

/* ------------------------------------------------------------------------- */
/* Argument parsing                                                          */
/* ------------------------------------------------------------------------- */

/*
 * Type-check and destructure a primitive's argument list into C variables.
 * The format string lists one character per expected positional argument;
 * each variadic pointer receives the corresponding extracted value:
 *
 *   "i"  long long *        -- MINO_INT
 *   "f"  double *           -- MINO_FLOAT or MINO_INT (promoted)
 *   "s"  const char **      -- MINO_STRING; pointer into mino-owned data
 *   "S"  const char **,     -- MINO_STRING; pointer plus byte length
 *        size_t *              (write both in order: "S" consumes two ptrs)
 *   "k"  const char **      -- MINO_KEYWORD name (without the leading :)
 *   "y"  const char **      -- MINO_SYMBOL name
 *   "b"  int *              -- MINO_BOOL (0 or 1)
 *   "c"  int *              -- MINO_CHAR codepoint (0..0x10FFFF)
 *   "v"  mino_val_t **      -- any value (no type check)
 *   "V"  mino_val_t **      -- MINO_VECTOR
 *   "M"  mino_val_t **      -- MINO_MAP
 *   "L"  mino_val_t **      -- MINO_CONS or MINO_NIL (a list)
 *   "H"  mino_val_t **      -- MINO_HANDLE
 *   "A"  mino_val_t **      -- MINO_ATOM
 *
 * Returns 0 on success, -1 on arity or type error. On failure the current
 * error is set via mino_last_error with kind "eval/arity" or "eval/type"
 * so the caller can just `return NULL;` after a non-zero return. The name
 * argument is used in error messages ("foo: expected int, got string").
 *
 * Extra arguments beyond the format string cause an arity error; missing
 * arguments do the same.
 */
int mino_args_parse(mino_state_t *S, const char *name, mino_val_t *args,
                    const char *fmt, ...);

/* ------------------------------------------------------------------------- */
/* Host interop                                                              */
/* ------------------------------------------------------------------------- */

/*
 * Enable interop dispatch. By default all host primitives return
 * "interop disabled". Call this after registration to activate them.
 */
void mino_host_enable(mino_state_t *S);

/*
 * Register host capabilities. String arguments must outlive the state.
 * Call before eval -- the registry is intended to be immutable after init.
 *
 * arity: expected argument count, or -1 for variadic.
 */
void mino_host_register_ctor(mino_state_t *S, const char *type_key,
                              int arity, mino_host_fn fn, void *ctx);
void mino_host_register_method(mino_state_t *S, const char *type_key,
                                const char *method_key, int arity,
                                mino_host_fn fn, void *ctx);
void mino_host_register_static(mino_state_t *S, const char *type_key,
                                const char *method_key, int arity,
                                mino_host_fn fn, void *ctx);
void mino_host_register_getter(mino_state_t *S, const char *type_key,
                                const char *field_key, mino_host_fn fn,
                                void *ctx);

/* ------------------------------------------------------------------------- */
/* Modules                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Module resolver callback. Given a module name (the argument to `require`),
 * return a file path to load, or NULL on failure. `ctx` is the opaque
 * pointer passed to mino_set_resolver.
 */
typedef const char *(*mino_resolve_fn)(const char *name, void *ctx);

/*
 * Register a module resolver. When mino code calls (require "name"), the
 * resolver is invoked to map the name to a file path. The file is loaded
 * once; subsequent requires of the same name return the cached value.
 * Pass NULL to remove the resolver.
 */
void mino_set_resolver(mino_state_t *S, mino_resolve_fn fn, void *ctx);

/*
 * Register a bundled-stdlib source under `name`. Subsequent calls to
 * (require '<name>) load this in-memory source instead of going to the
 * disk resolver, so embeds and brew/scoop installs without a lib/
 * directory still resolve their bundled namespaces.
 *
 * The source pointer must outlive the state -- typically a static
 * C-string literal in a generated header (e.g. lib_clojure_string.h).
 * The name is copied and freed at state teardown. Re-registering the
 * same name silently replaces the previous source.
 *
 * Use mino_install_clojure_<name> wrappers (declared below) when an
 * install hook fits the embedder's group; this raw API is exposed for
 * embedders that bundle their own non-clojure namespaces (mirroring
 * mino_set_resolver for the disk path).
 */
void mino_register_bundled_lib(mino_state_t *S, const char *name,
                                const char *source);

/* ------------------------------------------------------------------------- */
/* Bundled clojure.* stdlib install hooks                                    */
/* ------------------------------------------------------------------------- */

/*
 * Per-namespace install hooks for the bundled clojure.* stdlib. Each
 * call registers the in-binary source for its namespace so a
 * subsequent (require '[<ns>]) loads it from memory instead of
 * looking on disk. The C primitives that some of these namespaces
 * layer over are installed by `mino_install_core`; these hooks add
 * the wrapper sources only.
 *
 * Pairs that depend on each other ship as a single hook:
 *   - clojure.repl + clojure.stacktrace
 *   - clojure.datafy + clojure.core.protocols
 *
 * Standalone builds and embedders that want everything can call
 * `mino_install_all` instead of this surface.
 */
void mino_install_clojure_string(mino_state_t *S, mino_env_t *env);
void mino_install_clojure_set(mino_state_t *S, mino_env_t *env);
void mino_install_clojure_walk(mino_state_t *S, mino_env_t *env);
void mino_install_clojure_edn(mino_state_t *S, mino_env_t *env);
void mino_install_clojure_pprint(mino_state_t *S, mino_env_t *env);
void mino_install_clojure_zip(mino_state_t *S, mino_env_t *env);
void mino_install_clojure_data(mino_state_t *S, mino_env_t *env);
void mino_install_clojure_test(mino_state_t *S, mino_env_t *env);
void mino_install_clojure_repl(mino_state_t *S, mino_env_t *env);
void mino_install_clojure_datafy(mino_state_t *S, mino_env_t *env);
void mino_install_clojure_instant(mino_state_t *S, mino_env_t *env);
void mino_install_clojure_spec(mino_state_t *S, mino_env_t *env);

/*
 * Register the every-bundled-namespace + every-primitive-group set
 * the standalone binary ships with. Equivalent to the sequence:
 *
 *   mino_install_core(S, env);
 *   mino_install_io(S, env);
 *   mino_install_fs(S, env);
 *   mino_install_proc(S, env);
 *   mino_install_clojure_string(S, env);
 *   mino_install_clojure_set(S, env);
 *   mino_install_clojure_walk(S, env);
 *   mino_install_clojure_edn(S, env);
 *   mino_install_clojure_pprint(S, env);
 *   mino_install_clojure_zip(S, env);
 *   mino_install_clojure_data(S, env);
 *   mino_install_clojure_test(S, env);
 *   mino_install_clojure_repl(S, env);
 *   mino_install_clojure_datafy(S, env);
 *   mino_install_clojure_instant(S, env);
 *   mino_install_clojure_spec(S, env);
 *
 * Use this when you want the same surface a brew/scoop install
 * would provide. Embedders that need a tighter footprint pick the
 * subset they care about explicitly.
 */
void mino_install_all(mino_state_t *S, mino_env_t *env);

/* ------------------------------------------------------------------------- */
/* Execution limits                                                          */
/* ------------------------------------------------------------------------- */

#define MINO_LIMIT_STEPS  1   /* max eval steps per mino_eval/mino_eval_string */
#define MINO_LIMIT_HEAP   2   /* max bytes under GC management                */

/*
 * Set a global execution limit. Pass 0 to disable a limit. Step limits
 * are reset at the start of each mino_eval or mino_eval_string call.
 * When a limit is exceeded, the current eval returns NULL and
 * mino_last_error() reports the cause.
 */
void mino_set_limit(mino_state_t *S, int kind, size_t value);

/*
 * Request interruption of a running eval. Sets a flag that the eval loop
 * checks on each step. Safe to call from a different thread than the one
 * running eval. The flag is cleared at the start of the next eval call.
 */
void mino_interrupt(mino_state_t *S);

/*
 * Fault injection: schedule a simulated OOM after the next `n` GC-managed
 * allocations. When the counter reaches zero, gc_alloc_typed behaves as
 * if calloc returned NULL. Pass 0 to disable. Intended for testing only.
 */
void mino_set_fail_alloc_at(mino_state_t *S, long n);

/*
 * Fault injection for raw (non-GC) allocation paths such as the clone
 * serialization buffer. After `n` raw allocation attempts the next one
 * returns NULL. Pass 0 to disable. Intended for testing only.
 */
void mino_set_fail_raw_at(mino_state_t *S, long n);

/*
 * Check and decrement the raw fault injection counter. Returns 1 if
 * the allocation should be failed (simulated OOM), 0 otherwise.
 * Internal helper exposed for clone.c; not intended for embedder use.
 */
int mino_fi_should_fail_raw(mino_state_t *S);

/* ------------------------------------------------------------------------- */
/* Host thread grant                                                         */
/* ------------------------------------------------------------------------- */

/*
 * Threading is a per-state runtime capability, not a build-time toggle.
 * Each `mino_state_t` starts with `thread_limit = 1`; while the limit is
 * <= 1, `(future ...)`, `(promise)` / `deliver` / `realized?`,
 * `(thread ...)`, and the blocking core.async ops `<!!` / `>!!` /
 * `alts!!` throw `:mino/unsupported` with a message that names the policy
 * and points the host at this surface.
 *
 * Standalone `./mino` grants `thread_limit = <cpu_count>` right after
 * `mino_install_all`, so REPL/script users get the canonical surface
 * working out of the box. Embedders opt in per state by calling
 * `mino_set_thread_limit` with a value > 1.
 *
 * The actual host-thread implementation lands across upcoming versions;
 * v0.84.x ships the API surface and the throw-stub bodies so
 * embedders can code against the contract while the runtime catches up.
 * Status of each cycle is tracked in CHANGELOG.md.
 */

/*
 * Set the per-state thread limit. n=0 or n=1 disables host threads;
 * n>1 grants the runtime permission to spawn that many concurrent
 * worker threads. Calling with the same value twice is a no-op.
 *
 * Calling with n>1 BEFORE any `(future ...)` is in flight is the
 * supported sequence; lowering the limit while threads are running
 * does not interrupt them, but new spawns will respect the new ceiling.
 */
void mino_set_thread_limit(mino_state_t *S, int n);

/*
 * Read the current thread limit. Returns 1 by default (single-threaded).
 */
int  mino_get_thread_limit(mino_state_t *S);

/*
 * Return the count of host threads currently spawned by this state.
 * Decremented as threads complete and join. Returns 0 in single-
 * threaded mode.
 */
int  mino_thread_count(mino_state_t *S);

/*
 * Wait for all in-flight host threads to finish. Intended to be called
 * by the embedder before `mino_state_free`; calling it from a worker
 * thread that this state spawned is undefined behaviour. Safe to call
 * when no threads are in flight (returns immediately).
 */
void mino_quiesce_threads(mino_state_t *S);

/* ------------------------------------------------------------------------- */
/* Garbage collector control                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Kinds of collection the host can request. Use at quiescent points such
 * as between REPL turns, after bulk import, or before long-idle periods.
 *
 *   MINO_GC_MINOR -- nursery collection only; cheapest, bounded by young set
 *                   size. Safe at any time including mid-major.
 *   MINO_GC_MAJOR -- drain any in-flight incremental major cycle and sweep,
 *                   then start a fresh STW major if no cycle is active.
 *                   Reclaims old-gen dead; leaves young-gen alone.
 *   MINO_GC_FULL  -- minor, then finish or run a fresh STW major.
 *                   Strongest reclamation; highest pause.
 */
typedef enum {
    MINO_GC_MINOR = 1,
    MINO_GC_MAJOR = 2,
    MINO_GC_FULL  = 3
} mino_gc_kind_t;

void mino_gc_collect(mino_state_t *S, mino_gc_kind_t kind);

/*
 * Tunable parameters. Values out of range are rejected; mino_gc_set_param
 * returns 0 on success and -1 on a bad parameter or out-of-range value.
 *
 *   NURSERY_BYTES       young-gen trigger; minor fires when young exceeds.
 *                       Default 1 MiB. Range 64 KiB .. 256 MiB.
 *   MAJOR_GROWTH_TENTHS old-gen growth multiplier in tenths (15 = 1.5x).
 *                       Default 15. Range 11 .. 40.
 *   PROMOTION_AGE       minor-survival count before YOUNG->OLD promotion.
 *                       Default 1. Range 1 .. 8.
 *   INCREMENTAL_BUDGET  headers popped per incremental major slice.
 *                       Default 4096. Range 64 .. 65536.
 *   STEP_ALLOC_BYTES    bytes allocated between automatic slices.
 *                       Default 16 KiB. Range 1 KiB .. 16 MiB.
 */
typedef enum {
    MINO_GC_NURSERY_BYTES       = 1,
    MINO_GC_MAJOR_GROWTH_TENTHS = 2,
    MINO_GC_PROMOTION_AGE       = 3,
    MINO_GC_INCREMENTAL_BUDGET  = 4,
    MINO_GC_STEP_ALLOC_BYTES    = 5
} mino_gc_param_t;

int mino_gc_set_param(mino_state_t *S, mino_gc_param_t p, size_t value);

/* Phase tag returned in mino_gc_stats_t.phase. */
#define MINO_GC_PHASE_IDLE        0
#define MINO_GC_PHASE_MINOR       1
#define MINO_GC_PHASE_MAJOR_MARK  2
#define MINO_GC_PHASE_MAJOR_SWEEP 3

/*
 * Collector statistics. Populated by mino_gc_stats. No allocation is
 * performed; the host owns the out struct. Counters are cumulative
 * since state creation except where noted.
 *
 * Note: bytes_alloc is NOT a monotonic total. It tracks bytes live in
 * the bump path; minor GC decrements it by the bytes it sweeps and
 * major GC resets it to bytes_live. To recover a true allocation
 * total over a window, sum (delta bytes_alloc + delta bytes_freed);
 * bytes_freed IS monotonic. This is the formula the perf-gate
 * allocation tracker uses.
 */
typedef struct {
    size_t collections_minor;
    size_t collections_major;
    size_t bytes_live;         /* young + old */
    size_t bytes_young;
    size_t bytes_old;
    size_t bytes_alloc;        /* current; reset by major, decremented by minor */
    size_t bytes_freed;        /* monotonic: total ever swept */
    size_t total_gc_ns;
    size_t max_gc_ns;
    size_t remset_entries;     /* current remembered-set size */
    size_t remset_cap;         /* remembered-set capacity */
    size_t remset_high_water;  /* peak remset size this state */
    size_t mark_stack_cap;     /* mark-stack capacity */
    size_t mark_stack_high_water; /* peak mark-stack depth this state */
    int    phase;              /* MINO_GC_PHASE_* */
} mino_gc_stats_t;

void mino_gc_stats(mino_state_t *S, mino_gc_stats_t *out);

/* ------------------------------------------------------------------------- */
/* In-process REPL handle                                                    */
/* ------------------------------------------------------------------------- */

/*
 * Return codes for mino_repl_feed.
 */
#define MINO_REPL_OK     0   /* form evaluated; result written to *out      */
#define MINO_REPL_MORE   1   /* line accepted; more input needed            */
#define MINO_REPL_ERROR  2   /* parse or eval error; see mino_last_error()  */

typedef struct mino_repl mino_repl_t;

/*
 * Create a REPL handle that evaluates forms in `env`. The handle owns
 * an internal line buffer; the host drives it by feeding one line at a
 * time via mino_repl_feed. No thread is required: the host controls
 * the call cadence entirely.
 *
 * `env` must outlive the REPL handle.
 */
mino_repl_t *mino_repl_new(mino_state_t *S, mino_env_t *env);

/*
 * Feed one line of input to the REPL. Returns:
 *   MINO_REPL_OK    - a complete form was read and evaluated. The result
 *                      is written to *out (when out is non-NULL).
 *   MINO_REPL_MORE  - the line was accumulated; more input is needed to
 *                      complete the current form.
 *   MINO_REPL_ERROR - a parse or eval error occurred. The error message
 *                      is available via mino_last_error(). The buffer is
 *                      reset so the next feed starts a fresh form.
 *
 * Multiple complete forms on one line: only the first is evaluated per
 * call. Feed an empty line (or call again with "") to drain remaining
 * buffered forms.
 */
int mino_repl_feed(mino_repl_t *repl, const char *line, mino_val_t **out);

/*
 * Free the REPL handle and its internal buffer. Does not free `env`.
 */
void mino_repl_free(mino_repl_t *repl);

/* ------------------------------------------------------------------------- */
/* Value retention (refs)                                                     */
/* ------------------------------------------------------------------------- */

/*
 * Values returned by constructors and eval are borrowed: they survive until
 * the next GC cycle but are not pinned. A ref roots a value so it survives
 * collection indefinitely. The host must call mino_unref when the value is
 * no longer needed.
 *
 *   mino_ref_t *r = mino_ref(S, val);   // root val
 *   mino_val_t *v = mino_deref(r);      // get the value
 *   mino_unref(S, r);                   // release the root
 *
 * Refs are owned by the state that created them and freed when the state
 * is freed, but the host should unref explicitly to avoid holding objects
 * longer than necessary.
 */
mino_ref_t *mino_ref(mino_state_t *S, mino_val_t *val);
mino_val_t *mino_deref(const mino_ref_t *ref);
void        mino_unref(mino_state_t *S, mino_ref_t *ref);

/* ------------------------------------------------------------------------- */
/* Value cloning (cross-state transfer)                                      */
/* ------------------------------------------------------------------------- */

/*
 * Deep-copy a value from one state into another. Transferable types:
 * nil, bool, int, float, string, symbol, keyword, cons, vector, map, set.
 *
 * Returns NULL (and sets an error on dst) if the value contains a
 * non-transferable type (fn, macro, prim, handle, atom, lazy-seq).
 * Nested collections are cloned recursively; a non-transferable element
 * anywhere in the tree causes the entire clone to fail.
 */
mino_val_t *mino_clone(mino_state_t *dst, mino_state_t *src, mino_val_t *val);

#ifdef __cplusplus
}
#endif

#endif /* MINO_H */
