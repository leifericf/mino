/*
 * mino.h — public C API for the mino runtime.
 *
 * UNSTABLE until v1.0.0. Symbol names, types, and semantics may change.
 */

#ifndef MINO_H
#define MINO_H

#include <stddef.h>

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
    MINO_STRING,
    MINO_SYMBOL,
    MINO_KEYWORD,
    MINO_CONS,
    MINO_VECTOR,
    MINO_MAP,
    MINO_SET,     /* persistent set: HAMT of keys with sentinel values */
    MINO_PRIM,
    MINO_FN,
    MINO_MACRO,   /* user-defined macro (shares the fn struct layout) */
    MINO_HANDLE,  /* opaque host object: pointer + type tag string */
    MINO_ATOM,    /* mutable reference cell: wraps a single value */
    MINO_LAZY,    /* lazy sequence: thunk body + env, cached on first force */
    MINO_RECUR,   /* internal tail-call trampoline sentinel */
    MINO_TAIL_CALL, /* proper tail call: carries {fn, args} for trampoline */
    MINO_REDUCED  /* early termination wrapper for reduce */
} mino_type_t;

typedef struct mino_val   mino_val_t;
typedef struct mino_env   mino_env_t;
typedef struct mino_state mino_state_t;
typedef struct mino_ref   mino_ref_t;
typedef struct mino_vec_node  mino_vec_node_t;   /* opaque; see mino.c */
typedef struct mino_hamt_node mino_hamt_node_t;  /* opaque; see mino.c */

typedef mino_val_t *(*mino_prim_fn)(mino_state_t *S, mino_val_t *args,
                                    mino_env_t *env);
typedef void (*mino_finalizer_fn)(void *ptr, const char *tag);

