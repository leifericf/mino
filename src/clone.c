/*
 * clone.c -- value cloning for cross-state transfer.
 *
 * mino_clone deep-copies a data value from one mino_state_t into
 * another. Transferable types: nil, bool, int, float, string, symbol,
 * keyword, cons, vector, map, set. Non-transferable (fn, macro, prim,
 * handle, atom, lazy-seq) cause clone to fail and set a diagnostic on
 * the destination state. Useful for host code that manages multiple
 * isolated runtime instances.
 */

#include "mino_internal.h"

/* ------------------------------------------------------------------------- */
/* Value cloning (cross-state transfer)                                      */
/* ------------------------------------------------------------------------- */

static mino_val_t *clone_val(mino_state_t *dst, const mino_val_t *v);

/* Clone metadata if present, attaching it to the cloned value. */
static int clone_meta(mino_state_t *dst, const mino_val_t *src,
                      mino_val_t *out)
{
    mino_val_t *m;
    if (src->meta == NULL) return 0;
    m = clone_val(dst, src->meta);
    if (m == NULL) return -1;
    out->meta = m;
    return 0;
}

static mino_val_t *clone_val(mino_state_t *dst, const mino_val_t *v)
{
    if (v == NULL) return mino_nil(dst);

    switch (v->type) {
    case MINO_NIL:    return mino_nil(dst);
    case MINO_BOOL:   return v->as.b ? mino_true(dst) : mino_false(dst);
    case MINO_INT:    return mino_int(dst, v->as.i);
    case MINO_FLOAT:  return mino_float(dst, v->as.f);
    case MINO_STRING: return mino_string_n(dst, v->as.s.data, v->as.s.len);
    case MINO_SYMBOL: {
        mino_val_t *r = mino_symbol_n(dst, v->as.s.data, v->as.s.len);
        if (r != NULL && v->meta != NULL) {
            if (clone_meta(dst, v, r) != 0) return NULL;
        }
        return r;
    }
    case MINO_KEYWORD:return mino_keyword_n(dst, v->as.s.data, v->as.s.len);
    case MINO_CONS: {
        mino_val_t *car = clone_val(dst, v->as.cons.car);
        mino_val_t *cdr;
        mino_ref_t *rcar;
        if (car == NULL && v->as.cons.car != NULL
            && v->as.cons.car->type != MINO_NIL) return NULL;
        rcar = mino_ref(dst, car);
        cdr = clone_val(dst, v->as.cons.cdr);
        if (cdr == NULL && v->as.cons.cdr != NULL
            && v->as.cons.cdr->type != MINO_NIL) {
            mino_unref(dst, rcar);
            return NULL;
        }
        car = mino_deref(rcar);
        mino_unref(dst, rcar);
        {
            mino_val_t *r = mino_cons(dst, car, cdr);
            if (clone_meta(dst, v, r) != 0) return NULL;
            return r;
        }
    }
    case MINO_VECTOR: {
        size_t len = v->as.vec.len;
        size_t i;
        mino_val_t **items;
        mino_val_t *result;
        mino_ref_t **refs;
        if (len == 0) return mino_vector(dst, NULL, 0);
        if (mino_fi_should_fail_raw(dst)) return NULL;
        items = (mino_val_t **)malloc(len * sizeof(*items));
        refs  = (mino_ref_t **)malloc(len * sizeof(*refs));
        if (items == NULL || refs == NULL) {
            free(items); free(refs);
            return NULL;
        }
        for (i = 0; i < len; i++) {
            items[i] = clone_val(dst, vec_nth(v, i));
            if (items[i] == NULL) {
                size_t j;
                for (j = 0; j < i; j++) mino_unref(dst, refs[j]);
                free(items); free(refs);
                return NULL;
            }
            refs[i]  = mino_ref(dst, items[i]);
        }
        for (i = 0; i < len; i++) {
            items[i] = mino_deref(refs[i]);
        }
        result = mino_vector(dst, items, len);
        for (i = 0; i < len; i++) mino_unref(dst, refs[i]);
        free(items);
        free(refs);
        if (clone_meta(dst, v, result) != 0) return NULL;
        return result;
    }
    case MINO_MAP: {
        size_t len = v->as.map.len;
        size_t i;
        mino_val_t **keys, **vals;
        mino_ref_t **krefs, **vrefs;
        mino_val_t *result;
        if (len == 0) return mino_map(dst, NULL, NULL, 0);
        if (mino_fi_should_fail_raw(dst)) return NULL;
        keys  = (mino_val_t **)malloc(len * sizeof(*keys));
        vals  = (mino_val_t **)malloc(len * sizeof(*vals));
        krefs = (mino_ref_t **)malloc(len * sizeof(*krefs));
        vrefs = (mino_ref_t **)malloc(len * sizeof(*vrefs));
        if (!keys || !vals || !krefs || !vrefs) {
            free(keys); free(vals); free(krefs); free(vrefs);
            return NULL;
        }
        for (i = 0; i < len; i++) {
            mino_val_t *src_key = vec_nth(v->as.map.key_order, i);
            mino_val_t *src_val = hamt_get(v->as.map.root, src_key,
                                           hash_val(src_key), 0);
            keys[i]  = clone_val(dst, src_key);
            if (keys[i] == NULL) {
                size_t j;
                for (j = 0; j < i; j++) { mino_unref(dst, krefs[j]); mino_unref(dst, vrefs[j]); }
                free(keys); free(vals); free(krefs); free(vrefs);
                return NULL;
            }
            krefs[i] = mino_ref(dst, keys[i]);
            vals[i]  = clone_val(dst, src_val);
            if (vals[i] == NULL) {
                size_t j;
                mino_unref(dst, krefs[i]);
                for (j = 0; j < i; j++) { mino_unref(dst, krefs[j]); mino_unref(dst, vrefs[j]); }
                free(keys); free(vals); free(krefs); free(vrefs);
                return NULL;
            }
            vrefs[i] = mino_ref(dst, vals[i]);
        }
        for (i = 0; i < len; i++) {
            keys[i] = mino_deref(krefs[i]);
            vals[i] = mino_deref(vrefs[i]);
        }
        result = mino_map(dst, keys, vals, len);
        for (i = 0; i < len; i++) {
            mino_unref(dst, krefs[i]);
            mino_unref(dst, vrefs[i]);
        }
        free(keys); free(vals); free(krefs); free(vrefs);
        if (clone_meta(dst, v, result) != 0) return NULL;
        return result;
    }
    case MINO_SET: {
        size_t len = v->as.set.len;
        size_t i;
        mino_val_t **items;
        mino_ref_t **refs;
        mino_val_t *result;
        if (len == 0) return mino_set(dst, NULL, 0);
        if (mino_fi_should_fail_raw(dst)) return NULL;
        items = (mino_val_t **)malloc(len * sizeof(*items));
        refs  = (mino_ref_t **)malloc(len * sizeof(*refs));
        if (!items || !refs) { free(items); free(refs); return NULL; }
        for (i = 0; i < len; i++) {
            items[i] = clone_val(dst, vec_nth(v->as.set.key_order, i));
            if (items[i] == NULL) {
                size_t j;
                for (j = 0; j < i; j++) mino_unref(dst, refs[j]);
                free(items); free(refs);
                return NULL;
            }
            refs[i]  = mino_ref(dst, items[i]);
        }
        for (i = 0; i < len; i++) {
            items[i] = mino_deref(refs[i]);
        }
        result = mino_set(dst, items, len);
        for (i = 0; i < len; i++) mino_unref(dst, refs[i]);
        free(items);
        free(refs);
        if (clone_meta(dst, v, result) != 0) return NULL;
        return result;
    }
    /* Sorted collections with custom comparators hold function refs and
     * cannot be safely cloned across runtimes. Natural-order ones could
     * be rebuilt, but for now treat all as non-transferable. */
    case MINO_SORTED_MAP:
    case MINO_SORTED_SET:
    /* Non-transferable types. */
    case MINO_FN:
    case MINO_MACRO:
    case MINO_PRIM:
    case MINO_HANDLE:
    case MINO_ATOM:
    case MINO_LAZY:
    case MINO_RECUR:
    case MINO_TAIL_CALL:
    case MINO_REDUCED:
    case MINO_VAR:
        return NULL;
    }
    return NULL; /* unreachable */
}

mino_val_t *mino_clone(mino_state_t *dst, mino_state_t *src, mino_val_t *val)
{
    mino_val_t *result;
    (void)src;
    result = clone_val(dst, val);
    if (result == NULL && val != NULL) {
        set_eval_diag(dst, dst->eval_current_form, "internal", "MIN001",
                  "clone: value contains non-transferable types "
                  "(fn, macro, prim, handle, atom, or lazy-seq)");
    }
    return result;
}

