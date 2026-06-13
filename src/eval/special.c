/*
 * special.c -- eval dispatch (eval_impl), literal evaluation, and the
 *              top-level eval entry point.
 *
 * Special-form implementations live in domain-specific files:
 *   defs.c      -- def, defmacro, declare
 *   bindings.c  -- let, loop, binding, destructuring
 *   control.c   -- try/catch/finally
 *   fn.c        -- fn, apply_callable, arity dispatch
 */

#include "eval/special_internal.h"
#include "prim/internal.h"
#include "mino.h"

/* --- Evaluator helpers: one per value kind. --- */

/* Look up an alias in the current namespace's alias table; returns the
 * full module name or NULL if not found. Aliases are scoped per-ns:
 * the same alias name can resolve to different targets in different
 * namespaces. */
static const char *alias_resolve(mino_state *S, const char *alias)
{
    size_t i;
    const char *cur = S->ns_vars.current_ns != NULL ? S->ns_vars.current_ns : "user";
    for (i = 0; i < S->ns_vars.ns_alias_len; i++) {
        if (S->ns_vars.ns_aliases[i].owning_ns != NULL
            && strcmp(S->ns_vars.ns_aliases[i].owning_ns, cur) == 0
            && strcmp(S->ns_vars.ns_aliases[i].alias, alias) == 0) {
            return S->ns_vars.ns_aliases[i].full_name;
        }
    }
    return NULL;
}

/* Resolve an ns/name qualified symbol. The caller has located the '/'
 * separator; this helper handles the literal-binding fast path, alias
 * resolution, var lookup with private-access check, and the ns-env
 * fallback for primitives. On a miss it sets a diagnostic that names
 * the most likely cause (missing var, missing ns, missing alias). */
static mino_val *eval_qualified_symbol(mino_state *S, mino_env *env,
                                         const char *data, size_t n,
                                         const char *slash)
{
    char        ns_buf[256];
    const char *sym_name    = slash + 1;
    size_t      ns_len      = (size_t)(slash - data);
    const char *resolved_ns;
    mino_env *target_env;
    mino_val *var;
    mino_val *v;
    char        msg[300];
    int         is_alias;
    (void)n;

    /* Try full name as a literal env binding first (e.g. "host/new"). */
    v = mino_env_get(env, data);
    if (v != NULL) return v;

    /* Host capability primitives ("host/new", "host/call", etc.) are
     * installed under their literal slash-name in the clojure.core
     * namespace env. The namespace resolver below treats "host" as a
     * namespace alias, which it isn't, so it would throw before
     * finding them. Probe clojure.core directly for the literal name
     * before falling through to alias resolution. */
    {
        mino_env *core_env = ns_env_lookup(S, "clojure.core");
        if (core_env != NULL) {
            env_binding_t *b = env_find_here(core_env, data);
            if (b != NULL) return b->val;
        }
    }

    if (ns_len >= sizeof(ns_buf)) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
            "syntax", "MSY001", "symbol name too long");
        return NULL;
    }
    memcpy(ns_buf, data, ns_len);
    ns_buf[ns_len] = '\0';

    /* Resolve alias to full module name. */
    resolved_ns = alias_resolve(S, ns_buf);
    if (resolved_ns == NULL) resolved_ns = ns_buf;

    /* Look up in var registry by resolved namespace + name. */
    var = var_find(S, resolved_ns, sym_name);
    if (var != NULL) {
        /* Cross-ns access of a private var is rejected. Same-ns access
         * is fine since callers within a namespace are not outsiders. */
        if (var->as.var.is_private
            && (S->ns_vars.current_ns == NULL
                || strcmp(S->ns_vars.current_ns, resolved_ns) != 0)) {
            snprintf(msg, sizeof(msg),
                "var %s/%s is private", resolved_ns, sym_name);
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                "name", "MNS001", msg);
            return NULL;
        }
        /* A qualified read names the same var as any other spelling:
         * consult the thread-binding stack (keyed by var identity,
         * with the var-less text criterion for referred spellings)
         * before falling back to the root. */
        if (mino_current_ctx(S)->dyn_stack != NULL) {
            mino_val *bv = dyn_lookup_var_or_name(S, var,
                                                  var->as.var.sym);
            if (bv != NULL) return bv;
        }
        return var->as.var.root;
    }

    /* Primitives live in the ns env but aren't interned as vars, so a
     * var_find miss falls back to the ns env's own bindings. */
    target_env = ns_env_lookup(S, resolved_ns);
    if (target_env != NULL) {
        env_binding_t *b = env_find_here(target_env, sym_name);
        if (b != NULL) return b->val;
    }

    is_alias = (resolved_ns != ns_buf);
    if (target_env != NULL) {
        snprintf(msg, sizeof(msg),
            "no var %s in namespace %s", sym_name, resolved_ns);
    } else if (is_alias) {
        /* alias_resolve gave us a name but no ns env exists: the alias
         * points at an unloaded namespace. */
        snprintf(msg, sizeof(msg),
            "no such namespace: %s", resolved_ns);
    } else {
        /* Neither an alias nor a loaded namespace -- most likely the
         * user meant an alias that isn't set up. */
        const char *cur =
            (S->ns_vars.current_ns != NULL) ? S->ns_vars.current_ns : "user";
        snprintf(msg, sizeof(msg),
            "no such alias: %s in namespace %s", ns_buf, cur);
    }
    set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
        "name", "MNS001", msg);
    return NULL;
}

