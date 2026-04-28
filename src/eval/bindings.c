/*
 * bindings.c -- destructuring, let, loop, binding special forms.
 */

#include "eval/special_internal.h"
#include "collections/internal.h"

int kw_eq(const mino_val_t *v, const char *s)
{
    size_t n;
    if (v == NULL || v->type != MINO_KEYWORD) {
        return 0;
    }
    n = strlen(s);
    return v->as.s.len == n && memcmp(v->as.s.data, s, n) == 0;
}

/* Helper: bind a single symbol name to a value. The symbol is already
 * interned (came from the reader), so env_bind_sym can reuse its data
 * pointer and length directly instead of copying into a stack buffer. */
static int bind_sym(mino_state_t *S, mino_env_t *env, mino_val_t *sym,
                    mino_val_t *val)
{
    env_bind_sym(S, env, sym, val);
    return 1;
}

/* Forward declaration for recursive destructuring. */
static int bind_form(mino_state_t *S, mino_env_t *env, mino_val_t *pattern,
                     mino_val_t *val, const char *ctx);

/*
 * Destructure a vector pattern against a sequential value (list or vector).
 * Supports positional binding, `&` rest, and `:as` whole-binding.
 */
static int bind_vec_destructure(mino_state_t *S, mino_env_t *env,
                                mino_val_t *pattern, mino_val_t *val,
                                const char *ctx)
{
    size_t plen = pattern->as.vec.len;
    size_t i;
    /* Walk the sequential value into a cons list for uniform access. */
    mino_val_t *args = val;
    if (val != NULL && val->type == MINO_VECTOR) {
        /* Convert vector value to a cons list for positional walk. */
        mino_val_t *lst = mino_nil(S);
        size_t j = val->as.vec.len;
        while (j > 0) {
            j--;
            lst = mino_cons(S, vec_nth(val, j), lst);
        }
        args = lst;
    }
    for (i = 0; i < plen; i++) {
        mino_val_t *p = vec_nth(pattern, i);
        if (sym_eq(p, "&")) {
            /* Rest parameter: next element gets remaining args. */
            if (i + 1 >= plen) {
                set_eval_diag(S, S->ctx->eval_current_form, "syntax", "MSY003", "& must be followed by a binding form");
                return 0;
            }
            i++;
            p = vec_nth(pattern, i);
            if (!bind_form(S, env, p, args, ctx)) return 0;
            /* Check for :as after rest. */
            if (i + 1 < plen && kw_eq(vec_nth(pattern, i + 1), "as")) {
                if (i + 2 >= plen) {
                    set_eval_diag(S, S->ctx->eval_current_form, "syntax", "MSY003", ":as must be followed by a symbol");
                    return 0;
                }
                i += 2;
                p = vec_nth(pattern, i);
                if (p == NULL || p->type != MINO_SYMBOL) {
                    set_eval_diag(S, S->ctx->eval_current_form, "syntax", "MSY003", ":as must be followed by a symbol");
                    return 0;
                }
                if (!bind_sym(S, env, p, val)) return 0;
            }
            if (i + 1 < plen) {
                set_eval_diag(S, S->ctx->eval_current_form, "syntax", "MSY003", "unexpected forms after & binding");
                return 0;
            }
            return 1;
        }
        if (kw_eq(p, "as")) {
            /* Whole-collection binding. */
            if (i + 1 >= plen) {
                set_eval_diag(S, S->ctx->eval_current_form, "syntax", "MSY003", ":as must be followed by a symbol");
                return 0;
            }
            i++;
            p = vec_nth(pattern, i);
            if (p == NULL || p->type != MINO_SYMBOL) {
                set_eval_diag(S, S->ctx->eval_current_form, "syntax", "MSY003", ":as must be followed by a symbol");
                return 0;
            }
            if (!bind_sym(S, env, p, val)) return 0;
            continue;
        }
        /* Normal positional binding. */
        if (!mino_is_cons(args)) {
            /* Bind to nil when value is shorter than pattern. */
            if (!bind_form(S, env, p, mino_nil(S), ctx)) return 0;
        } else {
            if (!bind_form(S, env, p, args->as.cons.car, ctx)) return 0;
            args = args->as.cons.cdr;
        }
    }
    return 1;
}

