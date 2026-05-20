/*
 * meta.c -- metadata primitives: meta, with-meta, vary-meta, alter-meta!.
 */

#include "prim/internal.h"

/* Type tags whose meta can be CHANGED through with-meta / vary-meta
 * (which return a copy of the value carrying the new metadata).
 * Stateful types -- atom / agent -- are intentionally NOT here:
 * with-meta would shallow-copy the cell, producing a sibling whose
 * `val` slot diverges from the original on the next mutation. The
 * proper Clojure-canon fix needs a separate indirection cell so two
 * with-meta'd atoms share their state; absent that, throw a clear
 * error and direct callers at alter-meta! (in-place) or the
 * constructor's :meta option. */
static int supports_meta(mino_type t)
{
    return t == MINO_SYMBOL || t == MINO_CONS || t == MINO_VECTOR
        || t == MINO_MAP    || t == MINO_SET  || t == MINO_FN
        || t == MINO_MACRO  || t == MINO_VAR;
}

/* Type tags whose meta can be READ via (meta x) and MUTATED in
 * place via alter-meta!. Includes everything supports_meta covers
 * plus the stateful types (atom / agent / ref) -- alter-meta! does
 * an in-place mutation of obj->meta rather than copying the cell,
 * so it stays safe for identity-tied values, and (meta x) is
 * read-only. The constructor form `(ref init :meta m)` populates
 * the slot at construction; alter-meta! can mutate it later. */
static int meta_readable(mino_type t)
{
    return supports_meta(t) || t == MINO_ATOM || t == MINO_AGENT
        || t == MINO_TX_REF;
}

/* Vars don't store an explicit meta map -- :ns and :name come from the
 * var's ns and sym fields, and ^:private / ^:dynamic come from flags.
 * Synthesize a fresh map on each call (callers don't expect identity
 * stability on var meta). */
static mino_val *synth_var_meta(mino_state *S, mino_val *var)
{
    mino_val *keys[4];
    mino_val *vals[4];
    size_t      n = 0;
    keys[n] = mino_keyword(S, "ns");
    vals[n] = var->as.var.ns != NULL
              ? mino_symbol(S, var->as.var.ns)
              : mino_nil(S);
    n++;
    keys[n] = mino_keyword(S, "name");
    vals[n] = var->as.var.sym != NULL
              ? mino_symbol(S, var->as.var.sym)
              : mino_nil(S);
    n++;
    if (var->as.var.is_private) {
        keys[n] = mino_keyword(S, "private");
        vals[n] = mino_true(S);
        n++;
    }
    if (var->as.var.dynamic) {
        keys[n] = mino_keyword(S, "dynamic");
        vals[n] = mino_true(S);
        n++;
    }
    return mino_map(S, keys, vals, n);
}

mino_val *mino_meta(const mino_val *v)
{
    if (v == NULL) return NULL;
    if (!meta_readable(mino_type_of(v))) return NULL;
    return v->meta;
}

mino_val *mino_with_meta(mino_state *S, mino_val *v, mino_val *meta)
{
    /* Reuse prim_with_meta -- it does the type-validation and
     * throws on non-supported types (atom, agent, ...) via the
     * runtime's error mechanism. */
    mino_val *args = mino_cons(S, meta, mino_nil(S));
    args = mino_cons(S, v, args);
    return prim_with_meta(S, args, NULL);
}

mino_val *prim_meta(mino_state *S, mino_val *args,
                       mino_env *env)
{
    mino_val *obj;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "meta requires one argument");
    }
    obj = args->as.cons.car;
    if (obj == NULL) return mino_nil(S);
    if (!meta_readable(mino_type_of(obj))) {
        return mino_nil(S);
    }
    if (mino_type_of(obj) == MINO_VAR) {
        if (obj->meta != NULL) return obj->meta;
        return synth_var_meta(S, obj);
    }
    return obj->meta != NULL ? obj->meta : mino_nil(S);
}

