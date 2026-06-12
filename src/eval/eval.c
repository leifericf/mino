/*
 * mino.c -- evaluator core helpers: sym_eq, macroexpand, quasiquote,
 *           eval_value, eval_implicit_do, lazy_force, eval_args.
 */

#include "runtime/internal.h"

#if defined(_WIN32) && defined(_MSC_VER)
#  include <windows.h>  /* Sleep() for the lazy-realize spin yield */
#else
#  include <time.h>     /* nanosleep() for the lazy-realize spin yield */
#endif

int sym_eq(const mino_val *v, const char *s)
{
    size_t n;
    if (v == NULL || mino_type_of(v) != MINO_SYMBOL) {
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
mino_val *macroexpand1(mino_state *S, mino_val *form, mino_env *env,
                         int *expanded)
{
    char        buf[256];
    size_t      n;
    mino_val *head;
    mino_val *mac;
    *expanded = 0;
    if (!mino_is_cons(form)) {
        return form;
    }
    head = form->as.cons.car;
    if (head == NULL || mino_type_of(head) != MINO_SYMBOL) {
        return form;
    }
    n = head->as.s.len;
    if (n >= sizeof(buf)) {
        return form;
    }
    memcpy(buf, head->as.s.data, n);
    buf[n] = '\0';
    mac = mino_env_get(env, buf);
    if (mac == NULL) {
        mino_env *ns_env = current_ns_env(S);
        if (ns_env != NULL) mac = mino_env_get(ns_env, buf);
    }
    if (mac == NULL && S->ns_vars.fn_ambient_ns != NULL
        && S->ns_vars.fn_ambient_ns != S->ns_vars.current_ns
        && (S->ns_vars.current_ns == NULL
            || strcmp(S->ns_vars.fn_ambient_ns, S->ns_vars.current_ns) != 0)) {
        mino_env *amb = ns_env_lookup(S, S->ns_vars.fn_ambient_ns);
        if (amb != NULL) mac = mino_env_get(amb, buf);
    }
    /* A namespace env entry from :refer may be a var wrapping the macro.
     * Unwrap one level so macros referred via require/use are recognized. */
    if (mac != NULL && mino_type_of(mac) == MINO_VAR && mac->as.var.bound) {
        mac = mac->as.var.root;
    }
    if (mac == NULL || mino_type_of(mac) != MINO_MACRO) {
        return form;
    }
    *expanded = 1;
    return apply_callable(S, mac, form->as.cons.cdr, env);
}

/* Expand repeatedly until `form` is no longer a macro call at the top. */
mino_val *macroexpand_all(mino_state *S, mino_val *form, mino_env *env)
{
    for (;;) {
        int         expanded = 0;
        mino_val *next     = macroexpand1(S, form, env, &expanded);
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
 * Walk lexical-frame chain stopping at the first ns env, returning 1
 * if `name` is bound there. Used by syntax-quote auto-qualification to
 * leave macro-local symbols (let/fn args) bare.
 */
static int qq_locally_bound(mino_state *S, mino_env *env, const char *name)
{
    mino_env *e;
    for (e = env; e != NULL; e = e->parent) {
        size_t i;
        for (i = 0; i < S->ns_vars.ns_env_len; i++) {
            if (S->ns_vars.ns_env_table[i].env == e) return 0; /* hit ns boundary */
        }
        if (env_find_here(e, name) != NULL) return 1;
    }
    return 0;
}

/*
 * Pick the namespace context that syntax-quote should consult when
 * qualifying a bare symbol. Inside a macro expansion, that is the
 * macro's defining ns (carried in fn_ambient_ns by apply_callable),
 * not the consumer's current_ns -- otherwise a syntax-quoted bare
 * `atom` inside a clojure.core macro qualifies to the consumer's ns
 * whenever the consumer pulled `atom` in via :refer :all. Macro
 * context is exactly when fn_ambient_ns is set and differs from
 * current_ns; for fn bodies, apply_callable sets them to the same
 * value, so this falls through to the current_ns path.
 */
static void qq_qualifying_ns(mino_state *S, const char **ns_name_out,
                             mino_env **ns_env_out)
{
    if (S->ns_vars.fn_ambient_ns != NULL
        && S->ns_vars.fn_ambient_ns != S->ns_vars.current_ns
        && (S->ns_vars.current_ns == NULL
            || strcmp(S->ns_vars.fn_ambient_ns, S->ns_vars.current_ns) != 0)) {
        *ns_name_out = S->ns_vars.fn_ambient_ns;
        *ns_env_out  = ns_env_lookup(S, S->ns_vars.fn_ambient_ns);
        return;
    }
    *ns_name_out = S->ns_vars.current_ns;
    *ns_env_out  = current_ns_env(S);
}

/*
 * Auto-qualify a bare symbol against the current namespace's chain:
 * find the namespace whose env owns the binding (env_find_here), and
 * return ns/name. Symbols not found in any ns env stay bare so special
 * forms (try, catch, &) and gensym names pass through unchanged.
 */
static mino_val *qq_qualify_symbol(mino_state *S, mino_val *sym,
                                     mino_env *env)
{
    const char *name = sym->as.s.data;
    size_t      nlen = sym->as.s.len;
    mino_env *e;
    const char *slash;
    const char *qns_name;
    mino_env *qns_env;
    if (nlen == 0) return sym;
    if (nlen == 1 && name[0] == '/') return sym;
    qq_qualifying_ns(S, &qns_name, &qns_env);
    /* For namespaced symbols, expand a leading alias to its target ns
     * (matching Clojure syntax-quote). Bare ns/name passes through. */
    slash = memchr(name, '/', nlen);
    if (slash != NULL) {
        size_t prefix_len = (size_t)(slash - name);
        size_t suffix_len = nlen - prefix_len - 1;
        char   prefix_buf[256];
        size_t i;
        const char *cur = qns_name != NULL ? qns_name : "user";
        if (prefix_len == 0 || prefix_len >= sizeof(prefix_buf)) return sym;
        memcpy(prefix_buf, name, prefix_len);
        prefix_buf[prefix_len] = '\0';
        for (i = 0; i < S->ns_vars.ns_alias_len; i++) {
            if (S->ns_vars.ns_aliases[i].owning_ns == NULL
                || strcmp(S->ns_vars.ns_aliases[i].owning_ns, cur) != 0) continue;
            if (strcmp(S->ns_vars.ns_aliases[i].alias, prefix_buf) == 0) {
                const char *full   = S->ns_vars.ns_aliases[i].full_name;
                size_t      flen   = strlen(full);
                char        buf[512];
                if (flen + 1 + suffix_len + 1 > sizeof(buf)) return sym;
                memcpy(buf, full, flen);
                buf[flen] = '/';
                memcpy(buf + flen + 1, slash + 1, suffix_len);
                buf[flen + 1 + suffix_len] = '\0';
                return mino_symbol_n(S, buf, flen + 1 + suffix_len);
            }
        }
        return sym;
    }
    if (qq_locally_bound(S, env, name)) return sym;
    if (qns_name == NULL) return sym;
    for (e = qns_env; e != NULL; e = e->parent) {
        env_binding_t *b = env_find_here(e, name);
        if (b != NULL) {
            size_t      i;
            const char *nsn  = NULL;
            const char *bname = name;
            size_t      bnlen = nlen;
            /* When the binding is a var, the var carries its source ns
             * and original name -- use those so refer'd entries are
             * qualified back to where they live, matching the Clojure
             * syntax-quote contract. */
            if (b->val != NULL && mino_type_of(b->val) == MINO_VAR) {
                nsn   = b->val->as.var.ns;
                bname = b->val->as.var.sym;
                bnlen = bname != NULL ? strlen(bname) : 0;
            }
            if (nsn == NULL) {
                for (i = 0; i < S->ns_vars.ns_env_len; i++) {
                    if (S->ns_vars.ns_env_table[i].env == e) {
                        nsn = S->ns_vars.ns_env_table[i].name;
                        break;
                    }
                }
            }
            if (nsn != NULL) {
                size_t cnlen = strlen(nsn);
                char   buf[512];
                if (cnlen + 1 + bnlen + 1 > sizeof(buf)) return sym;
                memcpy(buf, nsn, cnlen);
                buf[cnlen] = '/';
                memcpy(buf + cnlen + 1, bname, bnlen);
                buf[cnlen + 1 + bnlen] = '\0';
                return mino_symbol_n(S, buf, cnlen + 1 + bnlen);
            }
            return sym; /* env without ns name (shouldn't happen) */
        }
    }
    return sym;
}

/* qq_expand_vector -- expand a vector template. The fast path (no ~@
 * present) keeps the same length; the slow path builds a cons list and
 * converts at the end so splicing can grow or shrink the result. */
static mino_val *qq_expand_vector(mino_state *S, mino_val *form,
                                    mino_env *env)
{
    size_t       nn  = form->as.vec.len;
    size_t       i;
    int          has_splice = 0;
    if (nn == 0) { return form; }
    /* Fast path: no ~@ means fixed-size output. */
    for (i = 0; i < nn; i++) {
        mino_val *e = vec_nth(form, i);
        if (mino_is_cons(e)
            && sym_eq(e->as.cons.car, "unquote-splicing")) {
            has_splice = 1; break;
        }
    }
    if (!has_splice) {
        mino_val **tmp = (mino_val **)gc_alloc_typed(S,
            GC_T_VALARR, nn * sizeof(*tmp));
        for (i = 0; i < nn; i++) {
            mino_val *e = quasiquote_expand(S, vec_nth(form, i), env);
            if (e == NULL) { return NULL; }
            gc_valarr_set(S, tmp, i, e);
        }
        return mino_vector(S, tmp, nn);
    }
    /* Slow path: ~@ present, build cons list then convert. */
    {
        mino_val  *out  = mino_nil(S);
        mino_val  *tail = NULL;
        size_t       count = 0;
        for (i = 0; i < nn; i++) {
            mino_val *elem = vec_nth(form, i);
            if (mino_is_cons(elem)
                && sym_eq(elem->as.cons.car, "unquote-splicing")) {
                mino_val *arg = elem->as.cons.cdr;
                mino_val *spliced;
                if (!mino_is_cons(arg)) {
                    set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "syntax",
                        "MSY001", "unquote-splicing requires one argument");
                    return NULL;
                }
                spliced = eval_value(S, arg->as.cons.car, env);
                if (spliced == NULL) { return NULL; }
                if (mino_type_of(spliced) == MINO_VECTOR) {
                    size_t j;
                    for (j = 0; j < spliced->as.vec.len; j++) {
                        mino_val *cell = mino_cons(S,
                            vec_nth(spliced, j), mino_nil(S));
                        if (tail == NULL) { out = cell; }
                        else { mino_cons_cdr_set(S, tail, cell); }
                        tail = cell;
                        count++;
                    }
                } else {
                    mino_val *sp = spliced;
                    while (sp != NULL && mino_type_of(sp) == MINO_LAZY) {
                        sp = lazy_force(S, sp);
                        if (sp == NULL) return NULL;
                    }
                    while (sp != NULL && mino_type_of(sp) != MINO_NIL
                           && mino_type_of(sp) != MINO_EMPTY_LIST) {
                        if (mino_type_of(sp) == MINO_CONS) {
                            mino_val *cell = mino_cons(S,
                                sp->as.cons.car, mino_nil(S));
                            if (tail == NULL) { out = cell; }
                            else { mino_cons_cdr_set(S, tail, cell); }
                            tail = cell;
                            count++;
                            sp = sp->as.cons.cdr;
                        } else if (mino_type_of(sp) == MINO_CHUNKED_CONS) {
                            const mino_val *ch = sp->as.chunked_cons.chunk;
                            unsigned k;
                            for (k = sp->as.chunked_cons.off;
                                 k < ch->as.chunk.len; k++) {
                                mino_val *cell = mino_cons(S,
                                    ch->as.chunk.vals[k], mino_nil(S));
                                if (tail == NULL) { out = cell; }
                                else { mino_cons_cdr_set(S, tail, cell); }
                                tail = cell;
                                count++;
                            }
                            sp = sp->as.chunked_cons.more;
                        } else {
                            break;
                        }
                        while (sp != NULL && mino_type_of(sp) == MINO_LAZY) {
                            sp = lazy_force(S, sp);
                            if (sp == NULL) return NULL;
                        }
                    }
                }
            } else {
                mino_val *expanded = quasiquote_expand(S, elem, env);
                mino_val *cell;
                if (expanded == NULL) { return NULL; }
                cell = mino_cons(S, expanded, mino_nil(S));
                if (tail == NULL) { out = cell; }
                else { mino_cons_cdr_set(S, tail, cell); }
                tail = cell;
                count++;
            }
        }
        {
            mino_val **tmp = (mino_val **)gc_alloc_typed(S,
                GC_T_VALARR, count * sizeof(*tmp));
            mino_val  *p   = out;
            size_t       idx = 0;
            while (mino_is_cons(p)) {
                tmp[idx++] = p->as.cons.car;
                p = p->as.cons.cdr;
            }
            return mino_vector(S, tmp, count);
        }
    }
}

/* qq_expand_map -- expand both keys and values; the result keeps the
 * map shape with possibly-different key/value identities. */
static mino_val *qq_expand_map(mino_state *S, mino_val *form,
                                 mino_env *env)
{
    size_t       nn = form->as.map.len;
    mino_val **ks;
    mino_val **vs;
    size_t       i;
    if (nn == 0) { return form; }
    ks = (mino_val **)gc_alloc_typed(S, GC_T_VALARR, nn * sizeof(*ks));
    vs = (mino_val **)gc_alloc_typed(S, GC_T_VALARR, nn * sizeof(*vs));
    for (i = 0; i < nn; i++) {
        mino_val *key = vec_nth(form->as.map.key_order, i);
        mino_val *val = map_get_val(form, key);
        mino_val *kk  = quasiquote_expand(S, key, env);
        mino_val *vv;
        if (kk == NULL) { return NULL; }
        vv = quasiquote_expand(S, val, env);
        if (vv == NULL) { return NULL; }
        gc_valarr_set(S, ks, i, kk);
        gc_valarr_set(S, vs, i, vv);
    }
    return mino_map(S, ks, vs, nn);
}

/* qq_expand_cons -- expand a cons-list template. Handles the (unquote
 * x) and (unquote-splicing x) special heads at top level, and walks
 * each child detecting unquote-splicing per-element. */
static mino_val *qq_expand_cons(mino_state *S, mino_val *form,
                                  mino_env *env)
{
    mino_val *head = form->as.cons.car;
    if (sym_eq(head, "unquote")) {
        mino_val *arg = form->as.cons.cdr;
        if (!mino_is_cons(arg)) {
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "syntax", "MSY001", "unquote requires one argument");
            return NULL;
        }
        return eval_value(S, arg->as.cons.car, env);
    }
    if (sym_eq(head, "unquote-splicing")) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "syntax", "MSY001", "unquote-splicing must appear inside a list");
        return NULL;
    }
    {
        mino_val *out  = mino_nil(S);
        mino_val *tail = NULL;
        mino_val *p    = form;
        while (mino_is_cons(p)) {
            mino_val *elem = p->as.cons.car;
            if (mino_is_cons(elem)
                && sym_eq(elem->as.cons.car, "unquote-splicing")) {
                mino_val *arg = elem->as.cons.cdr;
                mino_val *spliced;
                mino_val *sp;
                if (!mino_is_cons(arg)) {
                    set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "syntax", "MSY001", "unquote-splicing requires one argument");
                    return NULL;
                }
                spliced = eval_value(S, arg->as.cons.car, env);
                if (spliced == NULL) { return NULL; }
                sp = spliced;
                if (sp != NULL && mino_type_of(sp) == MINO_VECTOR) {
                    size_t j;
                    for (j = 0; j < sp->as.vec.len; j++) {
                        mino_val *cell = mino_cons(S,
                            vec_nth(sp, j), mino_nil(S));
                        if (tail == NULL) { out = cell; }
                        else { mino_cons_cdr_set(S, tail, cell); }
                        tail = cell;
                    }
                } else {
                    while (sp != NULL && mino_type_of(sp) == MINO_LAZY) {
                        sp = lazy_force(S, sp);
                        if (sp == NULL) return NULL;
                    }
                    while (sp != NULL && mino_type_of(sp) != MINO_NIL
                           && mino_type_of(sp) != MINO_EMPTY_LIST) {
                        if (mino_type_of(sp) == MINO_CONS) {
                            mino_val *cell = mino_cons(S,
                                sp->as.cons.car, mino_nil(S));
                            if (tail == NULL) { out = cell; }
                            else { mino_cons_cdr_set(S, tail, cell); }
                            tail = cell;
                            sp = sp->as.cons.cdr;
                        } else if (mino_type_of(sp) == MINO_CHUNKED_CONS) {
                            const mino_val *ch = sp->as.chunked_cons.chunk;
                            unsigned k;
                            for (k = sp->as.chunked_cons.off;
                                 k < ch->as.chunk.len; k++) {
                                mino_val *cell = mino_cons(S,
                                    ch->as.chunk.vals[k], mino_nil(S));
                                if (tail == NULL) { out = cell; }
                                else { mino_cons_cdr_set(S, tail, cell); }
                                tail = cell;
                            }
                            sp = sp->as.chunked_cons.more;
                        } else {
                            break;
                        }
                        while (sp != NULL && mino_type_of(sp) == MINO_LAZY) {
                            sp = lazy_force(S, sp);
                            if (sp == NULL) return NULL;
                        }
                    }
                }
            } else {
                mino_val *expanded = quasiquote_expand(S, elem, env);
                mino_val *cell;
                if (expanded == NULL) { return NULL; }
                cell = mino_cons(S, expanded, mino_nil(S));
                if (tail == NULL) { out = cell; } else { mino_cons_cdr_set(S, tail, cell); }
                tail = cell;
            }
            p = p->as.cons.cdr;
        }
        return out;
    }
}