/*
 * Destructure a map pattern against a map value.
 * Supports :keys [a b], explicit {sym :key} mapping, :or {defaults}, :as sym.
 */
static int bind_map_destructure(mino_state_t *S, mino_env_t *env,
                                mino_val_t *pattern, mino_val_t *val,
                                const char *ctx)
{
    /* Collect :keys, :or, :as, and explicit bindings from the pattern map. */
    mino_val_t *keys_vec  = NULL;
    mino_val_t *or_map    = NULL;
    mino_val_t *as_sym    = NULL;
    size_t i;

    if (val == NULL || val->type != MINO_MAP) {
        val = mino_nil(S);
    }

    /* Iterate pattern map entries by key_order. */
    for (i = 0; i < pattern->as.map.len; i++) {
        mino_val_t *pkey = vec_nth(pattern->as.map.key_order, i);
        mino_val_t *pval = map_get_val(pattern, pkey);
        if (pkey->type == MINO_KEYWORD && pkey->as.s.len == 4
            && memcmp(pkey->as.s.data, "keys", 4) == 0) {
            keys_vec = pval;
        } else if (pkey->type == MINO_KEYWORD && pkey->as.s.len == 2
                   && memcmp(pkey->as.s.data, "or", 2) == 0) {
            or_map = pval;
        } else if (pkey->type == MINO_KEYWORD && pkey->as.s.len == 2
                   && memcmp(pkey->as.s.data, "as", 2) == 0) {
            as_sym = pval;
        } else if (pkey->type == MINO_SYMBOL) {
            /* Explicit binding: {sym :key} */
            mino_val_t *found = NULL;
            if (val->type == MINO_MAP) {
                found = map_get_val(val, pval);
            }
            if (found == NULL && or_map != NULL && or_map->type == MINO_MAP) {
                found = map_get_val(or_map, pkey);
            }
            if (found == NULL) found = mino_nil(S);
            if (!bind_form(S, env, pkey, found, ctx)) return 0;
        }
    }

    /* :keys [a b] -- look up :a, :b in val. */
    if (keys_vec != NULL && keys_vec->type == MINO_VECTOR) {
        for (i = 0; i < keys_vec->as.vec.len; i++) {
            mino_val_t *ksym = vec_nth(keys_vec, i);
            mino_val_t *lookup_key;
            mino_val_t *found;
            if (ksym == NULL || ksym->type != MINO_SYMBOL) {
                set_eval_diag(S, S->ctx->eval_current_form, "syntax", "MSY003", ":keys elements must be symbols");
                return 0;
            }
            lookup_key = mino_keyword_n(S, ksym->as.s.data, ksym->as.s.len);
            found = NULL;
            if (val->type == MINO_MAP) {
                found = map_get_val(val, lookup_key);
            }
            if (found == NULL && or_map != NULL && or_map->type == MINO_MAP) {
                found = map_get_val(or_map, ksym);
            }
            if (found == NULL) found = mino_nil(S);
            if (!bind_sym(S, env, ksym, found)) return 0;
        }
    }

    /* :as sym -- bind whole map. */
    if (as_sym != NULL) {
        if (as_sym->type != MINO_SYMBOL) {
            set_eval_diag(S, S->ctx->eval_current_form, "syntax", "MSY003", ":as must be followed by a symbol");
            return 0;
        }
        if (!bind_sym(S, env, as_sym, val)) return 0;
    }

    return 1;
}

/*
 * Recursive destructuring binder. Dispatches on pattern type:
 * - Symbol: bind directly
 * - Vector: positional destructuring
 * - Map: key destructuring
 */
