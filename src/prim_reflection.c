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
        return prim_throw_classified(S, "eval/arity", "MAR001", "name requires one argument");
    }
    v = args->as.cons.car;
    if (v == NULL || v->type == MINO_NIL)
        return prim_throw_classified(S, "eval/type", "MTY001", "name: argument must not be nil");
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
    return prim_throw_classified(S, "eval/type", "MTY001", "name: expected a keyword, symbol, or string");
}

mino_val_t *prim_rand(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    double r;
    (void)env;
    if (mino_is_cons(args) && mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "rand takes zero or one argument");
    }
    r = (double)rand() / ((double)RAND_MAX + 1.0);
    if (mino_is_cons(args)) {
        double n;
        if (!as_double(args->as.cons.car, &n)) {
            return prim_throw_classified(S, "eval/type", "MTY001", "rand expects a number");
        }
        r *= n;
    }
    return mino_float(S, r);
}

mino_val_t *prim_eval(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "eval requires one argument");
    }
    return eval_value(S, args->as.cons.car, env);
}

mino_val_t *prim_symbol(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "symbol requires 1 or 2 arguments");
    }
    /* 2-arg: (symbol ns name) */
    if (mino_is_cons(args->as.cons.cdr)) {
        mino_val_t *ns_arg  = args->as.cons.car;
        mino_val_t *name_arg = args->as.cons.cdr->as.cons.car;
        char buf[512];
        size_t pos = 0;
        if (name_arg == NULL || name_arg->type != MINO_STRING) {
            return prim_throw_classified(S, "eval/type", "MTY001", "symbol: name must be a string");
        }
        if (ns_arg != NULL && ns_arg->type == MINO_STRING
            && ns_arg->as.s.len > 0) {
            if (ns_arg->as.s.len + 1 + name_arg->as.s.len >= sizeof(buf)) {
                return prim_throw_classified(S, "eval/type", "MTY001", "symbol: name too long");
            }
            memcpy(buf, ns_arg->as.s.data, ns_arg->as.s.len);
            pos = ns_arg->as.s.len;
            buf[pos++] = '/';
        } else if (ns_arg != NULL && ns_arg->type != MINO_NIL
                   && ns_arg->type != MINO_STRING) {
            return prim_throw_classified(S, "eval/type", "MTY001", "symbol: namespace must be a string");
        }
        if (pos + name_arg->as.s.len >= sizeof(buf)) {
            return prim_throw_classified(S, "eval/type", "MTY001", "symbol: name too long");
        }
        memcpy(buf + pos, name_arg->as.s.data, name_arg->as.s.len);
        pos += name_arg->as.s.len;
        return mino_symbol_n(S, buf, pos);
    }
    /* 1-arg */
    v = args->as.cons.car;
    if (v == NULL || v->type == MINO_NIL) {
        return prim_throw_classified(S, "eval/type", "MTY001", "symbol: argument must not be nil");
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
    return prim_throw_classified(S, "eval/type", "MTY001", "symbol: argument must be a string, symbol, or keyword");
}

mino_val_t *prim_keyword(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "keyword requires one or two arguments");
    }
    /* 2-arity: (keyword ns name) */
    if (mino_is_cons(args->as.cons.cdr)) {
        mino_val_t *ns_val  = args->as.cons.car;
        mino_val_t *nm_val  = args->as.cons.cdr->as.cons.car;
        char buf[256];
        if (mino_is_cons(args->as.cons.cdr->as.cons.cdr))
            return prim_throw_classified(S, "eval/arity", "MAR001", "keyword requires one or two arguments");
        if (nm_val->type != MINO_STRING)
            return prim_throw_classified(S, "eval/type", "MTY001", "keyword: name must be a string");
        if (ns_val == NULL || ns_val->type == MINO_NIL) {
            return mino_keyword_n(S, nm_val->as.s.data, nm_val->as.s.len);
        }
        if (ns_val->type != MINO_STRING)
            return prim_throw_classified(S, "eval/type", "MTY001", "keyword: namespace must be a string or nil");
        snprintf(buf, sizeof(buf), "%.*s/%.*s",
                 (int)ns_val->as.s.len, ns_val->as.s.data,
                 (int)nm_val->as.s.len, nm_val->as.s.data);
        return mino_keyword_n(S, buf, strlen(buf));
    }
    v = args->as.cons.car;
    if (v == NULL || v->type == MINO_NIL)
        return mino_nil(S);
    if (v->type == MINO_STRING)
        return mino_keyword_n(S, v->as.s.data, v->as.s.len);
    if (v->type == MINO_KEYWORD)
        return v;
    if (v->type == MINO_SYMBOL)
        return mino_keyword_n(S, v->as.s.data, v->as.s.len);
    return prim_throw_classified(S, "eval/type", "MTY001", "keyword: argument must be a string, keyword, or symbol");
}

