/*
 * reflection.c -- reflection, introspection, and utility primitives.
 */

#include "prim/internal.h"
#include "imath.h"

mino_val *prim_name(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "name requires one argument");
    }
    v = args->as.cons.car;
    if (v == NULL || mino_type_of(v) == MINO_NIL)
        return prim_throw_classified(S, "eval/type", "MTY001", "name: argument must not be nil");
    if (mino_type_of(v) == MINO_STRING)  return v;
    if (mino_type_of(v) == MINO_KEYWORD || mino_type_of(v) == MINO_SYMBOL) {
        const char *data = v->as.s.data;
        size_t len = v->as.s.len;
        size_t ns_len = v->as.s.ns_len;
        /* Use the explicit ns_len stored at construction time so
         * `(name (keyword "a" "b/c"))` returns "b/c", distinct from
         * `(name (keyword "a/b" "c"))` which returns "c". */
        if (ns_len > 0 && ns_len < len) {
            return mino_string_n(S, data + ns_len + 1, len - ns_len - 1);
        }
        /* Empty-string namespace encodes as a leading '/' with
         * ns_len == 0 and len > 1; the name is everything after that
         * slash. JVM Clojure: `(name (keyword "" "hi"))` is "hi",
         * not "/hi". Reserve data == "/" alone (len == 1) for the
         * bare-slash literal `:/`, whose name is "/" and namespace
         * is nil. */
        if (ns_len == 0 && len > 1 && data[0] == '/') {
            return mino_string_n(S, data + 1, len - 1);
        }
        return mino_string_n(S, data, len);
    }
    {
        char        buf[256];
        mino_val *printed = print_to_string(S, v);
        snprintf(buf, sizeof(buf),
                 "name: expected a keyword, symbol, or string, got: %.*s",
                 printed != NULL ? (int)printed->as.s.len : 0,
                 printed != NULL ? printed->as.s.data : "");
        return prim_throw_classified(S, "eval/type", "MTY001", buf);
    }
}

mino_val *prim_rand(mino_state *S, mino_val *args, mino_env *env)
{
    double r;
    (void)env;
    if (mino_is_cons(args) && mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "rand takes zero or one argument");
    }
    /* 53-bit mantissa out of our 64-bit xorshift result. */
    r = (double)(state_rand64(S) >> 11) * (1.0 / 9007199254740992.0);
    if (mino_is_cons(args)) {
        double n;
        if (!as_double(args->as.cons.car, &n)) {
            return prim_throw_classified(S, "eval/type", "MTY001", "rand expects a number");
        }
        r *= n;
    }
    return mino_float(S, r);
}

/* (random-seed! seed) -- seed the per-state PRNG to a known value
 * so subsequent rand / rand-int / rand-nth calls produce a
 * reproducible stream. Returns the seed. The seed must be a
 * non-zero integer; a zero seed degenerates the xorshift step, so
 * we re-seed to a fixed non-zero constant in that case. */
static mino_val *prim_random_seed_bang(mino_state *S, mino_val *args,
                                  mino_env *env)
{
    mino_val *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "random-seed! requires one argument");
    }
    v = args->as.cons.car;
    if (v == NULL || !mino_val_int_p(v)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "random-seed! seed must be an integer");
    }
    {
        uint64_t seed = (uint64_t)mino_val_int_get(v);
        if (seed == 0) seed = 0x243F6A8885A308D3ULL;
        S->rand_state = seed;
    }
    return v;
}

mino_val *prim_eval(mino_state *S, mino_val *args, mino_env *env)
{
    /* eval evaluates in the current namespace, not the calling fn's
     * ambient namespace. Clear fn_ambient_ns for the duration of the
     * eval so the form sees only its own current_ns + lexical chain,
     * matching Clojure's "*ns* is what counts" semantics. */
    const char *saved_ambient;
    mino_val *result;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "eval requires one argument");
    }
    saved_ambient = S->ns_vars.fn_ambient_ns;
    S->ns_vars.fn_ambient_ns = NULL;
    result = eval_value(S, args->as.cons.car, env);
    S->ns_vars.fn_ambient_ns = saved_ambient;
    return result;
}

mino_val *prim_load_string(mino_state *S, mino_val *args, mino_env *env)
{
    /* Read and eval all forms in the given source string; return the
     * last form's value. Evaluates in the current namespace with
     * ambient ns cleared, matching `eval` semantics. */
    const char *saved_ambient;
    mino_val *src;
    mino_val *result;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "load-string requires one string argument");
    }
    src = args->as.cons.car;
    if (src == NULL || mino_type_of(src) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "load-string: argument must be a string");
    }
    saved_ambient = S->ns_vars.fn_ambient_ns;
    S->ns_vars.fn_ambient_ns = NULL;
    result = mino_eval_string(S, src->as.s.data, env);
    S->ns_vars.fn_ambient_ns = saved_ambient;
    return result;
}

mino_val *prim_load_file(mino_state *S, mino_val *args, mino_env *env)
{
    /* Read and eval all forms in the file at `path`; return the last
     * form's value. Evaluates in the current namespace with ambient
     * ns cleared, matching `eval` and `load-string`. */
    const char *saved_ambient;
    mino_val *path;
    mino_val *result;
    char        cpath[1024];
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "load-file requires one string argument");
    }
    path = args->as.cons.car;
    if (path == NULL || mino_type_of(path) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "load-file: argument must be a string");
    }
    if (path->as.s.len >= sizeof(cpath)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "load-file: path too long");
    }
    memcpy(cpath, path->as.s.data, path->as.s.len);
    cpath[path->as.s.len] = '\0';
    saved_ambient = S->ns_vars.fn_ambient_ns;
    S->ns_vars.fn_ambient_ns = NULL;
    result = mino_load_file(S, cpath, env);
    S->ns_vars.fn_ambient_ns = saved_ambient;
    return result;
}

mino_val *prim_symbol(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *v;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "symbol requires 1 or 2 arguments");
    }
    /* 2-arg: (symbol ns name) */
    if (mino_is_cons(args->as.cons.cdr)) {
        mino_val *ns_arg  = args->as.cons.car;
        mino_val *name_arg = args->as.cons.cdr->as.cons.car;
        char buf[512];
        size_t pos = 0;
        if (name_arg == NULL || mino_type_of(name_arg) != MINO_STRING) {
            return prim_throw_classified(S, "eval/type", "MTY001", "symbol: name must be a string");
        }
        if (ns_arg != NULL && mino_type_of(ns_arg) == MINO_STRING) {
            /* Preserve an explicit (even empty) ns by emitting the
             * `ns/name` form. Clojure differentiates `(symbol "" "x")`
             * (namespace returns `""`) from `(symbol nil "x")` (no ns,
             * namespace returns `nil`). */
            if (ns_arg->as.s.len + 1 + name_arg->as.s.len >= sizeof(buf)) {
                return prim_throw_classified(S, "eval/type", "MTY001", "symbol: name too long");
            }
            memcpy(buf, ns_arg->as.s.data, ns_arg->as.s.len);
            pos = ns_arg->as.s.len;
            buf[pos++] = '/';
        } else if (ns_arg != NULL && mino_type_of(ns_arg) != MINO_NIL) {
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
    if (v == NULL || mino_type_of(v) == MINO_NIL) {
        return prim_throw_classified(S, "eval/type", "MTY001", "symbol: argument must not be nil");
    }
    if (mino_type_of(v) == MINO_STRING) {
        return mino_symbol_n(S, v->as.s.data, v->as.s.len);
    }
    if (mino_type_of(v) == MINO_SYMBOL) {
        return v;
    }
    if (mino_type_of(v) == MINO_KEYWORD) {
        return mino_symbol_n(S, v->as.s.data, v->as.s.len);
    }
    if (mino_type_of(v) == MINO_VAR) {
        /* (symbol #'foo) -> 'ns/foo. Vars are Named in Clojure, so a
         * fully-qualified symbol falls out of the var's stored ns +
         * name. Vars without an owning ns (rare; root user defs in
         * a state with no current_ns) yield just the name. */
        const char *ns_name = v->as.var.ns;
        const char *sym     = v->as.var.sym;
        size_t      sym_len = (sym != NULL) ? strlen(sym) : 0;
        if (sym == NULL) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "symbol: var has no name");
        }
        if (ns_name != NULL && ns_name[0] != '\0') {
            char        buf[512];
            size_t      ns_len = strlen(ns_name);
            if (ns_len + 1 + sym_len >= sizeof(buf)) {
                return prim_throw_classified(S, "eval/type", "MTY001",
                    "symbol: var name too long");
            }
            memcpy(buf, ns_name, ns_len);
            buf[ns_len] = '/';
            memcpy(buf + ns_len + 1, sym, sym_len);
            return mino_symbol_n(S, buf, ns_len + 1 + sym_len);
        }
        return mino_symbol_n(S, sym, sym_len);
    }
    return prim_throw_classified(S, "eval/type", "MTY001", "symbol: argument must be a string, symbol, keyword, or var");
}