static int bind_form(mino_state_t *S, mino_env_t *env, mino_val_t *pattern,
                     mino_val_t *val, const char *ctx)
{
    if (pattern == NULL || pattern->type == MINO_SYMBOL) {
        return bind_sym(S, env, pattern, val);
    }
    if (pattern->type == MINO_VECTOR) {
        return bind_vec_destructure(S, env, pattern, val, ctx);
    }
    if (pattern->type == MINO_MAP) {
        return bind_map_destructure(S, env, pattern, val, ctx);
    }
    set_eval_diag(S, S->ctx->eval_current_form, "syntax", "MSY003", "unsupported binding form (expected symbol, vector, or map)");
    return 0;
}

/*
 * Bind a parameter list (cons list or vector) to a list of argument values.
 * Returns 1 on success, 0 on arity mismatch or error (with error set).
 */
int bind_params(mino_state_t *S, mino_env_t *env, mino_val_t *params,
                mino_val_t *args, const char *ctx)
{
    /* Vector params: delegate to vector destructuring. */
    if (params != NULL && params->type == MINO_VECTOR) {
        return bind_vec_destructure(S, env, params, args, ctx);
    }
    /* Cons-list params: walk in parallel with args. */
    while (mino_is_cons(params)) {
        mino_val_t *name = params->as.cons.car;
        /* `&` marks a rest-parameter. */
        if (sym_eq(name, "&")) {
            params = params->as.cons.cdr;
            if (!mino_is_cons(params)
                || params->as.cons.car == NULL) {
                set_eval_diag(S, S->ctx->eval_current_form, "syntax", "MSY003", "& must be followed by a binding form");
                return 0;
            }
            if (mino_is_cons(params->as.cons.cdr)) {
                set_eval_diag(S, S->ctx->eval_current_form, "syntax", "MSY003", "& parameter must be last");
                return 0;
            }
            return bind_form(S, env, params->as.cons.car, args, ctx);
        }
        if (!mino_is_cons(args)) {
            char msg[96];
            snprintf(msg, sizeof(msg), "%s arity mismatch", ctx);
            set_eval_diag(S, S->ctx->eval_current_form, "syntax", "MSY001", msg);
            return 0;
        }
        if (!bind_form(S, env, name, args->as.cons.car, ctx)) return 0;
        params = params->as.cons.cdr;
        args   = args->as.cons.cdr;
    }
    if (mino_is_cons(args)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s arity mismatch", ctx);
        set_eval_diag(S, S->ctx->eval_current_form, "syntax", "MSY001", msg);
        return 0;
    }
    return 1;
}

/* --- let, loop, binding special forms ------------------------------------ */

mino_val_t *eval_let(mino_state_t *S, mino_val_t *form,
                     mino_val_t *args, mino_env_t *env, int tail)
{
    mino_val_t *bindings;
    mino_val_t *body;
    mino_env_t *local;
    if (!mino_is_cons(args)) {
        set_eval_diag(S, form, "syntax", "MSY001", "let requires a binding form and body");
        return NULL;
    }
    bindings = args->as.cons.car;
    body     = args->as.cons.cdr;
    local = env_child(S, env);
    if (bindings != NULL && bindings->type == MINO_VECTOR) {
        /* Vector binding form: [pattern val pattern val ...] */
        size_t vlen = bindings->as.vec.len;
        size_t vi;
        if (vlen % 2 != 0) {
            set_eval_diag(S, form, "syntax", "MSY003", "let vector bindings must have even number of forms");
            return NULL;
        }
        for (vi = 0; vi < vlen; vi += 2) {
            mino_val_t *pat = vec_nth(bindings, vi);
            mino_val_t *val = eval_value(S, vec_nth(bindings, vi + 1), local);
            if (val == NULL) return NULL;
            gc_pin(val);
            if (!bind_form(S, local, pat, val, "let")) {
                gc_unpin(1);
                return NULL;
            }
            gc_unpin(1);
        }
    } else if (mino_is_cons(bindings) || mino_is_nil(bindings)) {
        /* Legacy list binding form: (name val name val ...) */
        while (mino_is_cons(bindings)) {
            mino_val_t *name_form = bindings->as.cons.car;
            mino_val_t *rest_pair = bindings->as.cons.cdr;
            mino_val_t *val;
            if (!mino_is_cons(rest_pair)) {
                set_eval_diag(S, form, "syntax", "MSY003", "let binding missing value");
                return NULL;
            }
            val = eval_value(S, rest_pair->as.cons.car, local);
            if (val == NULL) return NULL;
            gc_pin(val);
            if (!bind_form(S, local, name_form, val, "let")) {
                gc_unpin(1);
                return NULL;
            }
            gc_unpin(1);
            bindings = rest_pair->as.cons.cdr;
        }
    } else {
        set_eval_diag(S, form, "syntax", "MSY003", "let bindings must be a list or vector");
        return NULL;
    }
    return eval_implicit_do_impl(S, body, local, tail);
}