static mino_val *eval_symbol(mino_state *S, mino_val *form, mino_env *env)
{
    size_t n = form->as.s.len;
    const char *data = form->as.s.data;  /* null-terminated (dup_n adds \0) */
    mino_val *v;
    const char *slash;
    int from_ns_env = 0;

    /* Check for namespace-qualified symbol (e.g. t/is, clojure.core/+).
     * Single-char "/" is the division function, not a qualified symbol. */
    slash = (n > 1) ? memchr(data, '/', n) : NULL;
    if (slash != NULL) {
        return eval_qualified_symbol(S, env, data, n, slash);
    }

    /* *ns* derefs to the current namespace symbol. Once
     * mino_install_clojure_core runs, *ns* is interned as a dynamic
     * var in clojure.core (see
     * runtime/ns_env.c:mino_publish_current_ns); this fast path stays
     * for embedders that read *ns* before installation completes and
     * for reading the running ns without going through the var registry.
     * The symbol carries the namespace's metadata so (meta *ns*) works. */
    if (n == 4 && memcmp(data, "*ns*", 4) == 0) {
        if (mino_env_get(env, data) == NULL) {
            const char *cur = S->ns_vars.current_ns != NULL ? S->ns_vars.current_ns : "user";
            mino_val *sym = mino_symbol(S, cur);
            mino_val *meta = ns_env_get_meta(S, cur);
            if (meta != NULL && sym != NULL) {
                mino_val *copy = alloc_val(S, mino_type_of(sym));
                copy->as   = sym->as;
                copy->meta = meta;
                return copy;
            }
            return sym;
        }
    }

    /* Unqualified: dynamic → lexical → current-ns env → fn ambient ns.
     * The lexical and ns-env walks use mino_env_get_sym to avoid
     * recomputing strlen on every parent frame; the symbol carries
     * its own length already. Track whether the value came from an
     * ns env lookup (vs. lexical/dynamic) so we can auto-deref var
     * bindings only on the ns-env path -- lexical/dynamic bindings to
     * a var (e.g. `(let [v (resolve 'foo)] ...)`) must preserve the
     * var as the binding value. */
    v = (mino_current_ctx(S)->dyn_stack != NULL)
        ? dyn_lookup_sym(S, data, n) : NULL;
    if (v == NULL) v = mino_env_get_sym(env, form);
    if (v == NULL) {
        mino_env *ns_env = current_ns_env(S);
        if (ns_env != NULL) {
            v = mino_env_get_sym(ns_env, form);
            if (v != NULL) from_ns_env = 1;
        }
    }
    if (v == NULL && S->ns_vars.fn_ambient_ns != NULL
        && S->ns_vars.fn_ambient_ns != S->ns_vars.current_ns
        && (S->ns_vars.current_ns == NULL
            || strcmp(S->ns_vars.fn_ambient_ns, S->ns_vars.current_ns) != 0)) {
        mino_env *amb = ns_env_lookup(S, S->ns_vars.fn_ambient_ns);
        if (amb != NULL) {
            v = mino_env_get_sym(amb, form);
            if (v != NULL) from_ns_env = 1;
        }
    }
    /* Auto-deref a var binding that came from the namespace env.
     * `def` and the ns-form refer path bind the value directly so
     * this is usually a no-op, but `clojure.core/refer` (the
     * function) binds the source var to preserve its source
     * namespace for syntax-quote and metadata, and `declare` binds
     * an unbound var so subsequent symbol access surfaces the
     * unbound state. Without this unwrap, calling a referred fn
     * like `(println ...)` after `(clojure.core/refer
     * 'clojure.core)` surfaces as "not a function (got var)"
     * because the symbol resolves to the var itself rather than
     * the fn at its root. Lexical / dynamic bindings that happen
     * to hold a var are left intact so `(let [v (resolve 'foo)] v)`
     * still returns the var. An unbound var (declared but not yet
     * def'd) throws "Var is unbound" so a reference-before-def bug
     * fails at the use site rather than propagating a silent nil. */
    if (from_ns_env && v != NULL && mino_type_of(v) == MINO_VAR) {
        /* A var bound into the ns env (refer / declare) reads through
         * the thread-binding stack like any other access of that var.
         * Checked before the unbound test: a thread binding satisfies
         * a read even when the root is unbound, per canon. */
        if (mino_current_ctx(S)->dyn_stack != NULL) {
            mino_val *bv = dyn_lookup_var_or_name(S, v, v->as.var.sym);
            if (bv != NULL) return bv;
        }
        if (!v->as.var.bound) {
            char msg[300];
            snprintf(msg, sizeof(msg),
                "Var is unbound: %s/%s",
                v->as.var.ns != NULL ? v->as.var.ns : "?",
                v->as.var.sym != NULL ? v->as.var.sym : data);
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                "name", "MNS003", msg);
            return NULL;
        }
        v = v->as.var.root;
    }
    if (v == NULL) {
        const mino_capability_info *cap = mino_capability_for_symbol(data);
        if (cap != NULL) {
            char  msg[400];
            char  note[200];
            mino_val *keys[4];
            mino_val *vals[4];
            mino_val *data_map;

            snprintf(msg, sizeof(msg),
                "%s is not installed in this runtime "
                "(capability '%s' disabled by host)",
                data, cap->name);
            snprintf(note, sizeof(note),
                "the host can enable this capability by calling "
                "mino_install(S, env, MINO_CAP_%s) from C before "
                "the first eval",
                cap->name);

            keys[0] = mino_keyword(S, "capability");
            vals[0] = mino_keyword(S, cap->name);
            keys[1] = mino_keyword(S, "symbol");
            vals[1] = mino_symbol(S, data);
            keys[2] = mino_keyword(S, "reason");
            vals[2] = mino_keyword(S, "not-installed");
            keys[3] = mino_keyword(S, "enable-via");
            vals[3] = mino_string(S, note);
            data_map = mino_map(S, keys, vals, 4);

            set_eval_diag_with_data(S,
                mino_current_ctx(S)->eval_current_form,
                "capability", "MNS002", msg, data_map, note);
            return NULL;
        }
        /* Constructor sugar: `ClassName.` or `pkg.path.ClassName.`
         * is JVM Clojure's shorthand for `(new ClassName args...)`.
         * mino has no JVM class layer, so the form is genuinely
         * unsupported -- but the bare "unbound symbol" message is
         * misleading because the user wrote a constructor call, not
         * a symbol reference. Detect the trailing dot (with at
         * least one char before it so `.` itself and leading-dot
         * forms like `.method` aren't caught) and surface a
         * dedicated diagnostic pointing at the supported
         * alternative. */
        if (n > 1 && data[n - 1] == '.' && data[0] != '.') {
            char msg[400];
            snprintf(msg, sizeof(msg),
                "constructor sugar `%s` is not supported on mino -- "
                "there is no JVM class layer; use `defrecord` and the "
                "generated `->%.*s` positional ctor instead",
                data, (int)(n - 1), data);
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                "name", "MNS004", msg);
            return NULL;
        }
        char msg[300];
        snprintf(msg, sizeof(msg), "unbound symbol: %s", data);
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "name", "MNS001", msg);
        return NULL;
    }
    return v;
}