mino_val *prim_keyword(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *v;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "keyword requires one or two arguments");
    }
    /* 2-arity: (keyword ns name) */
    if (mino_is_cons(args->as.cons.cdr)) {
        mino_val *ns_val  = args->as.cons.car;
        mino_val *nm_val  = args->as.cons.cdr->as.cons.car;
        if (mino_is_cons(args->as.cons.cdr->as.cons.cdr))
            return prim_throw_classified(S, "eval/arity", "MAR001", "keyword requires one or two arguments");
        if (mino_type_of(nm_val) != MINO_STRING)
            return prim_throw_classified(S, "eval/type", "MTY001", "keyword: name must be a string");
        if (ns_val == NULL || mino_type_of(ns_val) == MINO_NIL) {
            return mino_keyword_ns_n(S, NULL, 0,
                                     nm_val->as.s.data, nm_val->as.s.len);
        }
        if (mino_type_of(ns_val) != MINO_STRING)
            return prim_throw_classified(S, "eval/type", "MTY001", "keyword: namespace must be a string or nil");
        return mino_keyword_ns_n(S,
                                 ns_val->as.s.data, ns_val->as.s.len,
                                 nm_val->as.s.data, nm_val->as.s.len);
    }
    v = args->as.cons.car;
    if (v == NULL || mino_type_of(v) == MINO_NIL)
        return mino_nil(S);
    if (mino_type_of(v) == MINO_STRING)
        return mino_keyword_n(S, v->as.s.data, v->as.s.len);
    if (mino_type_of(v) == MINO_KEYWORD)
        return v;
    if (mino_type_of(v) == MINO_SYMBOL)
        return mino_keyword_n(S, v->as.s.data, v->as.s.len);
    return prim_throw_classified(S, "eval/type", "MTY001", "keyword: argument must be a string, keyword, or symbol");
}

mino_val *prim_hash(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "hash requires one argument");
    }
    return mino_int(S, (long long)hash_val(args->as.cons.car));
}

mino_val *prim_type(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "type requires one argument");
    }
    v = args->as.cons.car;
    if (v == NULL || mino_type_of(v) == MINO_NIL)  return mino_keyword(S, "nil");
    /* Records return their MINO_TYPE value directly so protocol
     * dispatch keys for built-ins (keywords) and user types (type
     * pointers) live in the same atom-keyed table. The :type
     * metadata path runs for non-records only, keeping the keyword
     * tagging mechanism (used by mino's multimethods and print
     * surface) unchanged. */
    if (mino_type_of(v) == MINO_RECORD) return v->as.record.type;
    /* Honor :type metadata (Clojure semantics). Enables print-method
     * dispatch for user types via (with-meta obj {:type :my-type}). */
    if (MINO_IS_PTR(v) && v->meta != NULL && mino_type_of(v->meta) == MINO_MAP) {
        mino_val *tk = mino_keyword(S, "type");
        mino_val *tv = map_get_val(v->meta, tk);
        if (tv != NULL) return tv;
    }
    switch (mino_type_of(v)) {
    case MINO_NIL:     return mino_keyword(S, "nil");
    case MINO_BOOL:    return mino_keyword(S, "bool");
    case MINO_INT:     return mino_keyword(S, "int");
    case MINO_FLOAT:   return mino_keyword(S, "float");
    case MINO_FLOAT32: return mino_keyword(S, "float32");
    case MINO_CHAR:    return mino_keyword(S, "char");
    case MINO_STRING:  return mino_keyword(S, "string");
    case MINO_SYMBOL:  return mino_keyword(S, "symbol");
    case MINO_KEYWORD:    return mino_keyword(S, "keyword");
    case MINO_EMPTY_LIST: return mino_keyword(S, "list");
    case MINO_CONS:       return mino_keyword(S, "list");
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
    case MINO_VOLATILE: return mino_keyword(S, "volatile");
    case MINO_LAZY:    return mino_keyword(S, "lazy-seq");
    case MINO_CHUNK:   return mino_keyword(S, "chunk");
    case MINO_CHUNKED_CONS: return mino_keyword(S, "list");
    case MINO_RECUR:     return mino_keyword(S, "recur");
    case MINO_TAIL_CALL: return mino_keyword(S, "tail-call");
    case MINO_REDUCED:   return mino_keyword(S, "reduced");
    case MINO_VAR:       return mino_keyword(S, "var");
    case MINO_TRANSIENT: return mino_keyword(S, "transient");
    case MINO_BIGINT:    return mino_keyword(S, "bigint");
    case MINO_RATIO:     return mino_keyword(S, "ratio");
    case MINO_BIGDEC:    return mino_keyword(S, "bigdec");
    case MINO_TYPE:      return mino_keyword(S, "record-type");
    case MINO_RECORD:
        /* Unreachable: handled above so dispatch sees the type
         * pointer. Returning a keyword here would be a leak in the
         * protocol-dispatch story. */
        return v->as.record.type;
    case MINO_FUTURE:    return mino_keyword(S, "future");
    case MINO_UUID:      return mino_keyword(S, "uuid");
    case MINO_REGEX:     return mino_keyword(S, "regex");
    case MINO_HOST_ARRAY: {
        static const char *kinds[] = {
            "object-array", "int-array", "long-array", "short-array",
            "byte-array",   "float-array", "double-array", "char-array",
            "boolean-array"
        };
        unsigned k = v->as.host_array.element_kind;
        if (k >= sizeof(kinds) / sizeof(kinds[0])) k = 0;
        return mino_keyword(S, kinds[k]);
    }
    case MINO_MAP_ENTRY: return mino_keyword(S, "map-entry");
    case MINO_TX_REF:    return mino_keyword(S, "ref");
    case MINO_AGENT:     return mino_keyword(S, "agent");
    case MINO_CHAN:      return mino_keyword(S, "chan");
    case MINO_QUEUE:     return mino_keyword(S, "queue");
    case MINO_BYTES:     return mino_keyword(S, "bytes");
    }
    return mino_keyword(S, "unknown");
}

/* Type-predicate primitives. Each mirrors the mino-level
 * `(fn [x] (= (type x) :foo))` form but skips the keyword allocation
 * and equality comparison by checking the tag directly. */

#define DEFINE_TYPE_PRED(fn_name, pred_expr, label) \
    mino_val *fn_name(mino_state *S, mino_val *args, mino_env *env) \
    { \
        mino_val *v; \
        (void)env; \
        if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) { \
            return prim_throw_classified(S, "eval/arity", "MAR001", \
                label " requires one argument"); \
        } \
        v = args->as.cons.car; \
        return (pred_expr) ? mino_true(S) : mino_false(S); \
    } \
    mino_val *fn_name##_argv(mino_state *S, mino_val **argv, int argc, \
                               mino_env *env) \
    { \
        mino_val *v; \
        (void)env; \
        if (argc != 1) { \
            return prim_throw_classified(S, "eval/arity", "MAR001", \
                label " requires one argument"); \
        } \
        v = argv[0]; \
        return (pred_expr) ? mino_true(S) : mino_false(S); \
    }

