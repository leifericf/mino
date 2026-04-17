/*
 * prim.c -- shared helpers, metadata, reflection, stateful, regex,
 *           module, and registration primitives.
 */

#include "prim_internal.h"
#include "re.h"

/* ------------------------------------------------------------------------- */
/* Shared helpers                                                            */
/* ------------------------------------------------------------------------- */

/*
 * Numeric coercion: if any argument is a float, the result is a float.
 * Otherwise integer arithmetic is used end-to-end.
 */

int args_have_float(mino_val_t *args)
{
    while (mino_is_cons(args)) {
        mino_val_t *a = args->as.cons.car;
        if (a != NULL && a->type == MINO_FLOAT) {
            return 1;
        }
        args = args->as.cons.cdr;
    }
    return 0;
}

/* Throw a catchable exception from a primitive.  If inside a try block,
 * this longjmps to the catch handler.  Otherwise it sets a fatal error
 * and the caller returns NULL to propagate to the host. */
mino_val_t *prim_throw_error(mino_state_t *S, const char *msg)
{
    mino_val_t *ex = mino_string(S, msg);
    if (S->try_depth > 0) {
        S->try_stack[S->try_depth - 1].exception = ex;
        longjmp(S->try_stack[S->try_depth - 1].buf, 1);
    }
    set_error(S, msg);
    return NULL;
}

int as_double(const mino_val_t *v, double *out)
{
    if (v == NULL) {
        return 0;
    }
    if (v->type == MINO_INT) {
        *out = (double)v->as.i;
        return 1;
    }
    if (v->type == MINO_FLOAT) {
        *out = v->as.f;
        return 1;
    }
    return 0;
}

int as_long(const mino_val_t *v, long long *out)
{
    if (v == NULL || v->type != MINO_INT) {
        return 0;
    }
    *out = v->as.i;
    return 1;
}


/*
 * Helper: print a value to a string buffer using the standard printer.
 * Returns a mino string. Uses tmpfile() for ANSI C portability.
 */
mino_val_t *print_to_string(mino_state_t *S, const mino_val_t *v)
{
    FILE  *f = tmpfile();
    long   n;
    char  *buf;
    mino_val_t *result;
    if (f == NULL) {
        set_error(S, "pr-str: tmpfile failed");
        return NULL;
    }
    mino_print_to(S, f, v);
    n = ftell(f);
    if (n < 0) n = 0;
    rewind(f);
    buf = (char *)malloc((size_t)n + 1);
    if (buf == NULL) {
        fclose(f);
        set_error(S, "out of memory");
        return NULL;
    }
    if (n > 0) {
        size_t rd = fread(buf, 1, (size_t)n, f);
        (void)rd;
    }
    buf[n] = '\0';
    fclose(f);
    result = mino_string_n(S, buf, (size_t)n);
    free(buf);
    return result;
}

/* ------------------------------------------------------------------------- */
/* Reflection primitives                                                     */
/* ------------------------------------------------------------------------- */

mino_val_t *prim_name(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "name requires one argument");
        return NULL;
    }
    v = args->as.cons.car;
    if (v == NULL || v->type == MINO_NIL) return mino_nil(S);
    if (v->type == MINO_STRING)  return v;
    if (v->type == MINO_KEYWORD) return mino_string_n(S, v->as.s.data, v->as.s.len);
    if (v->type == MINO_SYMBOL)  return mino_string_n(S, v->as.s.data, v->as.s.len);
    set_error(S, "name: expected a keyword, symbol, or string");
    return NULL;
}

mino_val_t *prim_rand(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (mino_is_cons(args)) {
        set_error(S, "rand takes no arguments");
        return NULL;
    }
    return mino_float(S, (double)rand() / ((double)RAND_MAX + 1.0));
}

mino_val_t *prim_eval(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "eval requires one argument");
        return NULL;
    }
    return eval_value(S, args->as.cons.car, env);
}

mino_val_t *prim_symbol(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "symbol requires one argument");
        return NULL;
    }
    v = args->as.cons.car;
    if (v != NULL && v->type == MINO_STRING) {
        return mino_symbol_n(S, v->as.s.data, v->as.s.len);
    }
    if (v != NULL && v->type == MINO_SYMBOL) {
        return v;
    }
    set_error(S, "symbol: argument must be a string");
    return NULL;
}

mino_val_t *prim_keyword(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "keyword requires one argument");
        return NULL;
    }
    v = args->as.cons.car;
    if (v != NULL && v->type == MINO_STRING) {
        return mino_keyword_n(S, v->as.s.data, v->as.s.len);
    }
    if (v != NULL && v->type == MINO_KEYWORD) {
        return v;
    }
    set_error(S, "keyword: argument must be a string");
    return NULL;
}