/* A form evaluates to itself when all of its children would also evaluate
 * to themselves — leaf primitives (int, string, etc.) always, and nested
 * vectors/maps/sets/lazies only when they contain no symbols or calls. For
 * these, collection literals can return the AST form directly instead of
 * rebuilding it, since mino's data structures are immutable. */
static int is_eval_constant(mino_val *v)
{
    if (v == NULL) return 1;
    switch (mino_type_of(v)) {
    case MINO_NIL: case MINO_BOOL: case MINO_INT: case MINO_FLOAT:
    case MINO_STRING: case MINO_KEYWORD:
        return 1;
    default:
        return 0;
    }
}

static mino_val *eval_vector_literal(mino_state *S, mino_val *form,
                                       mino_env *env)
{
    size_t i;
    size_t n = form->as.vec.len;
    mino_val **tmp;
    if (n == 0) {
        return form;
    }
    /* Fast path: all elements are self-evaluating, so the literal already
     * equals its own result. Skip per-element eval and collection rebuild. */
    for (i = 0; i < n; i++) {
        if (!is_eval_constant(vec_nth(form, i))) break;
    }
    if (i == n) {
        return form;
    }
    tmp = (mino_val **)gc_alloc_typed(S, GC_T_VALARR, n * sizeof(*tmp));
    gc_pin((mino_val *)tmp);
    for (i = 0; i < n; i++) {
        mino_val *ev = eval_value(S, vec_nth(form, i), env);
        if (ev == NULL) {
            gc_unpin(1);
            return NULL;
        }
        gc_valarr_set(S, tmp, i, ev);
    }
    gc_unpin(1);
    {
        mino_val *result = mino_vector(S, tmp, n);
        if (form->meta != NULL) {
            result->meta = form->meta;
        }
        return result;
    }
}

/* Reader-produced records -- the tagged-literal fallback and
 * preserved reader conditionals -- are values, marked by a meta flag.
 * They self-evaluate like records; re-evaluating their map shape as a
 * literal would try to resolve the stored tag symbol / form. */
