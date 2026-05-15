/*
 * src/eval/bc/stencils/runtime_layout.h
 *
 * Stencil-layer view of the runtime struct layouts the JIT's inline
 * fast paths read.
 *
 * Stencil .c sources are compiled standalone with `-fno-builtin
 * -fno-optimize-sibling-calls` and no -I path beyond the stencils/
 * directory, so they cannot reach the canonical runtime headers
 * (`mino.h`, `mino_internal.h`, `runtime/internal.h`,
 * `eval/bc/internal.h`). When a stencil's hot path needs to read
 * `S->ic_gen` or `bc->ic_slots[idx]` inline -- skipping the bl into a
 * slow helper that the coverage cycle's stencils route through -- it
 * needs visibility into those layouts without dragging the runtime
 * header tree into the stencil compilation unit.
 *
 * This header is the curated bridge:
 *
 *   - Mirrors the field set the stencils need (currently the IC slot
 *     full layout plus offset anchors for `mino_state_t::ic_gen`,
 *     `mino_state_t::bc_regs`, `mino_state_t::main_ctx`,
 *     `mino_thread_ctx_t::dyn_stack`, and `mino_bc_fn_t::ic_slots`).
 *   - Re-exports the tagged-int encoding macros from
 *     `src/mino_internal.h`.
 *   - Declares the `mino_tls_ctx` TLS slot so stencils can resolve
 *     the active ctx inline.
 *
 * Every typedef and tag macro is gated by the corresponding
 * canonical-header guard so this file remains safely includable from
 * runtime translation units (notably `src/eval/bc/jit.c`) that also
 * see the canonical definitions; in that scenario only the offset
 * constants and accessor macros materialise. jit.c uses that
 * dual-visibility to fire `_Static_assert`s that verify every offset
 * here matches the live `offsetof(...)` value, so any layout drift in
 * the canonical structs surfaces as a compile error before a stencil
 * could mis-read.
 *
 * No stencil .c file consumes this header yet at v0.210.0 -- the
 * generated stencil byte tables are unchanged. Subsequent releases
 * route their inlined hot paths through these macros.
 */

#ifndef MINO_BC_STENCIL_RUNTIME_LAYOUT_H
#define MINO_BC_STENCIL_RUNTIME_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward struct tags. Used unconditionally by the accessor macros
 * below; the matching typedefs are introduced selectively so callers
 * that already see the canonical typedefs don't get a duplicate
 * declaration. The MINO_BC_STENCIL_ABI_H guard catches the stencil-
 * layer case (where abi.h has already typedef'd `mino_val_t`,
 * `mino_state_t`, and `mino_bc_fn_t`) without dragging in the full
 * runtime headers. */
struct mino_val;
struct mino_state;
struct mino_thread_ctx;
struct mino_bc_fn;
struct mino_env;
struct dyn_frame;
struct mino_bc_ic_slot;

/* === Public typedefs (mino.h-equivalent) ============================ */

#if !defined(MINO_H) && !defined(MINO_BC_STENCIL_ABI_H)
typedef struct mino_val   mino_val_t;
typedef struct mino_state mino_state_t;
typedef struct mino_env   mino_env_t;
#endif

/* === Runtime-internal typedefs (runtime/internal.h-equivalent) ===== */

#ifndef RUNTIME_INTERNAL_H
typedef struct mino_thread_ctx mino_thread_ctx_t;
typedef struct dyn_frame       dyn_frame_t;
#endif

/* === Bytecode-internal typedefs (eval/bc/internal.h-equivalent) ==== */

#if !defined(MINO_EVAL_BC_INTERNAL_H) && !defined(MINO_BC_STENCIL_ABI_H)
typedef struct mino_bc_fn mino_bc_fn_t;
#endif

#ifndef MINO_EVAL_BC_INTERNAL_H

/* IC slot mirror. Field set, ordering, and trailing PROTOCOL-only
 * fields all match `struct mino_bc_ic_slot` in
 * src/eval/bc/internal.h. jit.c asserts the field offsets and total
 * size against the live struct. */
typedef enum {
    MINO_BC_IC_GLOBAL   = 0,
    MINO_BC_IC_PROTOCOL = 1
} mino_bc_ic_kind_t;

typedef struct mino_bc_ic_slot {
    mino_val_t   *sym;
    mino_val_t   *cached;
    unsigned      gen;
    unsigned char kind;
    /* PROTOCOL-only fields. Zero / NULL when kind == MINO_BC_IC_GLOBAL. */
    mino_val_t   *atom;
    mino_val_t   *cached_map;
    mino_val_t   *cached_type;
} mino_bc_ic_slot_t;
#endif

/* === Tagged-int encoding =========================================== */

/* Bit-identical to src/mino_internal.h. Gated so the canonical
 * definitions win when both headers are reachable. The MINO_MAKE_INT
 * production cast uses `struct mino_val *` rather than `mino_val_t *`
 * so it stays valid in stencil compilation units that don't typedef
 * the latter through mino.h. */