mino_val_t *prim_hash(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "hash requires one argument");
        return NULL;
    }
    return mino_int(S, (long long)hash_val(args->as.cons.car));
}

/* --- Metadata primitives ------------------------------------------------- */

/* Type tags that can carry metadata. */

static int supports_meta(mino_type_t t)
{
    return t == MINO_SYMBOL || t == MINO_CONS || t == MINO_VECTOR
        || t == MINO_MAP    || t == MINO_SET  || t == MINO_FN
        || t == MINO_MACRO;
}

mino_val_t *prim_meta(mino_state_t *S, mino_val_t *args,
                       mino_env_t *env)
{
    mino_val_t *obj;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "meta requires one argument");
        return NULL;
    }
    obj = args->as.cons.car;
    if (obj == NULL || !supports_meta(obj->type)) {
        return mino_nil(S);
    }
    return obj->meta != NULL ? obj->meta : mino_nil(S);
}

mino_val_t *prim_with_meta(mino_state_t *S, mino_val_t *args,
                            mino_env_t *env)
{
    mino_val_t *obj, *m, *copy;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "with-meta requires 2 arguments");
        return NULL;
    }
    obj = args->as.cons.car;
    m   = args->as.cons.cdr->as.cons.car;
    if (obj == NULL || !supports_meta(obj->type)) {
        set_error(S, "with-meta: type does not support metadata");
        return NULL;
    }
    if (m != NULL && m->type != MINO_NIL && m->type != MINO_MAP) {
        set_error(S, "with-meta: metadata must be a map or nil");
        return NULL;
    }
    /* Shallow-copy the value and attach the new metadata. */
    copy = alloc_val(S, obj->type);
    copy->as = obj->as;
    copy->meta = (m != NULL && m->type == MINO_NIL) ? NULL : m;
    return copy;
}

mino_val_t *prim_vary_meta(mino_state_t *S, mino_val_t *args,
                            mino_env_t *env)
{
    mino_val_t *obj, *f, *old_meta, *extra, *call_args, *new_meta, *copy;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "vary-meta requires at least 2 arguments");
        return NULL;
    }
    obj = args->as.cons.car;
    f   = args->as.cons.cdr->as.cons.car;
    extra = args->as.cons.cdr->as.cons.cdr; /* remaining args (cons list or nil) */
    if (obj == NULL || !supports_meta(obj->type)) {
        set_error(S, "vary-meta: type does not support metadata");
        return NULL;
    }
    old_meta = (obj->meta != NULL) ? obj->meta : mino_nil(S);
    /* Build (old-meta extra...) argument list for f. */
    call_args = mino_cons(S, old_meta, extra);
    new_meta = mino_call(S, f, call_args, env);
    if (new_meta == NULL) {
        return NULL;
    }
    if (new_meta->type != MINO_NIL && new_meta->type != MINO_MAP) {
        set_error(S, "vary-meta: f must return a map or nil");
        return NULL;
    }
    copy = alloc_val(S, obj->type);
    copy->as = obj->as;
    copy->meta = (new_meta->type == MINO_NIL) ? NULL : new_meta;
    return copy;
}

mino_val_t *prim_alter_meta(mino_state_t *S, mino_val_t *args,
                             mino_env_t *env)
{
    mino_val_t *obj, *f, *old_meta, *extra, *call_args, *new_meta;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "alter-meta! requires at least 2 arguments");
        return NULL;
    }
    obj   = args->as.cons.car;
    f     = args->as.cons.cdr->as.cons.car;
    extra = args->as.cons.cdr->as.cons.cdr;
    if (obj == NULL || !supports_meta(obj->type)) {
        set_error(S, "alter-meta!: type does not support metadata");
        return NULL;
    }
    old_meta = (obj->meta != NULL) ? obj->meta : mino_nil(S);
    call_args = mino_cons(S, old_meta, extra);
    new_meta = mino_call(S, f, call_args, env);
    if (new_meta == NULL) {
        return NULL;
    }
    if (new_meta->type != MINO_NIL && new_meta->type != MINO_MAP) {
        set_error(S, "alter-meta!: f must return a map or nil");
        return NULL;
    }
    obj->meta = (new_meta->type == MINO_NIL) ? NULL : new_meta;
    return obj->meta != NULL ? obj->meta : mino_nil(S);
}

/* ------------------------------------------------------------------------- */
/* Utility primitives                                                        */
/* ------------------------------------------------------------------------- */