DEFINE_TYPE_PRED(prim_nil_p,     (v == NULL || mino_type_of(v) == MINO_NIL),           "nil?")
DEFINE_TYPE_PRED(prim_cons_p,    (v != NULL && (mino_type_of(v) == MINO_CONS || mino_type_of(v) == MINO_CHUNKED_CONS)),          "cons?")
DEFINE_TYPE_PRED(prim_list_p,    (v != NULL && ((mino_type_of(v) == MINO_CONS && !v->as.cons.not_list) || mino_type_of(v) == MINO_EMPTY_LIST)),             "list?")
DEFINE_TYPE_PRED(prim_vector_p,  (v != NULL && (mino_type_of(v) == MINO_VECTOR || mino_type_of(v) == MINO_MAP_ENTRY)),        "vector?")
DEFINE_TYPE_PRED(prim_int_p,     (v != NULL && mino_val_int_p(v)),           "int?")
DEFINE_TYPE_PRED(prim_float_p,   (v != NULL && (mino_type_of(v) == MINO_FLOAT || mino_type_of(v) == MINO_FLOAT32)),         "float?")
DEFINE_TYPE_PRED(prim_string_p,  (v != NULL && mino_type_of(v) == MINO_STRING),        "string?")
DEFINE_TYPE_PRED(prim_keyword_p, (v != NULL && mino_type_of(v) == MINO_KEYWORD),       "keyword?")
DEFINE_TYPE_PRED(prim_symbol_p,  (v != NULL && mino_type_of(v) == MINO_SYMBOL),        "symbol?")
DEFINE_TYPE_PRED(prim_fn_p,      (v != NULL && (mino_type_of(v) == MINO_FN || mino_type_of(v) == MINO_PRIM)), "fn?")
DEFINE_TYPE_PRED(prim_char_p,    (v != NULL && mino_type_of(v) == MINO_CHAR),          "char?")
DEFINE_TYPE_PRED(prim_number_p,  (v != NULL && (mino_val_int_p(v) || mino_type_of(v) == MINO_FLOAT || mino_type_of(v) == MINO_FLOAT32 || mino_type_of(v) == MINO_BIGINT || mino_type_of(v) == MINO_RATIO || mino_type_of(v) == MINO_BIGDEC)), "number?")
DEFINE_TYPE_PRED(prim_map_p,     (v != NULL && (mino_type_of(v) == MINO_MAP || mino_type_of(v) == MINO_SORTED_MAP)), "map?")
DEFINE_TYPE_PRED(prim_set_p,     (v != NULL && (mino_type_of(v) == MINO_SET || mino_type_of(v) == MINO_SORTED_SET)), "set?")
DEFINE_TYPE_PRED(prim_seq_p,     (v != NULL && (mino_type_of(v) == MINO_CONS || mino_type_of(v) == MINO_LAZY || mino_type_of(v) == MINO_EMPTY_LIST || mino_type_of(v) == MINO_CHUNKED_CONS)), "seq?")
DEFINE_TYPE_PRED(prim_boolean_p, (v != NULL && mino_type_of(v) == MINO_BOOL),          "boolean?")
DEFINE_TYPE_PRED(prim_true_p,    (v != NULL && mino_type_of(v) == MINO_BOOL && mino_val_bool_get(v) != 0),  "true?")
DEFINE_TYPE_PRED(prim_false_p,   (v != NULL && mino_type_of(v) == MINO_BOOL && mino_val_bool_get(v) == 0),  "false?")
DEFINE_TYPE_PRED(prim_record_type_p, (v != NULL && mino_type_of(v) == MINO_TYPE), "record-type?")
DEFINE_TYPE_PRED(prim_record_p,      (v != NULL && mino_type_of(v) == MINO_RECORD), "record?")
DEFINE_TYPE_PRED(prim_uuid_p,        (v != NULL && mino_type_of(v) == MINO_UUID),    "uuid?")
DEFINE_TYPE_PRED(prim_regex_p,       (v != NULL && mino_type_of(v) == MINO_REGEX),   "regex?")
DEFINE_TYPE_PRED(prim_bytes_p,       (v != NULL && mino_type_of(v) == MINO_BYTES && v->as.bytes.bit_tail == 0), "bytes?")
DEFINE_TYPE_PRED(prim_bitstring_p,   (v != NULL && mino_type_of(v) == MINO_BYTES),   "bitstring?")

#undef DEFINE_TYPE_PRED

mino_val *prim_not(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "not requires one argument");
    }
    return mino_is_truthy_inline(args->as.cons.car) ? mino_false(S) : mino_true(S);
}

mino_val *prim_some_p(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "some? requires one argument");
    }
    v = args->as.cons.car;
    return (v != NULL && mino_type_of(v) != MINO_NIL) ? mino_true(S) : mino_false(S);
}

mino_val *prim_empty_p(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "empty? requires one argument");
    }
    v = args->as.cons.car;
    if (v == NULL || mino_type_of(v) == MINO_NIL) return mino_true(S);
    switch (mino_type_of(v)) {
    case MINO_EMPTY_LIST: return mino_true(S);
    case MINO_CONS:       return mino_false(S);
    case MINO_VECTOR:     return v->as.vec.len == 0 ? mino_true(S) : mino_false(S);
    case MINO_MAP:        return v->as.map.len == 0 ? mino_true(S) : mino_false(S);
    case MINO_SET:        return v->as.set.len == 0 ? mino_true(S) : mino_false(S);
    case MINO_SORTED_MAP:
    case MINO_SORTED_SET: return v->as.sorted.len == 0 ? mino_true(S) : mino_false(S);
    case MINO_STRING:     return v->as.s.len == 0 ? mino_true(S) : mino_false(S);
    case MINO_LAZY: {
        mino_val *forced = lazy_force(S, v);
        if (forced == NULL) return NULL;
        if (forced == NULL || mino_type_of(forced) == MINO_NIL) return mino_true(S);
        if (mino_type_of(forced) == MINO_CHUNKED_CONS) return mino_false(S);
        return mino_false(S);
    }
    case MINO_CHUNKED_CONS: return mino_false(S);
    case MINO_CHUNK:        return v->as.chunk.len == 0 ? mino_true(S) : mino_false(S);
    case MINO_HOST_ARRAY:   return v->as.host_array.len == 0 ? mino_true(S) : mino_false(S);
    case MINO_MAP_ENTRY:    return mino_false(S);
    case MINO_QUEUE:        return mino_queue_count(v) == 0 ? mino_true(S) : mino_false(S);
    case MINO_BYTES:        return mino_bytes_len(v) == 0 ? mino_true(S) : mino_false(S);
    default:
        return prim_throw_classified(S, "eval/type", "MTY001",
            "empty? expects a collection or nil");
    }
}

/* Numeric predicates. The mino-level versions invoke number? then compare,
 * doing two prim dispatches per call; inlining the check here is a direct
 * tag read plus a comparison.
 *
 * BigInt / Ratio / BigDec route through tower_to_double for the sign /
 * zero comparison. The double approximation preserves sign for any
 * finite value; (zero? 0N), (pos? 1N), (neg? -1.0M) etc. all work. */
static mino_val *num_pred_step(mino_state *S, mino_val *v,
                                 const char *name, int (*cmp_int)(long long),
                                 int (*cmp_float)(double))
{
    if (v == NULL) goto type_err;
    if (mino_val_int_p(v))   return cmp_int(mino_val_int_get(v)) ? mino_true(S) : mino_false(S);
    if (mino_type_of(v) == MINO_FLOAT) return cmp_float(v->as.f) ? mino_true(S) : mino_false(S);
    if (mino_type_of(v) == MINO_BIGINT || mino_type_of(v) == MINO_RATIO
        || mino_type_of(v) == MINO_BIGDEC) {
        double d = tower_to_double(v);
        return cmp_float(d) ? mino_true(S) : mino_false(S);
    }
type_err:
    {
        char buf[96];
        snprintf(buf, sizeof(buf), "%s requires a number", name);
        return prim_throw_classified(S, "eval/type", "MTY001", buf);
    }
}

static mino_val *num_pred(mino_state *S, mino_val *args,
                            const char *name, int (*cmp_int)(long long),
                            int (*cmp_float)(double))
{
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%s requires one argument", name);
        return prim_throw_classified(S, "eval/arity", "MAR001", buf);
    }
    return num_pred_step(S, args->as.cons.car, name, cmp_int, cmp_float);
}

static int is_zero_i(long long n) { return n == 0; }
static int is_zero_f(double   x) { return x == 0.0; }
static int is_pos_i(long long n)  { return n > 0; }
static int is_pos_f(double   x)  { return x > 0.0; }
static int is_neg_i(long long n)  { return n < 0; }
static int is_neg_f(double   x)  { return x < 0.0; }