/*
 * quasiquote_expand: walk `form` as a template. Sublists, subvectors, and
 * submaps are recursed into; (unquote x) evaluates x and uses its value;
 * (unquote-splicing x) evaluates x (expected to yield a list) and splices
 * its elements into the enclosing list. Bare symbols are auto-qualified
 * to the namespace owning the binding (when one exists), matching the
 * Clojure backquote contract.
 */
mino_val *quasiquote_expand(mino_state *S, mino_val *form,
                              mino_env *env)
{
    if (form == NULL) { return form; }
    if (mino_type_of(form) == MINO_SYMBOL) return qq_qualify_symbol(S, form, env);
    if (mino_type_of(form) == MINO_VECTOR) return qq_expand_vector(S, form, env);
    if (mino_type_of(form) == MINO_MAP)    return qq_expand_map(S, form, env);
    if (!mino_is_cons(form))       return form;
    return qq_expand_cons(S, form, env);
}

/*
 * Evaluate `form` for its value. Any MINO_RECUR escaping here is a
 * non-tail recur and is rejected. Use plain eval(S, ) in positions where
 * a recur is legitimately in tail position (if branches, implicit-do
 * trailing expression, fn/loop body through the trampoline).
 */
mino_val *eval_value(mino_state *S, mino_val *form, mino_env *env)
{
    mino_val *v = eval(S, form, env);
    if (v == NULL) {
        return NULL;
    }
    if (mino_type_of(v) == MINO_RECUR) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "syntax", "MSY001", "recur must be in tail position");
        return NULL;
    }
    if (mino_type_of(v) == MINO_TAIL_CALL) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "syntax", "MSY001", "tail call in non-tail position");
        return NULL;
    }
    return v;
}