mino_val_t *prim_type(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "type requires one argument");
        return NULL;
    }
    v = args->as.cons.car;
    if (v == NULL || v->type == MINO_NIL)  return mino_keyword(S, "nil");
    switch (v->type) {
    case MINO_NIL:     return mino_keyword(S, "nil");
    case MINO_BOOL:    return mino_keyword(S, "bool");
    case MINO_INT:     return mino_keyword(S, "int");
    case MINO_FLOAT:   return mino_keyword(S, "float");
    case MINO_STRING:  return mino_keyword(S, "string");
    case MINO_SYMBOL:  return mino_keyword(S, "symbol");
    case MINO_KEYWORD: return mino_keyword(S, "keyword");
    case MINO_CONS:    return mino_keyword(S, "list");
    case MINO_VECTOR:  return mino_keyword(S, "vector");
    case MINO_MAP:     return mino_keyword(S, "map");
    case MINO_SET:     return mino_keyword(S, "set");
    case MINO_PRIM:    return mino_keyword(S, "fn");
    case MINO_FN:      return mino_keyword(S, "fn");
    case MINO_MACRO:   return mino_keyword(S, "macro");
    case MINO_HANDLE:  return mino_keyword(S, "handle");
    case MINO_ATOM:    return mino_keyword(S, "atom");
    case MINO_LAZY:    return mino_keyword(S, "lazy-seq");
    case MINO_RECUR:     return mino_keyword(S, "recur");
    case MINO_TAIL_CALL: return mino_keyword(S, "tail-call");
    case MINO_REDUCED:   return mino_keyword(S, "reduced");
    }
    return mino_keyword(S, "unknown");
}

mino_val_t *prim_macroexpand_1(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    int expanded;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "macroexpand-1 requires one argument");
        return NULL;
    }
    return macroexpand1(S, args->as.cons.car, env, &expanded);
}

mino_val_t *prim_macroexpand(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "macroexpand requires one argument");
        return NULL;
    }
    return macroexpand_all(S, args->as.cons.car, env);
}

mino_val_t *prim_gensym(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    const char *prefix_src = "G__";
    size_t      prefix_len = 3;
    char        buf[256];
    size_t      nargs;
    (void)env;
    arg_count(S, args, &nargs);
    if (nargs > 1) {
        set_error(S, "gensym takes 0 or 1 arguments");
        return NULL;
    }
    if (nargs == 1) {
        mino_val_t *p = args->as.cons.car;
        if (p == NULL || p->type != MINO_STRING) {
            set_error(S, "gensym prefix must be a string");
            return NULL;
        }
        prefix_src = p->as.s.data;
        prefix_len = p->as.s.len;
        if (prefix_len >= sizeof(buf) - 32) {
            set_error(S, "gensym prefix too long");
            return NULL;
        }
    }
    {
        int used;
        memcpy(buf, prefix_src, prefix_len);
        used = snprintf(buf + prefix_len, sizeof(buf) - prefix_len,
                        "%ld", ++S->gensym_counter);
        if (used < 0) {
            set_error(S, "gensym formatting failed");
            return NULL;
        }
        return mino_symbol_n(S, buf, prefix_len + (size_t)used);
    }
}

/* (throw value) — raise a script exception. Caught by try/catch; if no
 * enclosing try, becomes a fatal runtime error. */
mino_val_t *prim_throw(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *ex;
    (void)env;
    if (!mino_is_cons(args)) {
        set_error(S, "throw requires one argument");
        return NULL;
    }
    ex = args->as.cons.car;
    if (S->try_depth <= 0) {
        /* No enclosing try — format as fatal error. */
        char msg[512];
        if (ex != NULL && ex->type == MINO_STRING) {
            snprintf(msg, sizeof(msg), "unhandled exception: %.*s",
                     (int)ex->as.s.len, ex->as.s.data);
        } else {
            snprintf(msg, sizeof(msg), "unhandled exception");
        }
        set_error(S, msg);
        return NULL;
    }
    S->try_stack[S->try_depth - 1].exception = ex;
    longjmp(S->try_stack[S->try_depth - 1].buf, 1);
    return NULL; /* unreachable */
}

/* --- Regex primitives (using bundled tiny-regex-c) --- */

/* (re-find pattern text) — find first match of pattern in text.
 * Returns the matched substring, or nil if no match. */
mino_val_t *prim_re_find(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *pat_val, *text_val;
    int match_len = 0;
    int match_idx;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error(S, "re-find requires two arguments");
        return NULL;
    }
    pat_val  = args->as.cons.car;
    text_val = args->as.cons.cdr->as.cons.car;
    if (pat_val == NULL || pat_val->type != MINO_STRING) {
        set_error(S, "re-find: first argument must be a pattern string");
        return NULL;
    }
    if (text_val == NULL || text_val->type != MINO_STRING) {
        set_error(S, "re-find: second argument must be a string");
        return NULL;
    }
    match_idx = re_match(pat_val->as.s.data, text_val->as.s.data, &match_len);
    if (match_idx == -1) {
        return mino_nil(S);
    }
    return mino_string_n(S, text_val->as.s.data + match_idx, (size_t)match_len);
}

