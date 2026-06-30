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

mino_env    *env_alloc(mino_state *S, mino_env *parent); /* GC-owned */
env_binding_t *env_find_here(mino_env *env, const char *name); /* borrowed */
env_binding_t *env_find_here_n(mino_env *env, const char *name, size_t nlen);
/* Symbol-aware lookup. Caller already has sym->as.s.{data,len}; we
 * skip strlen and walk the parent chain with the cached length. */
mino_val    *mino_env_get_sym(mino_env *env, const mino_val *sym);
void           env_bind(mino_state *S, mino_env *env,
                        const char *name,                      /* borrowed (copied) */
                        mino_val *val);                      /* GC-owned, retained */
void           env_bind_sym(mino_state *S, mino_env *env,
                        mino_val *sym,                       /* interned symbol */
                        mino_val *val);                      /* GC-owned, retained */
int            env_unbind(mino_state *S, mino_env *env,
                        const char *name);                     /* 1 if removed */
mino_env    *env_child(mino_state *S, mino_env *parent); /* GC-owned */
mino_env    *env_root(mino_state *S, mino_env *env);     /* borrowed (walks up) */
mino_val    *dyn_lookup(mino_state *S, const char *name);  /* borrowed; var-less entries only */
mino_val    *dyn_lookup_var(mino_state *S, const mino_val *var); /* borrowed */
mino_val    *dyn_lookup_var_or_name(mino_state *S, const mino_val *var,
                        const char *name);                     /* borrowed */
mino_val    *dyn_resolve_var(mino_state *S, const char *data, size_t n);
mino_val    *dyn_lookup_sym(mino_state *S, const char *data, size_t n);
dyn_binding_t *dyn_binding_make(mino_state *S, mino_val *key,   /* caller owns node */
                        mino_val *val, dyn_binding_t *next);
void           dyn_binding_list_free(dyn_binding_t *head);     /* frees malloc chain */
void           dyn_frame_restore_ns(mino_state *S, dyn_frame_t *f); /* restore *ns* on teardown */

/* Snapshot the calling thread's dyn_stack into a map (symbol -> value).
 * Returns mino_nil(S) when the stack is empty. Used by future spawn to
 * convey caller bindings to the worker, and by get-thread-bindings.
 * NOTE: the implementation lives in src/prim/stateful.c, not env.c,
 * because it needs gc_alloc_typed and mino_symbol which are only
 * available after the prim layer initialises. Callers reach it via
 * this header (pulled in through runtime/internal.h). */
mino_val    *mino_snapshot_thread_bindings(mino_state *S);

#endif /* RUNTIME_ENV_API_H */
