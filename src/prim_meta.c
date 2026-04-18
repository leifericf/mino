/*
 * prim_meta.c -- metadata primitives: meta, with-meta, vary-meta, alter-meta!.
 *
 * Extracted from prim.c. No behavior change.
 */

#include "prim_internal.h"

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
        return prim_throw_error(S, "meta requires one argument");
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
        return prim_throw_error(S, "with-meta requires 2 arguments");
    }
    obj = args->as.cons.car;
    m   = args->as.cons.cdr->as.cons.car;
    if (obj == NULL || !supports_meta(obj->type)) {
        return prim_throw_error(S, "with-meta: type does not support metadata");
    }
    if (m != NULL && m->type != MINO_NIL && m->type != MINO_MAP) {
        return prim_throw_error(S, "with-meta: metadata must be a map or nil");
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
        return prim_throw_error(S, "vary-meta requires at least 2 arguments");
    }
    obj = args->as.cons.car;
    f   = args->as.cons.cdr->as.cons.car;
    extra = args->as.cons.cdr->as.cons.cdr; /* remaining args (cons list or nil) */
    if (obj == NULL || !supports_meta(obj->type)) {
        return prim_throw_error(S, "vary-meta: type does not support metadata");
    }
    old_meta = (obj->meta != NULL) ? obj->meta : mino_nil(S);
    /* Build (old-meta extra...) argument list for f. */
    call_args = mino_cons(S, old_meta, extra);
    new_meta = mino_call(S, f, call_args, env);
    if (new_meta == NULL) {
        return NULL;
    }
    if (new_meta->type != MINO_NIL && new_meta->type != MINO_MAP) {
        return prim_throw_error(S, "vary-meta: f must return a map or nil");
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
        return prim_throw_error(S, "alter-meta! requires at least 2 arguments");
    }
    obj   = args->as.cons.car;
    f     = args->as.cons.cdr->as.cons.car;
    extra = args->as.cons.cdr->as.cons.cdr;
    if (obj == NULL || !supports_meta(obj->type)) {
        return prim_throw_error(S, "alter-meta!: type does not support metadata");
    }
    old_meta = (obj->meta != NULL) ? obj->meta : mino_nil(S);
    call_args = mino_cons(S, old_meta, extra);
    new_meta = mino_call(S, f, call_args, env);
    if (new_meta == NULL) {
        return NULL;
    }
    if (new_meta->type != MINO_NIL && new_meta->type != MINO_MAP) {
        return prim_throw_error(S, "alter-meta!: f must return a map or nil");
    }
    obj->meta = (new_meta->type == MINO_NIL) ? NULL : new_meta;
    return obj->meta != NULL ? obj->meta : mino_nil(S);
}
