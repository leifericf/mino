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
 * `S->ns_vars.ic_gen` or `bc->ic_slots[idx]` inline -- skipping the bl into a
 * slow helper that the coverage cycle's stencils route through -- it
 * needs visibility into those layouts without dragging the runtime
 * header tree into the stencil compilation unit.
 *
 * This header is the curated bridge:
 *
 *   - Mirrors the field set the stencils need (currently the IC slot
 *     full layout plus offset anchors for `mino_state::ic_gen`,
 *     `mino_state::bc_regs`, `mino_state::main_ctx`,
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
 * Stencils that need to read or write runtime-struct fields include
 * this header; the offset macros + _Static_asserts keep their byte
 * tables in lockstep with the canonical struct layout.
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
 * layer case (where abi.h has already typedef'd `mino_val`,
 * `mino_state`, and `mino_bc_fn_t`) without dragging in the full
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
typedef struct mino_val   mino_val;
typedef struct mino_state mino_state;
typedef struct mino_env   mino_env;
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
    mino_val   *sym;
    mino_val   *cached;
    unsigned      gen;
    unsigned char kind;
    /* GLOBAL-kind callable-shape fields (see internal.h for the
     * authoritative layout note). */
    unsigned char cached_callable_kind;
    unsigned char cached_fn_has_rest;
    unsigned char _pad_ic0;
    /* PROTOCOL-only fields. Zero / NULL when kind == MINO_BC_IC_GLOBAL. */
    mino_val   *atom;
    mino_val   *cached_map;
    mino_val   *cached_type;
    unsigned short cached_fn_n_params;
    unsigned short _pad_ic1;
    unsigned       _pad_ic2;
    /* MINO_FN_BC_SINGLE-only mirror; see internal.h. */
    void          *cached_bc;
} mino_bc_ic_slot_t;

/* Stable tags matching mino_ic_callable_kind_t. Defined as #defines
 * so they can be referenced from stencil sources without pulling in
 * the full bc internal header. */
#define MINO_IC_CALLABLE_NONE              0
#define MINO_IC_CALLABLE_PRIM_ARGV         1
#define MINO_IC_CALLABLE_MINO_FN_BC_SINGLE 2
#define MINO_IC_CALLABLE_MINO_FN_BC_MULTI  3
#define MINO_IC_CALLABLE_OTHER             4
#endif

/* === Tagged-int encoding =========================================== */

/* Bit-identical to src/mino_internal.h. Gated so the canonical
 * definitions win when both headers are reachable. The MINO_MAKE_INT
 * production cast uses `struct mino_val *` rather than `mino_val *`
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

/* Identical on every JIT target and libc pairing by construction:
 * the anchored fields live in POD-only blocks placed at the head of
 * their structs (see struct mino_state in runtime/internal.h and
 * struct mino_thread_ctx in runtime/thread_ctx.h), ahead of any
 * libc-defined type (jmp_buf, pthread_*), so no target-dependent
 * size can shift them. The layout asserts in eval/bc/jit/entry.c
 * verify each constant against offsetof(<real struct>, <field>) on
 * every JIT-enabled host build, so drift on any target fires a
 * compile error before a stencil could mis-read. Change a number
 * here only together with the struct layout, then re-run
 * gen-stencils-all. */
#define MINO_JIT_LAYOUT_OFFSET_STATE_IC_GEN         ((size_t)152)
#define MINO_JIT_LAYOUT_OFFSET_STATE_BC_REGS        ((size_t)0)
#define MINO_JIT_LAYOUT_OFFSET_STATE_JIT_INVOKE_CTX ((size_t)192)
#define MINO_JIT_LAYOUT_OFFSET_CTX_DYN_STACK        ((size_t)0)
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
 * calling thread's ctx into `S->jit.jit_invoke_ctx` before jumping into
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