mino_val *prim_zero_p(mino_state *S, mino_val *args, mino_env *env) {
    (void)env; return num_pred(S, args, "zero?", is_zero_i, is_zero_f);
}
mino_val *prim_pos_p(mino_state *S, mino_val *args, mino_env *env) {
    (void)env; return num_pred(S, args, "pos?", is_pos_i, is_pos_f);
}
mino_val *prim_neg_p(mino_state *S, mino_val *args, mino_env *env) {
    (void)env; return num_pred(S, args, "neg?", is_neg_i, is_neg_f);
}

mino_val *prim_zero_p_argv(mino_state *S, mino_val **argv, int argc,
                             mino_env *env) {
    (void)env;
    if (argc != 1) return prim_throw_classified(
        S, "eval/arity", "MAR001", "zero? requires one argument");
    return num_pred_step(S, argv[0], "zero?", is_zero_i, is_zero_f);
}
mino_val *prim_pos_p_argv(mino_state *S, mino_val **argv, int argc,
                            mino_env *env) {
    (void)env;
    if (argc != 1) return prim_throw_classified(
        S, "eval/arity", "MAR001", "pos? requires one argument");
    return num_pred_step(S, argv[0], "pos?", is_pos_i, is_pos_f);
}
mino_val *prim_neg_p_argv(mino_state *S, mino_val **argv, int argc,
                            mino_env *env) {
    (void)env;
    if (argc != 1) return prim_throw_classified(
        S, "eval/arity", "MAR001", "neg? requires one argument");
    return num_pred_step(S, argv[0], "neg?", is_neg_i, is_neg_f);
}

static mino_val *odd_p_step(mino_state *S, mino_val *v)
{
    if (v == NULL) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "odd? requires an integer");
    }
    if (mino_val_int_p(v)) {
        return (mino_val_int_get(v) & 1LL) != 0 ? mino_true(S) : mino_false(S);
    }
    if (mino_type_of(v) == MINO_BIGINT) {
        return mp_int_is_odd((mp_int)v->as.bigint.mpz)
            ? mino_true(S) : mino_false(S);
    }
    return prim_throw_classified(S, "eval/type", "MTY001",
        "odd? requires an integer");
}

mino_val *prim_odd_p(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "odd? requires one argument");
    }
    return odd_p_step(S, args->as.cons.car);
}

mino_val *prim_odd_p_argv(mino_state *S, mino_val **argv, int argc,
                            mino_env *env)
{
    (void)env;
    if (argc != 1) return prim_throw_classified(
        S, "eval/arity", "MAR001", "odd? requires one argument");
    return odd_p_step(S, argv[0]);
}

static mino_val *even_p_step(mino_state *S, mino_val *v)
{
    if (v == NULL) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "even? requires an integer");
    }
    if (mino_val_int_p(v)) {
        return (mino_val_int_get(v) & 1LL) == 0 ? mino_true(S) : mino_false(S);
    }
    if (mino_type_of(v) == MINO_BIGINT) {
        return mp_int_is_odd((mp_int)v->as.bigint.mpz)
            ? mino_false(S) : mino_true(S);
    }
    return prim_throw_classified(S, "eval/type", "MTY001",
        "even? requires an integer");
}

mino_val *prim_even_p(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "even? requires one argument");
    }
    return even_p_step(S, args->as.cons.car);
}

mino_val *prim_even_p_argv(mino_state *S, mino_val **argv, int argc,
                             mino_env *env)
{
    (void)env;
    if (argc != 1) return prim_throw_classified(
        S, "eval/arity", "MAR001", "even? requires one argument");
    return even_p_step(S, argv[0]);
}

mino_val *prim_macroexpand_1(mino_state *S, mino_val *args, mino_env *env)
{
    int expanded;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "macroexpand-1 requires one argument");
    }
    return macroexpand1(S, args->as.cons.car, env, &expanded);
}

mino_val *prim_macroexpand(mino_state *S, mino_val *args, mino_env *env)
{
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "macroexpand requires one argument");
    }
    return macroexpand_all(S, args->as.cons.car, env);
}

mino_val *prim_gensym(mino_state *S, mino_val *args, mino_env *env)
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
        mino_val *p = args->as.cons.car;
        if (p == NULL || mino_type_of(p) != MINO_STRING) {
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
        size_t total_len;
        memcpy(buf, prefix_src, prefix_len);
        used = snprintf(buf + prefix_len, sizeof(buf) - prefix_len,
                        "%ld", ++S->gensym_counter);
        if (used < 0) {
            return prim_throw_classified(S, "eval/type", "MTY001", "gensym formatting failed");
        }
        total_len = prefix_len + (size_t)used;
        /* Gensyms are unique by construction of the counter, so interning
         * adds zero dedup value -- every call produces a name no other
         * call ever sees. Interning would accumulate one permanent entry
         * in sym_intern per gensym call, which in spawn-heavy workloads
         * (one go/go-loop expansion per bot, ~15 gensyms per expansion)
         * climbs into the hundreds of thousands and inflates major-mark
         * cost. Allocate the symbol directly, bypassing sym_intern.
         * Equality on symbols is by string content (see mino_eq), so
         * downstream comparisons still behave correctly. */
        {
            char       *data = dup_n(S, buf, total_len);
            mino_val *v    = alloc_val(S, MINO_SYMBOL);
            v->as.s.data = data;
            v->as.s.len  = total_len;
            return v;
        }
    }
}

/*
 * (defrecord* ns-string name-string fields-vector) -- runtime fn that
 * the script-side defrecord macro expands into. Returns the MINO_TYPE
 * value for that (ns, name) pair, idempotent across calls.
 *
 * Field-vector entries may be keywords (preferred) or symbols; both
 * resolve to keywords in the type's stored fields.
 */
static mino_val *prim_defrecord_star(mino_state *S, mino_val *args,
                                mino_env *env)
{
    mino_val  *ns_arg, *name_arg, *fields_arg;
    const char  *ns_str, *name_str;
    const char **field_names;
    size_t       n_fields, i;
    mino_val  *result;
    (void)env;

    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || !mino_is_cons(args->as.cons.cdr->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "defrecord* requires three arguments: ns name fields");
    }
    ns_arg     = args->as.cons.car;
    name_arg   = args->as.cons.cdr->as.cons.car;
    fields_arg = args->as.cons.cdr->as.cons.cdr->as.cons.car;

    if (ns_arg == NULL || mino_type_of(ns_arg) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "defrecord*: ns must be a string");
    }
    if (name_arg == NULL || mino_type_of(name_arg) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "defrecord*: name must be a string");
    }
    if (fields_arg == NULL || mino_type_of(fields_arg) != MINO_VECTOR) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "defrecord*: fields must be a vector");
    }

    ns_str   = ns_arg->as.s.data;
    name_str = name_arg->as.s.data;
    n_fields = fields_arg->as.vec.len;

    field_names = NULL;
    if (n_fields > 0) {
        field_names = (const char **)malloc(n_fields * sizeof(*field_names));
        if (field_names == NULL) {
            return prim_throw_classified(S, "internal", "MIN001",
                "defrecord*: out of memory");
        }
        for (i = 0; i < n_fields; i++) {
            mino_val *f = vec_nth(fields_arg, i);
            if (f == NULL || (mino_type_of(f) != MINO_KEYWORD
                              && mino_type_of(f) != MINO_SYMBOL
                              && mino_type_of(f) != MINO_STRING)) {
                free(field_names);
                return prim_throw_classified(S, "eval/type", "MTY001",
                    "defrecord*: field names must be keywords, symbols, or strings");
            }
            field_names[i] = f->as.s.data;
        }
    }
    result = mino_defrecord(S, ns_str, name_str, field_names, n_fields);
    free(field_names);
    if (result == NULL) {
        return prim_throw_classified(S, "internal", "MIN001",
            "defrecord*: failed to construct record type");
    }
    return result;
}

/*
 * (record* type vals-vector) -- build a record value from positional
 * field values. Runtime helper for the script-side ->Type
 * constructor function. n_vals must equal the type's declared field
 * count.
 */
