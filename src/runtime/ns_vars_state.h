/*
 * runtime/ns_vars_state.h -- per-state namespace and var registry block.
 *
 * Holds the current ns, the ns-alias table, the per-ns root-env
 * table, the ambient-ns slot used during fn-body eval, the var
 * registry + its open-addressing hash mirror, the monomorphic
 * inline call cache (ic_table + ic_gen), and the transient owner-id
 * counter.
 *
 * ic_gen sits at the stencil-ABI-pinned offset 47856 inside
 * mino_state; the embedded sub-struct is placed so the absolute
 * offset is byte-stable. The runtime_layout.h offset constants are
 * unchanged; the offsetof site in eval/bc/jit/entry.c updates to
 * the nested path `ns_vars.ic_gen`.
 *
 * Internal to the runtime; embedders should only use mino.h.
 */

#ifndef RUNTIME_NS_VARS_STATE_H
#define RUNTIME_NS_VARS_STATE_H

#include "mino_internal.h"
#include "runtime/runtime_types.h"  /* ns_alias_t, ns_env_entry_t, var_entry_t, var_hash_slot_t */

#include <stddef.h>
#include <stdint.h>

typedef struct ns_vars_state {
    /* Namespace */
    const char     *current_ns;       /* from (ns ...), default "user" */
    ns_alias_t     *ns_aliases;
    size_t          ns_alias_len;
    size_t          ns_alias_cap;

    /* Per-namespace root env table. */
    ns_env_entry_t *ns_env_table;
    size_t          ns_env_len;
    size_t          ns_env_cap;
    mino_env_t     *mino_core_env;    /* clojure.core root env; parent NULL */

    /* Ambient namespace for free-var resolution inside the active fn body. */
    const char     *fn_ambient_ns;

    /* Var registry */
    var_entry_t    *var_registry;
    size_t          var_registry_len;
    size_t          var_registry_cap;

    /* Monomorphic inline call cache. Var redefinition bumps ic_gen,
     * invalidating every slot in one shot. */
    struct ic_slot {
        mino_val_t *form;          /* call form pointer, NULL = empty */
        const char *head_data;     /* sym->as.s.data, interned */
        mino_val_t *callable;
        unsigned    gen_at_fill;
    } *ic_table;
    size_t          ic_cap;
    unsigned        ic_gen;          /* stencil-ABI-pinned offset 47856 */

    /* Monotonic owner-ID generator for transient batch mutators. */
    uint32_t        transient_owner_next;

    /* Open-addressing hash mirror over var_registry. */
    var_hash_slot_t *var_hash;
    size_t          var_hash_cap;
    size_t          var_hash_len;
} ns_vars_state_t;

#endif /* RUNTIME_NS_VARS_STATE_H */