int mino_is_reader_record(mino_state *S, mino_val *form)
{
    mino_val *meta;
    mino_val *marker;
    if (form == NULL || mino_type_of(form) != MINO_MAP) return 0;
    meta = form->meta;
    if (meta == NULL || mino_type_of(meta) != MINO_MAP) return 0;
    marker = map_get_val(meta, mino_keyword(S, "mino/tagged-literal"));
    if (marker != NULL && marker != mino_nil(S)
        && marker != mino_false(S)) return 1;
    marker = map_get_val(meta, mino_keyword(S, "mino/reader-conditional"));
    if (marker != NULL && marker != mino_nil(S)
        && marker != mino_false(S)) return 1;
    return 0;
}

static mino_val *eval_map_literal(mino_state *S, mino_val *form,
                                    mino_env *env)
{
    size_t i;
    size_t n = form->as.map.len;
    mino_val **ks;
    mino_val **vs;
    if (n == 0) {
        return form;
    }
    if (mino_is_reader_record(S, form)) {
        return form;
    }
    /* Fast path: every key and value is self-evaluating. */
    for (i = 0; i < n; i++) {
        mino_val *form_key = vec_nth(form->as.map.key_order, i);
        if (!is_eval_constant(form_key)) break;
        if (!is_eval_constant(map_get_val(form, form_key))) break;
    }
    if (i == n) {
        return form;
    }
    ks = (mino_val **)gc_alloc_typed(S, GC_T_VALARR, n * sizeof(*ks));
    gc_pin((mino_val *)ks);
    vs = (mino_val **)gc_alloc_typed(S, GC_T_VALARR, n * sizeof(*vs));
    gc_pin((mino_val *)vs);
    for (i = 0; i < n; i++) {
        mino_val *form_key = vec_nth(form->as.map.key_order, i);
        mino_val *form_val = map_get_val(form, form_key);
        mino_val *k = eval_value(S, form_key, env);
        mino_val *v;
        if (k == NULL) {
            gc_unpin(2);
            return NULL;
        }
        v = eval_value(S, form_val, env);
        if (v == NULL) {
            gc_unpin(2);
            return NULL;
        }
        gc_valarr_set(S, ks, i, k);
        gc_valarr_set(S, vs, i, v);
    }
    gc_unpin(2);
    {
        mino_val *result = mino_map(S, ks, vs, n);
        if (form->meta != NULL) {
            result->meta = form->meta;
        }
        return result;
    }
}

static mino_val *eval_set_literal(mino_state *S, mino_val *form,
                                    mino_env *env)
{
    size_t i;
    size_t n = form->as.set.len;
    mino_val **tmp;
    if (n == 0) {
        return form;
    }
    /* Fast path: every element is self-evaluating. */
    for (i = 0; i < n; i++) {
        if (!is_eval_constant(vec_nth(form->as.set.key_order, i))) break;
    }
    if (i == n) {
        return form;
    }
    tmp = (mino_val **)gc_alloc_typed(S, GC_T_VALARR, n * sizeof(*tmp));
    gc_pin((mino_val *)tmp);
    for (i = 0; i < n; i++) {
        mino_val *ev = eval_value(S, vec_nth(form->as.set.key_order, i), env);
        if (ev == NULL) {
            gc_unpin(1);
            return NULL;
        }
        gc_valarr_set(S, tmp, i, ev);
    }
    gc_unpin(1);
    {
        mino_val *result = mino_set(S, tmp, n);
        if (form->meta != NULL) {
            result->meta = form->meta;
        }
        return result;
    }
}

static void sf_init(mino_state *S)
{
    S->sf_quote            = mino_symbol(S, "quote");
    S->sf_quasiquote       = mino_symbol(S, "quasiquote");
    S->sf_unquote          = mino_symbol(S, "unquote");
    S->sf_unquote_splicing = mino_symbol(S, "unquote-splicing");
    S->sf_defmacro         = mino_symbol(S, "defmacro");
    S->sf_declare          = mino_symbol(S, "declare");
    S->sf_ns               = mino_symbol(S, "ns");
    S->sf_var              = mino_symbol(S, "var");
    S->sf_def              = mino_symbol(S, "def");
    S->sf_if               = mino_symbol(S, "if");
    S->sf_do               = mino_symbol(S, "do");
    S->sf_let              = mino_symbol(S, "let");
    S->sf_let_star         = mino_symbol(S, "let*");
    S->sf_letfn_star       = mino_symbol(S, "letfn*");
    S->sf_fn               = mino_symbol(S, "fn");
    S->sf_fn_star          = mino_symbol(S, "fn*");
    S->sf_recur            = mino_symbol(S, "recur");
    S->sf_loop             = mino_symbol(S, "loop");
    S->sf_loop_star        = mino_symbol(S, "loop*");
    S->sf_try              = mino_symbol(S, "try");
    S->sf_binding          = mino_symbol(S, "binding");
    S->sf_lazy_seq         = mino_symbol(S, "lazy-seq");
    S->sf_new              = mino_symbol(S, "new");
    S->sf_when             = mino_symbol(S, "when");
    S->sf_and              = mino_symbol(S, "and");
    S->sf_or               = mino_symbol(S, "or");
    S->sf_initialized      = 1;
    /* Surface the public C special forms as clojure.core vars so they
     * appear in ns-publics / resolve / doc. Runs once per state at the
     * first eval, by which point the clojure.core ns env exists. */
    eval_special_register_vars(S);
}

