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
void load_stack_truncate(mino_state *S, size_t len);
/* state.c: roll back this thread's LAZY_REALIZING claims past MARK on
 * a try-frame landing pad (throw bypassed lazy_realize's rollback). */
void mino_lazy_inflight_unwind(mino_state *S, size_t mark);
mino_env *ns_env_lookup(mino_state *S, const char *name);   /* borrowed */
mino_env *ns_env_ensure(mino_state *S, const char *name);   /* GC-owned, rooted */
mino_val *ns_symbol_with_meta(mino_state *S, const char *name);
void        mino_publish_current_ns(mino_state *S);
mino_val *ns_env_get_meta(mino_state *S, const char *name);
void        ns_env_set_meta(mino_state *S, const char *name, mino_val *meta);
mino_env *current_ns_env(mino_state *S);                    /* GC-owned, rooted */

/* var.c: var registry helpers. */
mino_val    *var_intern(mino_state *S, const char *ns, const char *name);
const char *intern_var_str(mino_state *S, const char *s);
int            var_registry_add(mino_state *S, const char *i_ns,
                                const char *i_name, mino_val *var);
void           var_set_root(mino_state *S, mino_val *var, mino_val *val);
mino_val    *var_find(mino_state *S, const char *ns, const char *name);
void           var_unintern(mino_state *S, const char *ns, const char *name);

/* state.c: per-state PRNG. Seeds lazily on first call. */
uint64_t state_rand64(mino_state *S);

/* module.c: shared module-resolution helpers used by the ns special
 * form (eval/defs.c) and the require primitive (prim/module.c). */
int  runtime_module_dotted_to_path(const char *name, size_t nlen,
                                   char *buf, size_t bufsize);
int  runtime_module_add_alias(mino_state *S,
                              const char *alias, const char *full);

#endif /* RUNTIME_VAR_MODULE_H */
