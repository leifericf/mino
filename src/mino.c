/*
 * mino.c -- evaluator core helpers: sym_eq, macroexpand, quasiquote,
 *           eval_value, eval_implicit_do, lazy_force, eval_args.
 */

#include "mino_internal.h"

int sym_eq(const mino_val_t *v, const char *s)
{
    size_t n;
    if (v == NULL || v->type != MINO_SYMBOL) {
        return 0;
    }
    n = strlen(s);
    return v->as.s.len == n && memcmp(v->as.s.data, s, n) == 0;
}


/*
 * macroexpand1: if `form` is a call whose head resolves to a macro in env,
 * expand it once and return the new form. If not a macro call, return the
 * input unchanged and set *expanded = 0.
 */
mino_val_t *macroexpand1(mino_state_t *S, mino_val_t *form, mino_env_t *env,
                         int *expanded)
{
    char        buf[256];
    size_t      n;
    mino_val_t *head;
    mino_val_t *mac;
    *expanded = 0;
    if (!mino_is_cons(form)) {
        return form;
    }
    head = form->as.cons.car;
    if (head == NULL || head->type != MINO_SYMBOL) {
        return form;
    }
    n = head->as.s.len;
    if (n >= sizeof(buf)) {
        return form;
    }
    memcpy(buf, head->as.s.data, n);
    buf[n] = '\0';
    mac = mino_env_get(env, buf);
    if (mac == NULL || mac->type != MINO_MACRO) {
        return form;
    }
    *expanded = 1;
    return apply_callable(S, mac, form->as.cons.cdr, env);
}

/* Expand repeatedly until `form` is no longer a macro call at the top. */
mino_val_t *macroexpand_all(mino_state_t *S, mino_val_t *form, mino_env_t *env)
{
    for (;;) {
        int         expanded = 0;
        mino_val_t *next     = macroexpand1(S, form, env, &expanded);
        if (next == NULL) {
            return NULL;
        }
        if (!expanded) {
            return form;
        }
        form = next;
    }
}

/*
 * quasiquote_expand: walk `form` as a template. Sublists, subvectors, and
 * submaps are recursed into; (unquote x) evaluates x and uses its value;
 * (unquote-splicing x) evaluates x (expected to yield a list) and splices
 * its elements into the enclosing list.
 */
