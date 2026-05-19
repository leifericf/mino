/*
 * values/internal.h -- the value-layer API: constructors, interning,
 * hashing, equality, and the first-class var constructor.
 *
 * Implementations live in src/values/val.c.
 *
 * Interning depends on intern_table_t which is collection-owned
 * (its backing entries vector is structurally a vector). The struct
 * stays in collections/internal.h; this header declares the API that
 * operates on it.
 *
 * Internal to the runtime; embedders should only use mino.h.
 */

#ifndef VALUES_INTERNAL_H
#define VALUES_INTERNAL_H

#include "mino_internal.h"
#include "values/layout.h"

#include <stddef.h>
#include <stdint.h>

/* Forward decl of the collection-owned intern table. The struct body
 * lives in collections/internal.h; consumers that just need to declare
 * intern_lookup_or_create can include this header alone. */
struct intern_table;

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* val.c: constructors, interning, hashing, equality                         */
/* ------------------------------------------------------------------------- */

/* Interned values are GC-owned singletons (deduplicated by content). */
mino_val_t *intern_lookup_or_create(mino_state_t *S, struct intern_table *tbl,
                                    mino_type_t type,
                                    const char *s, size_t len);  /* GC-owned */
mino_val_t *make_fn(mino_state_t *S, mino_val_t *params, mino_val_t *body,
                    mino_env_t *env);                            /* GC-owned */

/* Hashing (pure, no allocation). */
uint32_t hash_val(const mino_val_t *v);
uint32_t fnv_mix(uint32_t h, unsigned char b);
uint32_t fnv_bytes(uint32_t h, const unsigned char *p, size_t n);

/* Equality (may force lazy seqs, triggering allocation). */
int mino_eq_force(mino_state_t *S, const mino_val_t *a, const mino_val_t *b);

/* First-class var constructor. */
mino_val_t *mino_mk_var(mino_state_t *S, const char *ns, const char *name,
                        mino_val_t *root);

/* Register the GC tracer for GC_T_VAL. Called from
 * runtime/state.c::state_init before the first allocation. */
void mino_values_register_gc_handlers(mino_state_t *S);

#ifdef __cplusplus
}
#endif

#endif /* VALUES_INTERNAL_H */