/* --- Main eval dispatch -------------------------------------------------- */

/* eval_try_host_syntax -- host-interop syntax sugar (.method, .-field,
 * new, TypeName., TypeName/staticMethod).  Defined in special_host.c;
 * declared in eval/special_internal.h. */

/* Best-effort check: is `name` bound somewhere in the local env chain
 * (i.e. lexical, not in the ns env)? Walks until env hits a known ns
 * root. Used by the inline call cache to skip filling on calls whose
 * head is locally shadowed. */
static int local_lexical_shadow(mino_state *S, mino_env *env,
                                const char *name, size_t nlen)
{
    mino_env *cur = env;
    size_t      i;
    while (cur != NULL) {
        for (i = 0; i < S->ns_vars.ns_env_len; i++) {
            if (S->ns_vars.ns_env_table[i].env == cur) return 0;
        }
        if (env_find_here_n(cur, name, nlen) != NULL) return 1;
        cur = cur->parent;
    }
    return 0;
}

/*
 * Apply a regular (non-special-form) call. Resolves the head into
 * a callable, evaluates the args, and dispatches by callable kind:
 * PRIM/FN go through apply_callable (with a tail-call sentinel for
 * FN in tail position); MACRO expands and re-evaluates the result;
 * other callables (keyword, map, vector, set, sorted-map,
 * sorted-set) flow through apply_non_fn_callable so the two
 * dispatch entries cannot drift.
 */
