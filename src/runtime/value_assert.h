/*
 * value_assert.h -- tagged-value debug invariants, type-discriminator,
 * unified accessors, and checked size arithmetic.
 *
 * Internal to the runtime; embedders should only use mino.h.
 *
 * No transitive dependencies on subsystem internal headers. Anyone who
 * needs a quick way to inspect a pointer-tagged mino_val_t or compute
 * a checked size_t can include just this file.
 */

#ifndef RUNTIME_VALUE_ASSERT_H
#define RUNTIME_VALUE_ASSERT_H

#include "mino_internal.h"

#include <assert.h>
#include <stdint.h>

/* ------------------------------------------------------------------------- */
/* Tagged-value debug invariants                                             */
/* ------------------------------------------------------------------------- */

/*
 * Internal assertion helpers for the pointer-tagged representation
 * (see mino.h "Pointer-tagged value representation"). Active in
 * builds with assertions enabled; compile to no-ops under -DNDEBUG.
 * Runtime-internal; embedders use the public MINO_IS_* / MINO_*_VAL
 * macros directly.
 */
#define MINO_ASSERT_INT(v)            assert(MINO_IS_INT(v))
#define MINO_ASSERT_PTR(v)            assert(MINO_IS_PTR(v))
#define MINO_ASSERT_TAGGED_NONNULL(v) assert((v) != NULL)

/* Alignment guard for newly-allocated heap objects: every alloc site
 * must return a pointer with the low three bits clear, otherwise the
 * tag scheme silently corrupts. */
#define MINO_ASSERT_ALIGNED(p) \
    assert(((uintptr_t)(p) & MINO_TAG_MASK) == 0)

/* Effective type discriminator: returns the inline-tagged type for
 * tagged scalars, MINO_NIL for NULL, otherwise the boxed header type.
 * Use this in switch / type comparisons so the dispatch is
 * form-agnostic and NULL-safe. */
static inline mino_type_t mino_type_of(const mino_val_t *v)
{
    uintptr_t tag;
    if (v == NULL) return MINO_NIL;
    tag = (uintptr_t)v & MINO_TAG_MASK;
    if (tag == MINO_TAG_PTR) return v->type;
    if (tag == MINO_TAG_INT) return MINO_INT;
    if (tag == MINO_TAG_BOOL) return MINO_BOOL;
    if (tag == MINO_TAG_NIL) return MINO_NIL;
    if (tag == MINO_TAG_CHAR) return MINO_CHAR;
    return v->type; /* unreachable: reserved tags */
}

/* Unified accessors that handle both inline-tagged scalars and boxed
 * cells. The constructors return tagged for in-range / supported
 * cases; readers go through these helpers to stay form-agnostic. The
 * boxed form is still reachable for out-of-tagged-range ints, so each
 * helper handles both forms. */
static inline int mino_val_int_p(const mino_val_t *v)
{
    return mino_type_of(v) == MINO_INT;
}

static inline long long mino_val_int_get(const mino_val_t *v)
{
    return MINO_IS_INT(v) ? MINO_INT_VAL(v) : v->as.i;
}

static inline int mino_val_bool_p(const mino_val_t *v)
{
    return mino_type_of(v) == MINO_BOOL;
}

static inline int mino_val_bool_get(const mino_val_t *v)
{
    return MINO_IS_BOOL(v) ? MINO_BOOL_VAL(v) : v->as.b;
}

static inline int mino_val_char_p(const mino_val_t *v)
{
    return mino_type_of(v) == MINO_CHAR;
}

static inline int mino_val_char_get(const mino_val_t *v)
{
    return MINO_IS_CHAR(v) ? MINO_CHAR_VAL(v) : v->as.ch;
}

/* Checked size-arithmetic helpers used by dynamic-growth code paths.
 * Each returns 1 on success (storing the result through `out`) and 0 on
 * overflow (leaving `*out` untouched). Callers route the overflow case
 * into the same OOM/diag path they already use for `realloc` failure.
 * Inline so the compiler can fold them through the surrounding cap and
 * length comparisons; static so each TU emits its own copy if needed. */
static inline int checked_add_sz(size_t a, size_t b, size_t *out)
{
    if (a > SIZE_MAX - b) return 0;
    *out = a + b;
    return 1;
}

static inline int checked_mul_sz(size_t a, size_t b, size_t *out)
{
    if (b != 0 && a > SIZE_MAX / b) return 0;
    *out = a * b;
    return 1;
}

static inline int checked_double_sz(size_t cap, size_t *out)
{
    if (cap > SIZE_MAX / 2) return 0;
    *out = cap * 2;
    return 1;
}

#endif /* RUNTIME_VALUE_ASSERT_H */
