/*
 * mino.c — runtime implementation.
 *
 * Single-file amalgamation: this translation unit, paired with mino.h,
 * is the entire runtime. ANSI C, no external dependencies.
 */

#include "mino.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Allocation                                                                */
/* ------------------------------------------------------------------------- */

/*
 * v0.1 uses a simple per-allocation malloc with no reclamation. A real
 * collector arrives later in the roadmap; the API treats values as opaque
 * handles so the representation can change without breaking embedders.
 */
static mino_val_t *alloc_val(mino_type_t type)
{
    mino_val_t *v = (mino_val_t *)calloc(1, sizeof(*v));
    if (v == NULL) {
        /* Fatal: out of memory during construction. v0.1 has no recovery. */
        abort();
    }
    v->type = type;
    return v;
}

static char *dup_n(const char *s, size_t len)
{
    char *out = (char *)malloc(len + 1);
    if (out == NULL) {
        abort();
    }
    if (len > 0) {
        memcpy(out, s, len);
    }
    out[len] = '\0';
    return out;
}

/* ------------------------------------------------------------------------- */
/* Singletons                                                                */
/* ------------------------------------------------------------------------- */

static mino_val_t nil_singleton  = { MINO_NIL,  { 0 } };
static mino_val_t true_singleton  = { MINO_BOOL, { 1 } };
static mino_val_t false_singleton = { MINO_BOOL, { 0 } };

mino_val_t *mino_nil(void)   { return &nil_singleton; }
mino_val_t *mino_true(void)  { return &true_singleton; }
mino_val_t *mino_false(void) { return &false_singleton; }

/* ------------------------------------------------------------------------- */
/* Constructors                                                              */
/* ------------------------------------------------------------------------- */

mino_val_t *mino_int(long long n)
{
    mino_val_t *v = alloc_val(MINO_INT);
    v->as.i = n;
    return v;
}

mino_val_t *mino_float(double f)
{
    mino_val_t *v = alloc_val(MINO_FLOAT);
    v->as.f = f;
    return v;
}

mino_val_t *mino_string_n(const char *s, size_t len)
{
    mino_val_t *v = alloc_val(MINO_STRING);
    v->as.s.data = dup_n(s, len);
    v->as.s.len  = len;
    return v;
}

mino_val_t *mino_string(const char *s)
{
    return mino_string_n(s, strlen(s));
}

mino_val_t *mino_symbol_n(const char *s, size_t len)
{
    mino_val_t *v = alloc_val(MINO_SYMBOL);
    v->as.s.data = dup_n(s, len);
    v->as.s.len  = len;
    return v;
}

mino_val_t *mino_symbol(const char *s)
{
    return mino_symbol_n(s, strlen(s));
}

mino_val_t *mino_cons(mino_val_t *car, mino_val_t *cdr)
{
    mino_val_t *v = alloc_val(MINO_CONS);
    v->as.cons.car = car;
    v->as.cons.cdr = cdr;
    return v;
}

mino_val_t *mino_prim(const char *name, mino_prim_fn fn)
{
    mino_val_t *v = alloc_val(MINO_PRIM);
    v->as.prim.name = name;
    v->as.prim.fn   = fn;
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
        return mino_nil();
    }
    return v->as.cons.car;
}

mino_val_t *mino_cdr(const mino_val_t *v)
{
    if (!mino_is_cons(v)) {
        return mino_nil();
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

int mino_eq(const mino_val_t *a, const mino_val_t *b)
{
    if (a == b) {
        return 1;
    }
    if (a == NULL || b == NULL) {
        return 0;
    }
    if (a->type != b->type) {
        /* Allow int/float numeric comparison to be added later. */
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
        return a->as.s.len == b->as.s.len
            && memcmp(a->as.s.data, b->as.s.data, a->as.s.len) == 0;
    case MINO_CONS:
        return mino_eq(a->as.cons.car, b->as.cons.car)
            && mino_eq(a->as.cons.cdr, b->as.cons.cdr);
    case MINO_PRIM:
        return a->as.prim.fn == b->as.prim.fn;
    }
    return 0;
}
