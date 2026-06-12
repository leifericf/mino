/*
 * bindings_destr.c -- macro-expander destructuring primitive (prim_destructure).
 *
 * Extracted from bindings.c to keep each translation unit under the
 * 1100-line limit.  prim_destructure is declared in prim/internal.h
 * (it is registered as a primitive callable from prim/install.c).
 */

#include "eval/special_internal.h"
#include "eval/internal.h"
#include "collections/internal.h"
#include "prim/internal.h"

/* ------------------------------------------------------------------------- */
/* destructure: emit a flat let-binding vector for macro authors             */
/* ------------------------------------------------------------------------- */

/* Build (sym arg1 arg2 arg3) as a cons list. */
static mino_val *call_form3(mino_state *S, const char *fn,
                              mino_val *a, mino_val *b, mino_val *c)
{
    mino_val *r = mino_cons(S, c, mino_nil(S));
    r = mino_cons(S, b, r);
    r = mino_cons(S, a, r);
    return mino_cons(S, mino_symbol(S, fn), r);
}

static mino_val *call_form2(mino_state *S, const char *fn,
                              mino_val *a, mino_val *b)
{
    mino_val *r = mino_cons(S, b, mino_nil(S));
    r = mino_cons(S, a, r);
    return mino_cons(S, mino_symbol(S, fn), r);
}

static mino_val *destr_gensym(mino_state *S, const char *prefix)
{
    char    buf[64];
    int     used;
    size_t  prefix_len = strlen(prefix);
    memcpy(buf, prefix, prefix_len);
    used = snprintf(buf + prefix_len, sizeof(buf) - prefix_len,
                    "%ld__auto__", ++S->gensym_counter);
    if (used < 0) return mino_symbol(S, "G__auto");
    /* Cap to actual buffer space: snprintf returns the would-be length,
     * which can exceed sizeof(buf)-prefix_len when the counter is large.
     * Using the uncapped value as a length would over-read buf. */
    if ((size_t)used >= sizeof(buf) - prefix_len)
        used = (int)(sizeof(buf) - prefix_len) - 1;
    return mino_symbol_n(S, buf, prefix_len + (size_t)used);
}

static int destructure_pair(mino_state *S, mino_val *lhs, mino_val *rhs,
                            mino_val **acc);

static void emit_pair(mino_state *S, mino_val **acc,
                      mino_val *name, mino_val *expr)
{
    *acc = vec_conj1(S, *acc, name);
    *acc = vec_conj1(S, *acc, expr);
}

static int destructure_vec_pair(mino_state *S, mino_val *lhs,
                                mino_val *rhs, mino_val **acc)
{
    mino_val *gen  = destr_gensym(S, "vec__");
    size_t      plen = lhs->as.vec.len;
    size_t      i;
    long long   idx  = 0;
    emit_pair(S, acc, gen, rhs);
    for (i = 0; i < plen; i++) {
        mino_val *p = vec_nth(lhs, i);
        if (sym_eq(p, "&")) {
            mino_val *expr;
            i++;
            if (i >= plen) {
                set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "syntax", "MSY003",
                              "destructure: & must be followed by a binding form");
                return 0;
            }
            expr = call_form2(S, "nthnext", gen, mino_int(S, idx));
            {
                mino_val *rest = vec_nth(lhs, i);
                if (rest != NULL && mino_type_of(rest) == MINO_SYMBOL) {
                    emit_pair(S, acc, rest, expr);
                } else {
                    if (!destructure_pair(S, rest, expr, acc)) return 0;
                }
            }
            /* Optional :as following & rest. */
            if (i + 1 < plen && kw_eq(vec_nth(lhs, i + 1), "as")) {
                i += 2;
                if (i >= plen) {
                    set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "syntax", "MSY003",
                                  "destructure: :as must be followed by a symbol");
                    return 0;
                }
                emit_pair(S, acc, vec_nth(lhs, i), gen);
            }
            continue;
        }
        if (kw_eq(p, "as")) {
            i++;
            if (i >= plen) {
                set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "syntax", "MSY003",
                              "destructure: :as must be followed by a symbol");
                return 0;
            }
            emit_pair(S, acc, vec_nth(lhs, i), gen);
            continue;
        }
        {
            mino_val *expr = call_form3(S, "nth", gen, mino_int(S, idx),
                                          mino_nil(S));
            if (p != NULL && mino_type_of(p) == MINO_SYMBOL) {
                emit_pair(S, acc, p, expr);
            } else {
                if (!destructure_pair(S, p, expr, acc)) return 0;
            }
            idx++;
        }
    }
    return 1;
}