/* (re-matches pattern text) — true if the entire text matches pattern. */
mino_val_t *prim_re_matches(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *pat_val, *text_val;
    int match_len = 0;
    int match_idx;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error(S, "re-matches requires two arguments");
        return NULL;
    }
    pat_val  = args->as.cons.car;
    text_val = args->as.cons.cdr->as.cons.car;
    if (pat_val == NULL || pat_val->type != MINO_STRING) {
        set_error(S, "re-matches: first argument must be a pattern string");
        return NULL;
    }
    if (text_val == NULL || text_val->type != MINO_STRING) {
        set_error(S, "re-matches: second argument must be a string");
        return NULL;
    }
    match_idx = re_match(pat_val->as.s.data, text_val->as.s.data, &match_len);
    if (match_idx == 0 && (size_t)match_len == text_val->as.s.len) {
        return text_val;
    }
    return mino_nil(S);
}

/* (require name) — load a module by name using the host-supplied resolver.
 * Returns the cached value on subsequent calls with the same name. */
mino_val_t *prim_require(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *name_val;
    const char *name;
    const char *path;
    size_t      i;
    mino_val_t *result;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "require requires one argument");
        return NULL;
    }
    name_val = args->as.cons.car;
    if (name_val == NULL || name_val->type != MINO_STRING) {
        set_error(S, "require: argument must be a string");
        return NULL;
    }
    name = name_val->as.s.data;
    /* Check cache. */
    for (i = 0; i < S->module_cache_len; i++) {
        if (strcmp(S->module_cache[i].name, name) == 0) {
            return S->module_cache[i].value;
        }
    }
    /* Resolve. */
    if (S->module_resolver == NULL) {
        set_error(S, "require: no module resolver configured");
        return NULL;
    }
    path = S->module_resolver(name, S->module_resolver_ctx);
    if (path == NULL) {
        char msg[300];
        snprintf(msg, sizeof(msg), "require: cannot resolve module: %s", name);
        set_error(S, msg);
        return NULL;
    }
    /* Load. */
    result = mino_load_file(S, path, env);
    if (result == NULL) {
        return NULL;
    }
    /* Cache. */
    if (S->module_cache_len == S->module_cache_cap) {
        size_t         new_cap = S->module_cache_cap == 0 ? 8 : S->module_cache_cap * 2;
        module_entry_t *nb     = (module_entry_t *)realloc(
            S->module_cache, new_cap * sizeof(*nb));
        if (nb == NULL) {
            set_error(S, "require: out of memory");
            return NULL;
        }
        S->module_cache     = nb;
        S->module_cache_cap = new_cap;
    }
    {
        size_t nlen = strlen(name);
        char *dup   = (char *)malloc(nlen + 1);
        if (dup == NULL) {
            set_error(S, "require: out of memory");
            return NULL;
        }
        memcpy(dup, name, nlen + 1);
        S->module_cache[S->module_cache_len].name  = dup;
        S->module_cache[S->module_cache_len].value = result;
        S->module_cache_len++;
    }
    return result;
}

/* (doc name) — print the docstring for a def/defmacro binding.
 * Returns the docstring as a string, or nil if no docstring. */
mino_val_t *prim_doc(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t   *name_val;
    char          buf[256];
    size_t        n;
    meta_entry_t *e;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "doc requires one argument");
        return NULL;
    }
    name_val = args->as.cons.car;
    if (name_val == NULL || name_val->type != MINO_SYMBOL) {
        set_error(S, "doc: argument must be a symbol");
        return NULL;
    }
    n = name_val->as.s.len;
    if (n >= sizeof(buf)) {
        set_error(S, "doc: name too long");
        return NULL;
    }
    memcpy(buf, name_val->as.s.data, n);
    buf[n] = '\0';
    e = meta_find(S, buf);
    if (e != NULL && e->docstring != NULL) {
        return mino_string(S, e->docstring);
    }
    return mino_nil(S);
}

/* (source name) — return the source form of a def/defmacro binding. */
mino_val_t *prim_source(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t   *name_val;
    char          buf[256];
    size_t        n;
    meta_entry_t *e;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "source requires one argument");
        return NULL;
    }
    name_val = args->as.cons.car;
    if (name_val == NULL || name_val->type != MINO_SYMBOL) {
        set_error(S, "source: argument must be a symbol");
        return NULL;
    }
    n = name_val->as.s.len;
    if (n >= sizeof(buf)) {
        set_error(S, "source: name too long");
        return NULL;
    }
    memcpy(buf, name_val->as.s.data, n);
    buf[n] = '\0';
    e = meta_find(S, buf);
    if (e != NULL && e->source != NULL) {
        return e->source;
    }
    return mino_nil(S);
}

