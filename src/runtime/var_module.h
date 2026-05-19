/*
 * var_module.h -- per-namespace root env table, var registry,
 * state-local PRNG, and module-resolution helpers.
 *
 * Each ns has a root env owning that ns's def/refer bindings. Every
 * ns env except clojure.core has parent → clojure.core, so
 * unqualified lookup walks lexical → current-ns env → clojure.core
 * env.
 *
 * Bodies live in runtime/ns_env.c, runtime/var.c, runtime/state.c
 * (state_rand64), and runtime/module.c. Internal to the runtime;
 * embedders should only use mino.h.
 */

#ifndef RUNTIME_VAR_MODULE_H
#define RUNTIME_VAR_MODULE_H

#include "mino_internal.h"

#include <stddef.h>
#include <stdint.h>

/* ns_env.c: per-namespace root env table. */
void load_stack_truncate(mino_state_t *S, size_t len);
mino_env_t *ns_env_lookup(mino_state_t *S, const char *name);   /* borrowed */
mino_env_t *ns_env_ensure(mino_state_t *S, const char *name);   /* GC-owned, rooted */
mino_val_t *ns_symbol_with_meta(mino_state_t *S, const char *name);
void        mino_publish_current_ns(mino_state_t *S);
mino_val_t *ns_env_get_meta(mino_state_t *S, const char *name);
void        ns_env_set_meta(mino_state_t *S, const char *name, mino_val_t *meta);
mino_env_t *current_ns_env(mino_state_t *S);                    /* GC-owned, rooted */

/* var.c: var registry helpers. */
mino_val_t    *var_intern(mino_state_t *S, const char *ns, const char *name);
void           var_set_root(mino_state_t *S, mino_val_t *var, mino_val_t *val);
mino_val_t    *var_find(mino_state_t *S, const char *ns, const char *name);
void           var_unintern(mino_state_t *S, const char *ns, const char *name);

/* state.c: per-state PRNG. Seeds lazily on first call. */
uint64_t state_rand64(mino_state_t *S);

/* module.c: shared module-resolution helpers used by the ns special
 * form (eval/defs.c) and the require primitive (prim/module.c). */
int  runtime_module_dotted_to_path(const char *name, size_t nlen,
                                   char *buf, size_t bufsize);
int  runtime_module_add_alias(mino_state_t *S,
                              const char *alias, const char *full);

#endif /* RUNTIME_VAR_MODULE_H */
