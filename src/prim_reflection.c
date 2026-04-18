/*
 * prim_reflection.c -- reflection, introspection, and utility primitives.
 *
 * Extracted from prim.c. No behavior change.
 */

#include "prim_internal.h"

mino_val_t *prim_name(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_error(S, "name requires one argument");
    }
    v = args->as.cons.car;
    if (v == NULL || v->type == MINO_NIL) return mino_nil(S);
    if (v->type == MINO_STRING)  return v;
    if (v->type == MINO_KEYWORD || v->type == MINO_SYMBOL) {
        const char *data = v->as.s.data;
        size_t len = v->as.s.len;
        /* For qualified names (foo/bar), return only the part after /. */
        if (len > 1) {
            const char *slash = memchr(data, '/', len);
            if (slash != NULL) {
                size_t after = len - (size_t)(slash - data) - 1;
                return mino_string_n(S, slash + 1, after);
            }
        }
        return mino_string_n(S, data, len);
    }
    return prim_throw_error(S, "name: expected a keyword, symbol, or string");
}

mino_val_t *prim_rand(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_error(S, "rand takes no arguments");
    }
    return mino_float(S, (double)rand() / ((double)RAND_MAX + 1.0));
}

mino_val_t *prim_eval(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_error(S, "eval requires one argument");
    }
    return eval_value(S, args->as.cons.car, env);
}

mino_val_t *prim_symbol(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_error(S, "symbol requires 1 or 2 arguments");
    }
    /* 2-arg: (symbol ns name) */
    if (mino_is_cons(args->as.cons.cdr)) {
        mino_val_t *ns_arg  = args->as.cons.car;
        mino_val_t *name_arg = args->as.cons.cdr->as.cons.car;
        char buf[512];
        size_t pos = 0;
        if (name_arg == NULL || name_arg->type != MINO_STRING) {
            return prim_throw_error(S, "symbol: name must be a string");
        }
        if (ns_arg != NULL && ns_arg->type == MINO_STRING
            && ns_arg->as.s.len > 0) {
            if (ns_arg->as.s.len + 1 + name_arg->as.s.len >= sizeof(buf)) {
                return prim_throw_error(S, "symbol: name too long");
            }
            memcpy(buf, ns_arg->as.s.data, ns_arg->as.s.len);
            pos = ns_arg->as.s.len;
            buf[pos++] = '/';
        } else if (ns_arg != NULL && ns_arg->type != MINO_NIL
                   && ns_arg->type != MINO_STRING) {
            return prim_throw_error(S, "symbol: namespace must be a string");
        }
        if (pos + name_arg->as.s.len >= sizeof(buf)) {
            return prim_throw_error(S, "symbol: name too long");
        }
        memcpy(buf + pos, name_arg->as.s.data, name_arg->as.s.len);
        pos += name_arg->as.s.len;
        return mino_symbol_n(S, buf, pos);
    }
    /* 1-arg */
    v = args->as.cons.car;
    if (v == NULL || v->type == MINO_NIL) {
        return prim_throw_error(S, "symbol: argument must not be nil");
    }
    if (v->type == MINO_STRING) {
        return mino_symbol_n(S, v->as.s.data, v->as.s.len);
    }
    if (v->type == MINO_SYMBOL) {
        return v;
    }
    if (v->type == MINO_KEYWORD) {
        return mino_symbol_n(S, v->as.s.data, v->as.s.len);
    }
    return prim_throw_error(S, "symbol: argument must be a string, symbol, or keyword");
}

mino_val_t *prim_keyword(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_error(S, "keyword requires one argument");
    }
    v = args->as.cons.car;
    if (v != NULL && v->type == MINO_STRING) {
        return mino_keyword_n(S, v->as.s.data, v->as.s.len);
    }
    if (v != NULL && v->type == MINO_KEYWORD) {
        return v;
    }
    return prim_throw_error(S, "keyword: argument must be a string");
}

mino_val_t *prim_hash(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_error(S, "hash requires one argument");
    }
    return mino_int(S, (long long)hash_val(args->as.cons.car));
}

mino_val_t *prim_type(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_error(S, "type requires one argument");
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
    case MINO_SET:        return mino_keyword(S, "set");
    case MINO_SORTED_MAP: return mino_keyword(S, "sorted-map");
    case MINO_SORTED_SET: return mino_keyword(S, "sorted-set");
    case MINO_PRIM:    return mino_keyword(S, "fn");
    case MINO_FN:      return mino_keyword(S, "fn");
    case MINO_MACRO:   return mino_keyword(S, "macro");
    case MINO_HANDLE:  return mino_keyword(S, "handle");
    case MINO_ATOM:    return mino_keyword(S, "atom");
    case MINO_LAZY:    return mino_keyword(S, "lazy-seq");
    case MINO_RECUR:     return mino_keyword(S, "recur");
    case MINO_TAIL_CALL: return mino_keyword(S, "tail-call");
    case MINO_REDUCED:   return mino_keyword(S, "reduced");
    case MINO_VAR:       return mino_keyword(S, "var");
    }
    return mino_keyword(S, "unknown");
}