mino_val_t *eval_loop(mino_state_t *S, mino_val_t *form,
                      mino_val_t *args, mino_env_t *env, int tail)
{
    mino_val_t *bindings;
    mino_val_t *body;
    mino_val_t *params      = mino_nil(S);
    mino_val_t *params_tail = NULL;
    mino_env_t *local;
    if (!mino_is_cons(args)) {
        set_eval_diag(S, form, "syntax", "MSY001", "loop requires a binding form and body");
        return NULL;
    }
    bindings = args->as.cons.car;
    body     = args->as.cons.cdr;
    local = env_child(S, env);
    if (bindings != NULL && bindings->type == MINO_VECTOR) {
        /* Vector binding form: [name val name val ...] */
        size_t vlen = bindings->as.vec.len;
        size_t vi;
        mino_val_t **ptmp;
        if (vlen % 2 != 0) {
            set_eval_diag(S, form, "syntax", "MSY003", "loop vector bindings must have even number of forms");
            return NULL;
        }
        /* Build a params vector of just the name symbols for recur. */
        ptmp = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR,
                    (vlen / 2) * sizeof(*ptmp));
        for (vi = 0; vi < vlen; vi += 2) {
            mino_val_t *pat = vec_nth(bindings, vi);
            mino_val_t *val = eval_value(S, vec_nth(bindings, vi + 1), local);
            if (val == NULL) return NULL;
            gc_pin(val);
            if (!bind_form(S, local, pat, val, "loop")) {
                gc_unpin(1);
                return NULL;
            }
            gc_unpin(1);
            ptmp[vi / 2] = pat;
        }
        params = mino_vector(S, ptmp, vlen / 2);
    } else if (mino_is_cons(bindings) || mino_is_nil(bindings)) {
        /* Legacy list binding form. */
        while (mino_is_cons(bindings)) {
            mino_val_t *name_form = bindings->as.cons.car;
            mino_val_t *rest_pair = bindings->as.cons.cdr;
            mino_val_t *val;
            mino_val_t *cell;
            if (!mino_is_cons(rest_pair)) {
                set_eval_diag(S, form, "syntax", "MSY003", "loop binding missing value");
                return NULL;
            }
            val = eval_value(S, rest_pair->as.cons.car, local);
            if (val == NULL) return NULL;
            gc_pin(val);
            if (!bind_form(S, local, name_form, val, "loop")) {
                gc_unpin(1);
                return NULL;
            }
            gc_unpin(1);
            cell = mino_cons(S, name_form, mino_nil(S));
            if (params_tail == NULL) {
                params = cell;
            } else {
                mino_cons_cdr_set(S, params_tail, cell);
            }
            params_tail = cell;
            bindings = rest_pair->as.cons.cdr;
        }
    } else {
        set_eval_diag(S, form, "syntax", "MSY003", "loop bindings must be a list or vector");
        return NULL;
    }
    for (;;) {
        mino_val_t *result = eval_implicit_do_impl(S, body, local, tail);
        if (result == NULL) {
            return NULL;
        }
        if (result->type != MINO_RECUR) {
            return result;
        }
        if (!bind_params(S, local, params, result->as.recur.args,
                         "recur")) {
            return NULL;
        }
    }
}

