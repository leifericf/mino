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
    MINO_RECUR    /* internal tail-call trampoline sentinel */
} mino_type_t;

typedef struct mino_val mino_val_t;
typedef struct mino_env mino_env_t;
typedef struct mino_vec_node  mino_vec_node_t;   /* opaque; see mino.c */
typedef struct mino_hamt_node mino_hamt_node_t;  /* opaque; see mino.c */

typedef mino_val_t *(*mino_prim_fn)(mino_val_t *args, mino_env_t *env);

struct mino_val {
    mino_type_t type;
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
        } handle;
        struct {          /* MINO_RECUR: carries rebind args for trampoline */
            mino_val_t *args;
        } recur;
    } as;
};

/* ------------------------------------------------------------------------- */
/* Constructors                                                              */
/* ------------------------------------------------------------------------- */

mino_val_t *mino_nil(void);
mino_val_t *mino_true(void);
mino_val_t *mino_false(void);
mino_val_t *mino_int(long long n);
mino_val_t *mino_float(double f);
mino_val_t *mino_string(const char *s);
mino_val_t *mino_string_n(const char *s, size_t len);
mino_val_t *mino_symbol(const char *s);
mino_val_t *mino_symbol_n(const char *s, size_t len);
mino_val_t *mino_keyword(const char *s);
mino_val_t *mino_keyword_n(const char *s, size_t len);
mino_val_t *mino_cons(mino_val_t *car, mino_val_t *cdr);
mino_val_t *mino_vector(mino_val_t **items, size_t len);
mino_val_t *mino_map(mino_val_t **keys, mino_val_t **vals, size_t len);
mino_val_t *mino_set(mino_val_t **items, size_t len);
mino_val_t *mino_prim(const char *name, mino_prim_fn fn);
mino_val_t *mino_handle(void *ptr, const char *tag);

/* Handle accessors — return NULL/0 if the value is not a handle. */
int         mino_is_handle(const mino_val_t *v);
void       *mino_handle_ptr(const mino_val_t *v);
const char *mino_handle_tag(const mino_val_t *v);

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

void mino_print(const mino_val_t *v);            /* to stdout, no newline */
void mino_println(const mino_val_t *v);          /* to stdout, with newline */
void mino_print_to(FILE *out, const mino_val_t *v);

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
mino_val_t *mino_read(const char *src, const char **end);

const char *mino_last_error(void);

/* ------------------------------------------------------------------------- */
/* Environment and evaluator                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Allocate a fresh root environment and register it with the collector so
 * every value reachable through it survives collection. The runtime holds
 * the returned env weakly: mino_env_free unregisters it and lets the next
 * sweep reclaim the frame and any closures that were reachable only from
 * within it. The host does not free any mino-owned pointers directly.
 */
mino_env_t *mino_env_new(void);
void        mino_env_free(mino_env_t *env);

/*
 * Convenience: allocate a new env and install core bindings in one call.
 * Equivalent to mino_env_new() followed by mino_install_core().
 */
mino_env_t *mino_new(void);

/* Define or replace a binding in `env`. */
void        mino_env_set(mino_env_t *env, const char *name, mino_val_t *val);
/* Look up `name`. Returns NULL if unbound. */
mino_val_t *mino_env_get(mino_env_t *env, const char *name);

/*
 * Install the core primitive bindings into `env`:
 *   arithmetic   + - * /
 *   comparison   = < <= > >= not=
 *   list         car cdr cons list
 *   collection   count nth first rest vector hash-map assoc get conj update
 *                keys vals
 *   sets         hash-set set? contains? disj
 *   sequences    map filter reduce take drop range repeat concat into apply
 *                reverse sort
 *   predicates   cons? nil? string? number? keyword? symbol? vector? map?
 *                set? fn? empty?
 *   utility      not identity some every?
 *   reflection   type doc source apropos
 *   strings      str subs split join starts-with? ends-with? includes?
 *                upper-case lower-case trim
 *   exceptions   throw
 *   modules      require
 *   macros       macroexpand macroexpand-1 gensym
 *   stdlib (mino-defined): when cond and or -> ->> comp partial complement
 * Special forms (quote, quasiquote, unquote, unquote-splicing, def,
 * defmacro, if, do, let, fn, loop, recur, try) are recognized directly by
 * the evaluator and do not need to be installed.
 *
 * NOTE: no I/O primitives are installed by this function. Call
 * mino_install_io to opt in to println, prn, and slurp.
 * Safe to call on a fresh env.
 */
void        mino_install_core(mino_env_t *env);

/*
 * Install I/O primitives: println, prn, slurp. These are kept separate
 * from mino_install_core so that sandboxed environments start with no
 * I/O capability; the host opts in by calling this function.
 */
void        mino_install_io(mino_env_t *env);

/*
 * Evaluate one form. Returns NULL on error and writes a message via
 * mino_last_error(). Returns mino_nil() for an explicit nil result.
 */
mino_val_t *mino_eval(mino_val_t *form, mino_env_t *env);

/*
 * Read and evaluate all forms in `src`. Returns the value of the last
 * form, or NULL on error. An empty string returns mino_nil().
 */
mino_val_t *mino_eval_string(const char *src, mino_env_t *env);

/*
 * Read a file at `path` and evaluate all forms. Returns the value of the
 * last form, or NULL on error (file I/O failures and parse/eval errors).
 */
mino_val_t *mino_load_file(const char *path, mino_env_t *env);

/*
 * Shorthand: bind a C function as a primitive in `env`.
 * Equivalent to mino_env_set(env, name, mino_prim(name, fn)).
 */
void mino_register_fn(mino_env_t *env, const char *name, mino_prim_fn fn);

/*
 * Call a callable value (fn, macro, prim) with an argument list.
 * Returns the result, or NULL on error (via mino_last_error).
 */
mino_val_t *mino_call(mino_val_t *fn, mino_val_t *args, mino_env_t *env);

/*
 * Protected call: same as mino_call but returns 0 on success (writing the
 * result to *out) or -1 on error. The error message is available via
 * mino_last_error(). *out is set to NULL on error.
 */
int mino_pcall(mino_val_t *fn, mino_val_t *args, mino_env_t *env,
               mino_val_t **out);

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
void mino_set_resolver(mino_resolve_fn fn, void *ctx);

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
void mino_set_limit(int kind, size_t value);

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
mino_repl_t *mino_repl_new(mino_env_t *env);

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

#ifdef __cplusplus
}
#endif

#endif /* MINO_H */