mino_val_t *prim_macroexpand_1(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    int expanded;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_error(S, "macroexpand-1 requires one argument");
    }
    return macroexpand1(S, args->as.cons.car, env, &expanded);
}

mino_val_t *prim_macroexpand(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_error(S, "macroexpand requires one argument");
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
        return prim_throw_error(S, "gensym takes 0 or 1 arguments");
    }
    if (nargs == 1) {
        mino_val_t *p = args->as.cons.car;
        if (p == NULL || p->type != MINO_STRING) {
            return prim_throw_error(S, "gensym prefix must be a string");
        }
        prefix_src = p->as.s.data;
        prefix_len = p->as.s.len;
        if (prefix_len >= sizeof(buf) - 32) {
            return prim_throw_error(S, "gensym prefix too long");
        }
    }
    {
        int used;
        memcpy(buf, prefix_src, prefix_len);
        used = snprintf(buf + prefix_len, sizeof(buf) - prefix_len,
                        "%ld", ++S->gensym_counter);
        if (used < 0) {
            return prim_throw_error(S, "gensym formatting failed");
        }
        return mino_symbol_n(S, buf, prefix_len + (size_t)used);
    }
}

/* (throw value) -- raise a script exception. */
mino_val_t *prim_throw(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *ex;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_error(S, "throw requires one argument");
    }
    ex = args->as.cons.car;
    if (S->try_depth <= 0) {
        /* No enclosing try -- format as fatal error. */
        char msg[512];
        if (ex != NULL && ex->type == MINO_STRING) {
            snprintf(msg, sizeof(msg), "unhandled exception: %.*s",
                     (int)ex->as.s.len, ex->as.s.data);
        } else {
            snprintf(msg, sizeof(msg), "unhandled exception");
        }
        return prim_throw_error(S, msg);
    }
    S->try_stack[S->try_depth - 1].exception = ex;
    longjmp(S->try_stack[S->try_depth - 1].buf, 1);
    return NULL; /* unreachable */
}

mino_val_t *prim_var_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_error(S, "var? requires one argument");
    }
    v = args->as.cons.car;
    return (v != NULL && v->type == MINO_VAR) ? mino_true(S) : mino_false(S);
}

mino_val_t *prim_resolve(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *sym;
    char buf[256];
    size_t n;
    const char *slash;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_error(S, "resolve requires one argument");
    }
    sym = args->as.cons.car;
    if (sym == NULL || sym->type != MINO_SYMBOL) {
        return prim_throw_error(S, "resolve: argument must be a symbol");
    }
    n = sym->as.s.len;
    if (n >= sizeof(buf)) return mino_nil(S);
    memcpy(buf, sym->as.s.data, n);
    buf[n] = '\0';

    /* Qualified symbol: ns/name */
    slash = (n > 1) ? strchr(buf, '/') : NULL;
    if (slash != NULL) {
        char ns_buf[256];
        size_t ns_len = (size_t)(slash - buf);
        const char *sym_name = slash + 1;
        const char *resolved_ns;
        mino_val_t *var;
        size_t i;

        memcpy(ns_buf, buf, ns_len);
        ns_buf[ns_len] = '\0';

        /* Check alias table. */
        for (i = 0; i < S->ns_alias_len; i++) {
            if (strcmp(S->ns_aliases[i].alias, ns_buf) == 0) {
                resolved_ns = S->ns_aliases[i].full_name;
                goto found_ns;
            }
        }
        resolved_ns = ns_buf;
found_ns:
        var = var_find(S, resolved_ns, sym_name);
        return var != NULL ? var : mino_nil(S);
    }

    /* Unqualified: try current ns, then "user", then scan all vars. */
    {
        mino_val_t *var = var_find(S, S->current_ns, buf);
        if (var == NULL) var = var_find(S, "user", buf);
        if (var == NULL) {
            size_t i;
            for (i = 0; i < S->var_registry_len; i++) {
                if (strcmp(S->var_registry[i].name, buf) == 0) {
                    return S->var_registry[i].var;
                }
            }
        }
        if (var != NULL) return var;
        /* Fallback: check root env for C primitives (no var object). */
        {
            mino_val_t *val = mino_env_get(env, buf);
            if (val != NULL) {
                /* Auto-create a var for this binding. */
                var = var_intern(S, "mino.core", buf);
                var_set_root(S, var, val);
                return var;
            }
        }
        return mino_nil(S);
    }
}

mino_val_t *prim_namespace(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    const char *data;
    size_t len;
    const char *slash;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_error(S, "namespace requires one argument");
    }
    v = args->as.cons.car;
    if (v == NULL || v->type == MINO_NIL) {
        return prim_throw_error(S, "namespace: argument must not be nil");
    }
    if (v->type != MINO_SYMBOL && v->type != MINO_KEYWORD) {
        return prim_throw_error(S, "namespace: expected a symbol or keyword");
    }
    data = v->as.s.data;
    len  = v->as.s.len;
    slash = memchr(data, '/', len);
    if (slash == NULL || len == 1) return mino_nil(S);
    return mino_string_n(S, data, (size_t)(slash - data));
}
