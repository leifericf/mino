/*
 * clone.c -- value cloning for cross-state transfer.
 *
 * mino_clone deep-copies a data value from one mino_state into
 * another. Transferable types: nil, bool, int, float, string, symbol,
 * keyword, cons, vector, map, set. Non-transferable (fn, macro, prim,
 * handle, atom, lazy-seq) cause clone to fail and set a diagnostic on
 * the destination state. Useful for host code that manages multiple
 * isolated runtime instances.
 */

#include "runtime/internal.h"

/* ------------------------------------------------------------------------- */
/* Value cloning (cross-state transfer)                                      */
/* ------------------------------------------------------------------------- */

static mino_val *clone_val(mino_state *dst, const mino_val *v);

/* Clone metadata if present, attaching it to the cloned value. */
static int clone_meta(mino_state *dst, const mino_val *src,
                      mino_val *out)
{
    mino_val *m;
    if (src->meta == NULL) return 0;
    m = clone_val(dst, src->meta);
    if (m == NULL) return -1;
    out->meta = m;
    return 0;
}

static mino_val *clone_val(mino_state *dst, const mino_val *v)
{
    if (v == NULL) return mino_nil(dst);

    switch (mino_type_of(v)) {
    case MINO_NIL:        return mino_nil(dst);
    case MINO_EMPTY_LIST: return mino_empty_list(dst);
    case MINO_BOOL:       return mino_val_bool_get(v) ? mino_true(dst) : mino_false(dst);
    case MINO_INT:    return mino_int(dst, mino_val_int_get(v));
    case MINO_FLOAT:   return mino_float(dst, v->as.f);
    case MINO_FLOAT32: return mino_float32(dst, v->as.f);
    case MINO_CHAR:   return mino_char(dst, mino_val_char_get(v));
    case MINO_TRANSIENT:
        /* Transients are identity-based and mutable; cross-state
         * cloning would break aliasing invariants. Fail loudly. */
        return NULL;
    case MINO_STRING: return mino_string_n(dst, v->as.s.data, v->as.s.len);
    case MINO_SYMBOL: {
        mino_val *r = mino_symbol_n(dst, v->as.s.data, v->as.s.len);
        if (r != NULL && v->meta != NULL) {
            if (clone_meta(dst, v, r) != 0) return NULL;
        }
        return r;
    }
    case MINO_KEYWORD:return mino_keyword_n(dst, v->as.s.data, v->as.s.len);
    case MINO_CONS: {
        mino_val *car = clone_val(dst, v->as.cons.car);
        mino_val *cdr;
        mino_ref *rcar;
        if (car == NULL && v->as.cons.car != NULL
            && mino_type_of(v->as.cons.car) != MINO_NIL) return NULL;
        rcar = mino_ref_new(dst, car);
        cdr = clone_val(dst, v->as.cons.cdr);
        if (cdr == NULL && v->as.cons.cdr != NULL
            && mino_type_of(v->as.cons.cdr) != MINO_NIL) {
            mino_unref(dst, rcar);
            return NULL;
        }
        car = mino_deref(rcar);
        mino_unref(dst, rcar);
        {
            mino_val *r = mino_cons(dst, car, cdr);
            if (clone_meta(dst, v, r) != 0) return NULL;
            return r;
        }
    }
    case MINO_VECTOR: {
        size_t len = v->as.vec.len;
        size_t i;
        mino_val **items;
        mino_val *result;
        mino_ref **refs;
        if (len == 0) return mino_vector(dst, NULL, 0);
        if (mino_fi_should_fail_raw(dst)) return NULL;
        items = (mino_val **)malloc(len * sizeof(*items));
        refs  = (mino_ref **)malloc(len * sizeof(*refs));
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
            refs[i]  = mino_ref_new(dst, items[i]);
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
        mino_val **keys, **vals;
        mino_ref **krefs, **vrefs;
        mino_val *result;
        if (len == 0) return mino_map(dst, NULL, NULL, 0);
        if (mino_fi_should_fail_raw(dst)) return NULL;
        keys  = (mino_val **)malloc(len * sizeof(*keys));
        vals  = (mino_val **)malloc(len * sizeof(*vals));
        krefs = (mino_ref **)malloc(len * sizeof(*krefs));
        vrefs = (mino_ref **)malloc(len * sizeof(*vrefs));
        if (!keys || !vals || !krefs || !vrefs) {
            free(keys); free(vals); free(krefs); free(vrefs);
            return NULL;
        }
        for (i = 0; i < len; i++) {
            mino_val *src_key = vec_nth(v->as.map.key_order, i);
            mino_val *src_val = mino_map_lookup(v, src_key);
            keys[i]  = clone_val(dst, src_key);
            if (keys[i] == NULL) {
                size_t j;
                for (j = 0; j < i; j++) { mino_unref(dst, krefs[j]); mino_unref(dst, vrefs[j]); }
                free(keys); free(vals); free(krefs); free(vrefs);
                return NULL;
            }
            krefs[i] = mino_ref_new(dst, keys[i]);
            vals[i]  = clone_val(dst, src_val);
            if (vals[i] == NULL) {
                size_t j;
                mino_unref(dst, krefs[i]);
                for (j = 0; j < i; j++) { mino_unref(dst, krefs[j]); mino_unref(dst, vrefs[j]); }
                free(keys); free(vals); free(krefs); free(vrefs);
                return NULL;
            }
            vrefs[i] = mino_ref_new(dst, vals[i]);
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
        mino_val **items;
        mino_ref **refs;
        mino_val *result;
        if (len == 0) return mino_set(dst, NULL, 0);
        if (mino_fi_should_fail_raw(dst)) return NULL;
        items = (mino_val **)malloc(len * sizeof(*items));
        refs  = (mino_ref **)malloc(len * sizeof(*refs));
        if (!items || !refs) { free(items); free(refs); return NULL; }
        for (i = 0; i < len; i++) {
            items[i] = clone_val(dst, vec_nth(v->as.set.key_order, i));
            if (items[i] == NULL) {
                size_t j;
                for (j = 0; j < i; j++) mino_unref(dst, refs[j]);
                free(items); free(refs);
                return NULL;
            }
            refs[i]  = mino_ref_new(dst, items[i]);
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
    case MINO_VOLATILE:
    case MINO_LAZY:
    case MINO_CHUNK:
    case MINO_CHUNKED_CONS:
    case MINO_RECUR:
    case MINO_TAIL_CALL:
    case MINO_REDUCED:
    case MINO_VAR:
    case MINO_TYPE:
    case MINO_RECORD:
    case MINO_FUTURE:
    case MINO_HOST_ARRAY:
    case MINO_MAP_ENTRY:
    case MINO_TX_REF:
    case MINO_AGENT:
        return NULL;
    case MINO_UUID:
        return mino_uuid_from_bytes(dst, v->as.uuid.bytes);
    case MINO_REGEX: {
        /* Cross-state cloning isn't meaningful for an identity-equal
         * type. Return NULL so callers (which already handle NULL for
         * VAR/TYPE/RECORD/FUTURE) treat regex the same way. */
        return NULL;
    }
    case MINO_BIGINT: {
        /* Round-trip through the base-10 string form so the destination
         * state gets its own imath allocation (no cross-state sharing). */
        char       *buf = mino_bigint_to_cstr(v);
        mino_val *out;
        if (buf == NULL) return NULL;
        out = mino_bigint_from_string(dst, buf);
        free(buf);
        return out;
    }
    case MINO_RATIO: {
        mino_val *n = clone_val(dst, v->as.ratio.num);
        mino_val *d;
        if (n == NULL) return NULL;
        d = clone_val(dst, v->as.ratio.denom);
        if (d == NULL) return NULL;
        return mino_ratio_make_unchecked(dst, n, d);
    }
    case MINO_BIGDEC: {
        mino_val *u = clone_val(dst, v->as.bigdec.unscaled);
        if (u == NULL) return NULL;
        return mino_bigdec_make(dst, u, v->as.bigdec.scale);
    }
    }
    return NULL; /* unreachable */
}

mino_val *mino_clone(mino_state *dst, mino_state *src, mino_val *val)
{
    mino_val *result;
    (void)src;
    result = clone_val(dst, val);
    if (result == NULL && val != NULL) {
        set_eval_diag(dst, mino_current_ctx(dst)->eval_current_form, "internal", "MIN001",
                  "clone: value contains non-transferable types "
                  "(fn, macro, prim, handle, atom, or lazy-seq)");
    }
    return result;
}