#ifndef MINO_INTERNAL_H
#define MINO_TAG_BITS  3
#define MINO_TAG_MASK  ((uintptr_t)0x7)
#define MINO_TAG_PTR   ((uintptr_t)0x0)
#define MINO_TAG_INT   ((uintptr_t)0x1)
#define MINO_TAG_BOOL  ((uintptr_t)0x2)
#define MINO_TAG_NIL   ((uintptr_t)0x3)
#define MINO_TAG_CHAR  ((uintptr_t)0x4)
#define MINO_TAG(v)    ((uintptr_t)(v) & MINO_TAG_MASK)
#define MINO_IS_INT(v) (MINO_TAG(v) == MINO_TAG_INT)
#define MINO_INT_MAX   ((long long)((1ULL << 60) - 1))
#define MINO_INT_MIN   (-((long long)(1LL << 60)))
#define MINO_MAKE_INT(n)                                                    \
    ((struct mino_val *)((uintptr_t)((uint64_t)(long long)(n)               \
                                     << MINO_TAG_BITS)                      \
                         | MINO_TAG_INT))
#define MINO_INT_VAL(v) (((long long)(intptr_t)(v)) >> MINO_TAG_BITS)
#define MINO_TRUE_PAYLOAD  ((uintptr_t)0x1)
#define MINO_FALSE_PAYLOAD ((uintptr_t)0x0)
#define MINO_MAKE_BOOL(b)                                                   \
    ((struct mino_val *)(((b) ? MINO_TRUE_PAYLOAD : MINO_FALSE_PAYLOAD)     \
                         << MINO_TAG_BITS                                   \
                         | MINO_TAG_BOOL))
#endif

/* === Layout-anchor offsets ========================================= */

/* Measured on ARM64 Darwin with the canonical -std=c99 -O2 CFLAGS.
 * jit.c's MINO_JIT_LAYOUT_VERIFY block asserts each against
 * offsetof(<real struct>, <field>) so canonical-side drift fires a
 * compile error before any stencil could mis-read. Change one
 * number here only after updating the corresponding _Static_assert
 * in jit.c and re-running gen-stencils. */
#define MINO_JIT_LAYOUT_OFFSET_STATE_IC_GEN         ((size_t)47824)
#define MINO_JIT_LAYOUT_OFFSET_STATE_BC_REGS        ((size_t)47856)
#define MINO_JIT_LAYOUT_OFFSET_STATE_JIT_INVOKE_CTX ((size_t)47904)
#define MINO_JIT_LAYOUT_OFFSET_CTX_DYN_STACK        ((size_t)25712)
#define MINO_JIT_LAYOUT_OFFSET_BC_IC_SLOTS          ((size_t)72)

/* === Field accessors =============================================== */

/* Plain pointer-cast reads at the verified offsets. The C compiler
 * folds the offset into the load's immediate when representable,
 * falling back to an `add` + `ldr` pair when the offset overruns
 * the encodable range. Macros (rather than inline fns) so the
 * compilation unit doesn't sprout a `bl` into a one-instruction
 * accessor when called from a stencil. */
#define MINO_JIT_STATE_IC_GEN(S)                                            \
    (*(unsigned *)((char *)(S) + MINO_JIT_LAYOUT_OFFSET_STATE_IC_GEN))

#define MINO_JIT_STATE_BC_REGS(S)                                           \
    (*(struct mino_val ***)((char *)(S) + MINO_JIT_LAYOUT_OFFSET_STATE_BC_REGS))

#define MINO_JIT_CTX_DYN_STACK(ctx)                                         \
    (*(struct dyn_frame **)((char *)(ctx) + MINO_JIT_LAYOUT_OFFSET_CTX_DYN_STACK))

#define MINO_JIT_BC_IC_SLOTS(bc)                                            \
    (*(struct mino_bc_ic_slot **)((char *)(bc)                              \
                                  + MINO_JIT_LAYOUT_OFFSET_BC_IC_SLOTS))

/* Resolve the active per-thread ctx. `mino_jit_invoke` publishes the
 * calling thread's ctx into `S->jit_invoke_ctx` before jumping into
 * the JIT region and restores it on return, so stencils reach the
 * ctx through a single fixed-offset load from S -- no Darwin TLVP
 * relocation in the stencil bytes (the stencil_extract tool does not
 * model TLV-class relocations). NULL outside an active JIT region. */
#define MINO_JIT_INVOKE_CTX(S)                                              \
    (*(struct mino_thread_ctx **)((char *)(S)                               \
                                  + MINO_JIT_LAYOUT_OFFSET_STATE_JIT_INVOKE_CTX))

#ifdef __cplusplus
}
#endif

#endif /* MINO_BC_STENCIL_RUNTIME_LAYOUT_H */