mino_val_t *prim_hash(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "hash requires one argument");
    }
    return mino_int(S, (long long)hash_val(args->as.cons.car));
}

mino_val_t *prim_type(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "type requires one argument");
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

/* Type-predicate primitives. Each mirrors the mino-level
 * `(fn [x] (= (type x) :foo))` form but skips the keyword allocation
 * and equality comparison by checking the tag directly. */

#define DEFINE_TYPE_PRED(fn_name, pred_expr, label) \
    mino_val_t *fn_name(mino_state_t *S, mino_val_t *args, mino_env_t *env) \
    { \
        mino_val_t *v; \
        (void)env; \
        if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) { \
            return prim_throw_classified(S, "eval/arity", "MAR001", \
                label " requires one argument"); \
        } \
        v = args->as.cons.car; \
        return (pred_expr) ? mino_true(S) : mino_false(S); \
    }

DEFINE_TYPE_PRED(prim_nil_p,     (v == NULL || v->type == MINO_NIL),           "nil?")
DEFINE_TYPE_PRED(prim_cons_p,    (v != NULL && v->type == MINO_CONS),          "cons?")
DEFINE_TYPE_PRED(prim_vector_p,  (v != NULL && v->type == MINO_VECTOR),        "vector?")
DEFINE_TYPE_PRED(prim_int_p,     (v != NULL && v->type == MINO_INT),           "int?")
DEFINE_TYPE_PRED(prim_float_p,   (v != NULL && v->type == MINO_FLOAT),         "float?")
DEFINE_TYPE_PRED(prim_string_p,  (v != NULL && v->type == MINO_STRING),        "string?")
DEFINE_TYPE_PRED(prim_keyword_p, (v != NULL && v->type == MINO_KEYWORD),       "keyword?")
DEFINE_TYPE_PRED(prim_symbol_p,  (v != NULL && v->type == MINO_SYMBOL),        "symbol?")
DEFINE_TYPE_PRED(prim_fn_p,      (v != NULL && (v->type == MINO_FN || v->type == MINO_PRIM)), "fn?")
DEFINE_TYPE_PRED(prim_char_p,    0, "char?") /* mino has no separate char type */
DEFINE_TYPE_PRED(prim_number_p,  (v != NULL && (v->type == MINO_INT || v->type == MINO_FLOAT)), "number?")
DEFINE_TYPE_PRED(prim_map_p,     (v != NULL && (v->type == MINO_MAP || v->type == MINO_SORTED_MAP)), "map?")
DEFINE_TYPE_PRED(prim_set_p,     (v != NULL && (v->type == MINO_SET || v->type == MINO_SORTED_SET)), "set?")
DEFINE_TYPE_PRED(prim_seq_p,     (v != NULL && (v->type == MINO_CONS || v->type == MINO_LAZY)), "seq?")
DEFINE_TYPE_PRED(prim_boolean_p, (v != NULL && v->type == MINO_BOOL),          "boolean?")
DEFINE_TYPE_PRED(prim_true_p,    (v != NULL && v->type == MINO_BOOL && v->as.b != 0),  "true?")
DEFINE_TYPE_PRED(prim_false_p,   (v != NULL && v->type == MINO_BOOL && v->as.b == 0),  "false?")

#undef DEFINE_TYPE_PRED

mino_val_t *prim_not(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "not requires one argument");
    }
    return mino_is_truthy(args->as.cons.car) ? mino_false(S) : mino_true(S);
}

mino_val_t *prim_some_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "some? requires one argument");
    }
    v = args->as.cons.car;
    return (v != NULL && v->type != MINO_NIL) ? mino_true(S) : mino_false(S);
}

