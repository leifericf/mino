/*
 * special_registry.c -- data-table dispatch for special forms.
 *
 * The evaluator's special-form recognition runs through one table.
 * Each entry pairs the cached interned-symbol pointer (looked up at
 * runtime via offsetof, since the cache lives on mino_state_t) with
 * the handler. Inline handlers for forms small enough to leave their
 * body in the registry (quote, quasiquote, if, do, recur, lazy-seq,
 * when, and, or, var) live here too.
 *
 * Pointer-eq against the cached symbol is the fast path; symbols
 * that carry metadata are fresh copies (the reader clones them
 * before attaching meta) and need a content-based fallback on the
 * symbol name.
 */

#include "eval/special_internal.h"
#include <stddef.h>

/* --- Inline-form handlers ----------------------------------------------- */

static mino_val_t *eval_quote(mino_state_t *S, mino_val_t *form,
                              mino_val_t *args, mino_env_t *env, int tail)
{
    (void)env; (void)tail;
    if (!mino_is_cons(args)) {
        set_eval_diag(S, form, "syntax", "MSY001",
                      "quote requires one argument");
        return NULL;
    }
    return args->as.cons.car;
}

static mino_val_t *eval_quasiquote(mino_state_t *S, mino_val_t *form,
                                   mino_val_t *args, mino_env_t *env,
                                   int tail)
{
    (void)tail;
    if (!mino_is_cons(args)) {
        set_eval_diag(S, form, "syntax", "MSY001",
                      "quasiquote requires one argument");
        return NULL;
    }
    return quasiquote_expand(S, args->as.cons.car, env);
}

static mino_val_t *eval_unquote_outside(mino_state_t *S, mino_val_t *form,
                                        mino_val_t *args, mino_env_t *env,
                                        int tail)
{
    (void)args; (void)env; (void)tail;
    set_eval_diag(S, form, "syntax", "MSY001",
                  "unquote outside of quasiquote");
    return NULL;
}

static mino_val_t *eval_var(mino_state_t *S, mino_val_t *form,
                            mino_val_t *args, mino_env_t *env, int tail)
{
    mino_val_t *sym_arg;
    mino_val_t *var;
    char        vbuf[256];
    size_t      vn;
    (void)tail;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_eval_diag(S, form, "syntax", "MSY001",
                      "var requires exactly one argument");
        return NULL;
    }
    sym_arg = args->as.cons.car;
    if (sym_arg->type != MINO_SYMBOL) {
        set_eval_diag(S, form, "syntax", "MSY001",
                      "var requires a symbol argument");
        return NULL;
    }
    vn = sym_arg->as.s.len;
    if (vn >= sizeof(vbuf)) {
        set_eval_diag(S, form, "syntax", "MSY001",
                      "var: symbol name too long");
        return NULL;
    }
    memcpy(vbuf, sym_arg->as.s.data, vn);
    vbuf[vn] = '\0';
    /* Try qualified (ns/name) lookup. */
    {
        const char *sl = memchr(vbuf, '/', vn);
        if (sl != NULL && vn > 1) {
            char        ns_buf[256];
            size_t      ns_len = (size_t)(sl - vbuf);
            const char *name   = sl + 1;
            mino_env_t *target;
            memcpy(ns_buf, vbuf, ns_len);
            ns_buf[ns_len] = '\0';
            var = var_find(S, ns_buf, name);
            if (var != NULL) return var;
            /* Auto-promote env binding to var so #'mino.core/inc works
             * for primitives that were never explicitly interned. */
            target = ns_env_lookup(S, ns_buf);
            if (target != NULL) {
                env_binding_t *b = env_find_here(target, name);
                if (b != NULL) {
                    var = var_intern(S, ns_buf, name);
                    if (var != NULL) {
                        var_set_root(S, var, b->val);
                        return var;
                    }
                }
            }
        }
    }
    /* Unqualified: try current ns, then "user", then scan all. */
    var = var_find(S, S->current_ns, vbuf);
    if (var == NULL) var = var_find(S, "user", vbuf);
    if (var == NULL) {
        size_t vi;
        for (vi = 0; vi < S->var_registry_len; vi++) {
            if (strcmp(S->var_registry[vi].name, vbuf) == 0) {
                var = S->var_registry[vi].var;
                break;
            }
        }
    }
    if (var != NULL) return var;
    /* No var found: check for a C primitive in the env. resolve does
     * the same auto-creation so var-on-fn tests pass for prim-backed
     * names like inc/map. */
    {
        mino_val_t *val = mino_env_get(env, vbuf);
        if (val != NULL) {
            var = var_intern(S, "mino.core", vbuf);
            var_set_root(S, var, val);
            return var;
        }
    }
    {
        char msg[300];
        snprintf(msg, sizeof(msg), "var: unbound symbol: %s", vbuf);
        set_eval_diag(S, form, "name", "MNS001", msg);
        return NULL;
    }
}