static mino_val *eval_apply_regular_call(mino_state *S, mino_val *form,
                                           mino_val *head, mino_val *args,
                                           mino_env *env, int tail)
{
    mino_val *fn = NULL;
    mino_val *evaled;
    /* IC lookup. Conservative: only consult the cache when head is an
     * unqualified symbol and no dynamic binding context is active.
     * Slots are GC-pinned in gc_mark_runtime_globals so a freed form
     * cannot alias a fresh allocation. */
    if (head != NULL && mino_type_of(head) == MINO_SYMBOL
        && S->ns_vars.ic_table != NULL
        && mino_current_ctx(S)->dyn_stack == NULL) {
        size_t bucket = ((uintptr_t)form >> 4) & (S->ns_vars.ic_cap - 1);
        struct ic_slot *slot = &S->ns_vars.ic_table[bucket];
        if (slot->form == form
            && slot->head_data == head->as.s.data
            && slot->gen_at_fill == S->ns_vars.ic_gen) {
            fn = slot->callable;
        }
    }
    if (fn == NULL) {
        fn = eval_value(S, head, env);
        if (fn == NULL) return NULL;
        /* Fill conditions: head is unqualified symbol, no dyn rebind in
         * scope, no lexical shadow up to the ns env, resolved value is
         * not a var (unbound or in-flight). MINO_MACRO is excluded too
         * because macros expand differently per call site. */
        if (mino_type_of(head) == MINO_SYMBOL
            && mino_current_ctx(S)->dyn_stack == NULL
            && mino_type_of(fn) != MINO_VAR
            && mino_type_of(fn) != MINO_MACRO
            && !local_lexical_shadow(S, env, head->as.s.data, head->as.s.len)) {
            size_t bucket;
            struct ic_slot *slot;
            if (S->ns_vars.ic_table == NULL) {
                S->ns_vars.ic_cap   = 1024;
                S->ns_vars.ic_table = (struct ic_slot *)calloc(
                    S->ns_vars.ic_cap, sizeof(*S->ns_vars.ic_table));
                if (S->ns_vars.ic_table == NULL) S->ns_vars.ic_cap = 0;
            }
            if (S->ns_vars.ic_table != NULL) {
                bucket = ((uintptr_t)form >> 4) & (S->ns_vars.ic_cap - 1);
                slot   = &S->ns_vars.ic_table[bucket];
                slot->form        = form;
                slot->head_data   = head->as.s.data;
                slot->callable    = fn;
                slot->gen_at_fill = S->ns_vars.ic_gen;
            }
        }
    }
    if (fn == NULL) {
        return NULL;
    }
    /* Pin fn: eval_args allocates, and the conservative stack
     * scanner may miss fn if the compiler keeps it in a register. */
    gc_pin(fn);
    /* Numeric int+int fast lane. Recognise the binary call shape
     * (op a b) for the canonical arithmetic / comparison prims;
     * evaluate both args straight into stack vars; if both are
     * MINO_INT, compute the op directly with overflow-aware
     * builtins. Any miss (mixed types, overflow) builds a 2-cell
     * cons spine and falls through to apply_callable. */
    if (mino_type_of(fn) == MINO_PRIM
        && fn->as.prim.fn != NULL
        && mino_is_cons(args)
        && mino_is_cons(args->as.cons.cdr)
        && !mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        mino_prim_fn p = fn->as.prim.fn;
        if (p == prim_add || p == prim_sub || p == prim_mul
            || p == prim_eq
            || p == prim_lt || p == prim_lte
            || p == prim_gt || p == prim_gte) {
            mino_val *a = eval_value(S, args->as.cons.car, env);
            mino_val *b;
            mino_val *spine;
            if (a == NULL) {
                gc_unpin(1);
                return NULL;
            }
            gc_pin(a);
            b = eval_value(S, args->as.cons.cdr->as.cons.car, env);
            gc_unpin(1);
            if (b == NULL) {
                gc_unpin(1);
                return NULL;
            }
            if (mino_val_int_p(a) && mino_val_int_p(b)) {
                long long ai = mino_val_int_get(a);
                long long bi = mino_val_int_get(b);
                long long r;
                if (p == prim_add) {
#if defined(__GNUC__) || defined(__clang__)
                    if (!__builtin_add_overflow(ai, bi, &r)) {
                        gc_unpin(1);
                        return mino_int(S, r);
                    }
#endif
                } else if (p == prim_sub) {
#if defined(__GNUC__) || defined(__clang__)
                    if (!__builtin_sub_overflow(ai, bi, &r)) {
                        gc_unpin(1);
                        return mino_int(S, r);
                    }
#endif
                } else if (p == prim_mul) {
#if defined(__GNUC__) || defined(__clang__)
                    if (!__builtin_mul_overflow(ai, bi, &r)) {
                        gc_unpin(1);
                        return mino_int(S, r);
                    }
#endif
                } else if (p == prim_eq) {
                    gc_unpin(1);
                    return (ai == bi) ? mino_true(S) : mino_false(S);
                } else if (p == prim_lt) {
                    gc_unpin(1);
                    return (ai < bi) ? mino_true(S) : mino_false(S);
                } else if (p == prim_lte) {
                    gc_unpin(1);
                    return (ai <= bi) ? mino_true(S) : mino_false(S);
                } else if (p == prim_gt) {
                    gc_unpin(1);
                    return (ai > bi) ? mino_true(S) : mino_false(S);
                } else if (p == prim_gte) {
                    gc_unpin(1);
                    return (ai >= bi) ? mino_true(S) : mino_false(S);
                }
            }
            /* Slow path: rebuild the 2-element cons spine and call. */
            spine = mino_cons(S, a, mino_cons(S, b, mino_nil(S)));
            gc_unpin(1);
            return apply_callable(S, fn, spine, env);
        }
    }
    /* argv-prim fast path: skip eval_args' cons spine entirely.
     * Each arg form evaluates straight into a stack-resident scratch
     * slot, GC-rooted by the conservative stack scan. Spillover
     * beyond 16 args falls through to the cons path below. */
    if (mino_type_of(fn) == MINO_PRIM && fn->as.prim.fn2 != NULL) {
        mino_val  *scratch[16];
        int          scratch_cap = (int)(sizeof(scratch) / sizeof(scratch[0]));
        int          argc        = 0;
        int          spilled     = 0;
        mino_val  *cur         = args;
        mino_val  *result;
        const char  *file        = NULL;
        int          line        = 0;
        int          col         = 0;
        while (mino_is_cons(cur)) {
            if (argc == scratch_cap) {
                spilled = 1;
                break;
            }
            {
                mino_val *v = eval_value(S, cur->as.cons.car, env);
                if (v == NULL) {
                    gc_unpin(1);
                    return NULL;
                }
                scratch[argc++] = v;
                cur             = cur->as.cons.cdr;
            }
        }
        if (spilled) {
            /* Too many args for the stack scratch -- fall back to cons
             * eval + apply_callable's argv-aware dispatch path. Same
             * always-bail-on-NULL contract as the main PRIM/FN path
             * below. */
            evaled = eval_args(S, args, env);
            gc_unpin(1);
            if (evaled == NULL) {
                if (mino_last_error(S) == NULL) {
                    set_eval_diag(S, form, "internal", "MIN002",
                        "argument evaluation produced no value");
                }
                return NULL;
            }
            return apply_callable(S, fn, evaled, env);
        }
        if (mino_current_ctx(S)->eval_current_form != NULL
            && mino_type_of(mino_current_ctx(S)->eval_current_form) == MINO_CONS) {
            file = mino_current_ctx(S)->eval_current_form->as.cons.file;
            line = mino_current_ctx(S)->eval_current_form->as.cons.line;
            col  = mino_current_ctx(S)->eval_current_form->as.cons.column;
        }
        push_frame(S, fn->as.prim.name, file, line, col);
        result = fn->as.prim.fn2(S, scratch, argc, env);
        gc_unpin(1);
        if (result == NULL) return NULL; /* leave frame for trace */
        pop_frame(S);
        return result;
    }
    /* Fast path for the dominant PRIM/FN case, checked first so
     * non-function callables (keyword/map/vector/etc.) don't delay
     * typical calls. */
    if (mino_type_of(fn) == MINO_PRIM || mino_type_of(fn) == MINO_FN) {
        evaled = eval_args(S, args, env);
        gc_unpin(1);
        /* eval_args returns NULL on any inner-eval failure. Always
         * bail when evaled is NULL: silently passing NULL to
         * apply_callable lets it reach prims that then complain about
         * the wrong condition (e.g. (seq) raises "seq requires one
         * argument" because args is the literal C NULL, not a one-cons
         * list). The previous `&& mino_last_error != NULL` gate let
         * NULL leak through when an inner step cleared the latched
         * error (try/catch handlers do this) without producing a real
         * result. Promote the silent state to an explicit diagnostic
         * so callers see "argument evaluation produced no value" at
         * the actual failure site rather than a downstream prim-arity
         * lie. */
        if (evaled == NULL) {
            if (mino_last_error(S) == NULL) {
                set_eval_diag(S, form, "internal", "MIN002",
                    "argument evaluation produced no value");
            }
            return NULL;
        }
        if (tail && mino_type_of(fn) == MINO_FN) {
            S->tail_call_sentinel.as.tail_call.fn   = fn;
            S->tail_call_sentinel.as.tail_call.args = evaled;
            return &S->tail_call_sentinel;
        }
        return apply_callable(S, fn, evaled, env);
    }
    if (mino_type_of(fn) == MINO_MACRO) {
        /* Expand with unevaluated args; re-eval the resulting form in
         * the caller's environment. */
        mino_val *expanded = apply_callable(S, fn, args, env);
        gc_unpin(1);
        if (expanded == NULL) {
            return NULL;
        }
        return eval_impl(S, expanded, env, tail);
    }
    /* Non-fn callables: keyword, map, vector, set, sorted-map,
     * sorted-set. Same always-bail-on-NULL contract. */
    gc_unpin(1);
    evaled = eval_args(S, args, env);
    if (evaled == NULL) {
        if (mino_last_error(S) == NULL) {
            set_eval_diag(S, form, "internal", "MIN002",
                "argument evaluation produced no value");
        }
        return NULL;
    }
    return apply_non_fn_callable(S, fn, evaled, form);
}