mino_val_t *eval_binding(mino_state_t *S, mino_val_t *form,
                         mino_val_t *args, mino_env_t *env, int tail)
{
    mino_val_t *pairs, *body, *result;
    /* Frame is heap-allocated so the pointer remains valid even if a
     * throw inside body unwinds this C frame past the cleanup at the
     * end. The control.c longjmp handler walks S->ctx->dyn_stack and frees
     * each frame's bindings; if frame were stack-local, that walk
     * would read popped stack memory. */
    dyn_frame_t   *frame;
    dyn_binding_t *bhead = NULL;
    (void)tail;
    if (!mino_is_cons(args)) {
        set_eval_diag(S, form, "syntax", "MSY001", "binding requires a binding list and body");
        return NULL;
    }
    pairs = args->as.cons.car;
    body  = args->as.cons.cdr;
    if (pairs != NULL && pairs->type == MINO_VECTOR) {
        /* Vector binding form: [sym val sym val ...] */
        size_t vlen = pairs->as.vec.len;
        size_t vi;
        if (vlen % 2 != 0) {
            set_eval_diag(S, form, "syntax", "MSY003", "binding: odd number of forms in binding vector");
            return NULL;
        }
        for (vi = 0; vi < vlen; vi += 2) {
            mino_val_t *sym_v = vec_nth(pairs, vi);
            mino_val_t *val_form = vec_nth(pairs, vi + 1);
            mino_val_t *val;
            dyn_binding_t *b;
            char nbuf[256];
            size_t nlen;
            if (sym_v == NULL || sym_v->type != MINO_SYMBOL) {
                set_eval_diag(S, form, "syntax", "MSY003", "binding: names must be symbols");
                dyn_binding_list_free(bhead);
                return NULL;
            }
            nlen = sym_v->as.s.len;
            if (nlen >= sizeof(nbuf)) {
                set_eval_diag(S, form, "syntax", "MSY003", "binding: name too long");
                dyn_binding_list_free(bhead);
                return NULL;
            }
            memcpy(nbuf, sym_v->as.s.data, nlen);
            nbuf[nlen] = '\0';
            val = eval(S, val_form, env);
            if (val == NULL) {
                dyn_binding_list_free(bhead);
                return NULL;
            }
            b = (dyn_binding_t *)malloc(sizeof(*b));
            if (b == NULL) {
                set_eval_diag(S, form, "syntax", "MSY003", "binding: out of memory");
                dyn_binding_list_free(bhead);
                return NULL;
            }
            b->name = mino_symbol(S, nbuf)->as.s.data; /* interned */
            b->val  = val;
            b->next = bhead;
            bhead   = b;
        }
    } else if (mino_is_cons(pairs)) {
        /* Legacy list binding form: (sym val sym val ...) */
        while (pairs != NULL && pairs->type == MINO_CONS) {
            mino_val_t *sym_v, *val_form, *val;
            dyn_binding_t *b;
            char nbuf[256];
            size_t nlen;
            sym_v = pairs->as.cons.car;
            if (sym_v == NULL || sym_v->type != MINO_SYMBOL) {
                set_eval_diag(S, form, "syntax", "MSY003", "binding: names must be symbols");
                dyn_binding_list_free(bhead);
                return NULL;
            }
            nlen = sym_v->as.s.len;
            if (nlen >= sizeof(nbuf)) {
                set_eval_diag(S, form, "syntax", "MSY003", "binding: name too long");
                dyn_binding_list_free(bhead);
                return NULL;
            }
            memcpy(nbuf, sym_v->as.s.data, nlen);
            nbuf[nlen] = '\0';
            pairs = pairs->as.cons.cdr;
            if (pairs == NULL || pairs->type != MINO_CONS) {
                set_eval_diag(S, form, "syntax", "MSY003", "binding: odd number of forms in binding list");
                dyn_binding_list_free(bhead);
                return NULL;
            }
            val_form = pairs->as.cons.car;
            pairs    = pairs->as.cons.cdr;
            val = eval(S, val_form, env);
            if (val == NULL) {
                dyn_binding_list_free(bhead);
                return NULL;
            }
            b = (dyn_binding_t *)malloc(sizeof(*b));
            if (b == NULL) {
                set_eval_diag(S, form, "syntax", "MSY003", "binding: out of memory");
                dyn_binding_list_free(bhead);
                return NULL;
            }
            b->name = mino_symbol(S, nbuf)->as.s.data; /* interned */
            b->val  = val;
            b->next = bhead;
            bhead   = b;
        }
    } else {
        set_eval_diag(S, form, "syntax", "MSY001", "binding requires a binding list and body");
        return NULL;
    }
    /* Push frame. */
    frame = (dyn_frame_t *)malloc(sizeof(*frame));
    if (frame == NULL) {
        set_eval_diag(S, form, "internal", "MIN001", "binding: out of memory");
        dyn_binding_list_free(bhead);
        return NULL;
    }
    frame->bindings = bhead;
    frame->prev     = S->ctx->dyn_stack;
    S->ctx->dyn_stack    = frame;
    result = eval_implicit_do(S, body, env);
    /* Pop frame. */
    S->ctx->dyn_stack = frame->prev;
    dyn_binding_list_free(bhead);
    free(frame);
    return result;
}