mino_val *eval_implicit_do_impl(mino_state *S, mino_val *body,
                                  mino_env *env, int tail)
{
    /* Force a lazy body, which apply/concat-built call forms leave
     * dangling. The cdr forcing in the loop handles intermediate
     * lazy cells in the spine. */
    while (body != NULL && mino_type_of(body) == MINO_LAZY) {
        body = lazy_force(S, body);
        if (body == NULL) return NULL;
    }
    if (!mino_is_cons(body)) {
        return mino_nil(S);
    }
    for (;;) {
        mino_val *rest = body->as.cons.cdr;
        while (rest != NULL && mino_type_of(rest) == MINO_LAZY) {
            rest = lazy_force(S, rest);
            if (rest == NULL) return NULL;
        }
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

mino_val *eval_implicit_do(mino_state *S, mino_val *body, mino_env *env)
{
    return eval_implicit_do_impl(S, body, env, 0);
}

/*
 * Realize one lazy. Concurrent forcers race on a CAS of `realized`
 * from LAZY_UNREALIZED to LAZY_REALIZING; the CAS winner runs the
 * thunk, publishes `cached`, then flips `realized` to LAZY_REALIZED.
 * Losers spin (yielding state_lock so the realizer can make progress
 * if its thunk itself blocks) until they observe LAZY_REALIZED. If
 * the thunk throws (result == NULL) the realizer reverts `realized`
 * back to LAZY_UNREALIZED so a retry can re-run the thunk -- this
 * matches JVM clojure.lang.LazySeq#sval(), which throws out the
 * cached state on exception and retries on next force.
 */
/* Record V as claimed (LAZY_REALIZING) by this thread so a throw that
 * longjmps out of the thunk can roll the claim back at the try-frame
 * landing pad (mino_lazy_inflight_unwind). Returns 0 on success, -1 if
 * the tracking array cannot grow. */
static int lazy_inflight_push(mino_state *S, mino_val *v)
{
    mino_thread_ctx_t *ctx = mino_current_ctx(S);
    if (ctx->lazy_inflight_len == ctx->lazy_inflight_cap) {
        size_t      ncap = ctx->lazy_inflight_cap == 0
            ? 16 : ctx->lazy_inflight_cap * 2;
        mino_val **na  = realloc(ctx->lazy_inflight,
                                   ncap * sizeof(*na));
        if (na == NULL) {
            return -1;
        }
        ctx->lazy_inflight     = na;
        ctx->lazy_inflight_cap = ncap;
    }
    ctx->lazy_inflight[ctx->lazy_inflight_len++] = v;
    return 0;
}

static void lazy_inflight_pop(mino_state *S)
{
    mino_current_ctx(S)->lazy_inflight_len--;
}

static mino_val *lazy_realize(mino_state *S, mino_val *v)
{
    for (;;) {
        int state = __atomic_load_n(&v->as.lazy.realized, __ATOMIC_ACQUIRE);
        if (state == LAZY_REALIZED) {
            return v->as.lazy.cached;
        }
        if (state == LAZY_UNREALIZED) {
            int expected = LAZY_UNREALIZED;
            if (__atomic_compare_exchange_n(&v->as.lazy.realized,
                                            &expected, LAZY_REALIZING,
                                            0,
                                            __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE)) {
                /* Track the claim so a throw that longjmps out of the
                 * thunk (try_depth > 0 routes raises through longjmp,
                 * skipping the result==NULL rollback below) gets the
                 * cell flipped back to UNREALIZED at the landing pad
                 * instead of stranding it REALIZING. */
                if (lazy_inflight_push(S, v) != 0) {
                    __atomic_store_n(&v->as.lazy.realized,
                                     LAZY_UNREALIZED, __ATOMIC_RELEASE);
                    set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                                  "limit", "MLM003",
                                  "out of memory tracking lazy realization");
                    return NULL;
                }
                /* Install the captured defining ns for the duration
                 * of the body evaluation so unqualified ns-level
                 * symbols (e.g. helper fns defined in the library
                 * that wrote the lazy-seq) resolve against the
                 * library's namespace rather than the realizer's
                 * current_ns. Mirrors the save/restore dance in the
                 * fn dispatch path (src/eval/fn.c). c_thunk lazies
                 * skip this -- their body is C and does not consult
                 * the namespace tables. */
                const char *saved_ns      = S->ns_vars.current_ns;
                const char *saved_ambient = S->ns_vars.fn_ambient_ns;
                if (v->as.lazy.c_thunk == NULL
                    && v->as.lazy.defining_ns != NULL) {
                    S->ns_vars.current_ns    = v->as.lazy.defining_ns;
                    S->ns_vars.fn_ambient_ns = v->as.lazy.defining_ns;
                }
                mino_val *result = v->as.lazy.c_thunk != NULL
                    ? v->as.lazy.c_thunk(S, v->as.lazy.body)
                    : eval_implicit_do(S, v->as.lazy.body, v->as.lazy.env);
                S->ns_vars.current_ns    = saved_ns;
                S->ns_vars.fn_ambient_ns = saved_ambient;
                if (result == NULL) {
                    /* Thunk threw. Roll back the claim so a retrying
                     * caller can re-run the thunk; if other threads
                     * are spinning, they'll see UNREALIZED and one of
                     * them will try the CAS. */
                    lazy_inflight_pop(S);
                    __atomic_store_n(&v->as.lazy.realized,
                                     LAZY_UNREALIZED, __ATOMIC_RELEASE);
                    return NULL;
                }
                /* Per the canonical lazy-seq contract, forcing coerces
                 * the thunk's value through seq, so a body that yields
                 * a vector / map / set / string behaves like its seq.
                 * Seq-shaped results (cons, chunked-cons), the empty
                 * markers, and nested lazies pass through: lazy_force
                 * unwraps nesting and each inner realize coerces its
                 * own value. A non-seqable result rethrows seq's type
                 * error, with the claim rolled back like a thunk
                 * throw. */
                {
                    int rt = mino_type_of(result);
                    if (rt != MINO_NIL && rt != MINO_EMPTY_LIST
                        && rt != MINO_CONS && rt != MINO_CHUNKED_CONS
                        && rt != MINO_LAZY) {
                        /* Pin: result's only reference is this frame,
                         * and mino_seq allocates before reading it. */
                        gc_pin(result);
                        result = mino_seq(S, result);
                        gc_unpin(1);
                        if (result == NULL) {
                            lazy_inflight_pop(S);
                            __atomic_store_n(&v->as.lazy.realized,
                                             LAZY_UNREALIZED,
                                             __ATOMIC_RELEASE);
                            return NULL;
                        }
                    }
                }
                /* Pre-realized lazy: cached is NULL, body/env hold
                 * the thunk. Realisation overwrites three slots. The
                 * body and env stores need SATB because the thunk
                 * they pointed to may otherwise be reachable only
                 * through this lazy and must survive the in-flight
                 * major cycle. The realized flip uses release
                 * ordering so a reader that observes LAZY_REALIZED
                 * also observes the cached/body/env writes. */
                gc_write_barrier(S, v, v->as.lazy.cached, result);
                v->as.lazy.cached = result;
                gc_write_barrier(S, v, v->as.lazy.body, NULL);
                v->as.lazy.body = NULL;
                gc_write_barrier(S, v, v->as.lazy.env, NULL);
                v->as.lazy.env = NULL;
                __atomic_store_n(&v->as.lazy.realized,
                                 LAZY_REALIZED, __ATOMIC_RELEASE);
                lazy_inflight_pop(S);
                return result;
            }
            /* CAS lost; expected now holds the observed state. Loop
             * back to re-check (state may be REALIZING, REALIZED, or
             * UNREALIZED again if a previous realizer threw). */
            continue;
        }
        /* state == LAZY_REALIZING: another thread is computing. Drop
         * state_lock so that thread (or its sub-evaluations) can make
         * progress; sleep a short slice so the OS scheduler actually
         * runs the realizer on a non-fair mutex impl before we
         * re-acquire and re-check. Matches the BC safepoint
         * auto-yield idiom in src/runtime/state.c. */
        {
            int depth = mino_yield_lock(S);
#if defined(_WIN32) && defined(_MSC_VER)
            Sleep(0);
#else
            {
                struct timespec ts;
                ts.tv_sec  = 0;
                ts.tv_nsec = 100000L; /* 100us */
                nanosleep(&ts, NULL);
            }
#endif
            mino_resume_lock(S, depth);
        }
    }
}

/*
 * Force a lazy sequence: evaluate the body in the captured environment,
 * cache the result, and release the thunk for GC. Iteratively unwraps
 * nested lazy seqs to avoid stack overflow.
 */
mino_val *lazy_force(mino_state *S, mino_val *v)
{
    mino_val *result = lazy_realize(S, v);
    mino_val *first  = result;
    while (result != NULL && mino_type_of(result) == MINO_LAZY) {
        result = lazy_realize(S, result);
    }
    /* Path-compress v.cached to the final non-lazy. Both v.cached
     * (== first) and `result` walk to the same value; readers that
     * observe either form get an equivalent traversal, so this is
     * race-free even though the store happens after the LAZY_REALIZED
     * flip. The barrier is required because v may be OLD and result
     * may be YOUNG; cached == first is already covered by the
     * realizer's SATB push, so we only need a fresh barrier when the
     * pointer actually changes. */
    if (result != first && result != NULL) {
        gc_write_barrier(S, v, v->as.lazy.cached, result);
        v->as.lazy.cached = result;
    }
    return result;
}

mino_val *eval_args(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *head = mino_nil(S);
    mino_val *tail = NULL;
    while (mino_is_cons(args)) {
        mino_val *v = eval_value(S, args->as.cons.car, env);
        mino_val *cell;
        if (v == NULL) {
            return NULL;
        }
        gc_pin(v);
        cell = mino_cons(S, v, mino_nil(S));
        gc_unpin(1);
        if (tail == NULL) {
            head = cell;
        } else {
            mino_cons_cdr_set(S, tail, cell);
        }
        tail = cell;
        args = args->as.cons.cdr;
    }
    return head;
}