static mino_val *prim_record_star(mino_state *S, mino_val *args,
                             mino_env *env)
{
    mino_val  *type_arg, *vals_arg, *result;
    mino_val **slots;
    size_t       n, i;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "record* requires two arguments: type vals-vector");
    }
    type_arg = args->as.cons.car;
    vals_arg = args->as.cons.cdr->as.cons.car;
    if (type_arg == NULL || mino_type_of(type_arg) != MINO_TYPE) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "record*: first argument must be a record type");
    }
    if (vals_arg == NULL || mino_type_of(vals_arg) != MINO_VECTOR) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "record*: second argument must be a vector of field values");
    }
    n = vals_arg->as.vec.len;
    slots = NULL;
    if (n > 0) {
        slots = (mino_val **)gc_alloc_typed(S, GC_T_VALARR,
                                              n * sizeof(*slots));
        if (slots == NULL) {
            return prim_throw_classified(S, "internal", "MIN001",
                "record*: out of memory");
        }
        for (i = 0; i < n; i++) slots[i] = vec_nth(vals_arg, i);
    }
    result = mino_record(S, type_arg, slots, n);
    if (result == NULL) {
        char msg[160];
        size_t expected = (type_arg->as.record_type.fields != NULL)
            ? type_arg->as.record_type.fields->as.vec.len : 0;
        snprintf(msg, sizeof(msg),
            "record*: type %s.%s expects %zu field values, got %zu",
            type_arg->as.record_type.ns ? type_arg->as.record_type.ns : "",
            type_arg->as.record_type.name ? type_arg->as.record_type.name : "",
            expected, n);
        return prim_throw_classified(S, "eval/arity", "MAR001", msg);
    }
    return result;
}

/*
 * (record-from-map type m) -- build a record by reading declared
 * fields from m by keyword; non-field keys land in ext. Runtime
 * helper for the script-side map->Type constructor function.
 */
static mino_val *prim_record_from_map(mino_state *S, mino_val *args,
                                 mino_env *env)
{
    mino_val  *type_arg, *map_arg, *result, *fields;
    mino_val **slots;
    size_t       n_fields, i;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "record-from-map requires two arguments: type map");
    }
    type_arg = args->as.cons.car;
    map_arg  = args->as.cons.cdr->as.cons.car;
    if (type_arg == NULL || mino_type_of(type_arg) != MINO_TYPE) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "record-from-map: first argument must be a record type");
    }
    if (map_arg == NULL || mino_type_of(map_arg) != MINO_MAP) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "record-from-map: second argument must be a map");
    }
    fields   = type_arg->as.record_type.fields;
    n_fields = (fields != NULL) ? fields->as.vec.len : 0;

    slots = NULL;
    if (n_fields > 0) {
        slots = (mino_val **)gc_alloc_typed(S, GC_T_VALARR,
                                              n_fields * sizeof(*slots));
        if (slots == NULL) {
            return prim_throw_classified(S, "internal", "MIN001",
                "record-from-map: out of memory");
        }
        for (i = 0; i < n_fields; i++) {
            mino_val *fk = vec_nth(fields, i);
            mino_val *v  = map_get_val(map_arg, fk);
            slots[i] = (v != NULL) ? v : mino_nil(S);
        }
    }
    result = mino_record(S, type_arg, slots, n_fields);
    if (result == NULL) {
        return prim_throw_classified(S, "internal", "MIN001",
            "record-from-map: failed to construct record");
    }

    /* Any keys in the source map that are not declared fields go
     * into ext, preserving insertion order. */
    {
        mino_val **ext_keys = NULL;
        mino_val **ext_vals = NULL;
        size_t       ext_n    = 0;
        size_t       j;
        if (map_arg->as.map.len > 0) {
            ext_keys = (mino_val **)gc_alloc_typed(S, GC_T_VALARR,
                map_arg->as.map.len * sizeof(*ext_keys));
            ext_vals = (mino_val **)gc_alloc_typed(S, GC_T_VALARR,
                map_arg->as.map.len * sizeof(*ext_vals));
            if (ext_keys == NULL || ext_vals == NULL) {
                return prim_throw_classified(S, "internal", "MIN001",
                    "record-from-map: out of memory");
            }
        }
        for (j = 0; j < map_arg->as.map.len; j++) {
            mino_val *k = vec_nth(map_arg->as.map.key_order, j);
            if (record_field_index(result, k) < 0) {
                ext_keys[ext_n] = k;
                ext_vals[ext_n] = map_get_val(map_arg, k);
                ext_n++;
            }
        }
        if (ext_n > 0) {
            result->as.record.ext = mino_map(S, ext_keys, ext_vals, ext_n);
        }
    }
    return result;
}

/*
 * (record-fields type) -- returns the declared field-name vector
 * (keywords) for a record type. Useful for tooling and reflection.
 */
static mino_val *prim_record_fields(mino_state *S, mino_val *args,
                               mino_env *env)
{
    mino_val *t;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "record-fields requires one argument");
    }
    t = args->as.cons.car;
    if (t == NULL || mino_type_of(t) != MINO_TYPE) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "record-fields: argument must be a record type");
    }
    return t->as.record_type.fields != NULL
        ? t->as.record_type.fields : mino_vector(S, NULL, 0);
}

/* (throw value) -- raise a script exception. */
mino_val *prim_throw(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *ex;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "throw requires one argument");
    }
    ex = args->as.cons.car;
    if (mino_current_ctx(S)->try_depth <= 0) {
        /* No enclosing try -- format as fatal error. The thrown value
         * may be a plain string, a normalized diagnostic map (carrying
         * :mino/kind/:mino/code/:mino/message), or an ex-info-style
         * {:message ... :data ...}. Probe each shape so the original
         * message survives in the surfaced diagnostic instead of the
         * bare "unhandled exception".
         *
         * For ex-info / classified-map shapes that carry a :data (or
         * :mino/data) payload, route through set_eval_diag_with_data
         * so the diag preserves the data field. This matters when the
         * throw happens inside a future worker -- the worker's exit
         * captures mino_last_error_map(S), and ex-info data must
         * survive into the consumer-side rethrow. */
        char msg[512];
        const char *kind = "user";
        const char *code = "MUS001";
        char        kbuf[64];
        char        cbuf[32];
        mino_val *data = NULL;
        if (ex != NULL && mino_type_of(ex) == MINO_STRING) {
            snprintf(msg, sizeof(msg), "unhandled exception: %.*s",
                     (int)ex->as.s.len, ex->as.s.data);
        } else if (ex != NULL && mino_type_of(ex) == MINO_MAP) {
            mino_val *m_msg = map_get_val(ex,
                mino_keyword(S, "mino/message"));
            mino_val *m_kind = map_get_val(ex,
                mino_keyword(S, "mino/kind"));
            mino_val *m_code = map_get_val(ex,
                mino_keyword(S, "mino/code"));
            mino_val *m_data = map_get_val(ex,
                mino_keyword(S, "mino/data"));
            if (m_msg == NULL || mino_type_of(m_msg) != MINO_STRING) {
                m_msg = map_get_val(ex, mino_keyword(S, "message"));
            }
            if (m_data == NULL) {
                m_data = map_get_val(ex, mino_keyword(S, "data"));
            }
            if (m_msg != NULL && mino_type_of(m_msg) == MINO_STRING) {
                snprintf(msg, sizeof(msg), "%.*s",
                         (int)m_msg->as.s.len, m_msg->as.s.data);
            } else {
                snprintf(msg, sizeof(msg), "unhandled exception");
            }
            if (m_kind != NULL && mino_type_of(m_kind) == MINO_KEYWORD
                && m_kind->as.s.len < sizeof(kbuf)) {
                memcpy(kbuf, m_kind->as.s.data, m_kind->as.s.len);
                kbuf[m_kind->as.s.len] = '\0';
                kind = kbuf;
            }
            if (m_code != NULL && mino_type_of(m_code) == MINO_STRING
                && m_code->as.s.len < sizeof(cbuf)) {
                memcpy(cbuf, m_code->as.s.data, m_code->as.s.len);
                cbuf[m_code->as.s.len] = '\0';
                code = cbuf;
            }
            data = m_data;
        } else {
            snprintf(msg, sizeof(msg), "unhandled exception");
        }
        if (data != NULL) {
            set_eval_diag_with_data(S,
                mino_current_ctx(S)->eval_current_form,
                kind, code, msg, data, NULL);
            append_trace(S);
            return NULL;
        }
        return prim_throw_classified(S, kind, code, msg);
    }
    mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth - 1].exception = ex;
    longjmp(mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth - 1].buf, 1);
    return NULL; /* unreachable */
}