/* ------------------------------------------------------------------------- */
/* destructure: emit a flat let-binding vector for macro authors             */
/* ------------------------------------------------------------------------- */

/* Build (sym arg1 arg2 arg3) as a cons list. */
static mino_val_t *call_form3(mino_state_t *S, const char *fn,
                              mino_val_t *a, mino_val_t *b, mino_val_t *c)
{
    mino_val_t *r = mino_cons(S, c, mino_nil(S));
    r = mino_cons(S, b, r);
    r = mino_cons(S, a, r);
    return mino_cons(S, mino_symbol(S, fn), r);
}

static mino_val_t *call_form2(mino_state_t *S, const char *fn,
                              mino_val_t *a, mino_val_t *b)
{
    mino_val_t *r = mino_cons(S, b, mino_nil(S));
    r = mino_cons(S, a, r);
    return mino_cons(S, mino_symbol(S, fn), r);
}

static mino_val_t *destr_gensym(mino_state_t *S, const char *prefix)
{
    char    buf[64];
    int     used;
    size_t  prefix_len = strlen(prefix);
    memcpy(buf, prefix, prefix_len);
    used = snprintf(buf + prefix_len, sizeof(buf) - prefix_len,
                    "%ld__auto__", ++S->gensym_counter);
    if (used < 0) return mino_symbol(S, "G__auto");
    return mino_symbol_n(S, buf, prefix_len + (size_t)used);
}

static int destructure_pair(mino_state_t *S, mino_val_t *lhs, mino_val_t *rhs,
                            mino_val_t **acc);

static void emit_pair(mino_state_t *S, mino_val_t **acc,
                      mino_val_t *name, mino_val_t *expr)
{
    *acc = vec_conj1(S, *acc, name);
    *acc = vec_conj1(S, *acc, expr);
}