/*
 * Per-step gate: bail out before doing real work if a host limit
 * (steps, heap), an interrupt, or a sticky limit-exceeded flag is
 * in effect. Sets the appropriate eval diagnostic and the
 * limit_exceeded latch so the rest of eval_impl observes a single
 * source of truth. Returns 0 if eval should bail (with diag set
 * when applicable), 1 to proceed.
 *
 * The safepoint poll for major-GC STW also folds in here — eval_impl
 * entry is the densest legitimate safepoint site, so one branch
 * covers both the cancel/limit path and the GC park request. The
 * poll is a single predictably-not-taken read on the single-threaded
 * fast path.
 */
static int eval_check_limits(mino_state *S)
{
    if (mino_current_ctx(S)->limit_exceeded) {
        return 0;
    }
    if (mino_current_ctx(S)->interrupted) {
        mino_current_ctx(S)->limit_exceeded = 1;
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "limit", "MLM001",
                      "eval interrupted");
        return 0;
    }
    mino_safepoint_poll(S);
    if (S->module.limit_steps > 0 && ++mino_current_ctx(S)->eval_steps > S->module.limit_steps) {
        mino_current_ctx(S)->limit_exceeded = 1;
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "limit", "MLM001",
                      "step limit exceeded");
        return 0;
    }
    if (S->module.limit_heap > 0 && S->gc.bytes_alloc > S->module.limit_heap) {
        mino_current_ctx(S)->limit_exceeded = 1;
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "limit", "MLM001",
                      "heap limit exceeded");
        return 0;
    }
    return 1;
}