mino_val_t *quasiquote_expand(mino_state_t *S, mino_val_t *form,
                                     mino_env_t *env)
{
    if (form != NULL && form->type == MINO_VECTOR) {
        size_t       nn  = form->as.vec.len;
        size_t       i;
        int          has_splice = 0;
        if (nn == 0) { return form; }
        /* Fast path: no ~@ means fixed-size output. */
        for (i = 0; i < nn; i++) {
            mino_val_t *e = vec_nth(form, i);
            if (mino_is_cons(e)
                && sym_eq(e->as.cons.car, "unquote-splicing")) {
                has_splice = 1; break;
            }
        }
        if (!has_splice) {
            mino_val_t **tmp = (mino_val_t **)gc_alloc_typed(S,
                GC_T_VALARR, nn * sizeof(*tmp));
            for (i = 0; i < nn; i++) {
                mino_val_t *e = quasiquote_expand(S, vec_nth(form, i), env);
                if (e == NULL) { return NULL; }
                tmp[i] = e;
            }
            return mino_vector(S, tmp, nn);
        }
        /* Slow path: ~@ present, build cons list then convert. */
        {
        mino_val_t  *out  = mino_nil(S);
        mino_val_t  *tail = NULL;
        size_t       count = 0;
        for (i = 0; i < nn; i++) {
            mino_val_t *elem = vec_nth(form, i);
            if (mino_is_cons(elem)
                && sym_eq(elem->as.cons.car, "unquote-splicing")) {
                mino_val_t *arg = elem->as.cons.cdr;
                mino_val_t *spliced;
                if (!mino_is_cons(arg)) {
                    set_eval_diag(S, S->eval_current_form, "syntax",
                        "MSY001", "unquote-splicing requires one argument");
                    return NULL;
                }
                spliced = eval_value(S, arg->as.cons.car, env);
                if (spliced == NULL) { return NULL; }
                if (spliced->type == MINO_VECTOR) {
                    size_t j;
                    for (j = 0; j < spliced->as.vec.len; j++) {
                        mino_val_t *cell = mino_cons(S,
                            vec_nth(spliced, j), mino_nil(S));
                        if (tail == NULL) { out = cell; }
                        else { tail->as.cons.cdr = cell; }
                        tail = cell;
                        count++;
                    }
                } else {
                    mino_val_t *sp = spliced;
                    while (mino_is_cons(sp)) {
                        mino_val_t *cell = mino_cons(S,
                            sp->as.cons.car, mino_nil(S));
                        if (tail == NULL) { out = cell; }
                        else { tail->as.cons.cdr = cell; }
                        tail = cell;
                        count++;
                        sp = sp->as.cons.cdr;
                    }
                }
            } else {
                mino_val_t *expanded = quasiquote_expand(S, elem, env);
                mino_val_t *cell;
                if (expanded == NULL) { return NULL; }
                cell = mino_cons(S, expanded, mino_nil(S));
                if (tail == NULL) { out = cell; }
                else { tail->as.cons.cdr = cell; }
                tail = cell;
                count++;
            }
        }
        {
            mino_val_t **tmp = (mino_val_t **)gc_alloc_typed(S,
                GC_T_VALARR, count * sizeof(*tmp));
            mino_val_t  *p   = out;
            size_t       idx = 0;
            while (mino_is_cons(p)) {
                tmp[idx++] = p->as.cons.car;
                p = p->as.cons.cdr;
            }
            return mino_vector(S, tmp, count);
        }
        } /* end slow path */
    }
    if (form != NULL && form->type == MINO_MAP) {
        size_t        nn = form->as.map.len;
        mino_val_t  **ks;
        mino_val_t  **vs;
        size_t        i;
        if (nn == 0) { return form; }
        ks = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, nn * sizeof(*ks));
        vs = (mino_val_t **)gc_alloc_typed(S, GC_T_VALARR, nn * sizeof(*vs));
        for (i = 0; i < nn; i++) {
            mino_val_t *key = vec_nth(form->as.map.key_order, i);
            mino_val_t *val = map_get_val(form, key);
            mino_val_t *kk  = quasiquote_expand(S, key, env);
            mino_val_t *vv;
            if (kk == NULL) { return NULL; }
            vv = quasiquote_expand(S, val, env);
            if (vv == NULL) { return NULL; }
            ks[i] = kk; vs[i] = vv;
        }
        return mino_map(S, ks, vs, nn);
    }
    if (!mino_is_cons(form)) {
        return form;
    }
    {
        mino_val_t *head = form->as.cons.car;
        if (sym_eq(head, "unquote")) {
            mino_val_t *arg = form->as.cons.cdr;
            if (!mino_is_cons(arg)) {
                set_eval_diag(S, S->eval_current_form, "syntax", "MSY001", "unquote requires one argument");
                return NULL;
            }
            return eval_value(S, arg->as.cons.car, env);
        }
        if (sym_eq(head, "unquote-splicing")) {
            set_eval_diag(S, S->eval_current_form, "syntax", "MSY001", "unquote-splicing must appear inside a list");
            return NULL;
        }
    }
    {
        mino_val_t *out  = mino_nil(S);
        mino_val_t *tail = NULL;
        mino_val_t *p    = form;
        while (mino_is_cons(p)) {
            mino_val_t *elem = p->as.cons.car;
            if (mino_is_cons(elem)
                && sym_eq(elem->as.cons.car, "unquote-splicing")) {
                mino_val_t *arg = elem->as.cons.cdr;
                mino_val_t *spliced;
                mino_val_t *sp;
                if (!mino_is_cons(arg)) {
                    set_eval_diag(S, S->eval_current_form, "syntax", "MSY001", "unquote-splicing requires one argument");
                    return NULL;
                }
                spliced = eval_value(S, arg->as.cons.car, env);
                if (spliced == NULL) { return NULL; }
                sp = spliced;
                while (mino_is_cons(sp)) {
                    mino_val_t *cell = mino_cons(S, sp->as.cons.car, mino_nil(S));
                    if (tail == NULL) { out = cell; } else { tail->as.cons.cdr = cell; }
                    tail = cell;
                    sp = sp->as.cons.cdr;
                }
            } else {
                mino_val_t *expanded = quasiquote_expand(S, elem, env);
                mino_val_t *cell;
                if (expanded == NULL) { return NULL; }
                cell = mino_cons(S, expanded, mino_nil(S));
                if (tail == NULL) { out = cell; } else { tail->as.cons.cdr = cell; }
                tail = cell;
            }
            p = p->as.cons.cdr;
        }
        return out;
    }
}

