/*
 * host.c -- host/new, host/call, host/static-call, host/get primitives.
 *
 * These are always registered but throw "interop disabled" unless the
 * host has called mino_host_enable().
 */

#include "prim/internal.h"
#include <stdarg.h>

/* Count args in a cons list. */
static int count_args(mino_val_t *args)
{
    int n = 0;
    while (mino_is_cons(args)) {
        n++;
        args = args->as.cons.cdr;
    }
    return n;
}

/* Extract keyword name as C string. Returns NULL if not a keyword. */
static const char *kw_name(const mino_val_t *v)
{
    if (v == NULL || v->type != MINO_KEYWORD) return NULL;
    return v->as.s.data;
}

/* Throw a formatted interop error. Never returns. */
static mino_val_t *interop_error(mino_state_t *S, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    return prim_throw_classified(S, "host", "MHO001", msg);
}

/* (host/new :Type arg1 arg2 ...) */
static mino_val_t *prim_host_new(mino_state_t *S, mino_val_t *args,
                                  mino_env_t *env)
{
    mino_val_t    *type_val;
    const char    *type_key;
    host_type_t   *ht;
    host_member_t *hm;
    int            nargs;
    mino_val_t    *ctor_args;
    (void)env;

    if (!S->interop_enabled)
        return interop_error(S, "interop disabled");
    if (!mino_is_cons(args)) {
        set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "host/new requires at least a type argument");
        return NULL;
    }
    type_val = args->as.cons.car;
    type_key = kw_name(type_val);
    if (type_key == NULL)
        return interop_error(S, "host/new: type must be a keyword");

    ctor_args = args->as.cons.cdr;
    nargs = count_args(ctor_args);

    ht = host_type_find(S, type_key);
    if (ht == NULL)
        return interop_error(S, "unknown type: :%s", type_key);

    hm = host_member_find(ht, NULL, HOST_CTOR, nargs);
    if (hm == NULL)
        return interop_error(S,
            "arity mismatch: :%s constructor, got %d", type_key, nargs);

    {
        mino_val_t *result = hm->fn(S, NULL, ctor_args, hm->fn_ctx);
        if (result == NULL)
            return interop_error(S, "host callback failed: :%s constructor",
                                 type_key);
        return result;
    }
}

/* (host/call target :method arg1 arg2 ...) */
static mino_val_t *prim_host_call(mino_state_t *S, mino_val_t *args,
                                   mino_env_t *env)
{
    mino_val_t    *target;
    mino_val_t    *method_val;
    const char    *method_key;
    const char    *type_key;
    host_type_t   *ht;
    host_member_t *hm;
    int            nargs;
    mino_val_t    *call_args;
    (void)env;

    if (!S->interop_enabled)
        return interop_error(S, "interop disabled");
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "host/call requires target and method arguments");
        return NULL;
    }
    target = args->as.cons.car;
    if (target == NULL || target->type != MINO_HANDLE)
        return interop_error(S, "target is not a host handle");

    type_key = target->as.handle.tag;
    if (type_key == NULL)
        return interop_error(S, "target handle has no type tag");

    method_val = args->as.cons.cdr->as.cons.car;
    method_key = kw_name(method_val);
    if (method_key == NULL)
        return interop_error(S, "host/call: method must be a keyword");

    call_args = args->as.cons.cdr->as.cons.cdr;
    nargs = count_args(call_args);

    ht = host_type_find(S, type_key);
    if (ht == NULL)
        return interop_error(S, "unknown type: :%s", type_key);

    hm = host_member_find(ht, method_key, HOST_METHOD, nargs);
    if (hm == NULL)
        return interop_error(S, "member not found: :%s/:%s",
                             type_key, method_key);

    {
        mino_val_t *result = hm->fn(S, target, call_args, hm->fn_ctx);
        if (result == NULL)
            return interop_error(S, "host callback failed: :%s/:%s",
                                 type_key, method_key);
        return result;
    }
}