struct mino_val {
    mino_type_t type;
    mino_val_t *meta;     /* metadata map (NULL when absent) */
    union {
        int b;            /* MINO_BOOL: 0 or 1 */
        long long i;      /* MINO_INT */
        double f;         /* MINO_FLOAT */
        struct {          /* MINO_STRING, MINO_SYMBOL, MINO_KEYWORD */
            char *data;
            size_t len;
        } s;
        struct {          /* MINO_CONS */
            mino_val_t *car;
            mino_val_t *cdr;
            const char *file;  /* source file (NULL if unknown) */
            int         line;  /* source line (0 if unknown) */
        } cons;
        struct {          /* MINO_VECTOR: persistent 32-way trie with tail */
            mino_vec_node_t *root;     /* trie spine (NULL when len <= 32) */
            mino_vec_node_t *tail;     /* partial leaf, 1..32 slots used */
            unsigned         tail_len; /* number of valid slots in tail */
            unsigned         shift;    /* height of root in multiples of 5 */
            size_t           len;      /* total element count */
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
        struct {          /* MINO_PRIM */
            const char *name;
            mino_prim_fn fn;
        } prim;
        struct {          /* MINO_FN: user-defined closure */
            mino_val_t *params;
            mino_val_t *body;
            mino_env_t *env;
        } fn;
        struct {          /* MINO_HANDLE: opaque host pointer + tag */
            void       *ptr;
            const char *tag;    /* static or interned; not GC-owned */
            void       (*finalizer)(void *ptr, const char *tag);
        } handle;
        struct {          /* MINO_ATOM: mutable reference cell */
            mino_val_t *val;
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
            mino_val_t *args;
        } recur;
        struct {          /* MINO_TAIL_CALL: proper tail call trampoline */
            mino_val_t *fn;
            mino_val_t *args;
        } tail_call;
        struct {          /* MINO_REDUCED: early termination wrapper */
            mino_val_t *val;
        } reduced;
    } as;
};

/* ------------------------------------------------------------------------- */
/* Constructors                                                              */
/* ------------------------------------------------------------------------- */

mino_val_t *mino_nil(mino_state_t *S);
mino_val_t *mino_true(mino_state_t *S);
mino_val_t *mino_false(mino_state_t *S);
mino_val_t *mino_int(mino_state_t *S, long long n);
mino_val_t *mino_float(mino_state_t *S, double f);
mino_val_t *mino_string(mino_state_t *S, const char *s);
mino_val_t *mino_string_n(mino_state_t *S, const char *s, size_t len);
mino_val_t *mino_symbol(mino_state_t *S, const char *s);
mino_val_t *mino_symbol_n(mino_state_t *S, const char *s, size_t len);
mino_val_t *mino_keyword(mino_state_t *S, const char *s);
mino_val_t *mino_keyword_n(mino_state_t *S, const char *s, size_t len);
mino_val_t *mino_cons(mino_state_t *S, mino_val_t *car, mino_val_t *cdr);
mino_val_t *mino_vector(mino_state_t *S, mino_val_t **items, size_t len);
mino_val_t *mino_map(mino_state_t *S, mino_val_t **keys, mino_val_t **vals,
                     size_t len);
mino_val_t *mino_set(mino_state_t *S, mino_val_t **items, size_t len);
mino_val_t *mino_prim(mino_state_t *S, const char *name, mino_prim_fn fn);
mino_val_t *mino_handle(mino_state_t *S, void *ptr, const char *tag);
mino_val_t *mino_handle_ex(mino_state_t *S, void *ptr, const char *tag,
                           mino_finalizer_fn finalizer);
mino_val_t *mino_atom(mino_state_t *S, mino_val_t *val);

/* Handle accessors — return NULL/0 if the value is not a handle. */
int         mino_is_handle(const mino_val_t *v);
void       *mino_handle_ptr(const mino_val_t *v);
const char *mino_handle_tag(const mino_val_t *v);

/* Atom accessors — mutable reference cells. */
int         mino_is_atom(const mino_val_t *v);
mino_val_t *mino_atom_deref(const mino_val_t *a);
void        mino_atom_reset(mino_val_t *a, mino_val_t *val);

/* ------------------------------------------------------------------------- */
/* Predicates and accessors                                                  */
/* ------------------------------------------------------------------------- */

int mino_is_nil(const mino_val_t *v);
int mino_is_truthy(const mino_val_t *v);
int mino_is_cons(const mino_val_t *v);
int mino_eq(const mino_val_t *a, const mino_val_t *b);

mino_val_t *mino_car(const mino_val_t *v);
mino_val_t *mino_cdr(const mino_val_t *v);
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

void mino_print(mino_state_t *S, const mino_val_t *v);
void mino_println(mino_state_t *S, const mino_val_t *v);
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

const char *mino_last_error(mino_state_t *S);

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
 * Install the core primitive bindings into `env`:
 *   arithmetic   + - * / mod rem quot
 *   bitwise      bit-and bit-or bit-xor bit-not bit-shift-left
 *                bit-shift-right
 *   comparison   = < <= > >= not=
 *   list         car cdr cons list
 *   collection   count nth first rest vector hash-map assoc dissoc get conj
 *                keys vals
 *   sets         hash-set set? contains? disj
 *   sequences    seq realized? reduce into apply reverse sort
 *   predicates   cons? nil? string? number? keyword? symbol? vector? map?
 *                set? fn? empty?
 *   metadata     meta with-meta vary-meta
 *   utility      not identity
 *   coercion     int float
 *   reflection   type name doc source apropos
 *   strings      str pr-str read-string format subs split join char-at
 *                starts-with? ends-with? includes? upper-case lower-case
 *                trim
 *   exceptions   throw
 *   modules      require
 *   macros       macroexpand macroexpand-1 gensym
 *   core.mino: when cond and or -> ->> map filter take drop range repeat
 *              concat update some every? comp partial complement
 * Special forms (quote, quasiquote, unquote, unquote-splicing, def,
 * defmacro, if, do, let, fn, loop, recur, try, lazy-seq) are recognized by
 * the evaluator and do not need to be installed.
 *
 * NOTE: no I/O primitives are installed by this function. Call
 * mino_install_io to opt in to println, prn, and slurp.
 * Safe to call on a fresh env.
 */
void        mino_install_core(mino_state_t *S, mino_env_t *env);

/*
 * Install I/O primitives: println, prn, slurp, spit, exit. These are kept
 * separate from mino_install_core so that sandboxed environments start with
 * no I/O capability; the host opts in by calling this function.
 */
void        mino_install_io(mino_state_t *S, mino_env_t *env);

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
 * Fault injection for raw (non-GC) allocation paths: clone serialization,
 * mailbox buffers, etc. After `n` raw allocation attempts the next one
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

/* ------------------------------------------------------------------------- */
/* Mailbox (thread-safe value queue)                                         */
/* ------------------------------------------------------------------------- */

/*
 * A mailbox is a thread-safe FIFO queue for passing values between states.
 * The mailbox is owned by the host, not by any state. Values are serialized
 * on send and deserialized on receive, so no cross-state pointers exist.
 *
 * The mailbox is the one concurrency primitive in mino that uses a mutex.
 * It is safe to call send and recv from different threads simultaneously.
 */
typedef struct mino_mailbox mino_mailbox_t;

mino_mailbox_t *mino_mailbox_new(void);

/*
 * Serialize val and enqueue it. Returns 0 on success, -1 on failure.
 * Only data values are serializable (same restrictions as mino_clone).
 */
int             mino_mailbox_send(mino_mailbox_t *mb, mino_state_t *S,
                                  mino_val_t *val);

/*
 * Dequeue and deserialize the next message into S. Returns the value,
 * or NULL if the mailbox is empty.
 */
mino_val_t     *mino_mailbox_recv(mino_mailbox_t *mb, mino_state_t *S);

/*
 * Free the mailbox and any queued messages. Does not affect any state.
 */
void            mino_mailbox_free(mino_mailbox_t *mb);

/* ------------------------------------------------------------------------- */
/* Actor (state + env + mailbox bundle)                                      */
/* ------------------------------------------------------------------------- */

/*
 * An actor bundles a runtime state, an environment with core bindings, and
 * a mailbox into a single entity. The host creates actors and drives them
 * by sending messages and calling eval; no background thread is started.
 *
 *   mino_actor_t *a = mino_actor_new();
 *   mino_actor_send(a, caller_state, msg);
 *   mino_val_t *m = mino_actor_recv(a);
 *   mino_eval(mino_actor_state(a), handler, mino_actor_env(a));
 *   mino_actor_free(a);
 */
typedef struct mino_actor mino_actor_t;

mino_actor_t   *mino_actor_new(void);
mino_state_t   *mino_actor_state(mino_actor_t *a);
mino_env_t     *mino_actor_env(mino_actor_t *a);
mino_mailbox_t *mino_actor_mailbox(mino_actor_t *a);
void            mino_actor_send(mino_actor_t *a, mino_state_t *src,
                                mino_val_t *val);
mino_val_t     *mino_actor_recv(mino_actor_t *a);
void            mino_actor_free(mino_actor_t *a);

#ifdef __cplusplus
}
#endif

#endif /* MINO_H */
