/*
 * mino.h — public C API for the mino runtime.
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
#define MINO_VERSION_MINOR 61
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
    MINO_BIGDEC     /* arbitrary-precision decimal, stored as an
                     * unscaled bigint plus a non-negative integer
                     * scale: value = unscaled * 10^-scale. Constructed
                     * from `1.5M` / `1M` literals or via `(bigdec x)`. */
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
            const char *ns;      /* namespace (interned) */
            const char *sym;     /* name (interned) */
            mino_val_t *root;    /* root binding value */
            int         dynamic; /* 1 if ^:dynamic */
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
/* Transient (batch-mutation) API                                            */
/* ------------------------------------------------------------------------- */

/*
 * Transients give embedders an efficient batch-mutation path for
 * building vectors, maps, and sets without allocating a new
 * persistent value per step. After calling mino_persistent(t) the
 * transient is sealed; calling any *_bang on it returns NULL and sets
 * a runtime error. This matches Clojure's transient/persistent!
 * semantics at the embedding level. A mino-level (user-visible)
 * transient API is NOT shipped in this cycle.
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
 * strings, reflection, and the `core.mino` stdlib. Does not install I/O
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
 * performed; the host owns the out struct. All counters are cumulative
 * since state creation except bytes_* which reflect current heap state.
 */
typedef struct {
    size_t collections_minor;
    size_t collections_major;
    size_t bytes_live;         /* young + old */
    size_t bytes_young;
    size_t bytes_old;
    size_t bytes_alloc;        /* total ever allocated */
    size_t bytes_freed;        /* total ever swept */
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
 *   MINO_REPL_OK    — a complete form was read and evaluated. The result
 *                      is written to *out (when out is non-NULL).
 *   MINO_REPL_MORE  — the line was accumulated; more input is needed to
 *                      complete the current form.
 *   MINO_REPL_ERROR — a parse or eval error occurred. The error message
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