static int destructure_map_pair(mino_state *S, mino_val *lhs,
                                mino_val *rhs, mino_val **acc)
{
    mino_val *gen      = destr_gensym(S, "map__");
    mino_val *or_map   = NULL;
    mino_val *as_sym   = NULL;
    mino_val *keys_vec = NULL;
    mino_val *strs_vec = NULL;
    mino_val *syms_vec = NULL;
    size_t      i;

    emit_pair(S, acc, gen, rhs);

    for (i = 0; i < lhs->as.map.len; i++) {
        mino_val *pk = vec_nth(lhs->as.map.key_order, i);
        mino_val *pv = map_get_val(lhs, pk);
        if      (kw_eq(pk, "keys")) keys_vec = pv;
        else if (kw_eq(pk, "strs")) strs_vec = pv;
        else if (kw_eq(pk, "syms")) syms_vec = pv;
        else if (kw_eq(pk, "or"))   or_map   = pv;
        else if (kw_eq(pk, "as"))   as_sym   = pv;
    }

    if (keys_vec != NULL && mino_type_of(keys_vec) == MINO_VECTOR) {
        for (i = 0; i < keys_vec->as.vec.len; i++) {
            mino_val *ksym  = vec_nth(keys_vec, i);
            mino_val *kkw;
            mino_val *deflt = NULL;
            if (ksym == NULL || mino_type_of(ksym) != MINO_SYMBOL) {
                set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "syntax", "MSY003",
                              "destructure: :keys elements must be symbols");
                return 0;
            }
            kkw = mino_keyword_n(S, ksym->as.s.data, ksym->as.s.len);
            if (or_map != NULL && mino_type_of(or_map) == MINO_MAP) {
                deflt = map_get_val(or_map, ksym);
            }
            if (deflt == NULL) deflt = mino_nil(S);
            emit_pair(S, acc, ksym, call_form3(S, "get", gen, kkw, deflt));
        }
    }

    if (strs_vec != NULL && mino_type_of(strs_vec) == MINO_VECTOR) {
        for (i = 0; i < strs_vec->as.vec.len; i++) {
            mino_val *ssym  = vec_nth(strs_vec, i);
            mino_val *kstr;
            mino_val *deflt = NULL;
            if (ssym == NULL || mino_type_of(ssym) != MINO_SYMBOL) {
                set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "syntax", "MSY003",
                              "destructure: :strs elements must be symbols");
                return 0;
            }
            kstr = mino_string_n(S, ssym->as.s.data, ssym->as.s.len);
            if (or_map != NULL && mino_type_of(or_map) == MINO_MAP) {
                deflt = map_get_val(or_map, ssym);
            }
            if (deflt == NULL) deflt = mino_nil(S);
            emit_pair(S, acc, ssym, call_form3(S, "get", gen, kstr, deflt));
        }
    }

    if (syms_vec != NULL && mino_type_of(syms_vec) == MINO_VECTOR) {
        for (i = 0; i < syms_vec->as.vec.len; i++) {
            mino_val *ssym = vec_nth(syms_vec, i);
            mino_val *quoted;
            mino_val *deflt = NULL;
            if (ssym == NULL || mino_type_of(ssym) != MINO_SYMBOL) {
                set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "syntax", "MSY003",
                              "destructure: :syms elements must be symbols");
                return 0;
            }
            quoted = mino_cons(S, mino_symbol(S, "quote"),
                               mino_cons(S, ssym, mino_nil(S)));
            if (or_map != NULL && mino_type_of(or_map) == MINO_MAP) {
                deflt = map_get_val(or_map, ssym);
            }
            if (deflt == NULL) deflt = mino_nil(S);
            emit_pair(S, acc, ssym, call_form3(S, "get", gen, quoted, deflt));
        }
    }

    /* Explicit {sym key} or nested-pattern keys. */
    for (i = 0; i < lhs->as.map.len; i++) {
        mino_val *pk = vec_nth(lhs->as.map.key_order, i);
        mino_val *pv;
        if (pk == NULL || mino_type_of(pk) == MINO_KEYWORD) continue;
        pv = map_get_val(lhs, pk);
        if (mino_type_of(pk) == MINO_SYMBOL) {
            mino_val *deflt = NULL;
            if (or_map != NULL && mino_type_of(or_map) == MINO_MAP) {
                deflt = map_get_val(or_map, pk);
            }
            if (deflt == NULL) deflt = mino_nil(S);
            emit_pair(S, acc, pk, call_form3(S, "get", gen, pv, deflt));
        } else if (mino_type_of(pk) == MINO_VECTOR || mino_type_of(pk) == MINO_MAP) {
            mino_val *expr = call_form3(S, "get", gen, pv, mino_nil(S));
            if (!destructure_pair(S, pk, expr, acc)) return 0;
        }
    }

    if (as_sym != NULL) {
        emit_pair(S, acc, as_sym, gen);
    }
    return 1;
}

static int destructure_pair(mino_state *S, mino_val *lhs, mino_val *rhs,
                            mino_val **acc)
{
    if (lhs == NULL || mino_type_of(lhs) == MINO_SYMBOL) {
        emit_pair(S, acc, lhs, rhs);
        return 1;
    }
    if (mino_type_of(lhs) == MINO_VECTOR) {
        return destructure_vec_pair(S, lhs, rhs, acc);
    }
    if (mino_type_of(lhs) == MINO_MAP) {
        return destructure_map_pair(S, lhs, rhs, acc);
    }
    set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "syntax", "MSY003",
                  "destructure: pattern must be a symbol, vector, or map");
    return 0;
}

mino_val *prim_destructure(mino_state *S, mino_val *args,
                             mino_env *env)
{
    mino_val *bindings;
    mino_val *acc;
    size_t      i;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "eval/arity", "MAR001",
            "destructure requires one argument: a binding-pairs vector");
        return NULL;
    }
    bindings = args->as.cons.car;
    if (bindings == NULL || mino_type_of(bindings) != MINO_VECTOR) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "eval/type", "MTY001",
                      "destructure: argument must be a vector");
        return NULL;
    }
    if ((bindings->as.vec.len % 2) != 0) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "eval/contract", "MCT001",
                      "destructure: binding vector requires an even number of forms");
        return NULL;
    }
    acc = mino_vector(S, NULL, 0);
    for (i = 0; i < bindings->as.vec.len; i += 2) {
        mino_val *lhs = vec_nth(bindings, i);
        mino_val *rhs = vec_nth(bindings, i + 1);
        if (!destructure_pair(S, lhs, rhs, &acc)) return NULL;
    }
    return acc;
}