/* (apropos substring) — return a list of bound names containing substring. */
mino_val_t *prim_apropos(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *pat_val;
    const char *pat;
    mino_val_t *head = mino_nil(S);
    mino_val_t *tail = NULL;
    mino_env_t *e;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "apropos requires one argument");
        return NULL;
    }
    pat_val = args->as.cons.car;
    if (pat_val == NULL || pat_val->type != MINO_STRING) {
        set_error(S, "apropos: argument must be a string");
        return NULL;
    }
    pat = pat_val->as.s.data;
    /* Walk every env frame from the given env up to root. */
    for (e = env; e != NULL; e = e->parent) {
        size_t i;
        for (i = 0; i < e->len; i++) {
            if (strstr(e->bindings[i].name, pat) != NULL) {
                mino_val_t *sym  = mino_symbol(S, e->bindings[i].name);
                mino_val_t *cell = mino_cons(S, sym, mino_nil(S));
                if (tail == NULL) {
                    head = cell;
                } else {
                    tail->as.cons.cdr = cell;
                }
                tail = cell;
            }
        }
    }
    return head;
}

void mino_set_resolver(mino_state_t *S, mino_resolve_fn fn, void *ctx)
{
    S->module_resolver     = fn;
    S->module_resolver_ctx = ctx;
}

/*
 * Stdlib macros defined in mino itself. Each form is read + evaluated in
 * order against the installing env during mino_install_core, so downstream
 * code can depend on them as if they were primitives.
 *
 * Hygiene: macro writers introduce temporaries via (gensym) to avoid
 * capturing names from the caller's environment. 0.x makes no automatic
 * hygiene promise; gensym is the convention.
 */
#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Woverlength-strings"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
#include "core_mino.h"
#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

static void install_core_mino(mino_state_t *S, mino_env_t *env)
{
    size_t i;

    /* On first call for this state, parse and eval each form, caching
     * the parsed ASTs for subsequent mino_install_core calls.  We set
     * S->core_forms immediately and update core_forms_len as we go so
     * that the GC root walker can pin the forms during collection. */
    if (S->core_forms == NULL) {
        const char  *src        = core_mino_src;
        const char  *saved_file = S->reader_file;
        int          saved_line = S->reader_line;
        size_t       cap        = 256;

        S->core_forms     = malloc(cap * sizeof(mino_val_t *));
        S->core_forms_len = 0;
        if (!S->core_forms) {
            /* Class I: init-time; no try-frame to recover through */
            fprintf(stderr, "core.mino: out of memory\n"); abort();
        }

        S->reader_file = intern_filename("<core>");
        S->reader_line = 1;
        while (*src != '\0') {
            const char *end  = NULL;
            mino_val_t *form = mino_read(S, src, &end);
            if (form == NULL) {
                if (mino_last_error(S) != NULL) {
                    /* Class I: core library parse failure is unrecoverable */
                    fprintf(stderr, "core.mino parse error: %s\n",
                            mino_last_error(S));
                    abort();
                }
                break;
            }
            if (S->core_forms_len >= cap) {
                cap *= 2;
                S->core_forms = realloc(S->core_forms,
                                        cap * sizeof(mino_val_t *));
                if (!S->core_forms) {
                    /* Class I: init-time; no try-frame to recover through */
                    fprintf(stderr, "core.mino: out of memory\n");
                    abort();
                }
            }
            S->core_forms[S->core_forms_len++] = form;
            if (mino_eval(S, form, env) == NULL) {
                /* Class I: core library eval failure is unrecoverable */
                fprintf(stderr, "core.mino eval error: %s\n",
                        mino_last_error(S));
                abort();
            }
            src = end;
        }
        S->reader_file = saved_file;
        S->reader_line = saved_line;
        return;
    }

    /* Subsequent calls: evaluate cached forms into the target env. */
    for (i = 0; i < S->core_forms_len; i++) {
        if (mino_eval(S, S->core_forms[i], env) == NULL) {
            /* Class I: cached core form eval failure is unrecoverable */
            fprintf(stderr, "core.mino eval error: %s\n", mino_last_error(S));
            abort();
        }
    }
}

/* --- Atom primitives --------------------------------------------------- */

mino_val_t *prim_atom(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "atom requires one argument");
        return NULL;
    }
    return mino_atom(S, args->as.cons.car);
}