static mino_val_t *eval_if(mino_state_t *S, mino_val_t *form,
                           mino_val_t *args, mino_env_t *env, int tail)
{
    mino_val_t *cond_form;
    mino_val_t *then_form;
    mino_val_t *else_form = mino_nil(S);
    mino_val_t *cond;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_eval_diag(S, form, "syntax", "MSY001",
                      "if requires a condition and a then-branch");
        return NULL;
    }
    cond_form = args->as.cons.car;
    then_form = args->as.cons.cdr->as.cons.car;
    if (mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        else_form = args->as.cons.cdr->as.cons.cdr->as.cons.car;
    }
    cond = eval_value(S, cond_form, env);
    if (cond == NULL) {
        return NULL;
    }
    /* Branch is tail position: propagate recur/tail-call. */
    return eval_impl(S, mino_is_truthy(cond) ? then_form : else_form,
                     env, tail);
}

static mino_val_t *eval_do(mino_state_t *S, mino_val_t *form,
                           mino_val_t *args, mino_env_t *env, int tail)
{
    (void)form;
    return eval_implicit_do_impl(S, args, env, tail);
}

static mino_val_t *eval_recur(mino_state_t *S, mino_val_t *form,
                              mino_val_t *args, mino_env_t *env, int tail)
{
    mino_val_t *evaled;
    (void)form; (void)tail;
    evaled = eval_args(S, args, env);
    if (evaled == NULL && mino_last_error(S) != NULL) {
        return NULL;
    }
    S->recur_sentinel.as.recur.args = evaled;
    return &S->recur_sentinel;
}

static mino_val_t *eval_lazy_seq(mino_state_t *S, mino_val_t *form,
                                 mino_val_t *args, mino_env_t *env, int tail)
{
    mino_val_t *lz;
    (void)form; (void)tail;
    lz = alloc_val(S, MINO_LAZY);
    lz->as.lazy.body     = args;
    lz->as.lazy.env      = env;
    lz->as.lazy.cached   = NULL;
    lz->as.lazy.realized = 0;
    return lz;
}

static mino_val_t *eval_when(mino_state_t *S, mino_val_t *form,
                             mino_val_t *args, mino_env_t *env, int tail)
{
    mino_val_t *cond;
    if (!mino_is_cons(args)) {
        set_eval_diag(S, form, "syntax", "MSY001",
                      "when requires a condition");
        return NULL;
    }
    cond = eval_value(S, args->as.cons.car, env);
    if (cond == NULL) return NULL;
    if (!mino_is_truthy(cond)) return mino_nil(S);
    return eval_implicit_do_impl(S, args->as.cons.cdr, env, tail);
}

static mino_val_t *eval_and(mino_state_t *S, mino_val_t *form,
                            mino_val_t *args, mino_env_t *env, int tail)
{
    mino_val_t *result = &S->true_singleton;
    (void)form;
    while (mino_is_cons(args)) {
        mino_val_t *rest = args->as.cons.cdr;
        /* Last clause is tail position. */
        if (!mino_is_cons(rest)) {
            return eval_impl(S, args->as.cons.car, env, tail);
        }
        result = eval_value(S, args->as.cons.car, env);
        if (result == NULL) return NULL;
        if (!mino_is_truthy(result)) return result;
        args = rest;
    }
    return result;
}

