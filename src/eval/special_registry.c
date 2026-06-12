/*
 * special_registry.c -- data-table dispatch for special forms.
 *
 * The evaluator's special-form recognition runs through one table.
 * Each entry pairs the cached interned-symbol pointer (looked up at
 * runtime via offsetof, since the cache lives on mino_state) with
 * the handler. Inline handlers for forms small enough to leave their
 * body in the registry (quote, quasiquote, unquote, unquote-splicing,
 * if, do, recur, lazy-seq, when, and, or, var) live here too.
 *
 * Pointer-eq against the cached symbol is the fast path; symbols
 * that carry metadata are fresh copies (the reader clones them
 * before attaching meta) and need a content-based fallback on the
 * symbol name.
 */

#include "eval/special_internal.h"
#include <stddef.h>

/* --- Inline-form handlers ----------------------------------------------- */

static mino_val *eval_quote(mino_state *S, mino_val *form,
                              mino_val *args, mino_env *env, int tail)
{
    (void)env; (void)tail;
    if (!mino_is_cons(args)) {
        set_eval_diag(S, form, "syntax", "MSY001",
                      "quote requires one argument");
        return NULL;
    }
    return args->as.cons.car;
}

static mino_val *eval_quasiquote(mino_state *S, mino_val *form,
                                   mino_val *args, mino_env *env,
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

static mino_val *eval_unquote_outside(mino_state *S, mino_val *form,
                                        mino_val *args, mino_env *env,
                                        int tail)
{
    (void)args; (void)env; (void)tail;
    set_eval_diag(S, form, "syntax", "MSY001",
                  "unquote outside of quasiquote");
    return NULL;
}

/* Look up a qualified ns/name symbol (e.g. clojure.core/inc) as a var.
 * Returns the var if found or successfully auto-promoted, else NULL.
 * vbuf must be the full NUL-terminated symbol text; vn is its length. */
static mino_val *var_find_qualified_sym(mino_state *S,
                                         const char *vbuf, size_t vn)
{
    const char *sl = memchr(vbuf, '/', vn);
    char        ns_buf[256];
    size_t      ns_len;
    const char *name;
    mino_val   *var;
    mino_env   *target;

    if (sl == NULL || vn <= 1) return NULL;
    ns_len = (size_t)(sl - vbuf);
    name   = sl + 1;
    memcpy(ns_buf, vbuf, ns_len);
    ns_buf[ns_len] = '\0';
    var = var_find(S, ns_buf, name);
    if (var != NULL) return var;
    /* Auto-promote env binding to var so #'clojure.core/inc works
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
    return NULL;
}

/* Intern a var for a C primitive bound under vbuf in env or the current ns.
 * Returns the new var if found, else NULL. */
static mino_val *var_promote_prim_to_var(mino_state *S,
                                          mino_env *env, const char *vbuf)
{
    mino_val *val = mino_env_get(env, vbuf);
    mino_val *var;

    if (val == NULL) {
        mino_env *ns_env = current_ns_env(S);
        if (ns_env != NULL) val = mino_env_get(ns_env, vbuf);
    }
    if (val == NULL) return NULL;
    var = var_intern(S, "clojure.core", vbuf);
    var_set_root(S, var, val);
    return var;
}

static mino_val *eval_var(mino_state *S, mino_val *form,
                            mino_val *args, mino_env *env, int tail)
{
    mino_val *sym_arg;
    mino_val *var;
    char      vbuf[256];
    size_t    vn;
    size_t    vi;
    char      msg[300];
    (void)tail;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_eval_diag(S, form, "syntax", "MSY001",
                      "var requires exactly one argument");
        return NULL;
    }
    sym_arg = args->as.cons.car;
    if (mino_type_of(sym_arg) != MINO_SYMBOL) {
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
    /* Try qualified (ns/name) lookup first. */
    var = var_find_qualified_sym(S, vbuf, vn);
    if (var != NULL) return var;
    /* Unqualified: try current ns, then "user", then scan all. */
    var = var_find(S, S->ns_vars.current_ns, vbuf);
    if (var == NULL) var = var_find(S, "user", vbuf);
    if (var == NULL) {
        for (vi = 0; vi < S->ns_vars.var_registry_len; vi++) {
            if (strcmp(S->ns_vars.var_registry[vi].name, vbuf) == 0) {
                var = S->ns_vars.var_registry[vi].var;
                break;
            }
        }
    }
    if (var != NULL) return var;
    /* No var found: check for a C primitive in the env or in the
     * current ns chain so #'inc works for prim-backed names. The
     * embedder env no longer chains to clojure.core, so we fall through
     * to current_ns_env (which does) to mirror eval_symbol's lookup. */
    var = var_promote_prim_to_var(S, env, vbuf);
    if (var != NULL) return var;
    snprintf(msg, sizeof(msg), "var: unbound symbol: %s", vbuf);
    set_eval_diag(S, form, "name", "MNS001", msg);
    return NULL;
}

static mino_val *eval_if(mino_state *S, mino_val *form,
                           mino_val *args, mino_env *env, int tail)
{
    mino_val *cond_form;
    mino_val *then_form;
    mino_val *else_form = mino_nil(S);
    mino_val *cond;
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
    return eval_impl(S, mino_is_truthy_inline(cond) ? then_form : else_form,
                     env, tail);
}

static mino_val *eval_do(mino_state *S, mino_val *form,
                           mino_val *args, mino_env *env, int tail)
{
    (void)form;
    return eval_implicit_do_impl(S, args, env, tail);
}

static mino_val *eval_recur(mino_state *S, mino_val *form,
                              mino_val *args, mino_env *env, int tail)
{
    mino_val *evaled;
    (void)form; (void)tail;
    evaled = eval_args(S, args, env);
    if (evaled == NULL && mino_last_error(S) != NULL) {
        return NULL;
    }
    S->recur_sentinel.as.recur.args = evaled;
    return &S->recur_sentinel;
}

static mino_val *eval_lazy_seq(mino_state *S, mino_val *form,
                                 mino_val *args, mino_env *env, int tail)
{
    mino_val *lz;
    (void)form; (void)tail;
    lz = alloc_val(S, MINO_LAZY);
    lz->as.lazy.body     = args;
    lz->as.lazy.env      = env;
    lz->as.lazy.cached   = NULL;
    /* Snapshot the defining ns so the body's unqualified symbols
     * resolve in the lexically enclosing namespace at realization
     * time. Prefer fn_ambient_ns (set by apply_callable when the
     * enclosing fn dispatches its body) over current_ns (which a
     * concurrent in-ns might have shifted); fall back to current_ns
     * for top-level lazy-seq forms outside any fn. Either pointer is
     * interned, so no new GC root is required. */
    lz->as.lazy.defining_ns = S->ns_vars.fn_ambient_ns != NULL
                              ? S->ns_vars.fn_ambient_ns
                              : S->ns_vars.current_ns;
    lz->as.lazy.realized = LAZY_UNREALIZED;
    return lz;
}

static mino_val *eval_when(mino_state *S, mino_val *form,
                             mino_val *args, mino_env *env, int tail)
{
    mino_val *cond;
    if (!mino_is_cons(args)) {
        set_eval_diag(S, form, "syntax", "MSY001",
                      "when requires a condition");
        return NULL;
    }
    cond = eval_value(S, args->as.cons.car, env);
    if (cond == NULL) return NULL;
    if (!mino_is_truthy_inline(cond)) return mino_nil(S);
    return eval_implicit_do_impl(S, args->as.cons.cdr, env, tail);
}

static mino_val *eval_and(mino_state *S, mino_val *form,
                            mino_val *args, mino_env *env, int tail)
{
    mino_val *result = mino_true(S);
    (void)form;
    while (mino_is_cons(args)) {
        mino_val *rest = args->as.cons.cdr;
        /* Last clause is tail position. */
        if (!mino_is_cons(rest)) {
            return eval_impl(S, args->as.cons.car, env, tail);
        }
        result = eval_value(S, args->as.cons.car, env);
        if (result == NULL) return NULL;
        if (!mino_is_truthy_inline(result)) return result;
        args = rest;
    }
    return result;
}

static mino_val *eval_or(mino_state *S, mino_val *form,
                           mino_val *args, mino_env *env, int tail)
{
    mino_val *result = mino_nil(S);
    (void)form;
    while (mino_is_cons(args)) {
        mino_val *rest = args->as.cons.cdr;
        if (!mino_is_cons(rest)) {
            return eval_impl(S, args->as.cons.car, env, tail);
        }
        result = eval_value(S, args->as.cons.car, env);
        if (result == NULL) return NULL;
        if (mino_is_truthy_inline(result)) return result;
        args = rest;
    }
    return result;
}

/* --- Registry table ----------------------------------------------------- */

/*
 * Each entry pairs the cached-symbol slot (offset into mino_state)
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
    { offsetof(mino_state, member), (lit), sizeof(lit) - 1, (handler) }

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
    SF(sf_letfn_star,       "letfn*",           eval_letfn_star),
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

static mino_val *cached_at(mino_state *S, size_t off)
{
    return *(mino_val **)((char *)S + off);
}

int eval_try_special_form(mino_state *S, mino_val *form,
                          mino_val *head, mino_val *args,
                          mino_env *env, int tail,
                          mino_val **out)
{
    size_t i;
    if (head == NULL || mino_type_of(head) != MINO_SYMBOL) {
        return 0;
    }
    for (i = 0; i < k_special_forms_count; i++) {
        const special_form_entry *e = &k_special_forms[i];
        mino_val *cached = cached_at(S, e->sf_offset);
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

int eval_is_special_form_name(const char *name, size_t len)
{
    size_t i;
    if (name == NULL) {
        return 0;
    }
    for (i = 0; i < k_special_forms_count; i++) {
        const special_form_entry *e = &k_special_forms[i];
        if (e->name_len == len && memcmp(e->name, name, len) == 0) {
            return 1;
        }
    }
    return 0;
}

/* --- Public-form var registry ------------------------------------------- */

/*
 * The eleven forms below are public macros in canonical Clojure but are
 * dispatched here as C special forms (eval_try_special_form wins before
 * any macro lookup, on both the tree-walker and bytecode-compiler
 * paths). Registering a var + clojure.core env binding for each makes
 * them visible to ns-publics / resolve / doc / apropos without changing
 * how they evaluate. Internal-only spellings (let*, fn*, loop*, def,
 * if, do, quote, ...) are deliberately absent: they are implementation
 * surface, not part of the documented namespace.
 *
 * Note: when/and/or are also defined as defmacros in core.clj, so
 * macroexpand-1 expands them canonically.  The C special-form handler
 * runs at eval time (before any macro lookup), giving the correct
 * semantic without going through the macro expansion path.
 */
typedef struct {
    const char *name;
    const char *doc;
} public_form_doc;

static const public_form_doc k_public_form_docs[] = {
    { "fn",
      "Defines an anonymous function. Takes an optional name, a vector "
      "of parameters, and a body; supports multiple arities and a "
      "variadic & rest parameter." },
    { "let",
      "Evaluates the body with a sequence of local bindings established "
      "left to right from the binding vector; later bindings may refer "
      "to earlier ones." },
    { "loop",
      "Like let, but establishes a recursion point: a recur in tail "
      "position rebinds the loop locals and jumps back to the top "
      "without growing the stack." },
    { "lazy-seq",
      "Returns a sequence whose body is not evaluated until the first "
      "element is requested, then caches the realized sequence for "
      "later traversals." },
    { "binding",
      "Establishes thread-local bindings for dynamic vars over the "
      "extent of the body, restoring the prior roots when the body "
      "returns or unwinds." },
    { "declare",
      "Interns one or more names as unbound vars so they can be "
      "referred to before their defining form appears." },
    { "defmacro",
      "Defines a macro: a named function, invoked at expansion time, "
      "whose return value replaces the calling form before it is "
      "evaluated." },
    { "ns",
      "Selects or creates a namespace and applies its require / refer / "
      "import clauses, becoming the current namespace for the forms "
      "that follow." },
    { "when",
      "If test is logical true, evaluates body in an implicit do, "
      "returning the result of the last expression. If test is false "
      "or nil, returns nil." },
    { "and",
      "Evaluates exprs one at a time, from left to right. Returns the "
      "first logical false value or the last value if none are false. "
      "Returns true with no args." },
    { "or",
      "Evaluates exprs one at a time, from left to right. Returns the "
      "first logical true value or the last value if none are true. "
      "Returns nil with no args." },
};

static const size_t k_public_form_docs_count =
    sizeof(k_public_form_docs) / sizeof(k_public_form_docs[0]);

/*
 * Register the public C special forms as clojure.core vars.
 *
 * For each name: intern a var in clojure.core and give it a safe,
 * non-macro placeholder root, add the matching clojure.core env binding
 * (ns-publics requires BOTH a binding and a var), stamp {:macro true}
 * onto the var's meta slot so (meta (resolve 'let)) reports it as a
 * macro, and record the docstring through meta_set so (doc let) and
 * apropos surface it.
 *
 * The placeholder root must never be a MINO_MACRO: macroexpand1 and the
 * bytecode compiler's head_resolves_to_macro both treat a MINO_MACRO
 * head value as expandable, which would route these forms away from
 * their special-form handlers. A keyword carries the intent without
 * that hazard, and evaluation never consults it because special-form
 * dispatch wins first.
 */
void eval_special_register_vars(mino_state *S)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    mino_val *placeholder = mino_keyword(S, "mino/special-form");
    mino_val *macro_kw = mino_keyword(S, "macro");
    mino_val *yes = mino_true(S);
    size_t i;

    if (core_env == NULL || placeholder == NULL) {
        return;
    }
    /* Pin the two keywords allocated above: var_intern (and the other
     * allocation calls inside the loop) can trigger GC, and a
     * conservative scanner may miss pointers that the compiler keeps
     * only in registers.
     *
     * These pins are safe from longjmp-bypass: eval_special_register_vars
     * is called from sf_init on the first eval, before any user try frame
     * is active.  OOM here terminates initialization; the embedder's error
     * handler will report it via the state's top-level error mechanism. */
    gc_pin(placeholder);
    gc_pin(macro_kw);
    for (i = 0; i < k_public_form_docs_count; i++) {
        const char *name = k_public_form_docs[i].name;
        const char *doc  = k_public_form_docs[i].doc;
        mino_val *var  = var_intern(S, "clojure.core", name);
        if (var == NULL) {
            continue;
        }
        var_set_root(S, var, placeholder);
        mino_env_set(S, core_env, name, placeholder);
        /* :macro is synthesized from var->meta (overlaid on the def-site
         * map by synth_var_meta) since the placeholder root is not a
         * MINO_MACRO.  Include :doc when available so that
         * (:doc (meta (resolve 'when))) returns the docstring directly. */
        {
            mino_val *new_meta;
            if (doc != NULL) {
                mino_val *doc_kw  = mino_keyword(S, "doc");
                gc_pin(doc_kw);
                mino_val *doc_str = mino_string(S, doc);
                gc_pin(doc_str);
                {
                    mino_val *keys[2] = {macro_kw, doc_kw};
                    mino_val *vals[2] = {yes,       doc_str};
                    new_meta = mino_map(S, keys, vals, 2);
                }
                gc_unpin(2); /* doc_kw, doc_str */
            } else {
                new_meta = mino_map(S, &macro_kw, &yes, 1);
            }
            gc_write_barrier(S, var, var->meta, new_meta);
            var->meta = new_meta;
        }
        if (doc != NULL) {
            meta_set(S, name, doc, strlen(doc), NULL);
        }
    }
    gc_unpin(2); /* placeholder, macro_kw */
}