mino_val *prim_var_p(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "var? requires one argument");
    }
    v = args->as.cons.car;
    return (v != NULL && mino_type_of(v) == MINO_VAR) ? mino_true(S) : mino_false(S);
}

mino_val *prim_resolve(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *sym;
    char buf[256];
    size_t n;
    const char *slash;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "resolve requires one argument");
    }
    sym = args->as.cons.car;
    if (sym == NULL || mino_type_of(sym) != MINO_SYMBOL) {
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
        mino_val *var;
        size_t i;

        memcpy(ns_buf, buf, ns_len);
        ns_buf[ns_len] = '\0';

        /* Check alias table. */
        for (i = 0; i < S->ns_vars.ns_alias_len; i++) {
            if (strcmp(S->ns_vars.ns_aliases[i].alias, ns_buf) == 0) {
                resolved_ns = S->ns_vars.ns_aliases[i].full_name;
                goto found_ns;
            }
        }
        resolved_ns = ns_buf;
found_ns:
        var = var_find(S, resolved_ns, sym_name);
        return var != NULL ? var : mino_nil(S);
    }

    /* Unqualified: walk current ns env chain (which terminates at
     * clojure.core) and return a var for whatever's bound. The wide
     * "scan every ns for any var with this name" fallback is gone --
     * it picked up unrelated names from sibling namespaces. */
    {
        const char *cur = S->ns_vars.current_ns != NULL ? S->ns_vars.current_ns : "user";
        mino_val *var = var_find(S, cur, buf);
        mino_env *e;
        if (var != NULL) return var;
        e = ns_env_lookup(S, cur);
        for (; e != NULL; e = e->parent) {
            env_binding_t *b = env_find_here(e, buf);
            if (b != NULL) {
                size_t k;
                for (k = 0; k < S->ns_vars.ns_env_len; k++) {
                    if (S->ns_vars.ns_env_table[k].env == e) {
                        const char *src_ns = S->ns_vars.ns_env_table[k].name;
                        mino_val *v = var_find(S, src_ns, buf);
                        if (v != NULL) return v;
                        v = var_intern(S, src_ns, buf);
                        if (v != NULL) {
                            var_set_root(S, v, b->val);
                            return v;
                        }
                        break;
                    }
                }
                break;
            }
        }
        return mino_nil(S);
    }
}

mino_val *prim_namespace(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *v;
    const char *data;
    size_t len;
    const char *slash;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "namespace requires one argument");
    }
    v = args->as.cons.car;
    if (v == NULL || mino_type_of(v) == MINO_NIL) {
        return prim_throw_classified(S, "eval/type", "MTY001", "namespace: argument must not be nil");
    }
    if (mino_type_of(v) != MINO_SYMBOL && mino_type_of(v) != MINO_KEYWORD) {
        char        buf[256];
        mino_val *printed = print_to_string(S, v);
        snprintf(buf, sizeof(buf),
                 "namespace: expected a symbol or keyword, got: %.*s",
                 printed != NULL ? (int)printed->as.s.len : 0,
                 printed != NULL ? printed->as.s.data : "");
        return prim_throw_classified(S, "eval/type", "MTY001", buf);
    }
    data = v->as.s.data;
    len  = v->as.s.len;
    (void)slash;
    /* Use the explicit ns_len from the construction site rather than
     * re-scanning data; preserves the (ns, name) split that 2-arg
     * (keyword ns name) recorded. Empty-string namespace encodes as
     * a leading '/' with ns_len == 0 and len > 1; distinguish it
     * from "no namespace at all" (data with no slash) and from the
     * bare-slash literal `:/` (data == "/", len == 1) so JVM canon
     * `(namespace (keyword "" "hi"))` returns "" rather than nil. */
    {
        size_t ns_len = v->as.s.ns_len;
        if (ns_len > 0) return mino_string_n(S, data, ns_len);
        if (len > 1 && data[0] == '/') return mino_string_n(S, data, 0);
        return mino_nil(S);
    }
}

/* (last-error) -- return the last diagnostic as a map, or nil. */
mino_val *prim_last_error(mino_state *S, mino_val *args, mino_env *env)
{
    (void)args; (void)env;
    return mino_last_error_map(S);
}

/* (error? x) -- true if x is a map with :mino/kind. */
mino_val *prim_error_p(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *v, *kind_key;
    (void)env;
    if (!mino_is_cons(args)) return mino_false(S);
    v = args->as.cons.car;
    if (v == NULL || mino_type_of(v) != MINO_MAP) return mino_false(S);
    kind_key = mino_keyword(S, "mino/kind");
    return map_get_val(v, kind_key) != NULL ? mino_true(S) : mino_false(S);
}

/* (ex-data e) -- extract :mino/data from a diagnostic map. */
mino_val *prim_ex_data(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *v, *data_key, *result;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "ex-data requires one argument");
    }
    v = args->as.cons.car;
    if (v == NULL || mino_type_of(v) != MINO_MAP) return mino_nil(S);
    data_key = mino_keyword(S, "mino/data");
    result = map_get_val(v, data_key);
    return result != NULL ? result : mino_nil(S);
}

/* (ex-message e) -- extract :mino/message from a diagnostic map. */
mino_val *prim_ex_message(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *v, *msg_key, *result;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "ex-message requires one argument");
    }
    v = args->as.cons.car;
    if (v == NULL || mino_type_of(v) != MINO_MAP) return mino_nil(S);
    msg_key = mino_keyword(S, "mino/message");
    result = map_get_val(v, msg_key);
    return result != NULL ? result : mino_nil(S);
}

/* (gc-stats) -- return a map of GC statistics built from the public
 * mino_gc_stats_out struct. The :phase key exposes the collector's
 * state machine position (one of :idle, :minor, :major-mark,
 * :major-sweep). The :threshold key is preserved alongside the public
 * struct fields; it is a private heuristic tracked by the major
 * collector and not part of mino_gc_stats_out. */
