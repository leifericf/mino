/*
 * runtime/module_state.h -- per-state module / load-stack / bundled
 * library / metadata / execution-limit block.
 *
 * Holds the module resolver hook + context, the loaded-module cache,
 * the in-flight load stack (for cycle detection), the bundled-source
 * registry, the runtime add-load-path! list, and the meta-entry table.
 *
 * Internal to the runtime; embedders should only use mino.h.
 */

#ifndef RUNTIME_MODULE_STATE_H
#define RUNTIME_MODULE_STATE_H

#include "mino.h"                       /* mino_resolve_fn */
#include "runtime/runtime_types.h"     /* module_entry_t, bundled_lib_entry_t, meta_entry_t */

#include <stddef.h>

typedef struct module_state {
    /* Execution limits (config knobs; set once by host, read by ctx). */
    size_t          limit_steps;
    size_t          limit_heap;

    /* Module system */
    mino_resolve_fn module_resolver;
    void           *module_resolver_ctx;
    module_entry_t *module_cache;
    size_t          module_cache_len;
    size_t          module_cache_cap;
    /* Load stack: names of modules currently being loaded (cycle detection). */
    char          **load_stack;
    size_t          load_stack_len;
    size_t          load_stack_cap;
    /* Bundled-stdlib registry. */
    bundled_lib_entry_t *bundled_libs;
    size_t               bundled_libs_len;
    size_t               bundled_libs_cap;
    /* Extra load paths registered via (add-load-path! ...). */
    char               **extra_load_paths;
    size_t               extra_load_paths_len;
    size_t               extra_load_paths_cap;

    /* Metadata table. */
    meta_entry_t   *meta_table;
    size_t          meta_table_len;
    size_t          meta_table_cap;
} module_state_t;

#endif /* RUNTIME_MODULE_STATE_H */