/*
 * Evaluate `form` for its value. Any MINO_RECUR escaping here is a
 * non-tail recur and is rejected. Use plain eval(S, ) in positions where
 * a recur is legitimately in tail position (if branches, implicit-do
 * trailing expression, fn/loop body through the trampoline).
 */
mino_val_t *eval_value(mino_state_t *S, mino_val_t *form, mino_env_t *env)
{
    mino_val_t *v = eval(S, form, env);
    if (v == NULL) {
        return NULL;
    }
    if (v->type == MINO_RECUR) {
        set_eval_diag(S, S->eval_current_form, "syntax", "MSY001", "recur must be in tail position");
        return NULL;
    }
    if (v->type == MINO_TAIL_CALL) {
        set_eval_diag(S, S->eval_current_form, "syntax", "MSY001", "tail call in non-tail position");
        return NULL;
    }
    return v;
}

mino_val_t *eval_implicit_do_impl(mino_state_t *S, mino_val_t *body,
                                  mino_env_t *env, int tail)
{
    if (!mino_is_cons(body)) {
        return mino_nil(S);
    }
    for (;;) {
        mino_val_t *rest = body->as.cons.cdr;
        if (!mino_is_cons(rest)) {
            /* Last expression: tail position, propagate recur/tail-call. */
            return eval_impl(S, body->as.cons.car, env, tail);
        }
        if (eval_value(S, body->as.cons.car, env) == NULL) {
            return NULL;
        }
        body = rest;
    }
}

mino_val_t *eval_implicit_do(mino_state_t *S, mino_val_t *body, mino_env_t *env)
{
    return eval_implicit_do_impl(S, body, env, 0);
}

/*
 * Force a lazy sequence: evaluate the body in the captured environment,
 * cache the result, and release the thunk for GC. Iteratively unwraps
 * nested lazy seqs to avoid stack overflow.
 */
mino_val_t *lazy_force(mino_state_t *S, mino_val_t *v)
{
    if (v->as.lazy.realized) {
        return v->as.lazy.cached;
    }
    {
        mino_val_t *result = v->as.lazy.c_thunk != NULL
            ? v->as.lazy.c_thunk(S, v->as.lazy.body)
            : eval_implicit_do(S, v->as.lazy.body, v->as.lazy.env);
        if (result == NULL) return NULL;
        /* Iteratively unwrap nested lazy seqs. */
        while (result != NULL && result->type == MINO_LAZY) {
            if (result->as.lazy.realized) {
                result = result->as.lazy.cached;
            } else {
                mino_val_t *inner = result->as.lazy.c_thunk != NULL
                    ? result->as.lazy.c_thunk(S, result->as.lazy.body)
                    : eval_implicit_do(S,
                        result->as.lazy.body, result->as.lazy.env);
                gc_write_barrier(S, result, inner);
                result->as.lazy.cached  = inner;
                result->as.lazy.realized = 1;
                result->as.lazy.body    = NULL;
                result->as.lazy.env     = NULL;
                result = inner;
                if (result == NULL) return NULL;
            }
        }
        gc_write_barrier(S, v, result);
        v->as.lazy.cached   = result;
        v->as.lazy.realized = 1;
        v->as.lazy.body     = NULL;
        v->as.lazy.env      = NULL;
        return result;
    }
}

mino_val_t *eval_args(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *head = mino_nil(S);
    mino_val_t *tail = NULL;
    while (mino_is_cons(args)) {
        mino_val_t *v = eval_value(S, args->as.cons.car, env);
        mino_val_t *cell;
        if (v == NULL) {
            return NULL;
        }
        gc_pin(v);
        cell = mino_cons(S, v, mino_nil(S));
        gc_unpin(1);
        if (tail == NULL) {
            head = cell;
        } else {
            tail->as.cons.cdr = cell;
        }
        tail = cell;
        args = args->as.cons.cdr;
    }
    return head;
}