mino_val *prim_gc_stats(mino_state *S, mino_val *args, mino_env *env)
{
    mino_gc_stats_out st;
    const char *phase_name;
    mino_val *ks[39];
    mino_val *vs[39];
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "gc-stats takes no arguments");
    }
    mino_gc_stats(S, &st);
    switch (st.phase) {
    case MINO_GC_PHASE_MINOR:       phase_name = "minor";       break;
    case MINO_GC_PHASE_MAJOR_MARK:  phase_name = "major-mark";  break;
    case MINO_GC_PHASE_MAJOR_SWEEP: phase_name = "major-sweep"; break;
    default:                        phase_name = "idle";        break;
    }
    ks[0]  = mino_keyword(S, "collections-minor");
    vs[0]  = mino_int(S, (long long)st.collections_minor);
    ks[1]  = mino_keyword(S, "collections-major");
    vs[1]  = mino_int(S, (long long)st.collections_major);
    ks[2]  = mino_keyword(S, "bytes-live");
    vs[2]  = mino_int(S, (long long)st.bytes_live);
    ks[3]  = mino_keyword(S, "bytes-young");
    vs[3]  = mino_int(S, (long long)st.bytes_young);
    ks[4]  = mino_keyword(S, "bytes-old");
    vs[4]  = mino_int(S, (long long)st.bytes_old);
    ks[5]  = mino_keyword(S, "bytes-alloc");
    vs[5]  = mino_int(S, (long long)st.bytes_alloc);
    ks[6]  = mino_keyword(S, "bytes-freed");
    vs[6]  = mino_int(S, (long long)st.bytes_freed);
    ks[7]  = mino_keyword(S, "threshold");
    vs[7]  = mino_int(S, (long long)S->gc.threshold);
    ks[8]  = mino_keyword(S, "total-gc-ns");
    vs[8]  = mino_int(S, (long long)st.total_gc_ns);
    ks[9]  = mino_keyword(S, "max-gc-ns");
    vs[9]  = mino_int(S, (long long)st.max_gc_ns);
    ks[10] = mino_keyword(S, "remset-entries");
    vs[10] = mino_int(S, (long long)st.remset_entries);
    ks[11] = mino_keyword(S, "phase");
    vs[11] = mino_keyword(S, phase_name);
    ks[12] = mino_keyword(S, "nursery-bytes");
    vs[12] = mino_int(S, (long long)S->gc.nursery_bytes);
    ks[13] = mino_keyword(S, "remset-cap");
    vs[13] = mino_int(S, (long long)st.remset_cap);
    ks[14] = mino_keyword(S, "remset-high-water");
    vs[14] = mino_int(S, (long long)st.remset_high_water);
    ks[15] = mino_keyword(S, "mark-stack-cap");
    vs[15] = mino_int(S, (long long)st.mark_stack_cap);
    ks[16] = mino_keyword(S, "mark-stack-high-water");
    vs[16] = mino_int(S, (long long)st.mark_stack_high_water);
    /* Alloc-source counters (probe-only; used by gc-alloc profiling). */
    ks[17] = mino_keyword(S, "alloc-freelist-hits");
    vs[17] = mino_int(S, (long long)S->gc_alloc_freelist_hits);
    ks[18] = mino_keyword(S, "alloc-calloc-class-miss");
    vs[18] = mino_int(S, (long long)S->gc_alloc_calloc_size_class_miss);
    ks[19] = mino_keyword(S, "alloc-calloc-no-class");
    vs[19] = mino_int(S, (long long)S->gc_alloc_calloc_no_class);
    ks[20] = mino_keyword(S, "alloc-bump-hits");
    vs[20] = mino_int(S, (long long)S->gc_bump_alloc_hits);
    ks[21] = mino_keyword(S, "alloc-bump-slab-refills");
    vs[21] = mino_int(S, (long long)S->gc_bump_slab_refills);
    /* Per-phase GC timers. Sub-timer note: root-scan-ns measures the
     * precise-root enumeration that runs inside the mark phases, so it
     * overlaps minor-mark-ns + major-mark-ns instead of adding to them. */
    ks[22] = mino_keyword(S, "minor-mark-ns");
    vs[22] = mino_int(S, (long long)st.minor_mark_ns);
    ks[23] = mino_keyword(S, "minor-sweep-ns");
    vs[23] = mino_int(S, (long long)st.minor_sweep_ns);
    ks[24] = mino_keyword(S, "major-mark-ns");
    vs[24] = mino_int(S, (long long)st.major_mark_ns);
    ks[25] = mino_keyword(S, "major-sweep-ns");
    vs[25] = mino_int(S, (long long)st.major_sweep_ns);
    ks[26] = mino_keyword(S, "root-scan-ns");
    vs[26] = mino_int(S, (long long)st.root_scan_ns);
    /* Write-barrier hit counters and mark-stack overflow drops. */
    ks[27] = mino_keyword(S, "barrier-satb-pushes");
    vs[27] = mino_int(S, (long long)st.barrier_satb_pushes);
    ks[28] = mino_keyword(S, "barrier-dijkstra-pushes");
    vs[28] = mino_int(S, (long long)st.barrier_dijkstra_pushes);
    ks[29] = mino_keyword(S, "mark-stack-overflows");
    vs[29] = mino_int(S, (long long)st.mark_stack_overflows);
    /* Generational promotion bookkeeping. :young-age-buckets is a
     * length-8 vector of cumulative survivor counts indexed by
     * clamp(log2(age+1), 0..7). */
    ks[30] = mino_keyword(S, "bytes-promoted-minor");
    vs[30] = mino_int(S, (long long)st.bytes_promoted_minor);
    {
        mino_val *buckets[8];
        size_t i;
        for (i = 0; i < 8; i++) {
            buckets[i] = mino_int(S, (long long)st.young_age_bucket[i]);
        }
        ks[31] = mino_keyword(S, "young-age-buckets");
        vs[31] = mino_vector(S, buckets, 8);
    }
    /* Pause-time distribution. Percentiles are computed from the
     * last 256 samples in the ring; the 24-bucket lifetime histogram
     * sits alongside under :pause-hist. */
    {
        uint64_t p50 = 0, p95 = 0, p99 = 0, pmax = 0;
        uint32_t hist[24];
        unsigned hist_count = 0;
        mino_val *hist_vals[24];
        size_t i;
        mino_gc_stats_pauses(S, &p50, &p95, &p99, &pmax);
        mino_gc_pause_hist(S, hist, &hist_count);
        ks[32] = mino_keyword(S, "pause-p50-ns");
        vs[32] = mino_int(S, (long long)p50);
        ks[33] = mino_keyword(S, "pause-p95-ns");
        vs[33] = mino_int(S, (long long)p95);
        ks[34] = mino_keyword(S, "pause-p99-ns");
        vs[34] = mino_int(S, (long long)p99);
        for (i = 0; i < 24; i++) {
            hist_vals[i] = mino_int(S, (long long)hist[i]);
        }
        ks[35] = mino_keyword(S, "pause-hist");
        vs[35] = mino_vector(S, hist_vals, 24);
    }
    /* Per-tag allocation histogram, surfaced as a map of
     * tag-keyword -> count. Indices 1..10 cover the current GC tag
     * enum; unused indices stay implicit zero. */
    {
        static const char *tag_names[16] = {
            NULL, "raw", "val", "env", "vec-node", "hamt-node",
            "hamt-entry", "ptrarr", "valarr", "rb-node", "bc",
            NULL, NULL, NULL, NULL, NULL
        };
        mino_val *tk[16];
        mino_val *tv[16];
        size_t i, n = 0;
        for (i = 0; i < 16; i++) {
            if (tag_names[i] == NULL) continue;
            if (st.alloc_by_tag[i] == 0) continue;
            tk[n] = mino_keyword(S, tag_names[i]);
            tv[n] = mino_int(S, (long long)st.alloc_by_tag[i]);
            n++;
        }
        ks[36] = mino_keyword(S, "alloc-by-tag");
        vs[36] = mino_map(S, tk, tv, n);
    }
    /* BC compile-decline histogram, surfaced as a keyword -> count
     * map. Zero-count buckets elided. */
    {
        static const char *reason_names[16] = {
            NULL, "macro", "special-form", "bad-form",
            "qualified-head", "destructure", "recur-outside",
            "nesting-limit", "oom", "other",
            NULL, NULL, NULL, NULL, NULL, NULL
        };
        mino_val *dk[16];
        mino_val *dv[16];
        size_t i, n = 0;
        for (i = 0; i < 16; i++) {
            if (reason_names[i] == NULL) continue;
            if (S->bc_declines[i] == 0) continue;
            dk[n] = mino_keyword(S, reason_names[i]);
            dv[n] = mino_int(S, (long long)S->bc_declines[i]);
            n++;
        }
        ks[37] = mino_keyword(S, "bc-declines");
        vs[37] = mino_map(S, dk, dv, n);
    }
    /* Collection-size histogram (env-gated). Map kind -> 32-vec of
     * log2-bucket counts. Empty when MINO_COLL_SIZE_STATS is unset. */
    {
        static const char *kind_names[3] = {"vector", "map", "set"};
        mino_val *ck[3];
        mino_val *cv[3];
        size_t kind, n = 0;
        for (kind = 0; kind < 3; kind++) {
            int any = 0;
            mino_val *bv[32];
            size_t b;
            for (b = 0; b < 32; b++) {
                if (S->coll_size_hist[kind][b] != 0) any = 1;
                bv[b] = mino_int(S, (long long)S->coll_size_hist[kind][b]);
            }
            if (!any) continue;
            ck[n] = mino_keyword(S, kind_names[kind]);
            cv[n] = mino_vector(S, bv, 32);
            n++;
        }
        ks[38] = mino_keyword(S, "coll-size-hist");
        vs[38] = mino_map(S, ck, cv, n);
    }
    return mino_map(S, ks, vs, 39);
}

/* (gc!) -- force a full (minor + major) collection. Useful for tests
 * and memory-accounting scripts that need deterministic state before
 * reading gc-stats. The public mino_gc_collect honours gc_depth, so a
 * nested call during construction is a no-op. */
mino_val *prim_gc_bang(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "gc! takes no arguments");
    }
    mino_gc_collect(S, MINO_GC_FULL);
    return mino_nil(S);
}

