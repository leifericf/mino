/*
 * atomic_ptr.h -- compiler-portable acquire/release atomics on a
 * `mino_val *` slot.
 *
 * meta.c (the alter-meta! retry loop) and stateful.c (atom and
 * watchable CAS) both publish a value pointer under a compare-and-swap
 * and need the same three-compiler surface the int shims in eval.c and
 * runtime/host_threads.h already carry: the GCC/Clang __atomic_*
 * builtins, the MSVC InterlockedCompareExchangePointer family, or a C11
 * <stdatomic.h> fallback. Pointer width is the only difference from the
 * int shims, so the two call sites share this header rather than each
 * inlining a guard.
 *
 * `expected` is taken by value: every caller either reuses its own
 * snapshot after a successful CAS or reloads at the top of a retry
 * loop, so the failure-path write-back of the builtins is not needed.
 */
#ifndef PRIM_ATOMIC_PTR_H
#define PRIM_ATOMIC_PTR_H

#include "prim/internal.h"   /* mino_val */

#if defined(__GNUC__) || defined(__clang__)

static inline mino_val *mino_atomic_load_acquire_ptr(mino_val **p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static inline int mino_atomic_cas_acqrel_ptr(mino_val **p,
                                             mino_val *expected,
                                             mino_val *desired)
{
    return __atomic_compare_exchange_n(p, &expected, desired, 0,
                                       __ATOMIC_RELEASE,
                                       __ATOMIC_RELAXED) ? 1 : 0;
}

#elif defined(_MSC_VER)

#include <windows.h>
static inline mino_val *mino_atomic_load_acquire_ptr(mino_val **p)
{
    return (mino_val *)InterlockedCompareExchangePointer(
        (PVOID volatile *)p, NULL, NULL);
}
static inline int mino_atomic_cas_acqrel_ptr(mino_val **p,
                                             mino_val *expected,
                                             mino_val *desired)
{
    return InterlockedCompareExchangePointer(
               (PVOID volatile *)p, (PVOID)desired, (PVOID)expected)
           == (PVOID)expected;
}

#else

#include <stdatomic.h>
static inline mino_val *mino_atomic_load_acquire_ptr(mino_val **p)
{
    return atomic_load_explicit((_Atomic(mino_val *) *)p,
                                memory_order_acquire);
}
static inline int mino_atomic_cas_acqrel_ptr(mino_val **p,
                                             mino_val *expected,
                                             mino_val *desired)
{
    return atomic_compare_exchange_strong_explicit(
        (_Atomic(mino_val *) *)p, &expected, desired,
        memory_order_acq_rel, memory_order_acquire) ? 1 : 0;
}

#endif

#endif /* PRIM_ATOMIC_PTR_H */