mino_val_t *prim_deref(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *a;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "deref requires one argument");
        return NULL;
    }
    a = args->as.cons.car;
    if (a != NULL && a->type == MINO_ATOM) {
        return a->as.atom.val;
    }
    if (a != NULL && a->type == MINO_REDUCED) {
        return a->as.reduced.val;
    }
    set_error(S, "deref: expected an atom or reduced");
    return NULL;
}

mino_val_t *prim_reset_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *a, *val;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_error(S, "reset! requires two arguments");
        return NULL;
    }
    a   = args->as.cons.car;
    val = args->as.cons.cdr->as.cons.car;
    if (a == NULL || a->type != MINO_ATOM) {
        set_error(S, "reset!: first argument must be an atom");
        return NULL;
    }
    a->as.atom.val = val;
    return val;
}

/* (swap! atom f & args) — applies (f current-val args...) and sets result. */
mino_val_t *prim_swap_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *a, *fn, *cur, *call_args, *extra, *tail, *result;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "swap! requires at least 2 arguments: atom and function");
        return NULL;
    }
    a  = args->as.cons.car;
    fn = args->as.cons.cdr->as.cons.car;
    if (a == NULL || a->type != MINO_ATOM) {
        set_error(S, "swap!: first argument must be an atom");
        return NULL;
    }
    cur = a->as.atom.val;
    /* Build arg list: (cur extra1 extra2 ...) */
    call_args = mino_nil(S);
    /* Append extra args in reverse then prepend cur. */
    extra = args->as.cons.cdr->as.cons.cdr; /* rest after fn */
    if (extra != NULL && extra->type == MINO_CONS) {
        /* Collect extras into a list. */
        tail = mino_nil(S);
        while (extra != NULL && extra->type == MINO_CONS) {
            tail = mino_cons(S, extra->as.cons.car, tail);
            extra = extra->as.cons.cdr;
        }
        /* Reverse to get correct order. */
        call_args = mino_nil(S);
        while (tail != NULL && tail->type == MINO_CONS) {
            call_args = mino_cons(S, tail->as.cons.car, call_args);
            tail = tail->as.cons.cdr;
        }
    }
    call_args = mino_cons(S, cur, call_args);
    result = mino_call(S, fn, call_args, env);
    if (result == NULL) return NULL;
    a->as.atom.val = result;
    return result;
}

mino_val_t *prim_atom_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        set_error(S, "atom? requires one argument");
        return NULL;
    }
    return mino_is_atom(args->as.cons.car) ? mino_true(S) : mino_false(S);
}

/* ------------------------------------------------------------------------- */
/* Primitive registration                                                    */
/* ------------------------------------------------------------------------- */