/* (alloc-profile-enabled?) -- compile-time flag. */
static mino_val *prim_alloc_profile_enabled_p(mino_state *S,
                                         mino_val *args, mino_env *env)
{
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "alloc-profile-enabled? takes no arguments");
    }
    return mino_alloc_profile_enabled() ? mino_true(S) : mino_false(S);
}

/* (alloc-profile-reset!) -- zero the per-callsite counters. */
static mino_val *prim_alloc_profile_reset_bang(mino_state *S,
                                          mino_val *args, mino_env *env)
{
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "alloc-profile-reset! takes no arguments");
    }
    mino_alloc_profile_reset(S);
    return mino_nil(S);
}

/* (alloc-profile-dump! n) -- dump top n callsites to stderr. n=0 means
 * all sites. Returns nil. */
static mino_val *prim_alloc_profile_dump_bang(mino_state *S,
                                         mino_val *args, mino_env *env)
{
    long long n = 30;
    (void)env;
    if (mino_is_cons(args)) {
        mino_val *first = args->as.cons.car;
        if (first == NULL || !mino_val_int_p(first)) {
            return prim_throw_classified(S, "eval/arg", "MAR002",
                                         "alloc-profile-dump! takes an integer count");
        }
        n = mino_val_int_get(first);
        if (mino_is_cons(args->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                                         "alloc-profile-dump! takes at most one argument");
        }
    }
    mino_alloc_profile_dump_top(S, stderr, (int)n);
    return mino_nil(S);
}

const mino_prim_def k_prims_reflection[] = {
    {"type",      prim_type,
     "Returns a keyword indicating the type of the value."},
    {"nil?",      prim_nil_p,
     "Returns true if x is nil.", prim_nil_p_argv},
    {"cons?",     prim_cons_p,
     "Returns true if x is a list (cons cell).", prim_cons_p_argv},
    {"list?",     prim_list_p,
     "Returns true if x is a list (cons chain or the empty-list singleton). "
     "Excludes lazy-seq and chunked-cons; for the broader 'is a sequence' "
     "predicate, use seq?.", prim_list_p_argv},
    {"vector?",   prim_vector_p,
     "Returns true if x is a vector.", prim_vector_p_argv},
    {"int?",      prim_int_p,
     "Returns true if x is an integer.", prim_int_p_argv},
    {"float?",    prim_float_p,
     "Returns true if x is a float.", prim_float_p_argv},
    {"string?",   prim_string_p,
     "Returns true if x is a string.", prim_string_p_argv},
    {"keyword?",  prim_keyword_p,
     "Returns true if x is a keyword.", prim_keyword_p_argv},
    {"symbol?",   prim_symbol_p,
     "Returns true if x is a symbol.", prim_symbol_p_argv},
    {"fn?",       prim_fn_p,
     "Returns true if x is callable as a function (fn or prim).", prim_fn_p_argv},
    {"char?",     prim_char_p,
     "Returns true if x is a one-character string.", prim_char_p_argv},
    {"number?",   prim_number_p,
     "Returns true if x is a number (int or float).", prim_number_p_argv},
    {"map?",      prim_map_p,
     "Returns true if x is a map (including sorted-map).", prim_map_p_argv},
    {"set?",      prim_set_p,
     "Returns true if x is a set (including sorted-set).", prim_set_p_argv},
    {"seq?",      prim_seq_p,
     "Returns true if x is a cons cell or lazy-seq.", prim_seq_p_argv},
    {"boolean?",  prim_boolean_p,
     "Returns true if x is true or false.", prim_boolean_p_argv},
    {"true?",     prim_true_p,
     "Returns true if x is the value true.", prim_true_p_argv},
    {"false?",    prim_false_p,
     "Returns true if x is the value false.", prim_false_p_argv},
    {"not",       prim_not,
     "Returns true if x is logical false, false otherwise."},
    {"some?",     prim_some_p,
     "Returns true if x is not nil."},
    {"empty?",    prim_empty_p,
     "Returns true if coll has no items."},
    {"zero?",     prim_zero_p,
     "Returns true if x is zero.", prim_zero_p_argv},
    {"pos?",      prim_pos_p,
     "Returns true if x is greater than zero.", prim_pos_p_argv},
    {"neg?",      prim_neg_p,
     "Returns true if x is less than zero.", prim_neg_p_argv},
    {"odd?",      prim_odd_p,
     "Returns true if x is an odd integer.", prim_odd_p_argv},
    {"even?",     prim_even_p,
     "Returns true if x is an even integer.", prim_even_p_argv},
    {"name",      prim_name,
     "Returns the name string of a symbol, keyword, or string."},
    {"namespace", prim_namespace,
     "Returns the namespace string of a symbol or keyword, or nil."},
    {"var?",      prim_var_p,
     "Returns true if x is a var."},
    {"resolve",   prim_resolve,
     "Returns the var to which a symbol resolves, or nil."},
    {"rand",      prim_rand,
     "Returns a random float between 0 inclusive and 1 exclusive, or between 0 and n."},
    {"random-seed!", prim_random_seed_bang,
     "Seeds the per-state PRNG to a known integer value so subsequent rand calls produce a reproducible stream. Returns the seed."},
    {"eval",      prim_eval,
     "Evaluates the given form."},
    {"load-string", prim_load_string,
     "Reads and evaluates all forms in the given source string. Returns the value of the last form."},
    {"load-file",   prim_load_file,
     "Reads and evaluates all forms in the file at the given path. Returns the value of the last form."},
    {"symbol",    prim_symbol,
     "Returns a symbol with the given name."},
    {"keyword",   prim_keyword,
     "Returns a keyword with the given name."},
    {"hash",      prim_hash,
     "Returns the hash code of the value."},
    {"macroexpand-1", prim_macroexpand_1,
     "Expands a macro form once."},
    {"macroexpand",   prim_macroexpand,
     "Repeatedly expands a macro form until it is no longer a macro call."},
    {"gensym",    prim_gensym,
     "Returns a new symbol with a unique name."},
    {"destructure", prim_destructure,
     "Takes a binding-pairs vector [lhs1 rhs1 lhs2 rhs2 ...] and returns a flat vector of [name init ...] suitable as a let binding form."},
    {"throw",     prim_throw,
     "Throws an exception with the given value."},
    {"last-error", prim_last_error,
     "Returns the last error as a diagnostic map, or nil."},
    {"error?",    prim_error_p,
     "Returns true if the value is a diagnostic map."},
    {"defrecord*", prim_defrecord_star,
     "Runtime constructor for record types. Takes ns name fields-vector and returns the MINO_TYPE value, idempotent across calls."},
    {"record-type?", prim_record_type_p,
     "Returns true if x is a record type (the value defrecord defines)."},
    {"record*",    prim_record_star,
     "Runtime constructor for record values. Takes a record type and a vector of declared field values. Used by the ->Type macro expansion."},
    {"record-from-map", prim_record_from_map,
     "Builds a record by reading declared fields from a map; non-field keys land in ext. Used by the map->Type macro expansion."},
    {"record-fields", prim_record_fields,
     "Returns the declared field-name vector for a record type."},
    {"record?",    prim_record_p,
     "Returns true if x is a record value."},
    {"uuid?",      prim_uuid_p,
     "Returns true if x is a UUID value."},
    {"regex?",     prim_regex_p,
     "Returns true if x is a regex value."},
    {"bytes?",     prim_bytes_p,
     "Returns true if x is a byte-aligned mino bytes value (the "
     "immutable binary-data type returned by byte-array)."},
    {"bitstring?", prim_bitstring_p,
     "Returns true if x is any mino bytes value -- byte-aligned or "
     "bit-aligned. bytes? is the byte-aligned subset."},
    {"alloc-profile-enabled?", prim_alloc_profile_enabled_p,
     "Returns true if this binary was built with -DMINO_ALLOC_PROFILE=1."},
    {"alloc-profile-reset!",   prim_alloc_profile_reset_bang,
     "Zero the per-callsite allocation counters. No-op in non-profile builds."},
    {"alloc-profile-dump!",    prim_alloc_profile_dump_bang,
     "Dump the top-N allocation call sites to stderr. Defaults to 30; pass 0 for all."},
};

const size_t k_prims_reflection_count =
    sizeof(k_prims_reflection) / sizeof(k_prims_reflection[0]);
