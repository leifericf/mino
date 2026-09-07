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
/* The one "which namespace owns this bare name" answer, shared by
 * syntax-quote qualification and `resolve`: walk the env chain from
 * start_env to the first frame binding name; a var binding names its
 * source ns and original spelling, any other binding names the ns
 * owning the frame. Returns 1 on a hit (*ns_out may be NULL for a
 * frame in no ns table entry; *val_out gets the bound value), 0 when
 * nothing in the chain binds it. Out params may be NULL. */
int ns_owner_for_name(mino_state *S, mino_env *start_env,
                      const char *name, size_t nlen,
                      const char **ns_out, const char **name_out,
                      size_t *name_len_out, mino_val **val_out);

/* var.c: var registry helpers. */
mino_val    *var_intern(mino_state *S, const char *ns, const char *name);
const char *intern_var_str(mino_state *S, const char *s);
/* var_registry_add: append (ns, name, var) to the linear registry and
 * the lookup hash. ns and name are interned internally (idempotent), so
 * callers may pass either an already-interned pointer or a fresh string;
 * the registry never stores a non-interned pointer that a later
 * var_find (which keys on the interned pointer) would miss. */
int            var_registry_add(mino_state *S, const char *ns,
                                const char *name, mino_val *var);
void           var_set_root(mino_state *S, mino_val *var, mino_val *val);
/* The one var read path shared by symbol access, qualified-symbol
 * access, and deref: thread-binding stack first (a thread binding
 * satisfies the read even when the root is unbound, per canon), then
 * the bound root, else the loud unbound throw (name/MNS003, NULL
 * return with the diagnostic set). */
mino_val    *var_read(mino_state *S, mino_val *var);
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
/* Append `path` to the runtime require search path, growing the backing
 * array as needed. Idempotent: a path already present is not re-added.
 * Returns 0 on success (or when the path was already present) and -1 on
 * allocation failure. Shared by the add-load-path! primitive and the
 * public mino_add_load_path. */
int  runtime_module_add_load_path(mino_state *S, const char *path);

#endif /* RUNTIME_VAR_MODULE_H */