/* (host/static-call :Type :method arg1 arg2 ...) */
static mino_val_t *prim_host_static_call(mino_state_t *S, mino_val_t *args,
                                          mino_env_t *env)
{
    mino_val_t    *type_val;
    mino_val_t    *method_val;
    const char    *type_key;
    const char    *method_key;
    host_type_t   *ht;
    host_member_t *hm;
    int            nargs;
    mino_val_t    *call_args;
    (void)env;

    if (!S->interop_enabled)
        return interop_error(S, "interop disabled");
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "host/static-call requires type and method arguments");
        return NULL;
    }
    type_val = args->as.cons.car;
    type_key = kw_name(type_val);
    if (type_key == NULL)
        return interop_error(S, "host/static-call: type must be a keyword");

    method_val = args->as.cons.cdr->as.cons.car;
    method_key = kw_name(method_val);
    if (method_key == NULL)
        return interop_error(S, "host/static-call: method must be a keyword");

    call_args = args->as.cons.cdr->as.cons.cdr;
    nargs = count_args(call_args);

    ht = host_type_find(S, type_key);
    if (ht == NULL)
        return interop_error(S, "unknown type: :%s", type_key);

    hm = host_member_find(ht, method_key, HOST_STATIC, nargs);
    if (hm == NULL)
        return interop_error(S, "member not found: :%s/:%s",
                             type_key, method_key);

    {
        mino_val_t *result = hm->fn(S, NULL, call_args, hm->fn_ctx);
        if (result == NULL)
            return interop_error(S, "host callback failed: :%s/:%s",
                                 type_key, method_key);
        return result;
    }
}

/* (host/get target :field) */
static mino_val_t *prim_host_get(mino_state_t *S, mino_val_t *args,
                                  mino_env_t *env)
{
    mino_val_t    *target;
    mino_val_t    *field_val;
    const char    *field_key;
    const char    *type_key;
    host_type_t   *ht;
    host_member_t *hm;
    (void)env;

    if (!S->interop_enabled)
        return interop_error(S, "interop disabled");
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "host/get requires target and field arguments");
        return NULL;
    }
    if (mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        set_eval_diag(S, S->eval_current_form, "eval/arity", "MAR001", "host/get takes exactly two arguments");
        return NULL;
    }
    target = args->as.cons.car;
    if (target == NULL || target->type != MINO_HANDLE)
        return interop_error(S, "target is not a host handle");

    type_key = target->as.handle.tag;
    if (type_key == NULL)
        return interop_error(S, "target handle has no type tag");

    field_val = args->as.cons.cdr->as.cons.car;
    field_key = kw_name(field_val);
    if (field_key == NULL)
        return interop_error(S, "host/get: field must be a keyword");

    ht = host_type_find(S, type_key);
    if (ht == NULL)
        return interop_error(S, "unknown type: :%s", type_key);

    hm = host_member_find(ht, field_key, HOST_GETTER, 0);
    if (hm == NULL)
        return interop_error(S, "member not found: :%s/:%s",
                             type_key, field_key);

    {
        mino_val_t *result = hm->fn(S, target, NULL, hm->fn_ctx);
        if (result == NULL)
            return interop_error(S, "host callback failed: :%s/:%s",
                                 type_key, field_key);
        return result;
    }
}

const mino_prim_def k_prims_host[] = {
    {"host/new",         prim_host_new,
     "Creates a new instance of a host-registered type."},
    {"host/call",        prim_host_call,
     "Calls a method on a host handle."},
    {"host/static-call", prim_host_static_call,
     "Calls a static method on a host-registered type."},
    {"host/get",         prim_host_get,
     "Returns the value of a field on a host handle."},
};

const size_t k_prims_host_count =
    sizeof(k_prims_host) / sizeof(k_prims_host[0]);

void mino_install_host(mino_state_t *S, mino_env_t *env)
{
    prim_install_table(S, env, "clojure.core", k_prims_host, k_prims_host_count);
}
