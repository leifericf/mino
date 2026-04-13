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
    MINO_PRIM,
    MINO_FN,
    MINO_RECUR    /* internal tail-call trampoline sentinel */
} mino_type_t;

typedef struct mino_val mino_val_t;
typedef struct mino_env mino_env_t;

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
        } cons;
        struct {          /* MINO_VECTOR */
            mino_val_t **data;
            size_t       len;
        } vec;
        struct {          /* MINO_PRIM */
            const char *name;
            mino_prim_fn fn;
        } prim;
        struct {          /* MINO_FN: user-defined closure */
            mino_val_t *params;
            mino_val_t *body;
            mino_env_t *env;
        } fn;
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
mino_val_t *mino_prim(const char *name, mino_prim_fn fn);

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

mino_env_t *mino_env_new(void);
void        mino_env_free(mino_env_t *env);

/* Define or replace a binding in `env`. */
void        mino_env_set(mino_env_t *env, const char *name, mino_val_t *val);
/* Look up `name`. Returns NULL if unbound. */
mino_val_t *mino_env_get(mino_env_t *env, const char *name);

/*
 * Install the core primitive bindings (+ - * / = < <= > >= car cdr cons list)
 * into `env`. Special forms (quote, def, if, do, let, fn, loop, recur) are
 * recognized directly by the evaluator and do not need to be installed.
 * Safe to call on a fresh env.
 */
void        mino_install_core(mino_env_t *env);

/*
 * Evaluate one form. Returns NULL on error and writes a message via
 * mino_last_error(). Returns mino_nil() for an explicit nil result.
 */
mino_val_t *mino_eval(mino_val_t *form, mino_env_t *env);

#ifdef __cplusplus
}
#endif

#endif /* MINO_H */