mino_val_t *prim_empty_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "empty? requires one argument");
    }
    v = args->as.cons.car;
    if (v == NULL || v->type == MINO_NIL) return mino_true(S);
    switch (v->type) {
    case MINO_CONS:       return mino_false(S);
    case MINO_VECTOR:     return v->as.vec.len == 0 ? mino_true(S) : mino_false(S);
    case MINO_MAP:        return v->as.map.len == 0 ? mino_true(S) : mino_false(S);
    case MINO_SET:        return v->as.set.len == 0 ? mino_true(S) : mino_false(S);
    case MINO_SORTED_MAP:
    case MINO_SORTED_SET: return v->as.sorted.len == 0 ? mino_true(S) : mino_false(S);
    case MINO_STRING:     return v->as.s.len == 0 ? mino_true(S) : mino_false(S);
    case MINO_LAZY: {
        mino_val_t *forced = lazy_force(S, v);
        if (forced == NULL) return NULL;
        return (forced == NULL || forced->type == MINO_NIL)
                ? mino_true(S) : mino_false(S);
    }
    default:
        return prim_throw_classified(S, "eval/type", "MTY001",
            "empty? expects a collection or nil");
    }
}

/* Numeric predicates. The mino-level versions invoke number? then compare,
 * doing two prim dispatches per call; inlining the check here is a direct
 * tag read plus a comparison. */
static mino_val_t *num_pred(mino_state_t *S, mino_val_t *args,
                            const char *name, int (*cmp_int)(long long),
                            int (*cmp_float)(double))
{
    mino_val_t *v;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%s requires one argument", name);
        return prim_throw_classified(S, "eval/arity", "MAR001", buf);
    }
    v = args->as.cons.car;
    if (v == NULL) goto type_err;
    if (v->type == MINO_INT)   return cmp_int(v->as.i) ? mino_true(S) : mino_false(S);
    if (v->type == MINO_FLOAT) return cmp_float(v->as.f) ? mino_true(S) : mino_false(S);
type_err:
    {
        char buf[96];
        snprintf(buf, sizeof(buf), "%s requires a number", name);
        return prim_throw_classified(S, "eval/type", "MTY001", buf);
    }
}

static int is_zero_i(long long n) { return n == 0; }
static int is_zero_f(double   x) { return x == 0.0; }
static int is_pos_i(long long n)  { return n > 0; }
static int is_pos_f(double   x)  { return x > 0.0; }
static int is_neg_i(long long n)  { return n < 0; }
static int is_neg_f(double   x)  { return x < 0.0; }

mino_val_t *prim_zero_p(mino_state_t *S, mino_val_t *args, mino_env_t *env) {
    (void)env; return num_pred(S, args, "zero?", is_zero_i, is_zero_f);
}
mino_val_t *prim_pos_p(mino_state_t *S, mino_val_t *args, mino_env_t *env) {
    (void)env; return num_pred(S, args, "pos?", is_pos_i, is_pos_f);
}
mino_val_t *prim_neg_p(mino_state_t *S, mino_val_t *args, mino_env_t *env) {
    (void)env; return num_pred(S, args, "neg?", is_neg_i, is_neg_f);
}

mino_val_t *prim_odd_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "odd? requires one argument");
    }
    v = args->as.cons.car;
    if (v == NULL || v->type != MINO_INT) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "odd? requires an integer");
    }
    return (v->as.i & 1LL) != 0 ? mino_true(S) : mino_false(S);
}

mino_val_t *prim_even_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "even? requires one argument");
    }
    v = args->as.cons.car;
    if (v == NULL || v->type != MINO_INT) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "even? requires an integer");
    }
    return (v->as.i & 1LL) == 0 ? mino_true(S) : mino_false(S);
}

mino_val_t *prim_macroexpand_1(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    int expanded;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "macroexpand-1 requires one argument");
    }
    return macroexpand1(S, args->as.cons.car, env, &expanded);
}