mino_val *eval_impl(mino_state *S, mino_val *form, mino_env *env, int tail)
{
    if (!S->sf_initialized) {
        sf_init(S);
    }
    if (!eval_check_limits(S)) {
        return NULL;
    }
    if (form == NULL) {
        return mino_nil(S);
    }
    switch (mino_type_of(form)) {
    case MINO_NIL:
    case MINO_EMPTY_LIST:
    case MINO_BOOL:
    case MINO_INT:
    case MINO_FLOAT:
    case MINO_FLOAT32:
    case MINO_CHAR:
    case MINO_STRING:
    case MINO_KEYWORD:
    case MINO_PRIM:
    case MINO_FN:
    case MINO_MACRO:
    case MINO_HANDLE:
    case MINO_ATOM:
    case MINO_VOLATILE:
    case MINO_CHUNK:
    case MINO_RECUR:
    case MINO_TAIL_CALL:
    case MINO_REDUCED:
    case MINO_VAR:
    case MINO_TRANSIENT:
    case MINO_SORTED_MAP:
    case MINO_SORTED_SET:
    case MINO_BIGINT:
    case MINO_RATIO:
    case MINO_BIGDEC:
    case MINO_TYPE:
    case MINO_RECORD:
    case MINO_FUTURE:
    case MINO_UUID:
    case MINO_REGEX:
    case MINO_HOST_ARRAY:
    case MINO_MAP_ENTRY:
    case MINO_TX_REF:
    case MINO_AGENT:
    case MINO_CHAN:
    case MINO_QUEUE:
    case MINO_BYTES:
        return form;
    case MINO_SYMBOL:
        return eval_symbol(S, form, env);
    case MINO_VECTOR:
        return eval_vector_literal(S, form, env);
    case MINO_MAP:
        return eval_map_literal(S, form, env);
    case MINO_SET:
        return eval_set_literal(S, form, env);
    case MINO_LAZY: {
        /* A lazy seq used as a form (typical macro-built shape via
         * concat / sequence) — force it into a CONS chain so the
         * call-form path below handles it the same way a literal
         * list would. JVM Clojure's eval treats any ISeq as a call
         * form when the head is callable. */
        mino_val *forced = lazy_force(S, form);
        if (forced == NULL || mino_type_of(forced) == MINO_NIL
            || mino_type_of(forced) == MINO_EMPTY_LIST) {
            return forced;
        }
        if (mino_type_of(forced) == MINO_CONS) {
            form = forced;
            goto eval_cons_label;
        }
        /* CHUNKED_CONS or another seq shape: fall through to
         * CHUNKED_CONS handling below. */
        form = forced;
        if (mino_type_of(form) != MINO_CHUNKED_CONS) return form;
    }
    MINO_FALLTHROUGH; /* into CHUNKED_CONS handling below */
    case MINO_CHUNKED_CONS: {
        mino_val *as_cons = val_to_seq(S, form);
        if (as_cons == NULL) return NULL;
        if (mino_type_of(as_cons) != MINO_CONS) return as_cons;
        form = as_cons;
        goto eval_cons_label;
    }
    /* fallthrough */
    case MINO_CONS:
    eval_cons_label: {
        mino_val *head = form->as.cons.car;
        mino_val *args = form->as.cons.cdr;
        /* Macros that build call forms via concat / sequence often
         * leave a lazy seq as the cdr; special forms (`quote`, `if`,
         * etc.) probe args with mino_is_cons and would treat a lazy
         * cdr as "no args". Force the cdr-chain into a CONS spine
         * here so the downstream dispatchers see a normal call form. */
        while (args != NULL && mino_type_of(args) == MINO_LAZY) {
            args = lazy_force(S, args);
            if (args == NULL) return NULL;
        }
        if (args != NULL && mino_type_of(args) == MINO_CHUNKED_CONS) {
            args = val_to_seq(S, args);
            if (args == NULL) return NULL;
        }
        if (args != NULL && mino_type_of(args) == MINO_EMPTY_LIST) {
            args = mino_nil(S);
        }
        mino_val *host_result;
        mino_val *result;
        /* Save and restore eval_current_form around the recursive
         * descent so a parent form's source span doesn't get clobbered
         * by a child sub-eval. Without this, eval_current_form lingers
         * at the LAST sub-form evaluated, so any throw that fires
         * after eval returns (e.g. a thread-limit throw from inside
         * mino_future_spawn, after eval_args has finished walking the
         * argument list) blames the wrong source location -- often a
         * form from a previously-loaded test file. */
        const mino_val *prev_form = mino_current_ctx(S)->eval_current_form;
        mino_current_ctx(S)->eval_current_form = form;

        if (eval_try_host_syntax(S, form, head, args, env, &host_result)) {
            mino_current_ctx(S)->eval_current_form = prev_form;
            return host_result;
        }

        /* Special forms run through the data-table dispatch in
         * eval/special_registry.c. */
        {
            mino_val *sf_result;
            if (eval_try_special_form(S, form, head, args, env, tail,
                                      &sf_result)) {
                mino_current_ctx(S)->eval_current_form = prev_form;
                return sf_result;
            }
        }

        /* Function or macro application. */
        result = eval_apply_regular_call(S, form, head, args, env, tail);
        mino_current_ctx(S)->eval_current_form = prev_form;
        return result;
    }
    }
    set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                  "internal", "MIN001", "eval: unknown value type");
    return NULL;
}

mino_val *eval(mino_state *S, mino_val *form, mino_env *env)
{
    return eval_impl(S, form, env, 0);
}
