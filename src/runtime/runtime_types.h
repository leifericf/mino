/*
 * runtime_types.h -- runtime support types shared by struct mino_state,
 * the per-thread ctx, and the per-subsystem forward decls.
 *
 * Environments and dyn-binding frames; module / metadata / var registry
 * entries; the call-stack frame record for stack traces; the GC root-env
 * registry node; small-int cache range constants.
 *
 * Internal to the runtime; embedders should only use mino.h.
 */

#ifndef RUNTIME_TYPES_H
#define RUNTIME_TYPES_H

#include "mino_internal.h"

#include <stddef.h>

/* Module cache entry. */
typedef struct {
    char       *name;
    mino_val *value;
} module_entry_t;

/* Bundled-stdlib registry entry. Source pointer is a static C-string
 * literal in a generated header (e.g. lib_clojure_string.h); never
 * freed. Name is malloc-owned (copied in mino_register_bundled_lib). */
typedef struct {
    char       *name;
    const char *source;
} bundled_lib_entry_t;

/* Metadata table entry. The capability is the install-group label
 * (e.g. "fs", "proc", "io", "host", "async") used by the doc and
 * mino-capability primitives. NULL means the binding is part of the
 * always-installed core and carries no capability gate. */
typedef struct {
    char       *name;
    char       *docstring;
    char       *capability;
    mino_val *source;
} meta_entry_t;

/* Call-stack frame for stack traces. */
#define MAX_CALL_DEPTH 256

typedef struct {
    const char *name;
    const char *file;
    int         line;
    int         column;
} call_frame_t;

/* GC root-environment registry node (malloc-owned). */
typedef struct root_env {
    mino_env      *env;
    struct root_env *next;
} root_env_t;

/* Host-retained value ref (malloc-owned). */
struct mino_ref {
    mino_val      *val;
    struct mino_ref *next;
    struct mino_ref *prev;
};

/* Dynamic binding frame. `var` is the canonical var the binding
 * targets -- pointer identity is the lookup key, so a binding
 * established under any spelling (bare, ns-qualified, alias-
 * qualified) is visible to every read of that var. NULL marks a
 * var-less dynamic-scope name, which falls back to text matching on
 * `name`. `name` is the var's interned bare name (or the literal
 * spelling when var == NULL). */
typedef struct dyn_binding {
    const char          *name;
    mino_val          *var;
    mino_val          *val;
    struct dyn_binding  *next;
} dyn_binding_t;

/* `building` is 1 while a `binding` form is still evaluating its
 * value forms: the GC root walk marks the frame's values (they are
 * only reachable through it), but lookups skip it so the bindings
 * install in parallel, not sequentially. */
typedef struct dyn_frame {
    dyn_binding_t       *bindings;
    int                  building;
    struct dyn_frame    *prev;
} dyn_frame_t;

/* Environment binding. */
typedef struct {
    char       *name;
    mino_val *val;
} env_binding_t;

/* Namespace alias entry. Each alias is owned by the namespace that
 * declared it via require/use/alias; the same alias name can mean
 * different targets in different namespaces. */
typedef struct {
    char *owning_ns;
    char *alias;
    char *full_name;
} ns_alias_t;

/* Per-namespace root env entry. */
typedef struct {
    const char *name;     /* interned ns name */
    mino_env *env;      /* root env for this ns; parent → clojure.core (or NULL for clojure.core itself) */
    mino_val *meta;     /* nil or a map of ns-level metadata */
} ns_env_entry_t;

/* Var registry entry. */
typedef struct {
    const char *ns;      /* interned namespace */
    const char *name;    /* interned name */
    mino_val *var;     /* the MINO_VAR value */
} var_entry_t;

/* Open-addressing hash slot for the var registry. Keyed on the
 * (ns*, name*) pointer pair: both are interned so equality is pointer
 * equality. ns == NULL marks an empty slot. */
typedef struct {
    const char *ns;
    const char *name;
    mino_val *var;
} var_hash_slot_t;

/* Record-type registry entry. Pinned for the life of the state so
 * MINO_TYPE values keep stable pointer identity across re-evaluation
 * of the same defrecord form. The fields vector is GC-owned and
 * traced via the registry walk in gc_mark_roots. */
typedef struct record_type_entry {
    const char               *ns;    /* interned ns */
    const char               *name;  /* interned name */
    mino_val               *type;  /* the MINO_TYPE value */
    struct record_type_entry *next;
} record_type_entry_t;

/* Full environment definition.
 * Large frames (>= ENV_HASH_THRESHOLD bindings) get a hash index for O(1)
 * lookup; small frames use linear scan (faster for typical let/fn sizes). */
#define ENV_HASH_THRESHOLD 32

/* Small-integer cache range. Must fit in the small_ints[] array (256 slots). */
#define MINO_SMALL_INT_LO (-128)
#define MINO_SMALL_INT_HI  127

struct mino_env {
    env_binding_t *bindings;
    size_t         len;
    size_t         cap;
    mino_env    *parent;
    size_t        *ht_buckets;  /* hash index: maps hash -> binding slot */
    size_t         ht_cap;      /* power of 2; SIZE_MAX = empty slot */
};

#endif /* RUNTIME_TYPES_H */