mino_val *prim_with_meta(mino_state *S, mino_val *args,
                            mino_env *env)
{
    mino_val *obj, *m, *copy;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "with-meta requires 2 arguments");
    }
    obj = args->as.cons.car;
    m   = args->as.cons.cdr->as.cons.car;
    if (obj == NULL) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "with-meta: type does not support metadata");
    }
    if (mino_type_of(obj) == MINO_ATOM || mino_type_of(obj) == MINO_AGENT) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "with-meta: stateful types (atom/agent) cannot be copied "
            "to attach new meta; use alter-meta! for in-place mutation "
            "or the constructor's :meta option");
    }
    if (!supports_meta(mino_type_of(obj))) {
        return prim_throw_classified(S, "eval/type", "MTY001", "with-meta: type does not support metadata");
    }
    if (m != NULL && mino_type_of(m) != MINO_NIL && mino_type_of(m) != MINO_MAP) {
        return prim_throw_classified(S, "eval/type", "MTY001", "with-meta: metadata must be a map or nil");
    }
    /* Shallow-copy the value and attach the new metadata. */
    copy = alloc_val(S, mino_type_of(obj));
    copy->as = obj->as;
    copy->meta = (m != NULL && mino_type_of(m) == MINO_NIL) ? NULL : m;
    return copy;
}

mino_val *prim_vary_meta(mino_state *S, mino_val *args,
                            mino_env *env)
{
    mino_val *obj, *f, *old_meta, *extra, *call_args, *new_meta, *copy;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "vary-meta requires at least 2 arguments");
    }
    obj = args->as.cons.car;
    f   = args->as.cons.cdr->as.cons.car;
    extra = args->as.cons.cdr->as.cons.cdr; /* remaining args (cons list or nil) */
    if (obj == NULL) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "vary-meta: type does not support metadata");
    }
    if (mino_type_of(obj) == MINO_ATOM || mino_type_of(obj) == MINO_AGENT) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "vary-meta: stateful types (atom/agent) cannot be copied "
            "to attach new meta; use alter-meta! for in-place mutation "
            "or the constructor's :meta option");
    }
    if (!supports_meta(mino_type_of(obj))) {
        return prim_throw_classified(S, "eval/type", "MTY001", "vary-meta: type does not support metadata");
    }
    old_meta = (obj->meta != NULL) ? obj->meta : mino_nil(S);
    /* Build (old-meta extra...) argument list for f. */
    call_args = mino_cons(S, old_meta, extra);
    new_meta = mino_call(S, f, call_args, env);
    if (new_meta == NULL) {
        return NULL;
    }
    if (mino_type_of(new_meta) != MINO_NIL && mino_type_of(new_meta) != MINO_MAP) {
        return prim_throw_classified(S, "eval/type", "MTY001", "vary-meta: f must return a map or nil");
    }
    copy = alloc_val(S, mino_type_of(obj));
    copy->as = obj->as;
    copy->meta = (mino_type_of(new_meta) == MINO_NIL) ? NULL : new_meta;
    return copy;
}

mino_val *prim_alter_meta(mino_state *S, mino_val *args,
                             mino_env *env)
{
    mino_val *obj, *f, *old_meta, *extra, *call_args, *new_meta;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "alter-meta! requires at least 2 arguments");
    }
    obj   = args->as.cons.car;
    f     = args->as.cons.cdr->as.cons.car;
    extra = args->as.cons.cdr->as.cons.cdr;
    /* alter-meta! mutates obj->meta in place; safe for atom / agent
     * (their identity is preserved) so use the broader meta_readable
     * gate rather than supports_meta. */
    if (obj == NULL || !meta_readable(mino_type_of(obj))) {
        return prim_throw_classified(S, "eval/type", "MTY001", "alter-meta!: type does not support metadata");
    }
    old_meta = (obj->meta != NULL) ? obj->meta : mino_nil(S);
    call_args = mino_cons(S, old_meta, extra);
    new_meta = mino_call(S, f, call_args, env);
    if (new_meta == NULL) {
        return NULL;
    }
    if (mino_type_of(new_meta) != MINO_NIL && mino_type_of(new_meta) != MINO_MAP) {
        return prim_throw_classified(S, "eval/type", "MTY001", "alter-meta!: f must return a map or nil");
    }
    {
        mino_val *next = (mino_type_of(new_meta) == MINO_NIL) ? NULL : new_meta;
        gc_write_barrier(S, obj, obj->meta, next);
        obj->meta = next;
    }
    return obj->meta != NULL ? obj->meta : mino_nil(S);
}

const mino_prim_def k_prims_meta[] = {
    {"meta",        prim_meta,
     "Returns the metadata map of the given value, or nil."},
    {"with-meta",   prim_with_meta,
     "Returns a copy of the value with the given metadata map."},
    {"vary-meta",   prim_vary_meta,
     "Returns a copy of the value with (apply f meta args) as its metadata."},
    {"alter-meta!", prim_alter_meta,
     "Atomically applies f to the metadata of a reference."},
};

const size_t k_prims_meta_count =
    sizeof(k_prims_meta) / sizeof(k_prims_meta[0]);
