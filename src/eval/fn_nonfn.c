/*
 * fn_nonfn.c -- non-function callable dispatch.
 *
 * Extracted from fn.c to keep each translation unit under the 1100-line
 * limit.  Provides apply_non_fn_callable.
 *
 * Handles keyword/symbol, map, record, vector, set, sorted-map, and
 * sorted-set values used as functions.  Declaration lives in
 * eval/special_internal.h.
 */

#include "eval/special_internal.h"

mino_val *apply_non_fn_callable(mino_state *S, mino_val *fn,
                                  mino_val *args, const mino_val *form)
{
    int         nargs = 0;
    mino_val *tmp;
    for (tmp = args; mino_is_cons(tmp); tmp = tmp->as.cons.cdr)
        nargs++;

    /* Transients delegate the read interface to their persistent view.
     * (t-vec idx), (t-map :k), (t-set v) all behave identically to the
     * persistent original until persistent! is called -- matching
     * Clojure's read-only-on-transient contract. */
    if (mino_type_of(fn) == MINO_TRANSIENT) {
        if (!fn->as.transient.valid) {
            set_eval_diag(S, form, "eval/state", "MST001",
                "transient is no longer valid");
            return NULL;
        }
        fn = fn->as.transient.current;
        if (fn == NULL || mino_type_of(fn) == MINO_NIL) {
            set_eval_diag(S, form, "eval/type", "MTY002",
                "transient has no underlying collection");
            return NULL;
        }
    }

    if (mino_type_of(fn) == MINO_KEYWORD) {
        /* (:k m) => (get m :k); (:k m default) => (get m :k default). */
        if (nargs < 1 || nargs > 2) {
            set_eval_diag(S, form, "eval/arity", "MAR001",
                "keyword as function takes 1 or 2 arguments");
            return NULL;
        }
        {
            mino_val *coll    = args->as.cons.car;
            mino_val *def_val = nargs == 2
                ? args->as.cons.cdr->as.cons.car
                : mino_nil(S);
            if (coll != NULL && mino_type_of(coll) == MINO_TRANSIENT) {
                if (!coll->as.transient.valid) {
                    set_eval_diag(S, form, "eval/state", "MST001",
                        "transient is no longer valid");
                    return NULL;
                }
                coll = coll->as.transient.current;
                if (coll == NULL || mino_type_of(coll) == MINO_NIL) return def_val;
            }
            if (coll != NULL && mino_type_of(coll) == MINO_MAP) {
                mino_val *v = map_get_val(coll, fn);
                return v == NULL ? def_val : v;
            }
            if (coll != NULL && mino_type_of(coll) == MINO_SORTED_MAP) {
                mino_val *v = rb_get(S, coll->as.sorted.root, fn,
                                        coll->as.sorted.comparator);
                return v == NULL ? def_val : v;
            }
            if (coll != NULL && mino_type_of(coll) == MINO_RECORD) {
                int idx = record_field_index(coll, fn);
                if (idx >= 0) return coll->as.record.vals[idx];
                if (coll->as.record.ext != NULL) {
                    mino_val *v = map_get_val(coll->as.record.ext, fn);
                    if (v != NULL) return v;
                }
                return def_val;
            }
            if (coll != NULL && mino_type_of(coll) == MINO_SET) {
                /* (:k #{:k :other}) returns :k if present, else default.
                 * Mirrors Clojure's keyword-as-fn behaviour against a
                 * set: the set acts as a "is this key present?" check
                 * and the keyword is its own value. */
                uint32_t h = hash_val(fn);
                if (hamt_get(coll->as.set.root, fn, h, 0u) != NULL)
                    return fn;
                return def_val;
            }
            if (coll != NULL && mino_type_of(coll) == MINO_SORTED_SET) {
                if (rb_contains(S, coll->as.sorted.root, fn,
                                coll->as.sorted.comparator))
                    return fn;
                return def_val;
            }
            return def_val;
        }
    }
    if (mino_type_of(fn) == MINO_SYMBOL) {
        /* ('sym m) => (get m 'sym); ('sym m default) => (get m 'sym default).
         * Mirrors JVM Clojure: Symbols implement IFn through getLookup,
         * yielding nil for non-map collections. */
        if (nargs < 1 || nargs > 2) {
            set_eval_diag(S, form, "eval/arity", "MAR001",
                "symbol as function takes 1 or 2 arguments");
            return NULL;
        }
        {
            mino_val *coll    = args->as.cons.car;
            mino_val *def_val = nargs == 2
                ? args->as.cons.cdr->as.cons.car
                : mino_nil(S);
            if (coll != NULL && mino_type_of(coll) == MINO_MAP) {
                mino_val *v = map_get_val(coll, fn);
                return v == NULL ? def_val : v;
            }
            if (coll != NULL && mino_type_of(coll) == MINO_SORTED_MAP) {
                mino_val *v = rb_get(S, coll->as.sorted.root, fn,
                                        coll->as.sorted.comparator);
                return v == NULL ? def_val : v;
            }
            if (coll != NULL && mino_type_of(coll) == MINO_RECORD) {
                int idx = record_field_index(coll, fn);
                if (idx >= 0) return coll->as.record.vals[idx];
                if (coll->as.record.ext != NULL) {
                    mino_val *v = map_get_val(coll->as.record.ext, fn);
                    if (v != NULL) return v;
                }
                return def_val;
            }
            return def_val;
        }
    }
    if (mino_type_of(fn) == MINO_MAP) {
        /* ({:a 1} :k) => (get {:a 1} :k). */
        if (nargs < 1 || nargs > 2) {
            set_eval_diag(S, form, "eval/arity", "MAR001",
                "map as function takes 1 or 2 arguments");
            return NULL;
        }
        {
            mino_val *key     = args->as.cons.car;
            mino_val *def_val = nargs == 2
                ? args->as.cons.cdr->as.cons.car
                : mino_nil(S);
            mino_val *v = map_get_val(fn, key);
            return v == NULL ? def_val : v;
        }
    }
    if (mino_type_of(fn) == MINO_RECORD) {
        /* (record :key) and (record :key default) -- same lookup
         * surface as map. Goes through record_field_index (declared
         * fields) and falls back to ext lookup before returning the
         * default. */
        if (nargs < 1 || nargs > 2) {
            set_eval_diag(S, form, "eval/arity", "MAR001",
                "record as function takes 1 or 2 arguments");
            return NULL;
        }
        {
            mino_val *key     = args->as.cons.car;
            mino_val *def_val = nargs == 2
                ? args->as.cons.cdr->as.cons.car
                : mino_nil(S);
            int idx = record_field_index(fn, key);
            if (idx >= 0) return fn->as.record.vals[idx];
            if (fn->as.record.ext != NULL) {
                mino_val *v = map_get_val(fn->as.record.ext, key);
                if (v != NULL) return v;
            }
            return def_val;
        }
    }
    if (mino_type_of(fn) == MINO_VECTOR) {
        /* ([1 2 3] 0) => (nth [1 2 3] 0). */
        if (nargs != 1) {
            set_eval_diag(S, form, "eval/arity", "MAR001",
                "vector as function takes 1 argument");
            return NULL;
        }
        {
            mino_val *idx = args->as.cons.car;
            long long i;
            if (idx == NULL || !mino_val_int_p(idx)) {
                set_eval_diag(S, form, "eval/type", "MTY001",
                    "vector index must be an integer");
                return NULL;
            }
            i = mino_val_int_get(idx);
            if (i < 0 || (size_t)i >= fn->as.vec.len) {
                set_eval_diag(S, form, "eval/bounds", "MBD001",
                    "vector index out of bounds");
                return NULL;
            }
            return vec_nth(fn, (size_t)i);
        }
    }
    if (mino_type_of(fn) == MINO_SET) {
        /* (#{:a :b} :a) => :a or nil. */
        if (nargs != 1) {
            set_eval_diag(S, form, "eval/arity", "MAR001",
                "set as function takes 1 argument");
            return NULL;
        }
        {
            mino_val *key = args->as.cons.car;
            uint32_t h = hash_val(key);
            return hamt_get(fn->as.set.root, key, h, 0u) != NULL
                ? key : mino_nil(S);
        }
    }
    if (mino_type_of(fn) == MINO_SORTED_MAP) {
        if (nargs < 1 || nargs > 2) {
            set_eval_diag(S, form, "eval/arity", "MAR001",
                "sorted-map as function takes 1 or 2 arguments");
            return NULL;
        }
        {
            mino_val *key     = args->as.cons.car;
            mino_val *def_val = nargs == 2
                ? args->as.cons.cdr->as.cons.car
                : mino_nil(S);
            mino_val *v = rb_get(S, fn->as.sorted.root, key,
                                    fn->as.sorted.comparator);
            return v == NULL ? def_val : v;
        }
    }
    if (mino_type_of(fn) == MINO_SORTED_SET) {
        if (nargs != 1) {
            set_eval_diag(S, form, "eval/arity", "MAR001",
                "sorted-set as function takes 1 argument");
            return NULL;
        }
        {
            mino_val *key = args->as.cons.car;
            return rb_contains(S, fn->as.sorted.root, key,
                                fn->as.sorted.comparator)
                ? key : mino_nil(S);
        }
    }
    {
        char msg[128];
        snprintf(msg, sizeof(msg), "not a function (got %s)",
                 type_tag_str(fn));
        set_eval_diag(S, form, "eval/type", "MTY002", msg);
    }
    return NULL;
}
