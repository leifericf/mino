/*
 * interop_internal.h -- host-interop capability registry types.
 *
 * Internal to the runtime; embedders should only use mino.h.
 */

#ifndef INTEROP_INTERNAL_H
#define INTEROP_INTERNAL_H

#include "mino.h"

#include <stddef.h>

/* Host interop: capability registry kinds. */
enum {
    HOST_CTOR   = 0,
    HOST_METHOD = 1,
    HOST_STATIC = 2,
    HOST_GETTER = 3
};

typedef struct {
    const char  *name;    /* interned member name (NULL for ctor) */
    int          arity;   /* expected arg count (-1 = variadic) */
    int          kind;    /* HOST_CTOR, HOST_METHOD, HOST_STATIC, HOST_GETTER */
    mino_host_fn fn;
    void        *fn_ctx;
} host_member_t;

typedef struct {
    const char    *type_key;  /* interned type tag */
    host_member_t *members;   /* malloc-owned array */
    size_t         members_len;
    size_t         members_cap;
} host_type_t;

/* host_interop.c: capability registry lookup. */
host_type_t   *host_type_find(mino_state_t *S, const char *type_key);
host_member_t *host_member_find(host_type_t *t, const char *name,
                                int kind, int arity);

#endif /* INTEROP_INTERNAL_H */