void mino_install_core(mino_state_t *S, mino_env_t *env)
{
    volatile char probe = 0;
    gc_note_host_frame(S, (void *)&probe);
    (void)probe;
    mino_env_set(S, env, "+",        mino_prim(S, "+",        prim_add));
    mino_env_set(S, env, "-",        mino_prim(S, "-",        prim_sub));
    mino_env_set(S, env, "*",        mino_prim(S, "*",        prim_mul));
    mino_env_set(S, env, "/",        mino_prim(S, "/",        prim_div));
    mino_env_set(S, env, "=",        mino_prim(S, "=",        prim_eq));
    mino_env_set(S, env, "identical?", mino_prim(S, "identical?", prim_identical));
    mino_env_set(S, env, "meta",      mino_prim(S, "meta",      prim_meta));
    mino_env_set(S, env, "with-meta", mino_prim(S, "with-meta", prim_with_meta));
    mino_env_set(S, env, "vary-meta", mino_prim(S, "vary-meta", prim_vary_meta));
    mino_env_set(S, env, "alter-meta!", mino_prim(S, "alter-meta!", prim_alter_meta));
    mino_env_set(S, env, "<",        mino_prim(S, "<",        prim_lt));
    mino_env_set(S, env, "mod",      mino_prim(S, "mod",      prim_mod));
    mino_env_set(S, env, "rem",      mino_prim(S, "rem",      prim_rem));
    mino_env_set(S, env, "quot",     mino_prim(S, "quot",     prim_quot));
    /* math */
    mino_env_set(S, env, "math-floor", mino_prim(S, "math-floor", prim_math_floor));
    mino_env_set(S, env, "math-ceil",  mino_prim(S, "math-ceil",  prim_math_ceil));
    mino_env_set(S, env, "math-round", mino_prim(S, "math-round", prim_math_round));
    mino_env_set(S, env, "math-sqrt",  mino_prim(S, "math-sqrt",  prim_math_sqrt));
    mino_env_set(S, env, "math-pow",   mino_prim(S, "math-pow",   prim_math_pow));
    mino_env_set(S, env, "math-log",   mino_prim(S, "math-log",   prim_math_log));
    mino_env_set(S, env, "math-exp",   mino_prim(S, "math-exp",   prim_math_exp));
    mino_env_set(S, env, "math-sin",   mino_prim(S, "math-sin",   prim_math_sin));
    mino_env_set(S, env, "math-cos",   mino_prim(S, "math-cos",   prim_math_cos));
    mino_env_set(S, env, "math-tan",   mino_prim(S, "math-tan",   prim_math_tan));
    mino_env_set(S, env, "math-atan2", mino_prim(S, "math-atan2", prim_math_atan2));
    mino_env_set(S, env, "math-pi",    mino_float(S, 3.14159265358979323846));
    mino_env_set(S, env, "bit-and", mino_prim(S, "bit-and", prim_bit_and));
    mino_env_set(S, env, "bit-or",  mino_prim(S, "bit-or",  prim_bit_or));
    mino_env_set(S, env, "bit-xor", mino_prim(S, "bit-xor", prim_bit_xor));
    mino_env_set(S, env, "bit-not", mino_prim(S, "bit-not", prim_bit_not));
    mino_env_set(S, env, "bit-shift-left",
                 mino_prim(S, "bit-shift-left", prim_bit_shift_left));
    mino_env_set(S, env, "bit-shift-right",
                 mino_prim(S, "bit-shift-right", prim_bit_shift_right));
    mino_env_set(S, env, "car",      mino_prim(S, "car",      prim_car));
    mino_env_set(S, env, "cdr",      mino_prim(S, "cdr",      prim_cdr));
    mino_env_set(S, env, "cons",     mino_prim(S, "cons",     prim_cons));
    mino_env_set(S, env, "count",    mino_prim(S, "count",    prim_count));
    mino_env_set(S, env, "nth",      mino_prim(S, "nth",      prim_nth));
    mino_env_set(S, env, "first",    mino_prim(S, "first",    prim_first));
    mino_env_set(S, env, "rest",     mino_prim(S, "rest",     prim_rest));
    mino_env_set(S, env, "vector",   mino_prim(S, "vector",   prim_vector));
    mino_env_set(S, env, "hash-map", mino_prim(S, "hash-map", prim_hash_map));
    mino_env_set(S, env, "assoc",    mino_prim(S, "assoc",    prim_assoc));
    mino_env_set(S, env, "get",      mino_prim(S, "get",      prim_get));
    mino_env_set(S, env, "conj",     mino_prim(S, "conj",     prim_conj));
    mino_env_set(S, env, "keys",     mino_prim(S, "keys",     prim_keys));
    mino_env_set(S, env, "vals",     mino_prim(S, "vals",     prim_vals));
    mino_env_set(S, env, "macroexpand-1",
                 mino_prim(S, "macroexpand-1", prim_macroexpand_1));
    mino_env_set(S, env, "macroexpand",
                 mino_prim(S, "macroexpand", prim_macroexpand));
    mino_env_set(S, env, "gensym",   mino_prim(S, "gensym",   prim_gensym));
    mino_env_set(S, env, "type",     mino_prim(S, "type",     prim_type));
    mino_env_set(S, env, "name",     mino_prim(S, "name",     prim_name));
    mino_env_set(S, env, "rand",     mino_prim(S, "rand",     prim_rand));
    /* regex */
    mino_env_set(S, env, "re-find",    mino_prim(S, "re-find",    prim_re_find));
    mino_env_set(S, env, "re-matches", mino_prim(S, "re-matches", prim_re_matches));
    mino_env_set(S, env, "eval",     mino_prim(S, "eval",     prim_eval));
    mino_env_set(S, env, "symbol",   mino_prim(S, "symbol",   prim_symbol));
    mino_env_set(S, env, "keyword",  mino_prim(S, "keyword",  prim_keyword));
    mino_env_set(S, env, "hash",     mino_prim(S, "hash",     prim_hash));
    mino_env_set(S, env, "compare",  mino_prim(S, "compare",  prim_compare));
    mino_env_set(S, env, "int",      mino_prim(S, "int",      prim_int));
    mino_env_set(S, env, "float",    mino_prim(S, "float",    prim_float));
    mino_env_set(S, env, "str",      mino_prim(S, "str",      prim_str));
    mino_env_set(S, env, "pr-str",   mino_prim(S, "pr-str",   prim_pr_str));
    mino_env_set(S, env, "read-string",
                 mino_prim(S, "read-string", prim_read_string));
    mino_env_set(S, env, "format",   mino_prim(S, "format",   prim_format));
    mino_env_set(S, env, "throw",    mino_prim(S, "throw",    prim_throw));
    mino_env_set(S, env, "require",  mino_prim(S, "require",  prim_require));
    mino_env_set(S, env, "doc",      mino_prim(S, "doc",      prim_doc));
    mino_env_set(S, env, "source",   mino_prim(S, "source",   prim_source));
    mino_env_set(S, env, "apropos",  mino_prim(S, "apropos",  prim_apropos));
    /* set operations */
    mino_env_set(S, env, "hash-set", mino_prim(S, "hash-set", prim_hash_set));
    mino_env_set(S, env, "set",      mino_prim(S, "set",      prim_set));
    mino_env_set(S, env, "contains?",mino_prim(S, "contains?",prim_contains_p));
    mino_env_set(S, env, "disj",     mino_prim(S, "disj",     prim_disj));
    mino_env_set(S, env, "dissoc",   mino_prim(S, "dissoc",   prim_dissoc));
    /* sequence operations (map, filter, take, drop, range, repeat,
       concat are now lazy in core.mino) */
    mino_env_set(S, env, "reduce",   mino_prim(S, "reduce",   prim_reduce));
    mino_env_set(S, env, "reduced",  mino_prim(S, "reduced",  prim_reduced));
    mino_env_set(S, env, "reduced?", mino_prim(S, "reduced?", prim_reduced_p));
    mino_env_set(S, env, "into",     mino_prim(S, "into",     prim_into));
    /* eager collection builders -- bypass lazy thunk overhead */
    mino_env_set(S, env, "rangev",   mino_prim(S, "rangev",   prim_rangev));
    mino_env_set(S, env, "mapv",     mino_prim(S, "mapv",     prim_mapv));
    mino_env_set(S, env, "filterv",  mino_prim(S, "filterv",  prim_filterv));
    mino_env_set(S, env, "apply",    mino_prim(S, "apply",    prim_apply));
    mino_env_set(S, env, "reverse",  mino_prim(S, "reverse",  prim_reverse));
    mino_env_set(S, env, "sort",     mino_prim(S, "sort",     prim_sort));
    /* string operations */
    mino_env_set(S, env, "subs",     mino_prim(S, "subs",     prim_subs));
    mino_env_set(S, env, "split",    mino_prim(S, "split",    prim_split));
    mino_env_set(S, env, "join",     mino_prim(S, "join",     prim_join));
    mino_env_set(S, env, "starts-with?",
                 mino_prim(S, "starts-with?", prim_starts_with_p));
    mino_env_set(S, env, "ends-with?",
                 mino_prim(S, "ends-with?", prim_ends_with_p));
    mino_env_set(S, env, "includes?",
                 mino_prim(S, "includes?", prim_includes_p));
    mino_env_set(S, env, "upper-case",
                 mino_prim(S, "upper-case", prim_upper_case));
    mino_env_set(S, env, "lower-case",
                 mino_prim(S, "lower-case", prim_lower_case));
    mino_env_set(S, env, "trim",     mino_prim(S, "trim",     prim_trim));
    mino_env_set(S, env, "char-at",  mino_prim(S, "char-at",  prim_char_at));
    /* (some and every? are now in core.mino) */
    /* sequences */
    mino_env_set(S, env, "seq",       mino_prim(S, "seq",       prim_seq));
    mino_env_set(S, env, "realized?", mino_prim(S, "realized?", prim_realized_p));
    /* atoms */
    mino_env_set(S, env, "atom",     mino_prim(S, "atom",     prim_atom));
    mino_env_set(S, env, "deref",    mino_prim(S, "deref",    prim_deref));
    mino_env_set(S, env, "reset!",   mino_prim(S, "reset!",   prim_reset_bang));
    mino_env_set(S, env, "swap!",    mino_prim(S, "swap!",    prim_swap_bang));
    mino_env_set(S, env, "atom?",    mino_prim(S, "atom?",    prim_atom_p));
    /* actors */
    mino_env_set(S, env, "spawn*",   mino_prim(S, "spawn*",   prim_spawn));
    mino_env_set(S, env, "send!",    mino_prim(S, "send!",    prim_send_bang));
    mino_env_set(S, env, "receive",  mino_prim(S, "receive",  prim_receive));
    install_core_mino(S, env);
}

void mino_install_io(mino_state_t *S, mino_env_t *env)
{
    mino_env_set(S, env, "println",  mino_prim(S, "println",  prim_println));
    mino_env_set(S, env, "prn",      mino_prim(S, "prn",      prim_prn));
    mino_env_set(S, env, "slurp",    mino_prim(S, "slurp",    prim_slurp));
    mino_env_set(S, env, "spit",     mino_prim(S, "spit",     prim_spit));
    mino_env_set(S, env, "exit",     mino_prim(S, "exit",     prim_exit));
    mino_env_set(S, env, "time-ms",  mino_prim(S, "time-ms",  prim_time_ms));
}