static mino_val_t *eval_or(mino_state_t *S, mino_val_t *form,
                           mino_val_t *args, mino_env_t *env, int tail)
{
    mino_val_t *result = mino_nil(S);
    (void)form;
    while (mino_is_cons(args)) {
        mino_val_t *rest = args->as.cons.cdr;
        if (!mino_is_cons(rest)) {
            return eval_impl(S, args->as.cons.car, env, tail);
        }
        result = eval_value(S, args->as.cons.car, env);
        if (result == NULL) return NULL;
        if (mino_is_truthy(result)) return result;
        args = rest;
    }
    return result;
}

/* --- Registry table ----------------------------------------------------- */

/*
 * Each entry pairs the cached-symbol slot (offset into mino_state_t)
 * with the handler. The first three pairs that share a handler
 * (let/let*, fn/fn*, loop/loop*, unquote/unquote-splicing) appear
 * twice so pointer-eq resolves both spellings; the order is the
 * original cascading-if order so the dominant cases stay fast.
 */
typedef struct {
    size_t      sf_offset;
    const char *name;
    size_t      name_len;
    special_fn  fn;
} special_form_entry;

#define SF(member, lit, handler) \
    { offsetof(mino_state_t, member), (lit), sizeof(lit) - 1, (handler) }

static const special_form_entry k_special_forms[] = {
    SF(sf_quote,            "quote",            eval_quote),
    SF(sf_quasiquote,       "quasiquote",       eval_quasiquote),
    SF(sf_unquote,          "unquote",          eval_unquote_outside),
    SF(sf_unquote_splicing, "unquote-splicing", eval_unquote_outside),
    SF(sf_defmacro,         "defmacro",         eval_defmacro),
    SF(sf_declare,          "declare",          eval_declare),
    SF(sf_ns,               "ns",               eval_ns),
    SF(sf_var,              "var",              eval_var),
    SF(sf_def,              "def",              eval_def),
    SF(sf_if,               "if",               eval_if),
    SF(sf_do,               "do",               eval_do),
    SF(sf_let,              "let",              eval_let),
    SF(sf_let_star,         "let*",             eval_let),
    SF(sf_fn,               "fn",               eval_fn),
    SF(sf_fn_star,          "fn*",              eval_fn),
    SF(sf_recur,            "recur",            eval_recur),
    SF(sf_loop,             "loop",             eval_loop),
    SF(sf_loop_star,        "loop*",            eval_loop),
    SF(sf_try,              "try",              eval_try),
    SF(sf_binding,          "binding",          eval_binding),
    SF(sf_lazy_seq,         "lazy-seq",         eval_lazy_seq),
    SF(sf_when,             "when",             eval_when),
    SF(sf_and,              "and",              eval_and),
    SF(sf_or,               "or",               eval_or),
};

static const size_t k_special_forms_count =
    sizeof(k_special_forms) / sizeof(k_special_forms[0]);

#undef SF

static mino_val_t *cached_at(mino_state_t *S, size_t off)
{
    return *(mino_val_t **)((char *)S + off);
}

int eval_try_special_form(mino_state_t *S, mino_val_t *form,
                          mino_val_t *head, mino_val_t *args,
                          mino_env_t *env, int tail,
                          mino_val_t **out)
{
    size_t i;
    if (head == NULL || head->type != MINO_SYMBOL) {
        return 0;
    }
    for (i = 0; i < k_special_forms_count; i++) {
        const special_form_entry *e = &k_special_forms[i];
        mino_val_t *cached = cached_at(S, e->sf_offset);
        if (head == cached
            || (head->meta != NULL
                && head->as.s.len == e->name_len
                && memcmp(head->as.s.data, e->name, e->name_len) == 0)) {
            *out = e->fn(S, form, args, env, tail);
            return 1;
        }
    }
    return 0;
}