static int destructure_vec_pair(mino_state_t *S, mino_val_t *lhs,
                                mino_val_t *rhs, mino_val_t **acc)
{
    mino_val_t *gen  = destr_gensym(S, "vec__");
    size_t      plen = lhs->as.vec.len;
    size_t      i;
    long long   idx  = 0;
    emit_pair(S, acc, gen, rhs);
    for (i = 0; i < plen; i++) {
        mino_val_t *p = vec_nth(lhs, i);
        if (sym_eq(p, "&")) {
            mino_val_t *expr;
            i++;
            if (i >= plen) {
                set_eval_diag(S, S->ctx->eval_current_form, "syntax", "MSY003",
                              "destructure: & must be followed by a binding form");
                return 0;
            }
            expr = call_form2(S, "nthnext", gen, mino_int(S, idx));
            {
                mino_val_t *rest = vec_nth(lhs, i);
                if (rest != NULL && rest->type == MINO_SYMBOL) {
                    emit_pair(S, acc, rest, expr);
                } else {
                    if (!destructure_pair(S, rest, expr, acc)) return 0;
                }
            }
            /* Optional :as following & rest. */
            if (i + 1 < plen && kw_eq(vec_nth(lhs, i + 1), "as")) {
                i += 2;
                if (i >= plen) {
                    set_eval_diag(S, S->ctx->eval_current_form, "syntax", "MSY003",
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
                set_eval_diag(S, S->ctx->eval_current_form, "syntax", "MSY003",
                              "destructure: :as must be followed by a symbol");
                return 0;
            }
            emit_pair(S, acc, vec_nth(lhs, i), gen);
            continue;
        }
        {
            mino_val_t *expr = call_form3(S, "nth", gen, mino_int(S, idx),
                                          mino_nil(S));
            if (p != NULL && p->type == MINO_SYMBOL) {
                emit_pair(S, acc, p, expr);
            } else {
                if (!destructure_pair(S, p, expr, acc)) return 0;
            }
            idx++;
        }
    }
    return 1;
}

static int destructure_map_pair(mino_state_t *S, mino_val_t *lhs,
                                mino_val_t *rhs, mino_val_t **acc)
{
    mino_val_t *gen      = destr_gensym(S, "map__");
    mino_val_t *or_map   = NULL;
    mino_val_t *as_sym   = NULL;
    mino_val_t *keys_vec = NULL;
    mino_val_t *strs_vec = NULL;
    mino_val_t *syms_vec = NULL;
    size_t      i;

    emit_pair(S, acc, gen, rhs);

    for (i = 0; i < lhs->as.map.len; i++) {
        mino_val_t *pk = vec_nth(lhs->as.map.key_order, i);
        mino_val_t *pv = map_get_val(lhs, pk);
        if      (kw_eq(pk, "keys")) keys_vec = pv;
        else if (kw_eq(pk, "strs")) strs_vec = pv;
        else if (kw_eq(pk, "syms")) syms_vec = pv;
        else if (kw_eq(pk, "or"))   or_map   = pv;
        else if (kw_eq(pk, "as"))   as_sym   = pv;
    }

    if (keys_vec != NULL && keys_vec->type == MINO_VECTOR) {
        for (i = 0; i < keys_vec->as.vec.len; i++) {
            mino_val_t *ksym  = vec_nth(keys_vec, i);
            mino_val_t *kkw;
            mino_val_t *deflt = NULL;
            if (ksym == NULL || ksym->type != MINO_SYMBOL) {
                set_eval_diag(S, S->ctx->eval_current_form, "syntax", "MSY003",
                              "destructure: :keys elements must be symbols");
                return 0;
            }
            kkw = mino_keyword_n(S, ksym->as.s.data, ksym->as.s.len);
            if (or_map != NULL && or_map->type == MINO_MAP) {
                deflt = map_get_val(or_map, ksym);
            }
            if (deflt == NULL) deflt = mino_nil(S);
            emit_pair(S, acc, ksym, call_form3(S, "get", gen, kkw, deflt));
        }
    }

    if (strs_vec != NULL && strs_vec->type == MINO_VECTOR) {
        for (i = 0; i < strs_vec->as.vec.len; i++) {
            mino_val_t *ssym  = vec_nth(strs_vec, i);
            mino_val_t *kstr;
            mino_val_t *deflt = NULL;
            if (ssym == NULL || ssym->type != MINO_SYMBOL) {
                set_eval_diag(S, S->ctx->eval_current_form, "syntax", "MSY003",
                              "destructure: :strs elements must be symbols");
                return 0;
            }
            kstr = mino_string_n(S, ssym->as.s.data, ssym->as.s.len);
            if (or_map != NULL && or_map->type == MINO_MAP) {
                deflt = map_get_val(or_map, ssym);
            }
            if (deflt == NULL) deflt = mino_nil(S);
            emit_pair(S, acc, ssym, call_form3(S, "get", gen, kstr, deflt));
        }
    }

    if (syms_vec != NULL && syms_vec->type == MINO_VECTOR) {
        for (i = 0; i < syms_vec->as.vec.len; i++) {
            mino_val_t *ssym = vec_nth(syms_vec, i);
            mino_val_t *quoted;
            mino_val_t *deflt = NULL;
            if (ssym == NULL || ssym->type != MINO_SYMBOL) {
                set_eval_diag(S, S->ctx->eval_current_form, "syntax", "MSY003",
                              "destructure: :syms elements must be symbols");
                return 0;
            }
            quoted = mino_cons(S, mino_symbol(S, "quote"),
                               mino_cons(S, ssym, mino_nil(S)));
            if (or_map != NULL && or_map->type == MINO_MAP) {
                deflt = map_get_val(or_map, ssym);
            }
            if (deflt == NULL) deflt = mino_nil(S);
            emit_pair(S, acc, ssym, call_form3(S, "get", gen, quoted, deflt));
        }
    }

    /* Explicit {sym key} or nested-pattern keys. */
    for (i = 0; i < lhs->as.map.len; i++) {
        mino_val_t *pk = vec_nth(lhs->as.map.key_order, i);
        mino_val_t *pv;
        if (pk == NULL || pk->type == MINO_KEYWORD) continue;
        pv = map_get_val(lhs, pk);
        if (pk->type == MINO_SYMBOL) {
            mino_val_t *deflt = NULL;
            if (or_map != NULL && or_map->type == MINO_MAP) {
                deflt = map_get_val(or_map, pk);
            }
            if (deflt == NULL) deflt = mino_nil(S);
            emit_pair(S, acc, pk, call_form3(S, "get", gen, pv, deflt));
        } else if (pk->type == MINO_VECTOR || pk->type == MINO_MAP) {
            mino_val_t *expr = call_form3(S, "get", gen, pv, mino_nil(S));
            if (!destructure_pair(S, pk, expr, acc)) return 0;
        }
    }

    if (as_sym != NULL) {
        emit_pair(S, acc, as_sym, gen);
    }
    return 1;
}

static int destructure_pair(mino_state_t *S, mino_val_t *lhs, mino_val_t *rhs,
                            mino_val_t **acc)
{
    if (lhs == NULL || lhs->type == MINO_SYMBOL) {
        emit_pair(S, acc, lhs, rhs);
        return 1;
    }
    if (lhs->type == MINO_VECTOR) {
        return destructure_vec_pair(S, lhs, rhs, acc);
    }
    if (lhs->type == MINO_MAP) {
        return destructure_map_pair(S, lhs, rhs, acc);
    }
    set_eval_diag(S, S->ctx->eval_current_form, "syntax", "MSY003",
                  "destructure: pattern must be a symbol, vector, or map");
    return 0;
}

mino_val_t *prim_destructure(mino_state_t *S, mino_val_t *args,
                             mino_env_t *env)
{
    mino_val_t *bindings;
    mino_val_t *acc;
    size_t      i;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_eval_diag(S, S->ctx->eval_current_form, "eval/arity", "MAR001",
            "destructure requires one argument: a binding-pairs vector");
        return NULL;
    }
    bindings = args->as.cons.car;
    if (bindings == NULL || bindings->type != MINO_VECTOR) {
        set_eval_diag(S, S->ctx->eval_current_form, "eval/type", "MTY001",
                      "destructure: argument must be a vector");
        return NULL;
    }
    if ((bindings->as.vec.len % 2) != 0) {
        set_eval_diag(S, S->ctx->eval_current_form, "eval/contract", "MCT001",
                      "destructure: binding vector requires an even number of forms");
        return NULL;
    }
    acc = mino_vector(S, NULL, 0);
    for (i = 0; i < bindings->as.vec.len; i += 2) {
        mino_val_t *lhs = vec_nth(bindings, i);
        mino_val_t *rhs = vec_nth(bindings, i + 1);
        if (!destructure_pair(S, lhs, rhs, &acc)) return NULL;
    }
    return acc;
}
