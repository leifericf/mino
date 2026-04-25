/*
 * syntax.c -- capability registry and registration API for host interop.
 *
 * Hosts register constructors, methods, static methods, and getters per
 * type tag. The registry is immutable after init: all registration must
 * happen before eval starts.
 */

#include "runtime/internal.h"

/* Find or create a host_type_t entry for the given type key. */
static host_type_t *type_ensure(mino_state_t *S, const char *type_key)
{
    size_t i;
    for (i = 0; i < S->host_types_len; i++) {
        if (strcmp(S->host_types[i].type_key, type_key) == 0) {
            return &S->host_types[i];
        }
    }
    if (S->host_types_len == S->host_types_cap) {
        size_t nc = S->host_types_cap == 0 ? 4 : S->host_types_cap * 2;
        host_type_t *nb = (host_type_t *)realloc(
            S->host_types, nc * sizeof(*nb));
        if (nb == NULL) return NULL;
        S->host_types     = nb;
        S->host_types_cap = nc;
    }
    {
        host_type_t *t = &S->host_types[S->host_types_len++];
        t->type_key    = type_key;
        t->members     = NULL;
        t->members_len = 0;
        t->members_cap = 0;
        return t;
    }
}

/* Add a member entry to a type. */
static void member_add(host_type_t *t, const char *name, int arity,
                       int kind, mino_host_fn fn, void *ctx)
{
    if (t->members_len == t->members_cap) {
        size_t nc = t->members_cap == 0 ? 4 : t->members_cap * 2;
        host_member_t *nb = (host_member_t *)realloc(
            t->members, nc * sizeof(*nb));
        if (nb == NULL) return;
        t->members     = nb;
        t->members_cap = nc;
    }
    {
        host_member_t *m = &t->members[t->members_len++];
        m->name   = name;
        m->arity  = arity;
        m->kind   = kind;
        m->fn     = fn;
        m->fn_ctx = ctx;
    }
}

void mino_host_enable(mino_state_t *S)
{
    S->interop_enabled = 1;
}

void mino_host_register_ctor(mino_state_t *S, const char *type_key,
                              int arity, mino_host_fn fn, void *ctx)
{
    host_type_t *t = type_ensure(S, type_key);
    if (t != NULL) member_add(t, NULL, arity, HOST_CTOR, fn, ctx);
}

void mino_host_register_method(mino_state_t *S, const char *type_key,
                                const char *method_key, int arity,
                                mino_host_fn fn, void *ctx)
{
    host_type_t *t = type_ensure(S, type_key);
    if (t != NULL) member_add(t, method_key, arity, HOST_METHOD, fn, ctx);
}

void mino_host_register_static(mino_state_t *S, const char *type_key,
                                const char *method_key, int arity,
                                mino_host_fn fn, void *ctx)
{
    host_type_t *t = type_ensure(S, type_key);
    if (t != NULL) member_add(t, method_key, arity, HOST_STATIC, fn, ctx);
}

void mino_host_register_getter(mino_state_t *S, const char *type_key,
                                const char *field_key, mino_host_fn fn,
                                void *ctx)
{
    host_type_t *t = type_ensure(S, type_key);
    if (t != NULL) member_add(t, field_key, 0, HOST_GETTER, fn, ctx);
}

/* Lookup helpers used by prim/host.c. */

host_type_t *host_type_find(mino_state_t *S, const char *type_key)
{
    size_t i;
    for (i = 0; i < S->host_types_len; i++) {
        if (strcmp(S->host_types[i].type_key, type_key) == 0) {
            return &S->host_types[i];
        }
    }
    return NULL;
}

host_member_t *host_member_find(host_type_t *t, const char *name,
                                 int kind, int arity)
{
    size_t i;
    for (i = 0; i < t->members_len; i++) {
        host_member_t *m = &t->members[i];
        if (m->kind != kind) continue;
        /* For ctor, name is NULL; for others, compare name. */
        if (kind == HOST_CTOR) {
            if (m->arity == arity || m->arity == -1) return m;
        } else {
            if (name != NULL && m->name != NULL
                && strcmp(m->name, name) == 0
                && (m->arity == arity || m->arity == -1)) {
                return m;
            }
        }
    }
    return NULL;
}
