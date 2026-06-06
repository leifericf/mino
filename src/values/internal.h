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
mino_val *intern_lookup_or_create(mino_state *S, struct intern_table *tbl,
                                    mino_type type,
                                    const char *s, size_t len);  /* GC-owned */
mino_val *make_fn(mino_state *S, mino_val *params, mino_val *body,
                    mino_env *env);                            /* GC-owned */

/* Hashing (pure, no allocation). */
uint32_t hash_val(const mino_val *v);
uint32_t fnv_mix(uint32_t h, unsigned char b);
uint32_t fnv_bytes(uint32_t h, const unsigned char *p, size_t n);

/* Equality (may force lazy seqs, triggering allocation). */
int mino_eq_force(mino_state *S, const mino_val *a, const mino_val *b);

/* First-class var constructor. */
mino_val *mino_mk_var(mino_state *S, const char *ns, const char *name,
                        mino_val *root);

/* PersistentQueue internal helpers — implemented in collections/queue.c.
 * Public ctor / accessors live on the embedder surface (mino_queue_*). */
int       mino_queue_eq  (const mino_val *a, const mino_val *b);
mino_val *mino_queue_nth(const mino_val *q, size_t i);

/* MINO_BYTES internal helpers — implemented in collections/bytes.c.
 * Public ctor / accessors live on the embedder surface (mino_bytes_*). */
int                  mino_bytes_eq  (const mino_val *a, const mino_val *b);
uint32_t             mino_bytes_hash(const mino_val *v);
mino_val          *mino_bytes_seq (mino_state *S, const mino_val *v);

/* Register the GC tracer for GC_T_VAL. Called from
 * runtime/state.c::state_init before the first allocation. */
void mino_values_register_gc_handlers(mino_state *S);

#ifdef __cplusplus
}
#endif

#endif /* VALUES_INTERNAL_H */