mino_val_t *prim_macroexpand(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "macroexpand requires one argument");
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
        return prim_throw_classified(S, "eval/arity", "MAR001", "gensym takes 0 or 1 arguments");
    }
    if (nargs == 1) {
        mino_val_t *p = args->as.cons.car;
        if (p == NULL || p->type != MINO_STRING) {
            return prim_throw_classified(S, "eval/type", "MTY001", "gensym prefix must be a string");
        }
        prefix_src = p->as.s.data;
        prefix_len = p->as.s.len;
        if (prefix_len >= sizeof(buf) - 32) {
            return prim_throw_classified(S, "eval/type", "MTY001", "gensym prefix too long");
        }
    }
    {
        int used;
        memcpy(buf, prefix_src, prefix_len);
        used = snprintf(buf + prefix_len, sizeof(buf) - prefix_len,
                        "%ld", ++S->gensym_counter);
        if (used < 0) {
            return prim_throw_classified(S, "eval/type", "MTY001", "gensym formatting failed");
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
        return prim_throw_classified(S, "eval/arity", "MAR001", "throw requires one argument");
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
        return prim_throw_classified(S, "user", "MUS001", msg);
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
        return prim_throw_classified(S, "eval/arity", "MAR001", "var? requires one argument");
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
        return prim_throw_classified(S, "eval/arity", "MAR001", "resolve requires one argument");
    }
    sym = args->as.cons.car;
    if (sym == NULL || sym->type != MINO_SYMBOL) {
        return prim_throw_classified(S, "eval/type", "MTY001", "resolve: argument must be a symbol");
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
        return prim_throw_classified(S, "eval/arity", "MAR001", "namespace requires one argument");
    }
    v = args->as.cons.car;
    if (v == NULL || v->type == MINO_NIL) {
        return prim_throw_classified(S, "eval/type", "MTY001", "namespace: argument must not be nil");
    }
    if (v->type != MINO_SYMBOL && v->type != MINO_KEYWORD) {
        return prim_throw_classified(S, "eval/type", "MTY001", "namespace: expected a symbol or keyword");
    }
    data = v->as.s.data;
    len  = v->as.s.len;
    slash = memchr(data, '/', len);
    if (slash == NULL || len == 1) return mino_nil(S);
    return mino_string_n(S, data, (size_t)(slash - data));
}

/* (last-error) -- return the last diagnostic as a map, or nil. */
mino_val_t *prim_last_error(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)args; (void)env;
    return mino_last_error_map(S);
}

/* (error? x) -- true if x is a map with :mino/kind. */
mino_val_t *prim_error_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v, *kind_key;
    (void)env;
    if (!mino_is_cons(args)) return mino_false(S);
    v = args->as.cons.car;
    if (v == NULL || v->type != MINO_MAP) return mino_false(S);
    kind_key = mino_keyword(S, "mino/kind");
    return map_get_val(v, kind_key) != NULL ? mino_true(S) : mino_false(S);
}

/* (ex-data e) -- extract :mino/data from a diagnostic map. */
mino_val_t *prim_ex_data(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v, *data_key, *result;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "ex-data requires one argument");
    }
    v = args->as.cons.car;
    if (v == NULL || v->type != MINO_MAP) return mino_nil(S);
    data_key = mino_keyword(S, "mino/data");
    result = map_get_val(v, data_key);
    return result != NULL ? result : mino_nil(S);
}

/* (ex-message e) -- extract :mino/message from a diagnostic map. */
mino_val_t *prim_ex_message(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *v, *msg_key, *result;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "ex-message requires one argument");
    }
    v = args->as.cons.car;
    if (v == NULL || v->type != MINO_MAP) return mino_nil(S);
    msg_key = mino_keyword(S, "mino/message");
    result = map_get_val(v, msg_key);
    return result != NULL ? result : mino_nil(S);
}

/* (gc-stats) — return a map of GC statistics:
 *   {:collections N :bytes-live N :bytes-alloc N :bytes-freed N :threshold N
 *    :total-gc-ns N :max-gc-ns N} */
mino_val_t *prim_gc_stats(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *ks[7];
    mino_val_t *vs[7];
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "gc-stats takes no arguments");
    }
    ks[0] = mino_keyword(S, "collections");
    vs[0] = mino_int(S, (long long)S->gc_collections);
    ks[1] = mino_keyword(S, "bytes-live");
    vs[1] = mino_int(S, (long long)S->gc_bytes_live);
    ks[2] = mino_keyword(S, "bytes-alloc");
    vs[2] = mino_int(S, (long long)S->gc_bytes_alloc);
    ks[3] = mino_keyword(S, "bytes-freed");
    vs[3] = mino_int(S, (long long)S->gc_total_freed);
    ks[4] = mino_keyword(S, "threshold");
    vs[4] = mino_int(S, (long long)S->gc_threshold);
    ks[5] = mino_keyword(S, "total-gc-ns");
    vs[5] = mino_int(S, (long long)S->gc_total_ns);
    ks[6] = mino_keyword(S, "max-gc-ns");
    vs[6] = mino_int(S, (long long)S->gc_max_ns);
    return mino_map(S, ks, vs, 7);
}
