/*
 * env_api.h -- env allocation, binding lookup, dynamic-binding stack.
 *
 * Environments are GC-owned. Bindings within are borrowed views.
 * Bodies live in runtime/env.c. Internal to the runtime; embedders
 * should only use mino.h.
 */

#ifndef RUNTIME_ENV_API_H
#define RUNTIME_ENV_API_H

#include "mino_internal.h"
#include "runtime/runtime_types.h"   /* env_binding_t, dyn_binding_t */

#include <stddef.h>

mino_env_t    *env_alloc(mino_state_t *S, mino_env_t *parent); /* GC-owned */
env_binding_t *env_find_here(mino_env_t *env, const char *name); /* borrowed */
env_binding_t *env_find_here_n(mino_env_t *env, const char *name, size_t nlen);
/* Symbol-aware lookup. Caller already has sym->as.s.{data,len}; we
 * skip strlen and walk the parent chain with the cached length. */
mino_val_t    *mino_env_get_sym(mino_env_t *env, const mino_val_t *sym);
void           env_bind(mino_state_t *S, mino_env_t *env,
                        const char *name,                      /* borrowed (copied) */
                        mino_val_t *val);                      /* GC-owned, retained */
void           env_bind_sym(mino_state_t *S, mino_env_t *env,
                        mino_val_t *sym,                       /* interned symbol */
                        mino_val_t *val);                      /* GC-owned, retained */
int            env_unbind(mino_state_t *S, mino_env_t *env,
                        const char *name);                     /* 1 if removed */
mino_env_t    *env_child(mino_state_t *S, mino_env_t *parent); /* GC-owned */
mino_env_t    *env_root(mino_state_t *S, mino_env_t *env);     /* borrowed (walks up) */
mino_val_t    *dyn_lookup(mino_state_t *S, const char *name);  /* borrowed */
void           dyn_binding_list_free(dyn_binding_t *head);     /* frees malloc chain */

/* Snapshot the calling thread's dyn_stack into a map (symbol -> value).
 * Returns mino_nil(S) when the stack is empty. Used by future spawn to
 * convey caller bindings to the worker, and by get-thread-bindings. */
mino_val_t    *mino_snapshot_thread_bindings(mino_state_t *S);

#endif /* RUNTIME_ENV_API_H */
