/*
 * src/eval/bc/jit.h -- copy-and-patch JIT public interface.
 *
 * The runtime calls `mino_jit_compile` when a bc fn's hot_counter crosses
 * `MINO_JIT_THRESHOLD`; the compiler materialises a sequence of stencils
 * from src/eval/bc/stencils/generated/ into an mmap'd RWX page, patches
 * the relocations with the per-instruction operands, mprotects to RX,
 * and stores the resulting pointer in bc->native. apply_callable's
 * tier-selection branch picks the native arm when bc->native != NULL
 * and bc->native_gen matches S->ic_gen.
 *
 * The whole module is compiled in only when MINO_CPJIT is defined; the
 * default build is unaffected. The `jit_regions` slot in mino_state_t
 * stays present in both configurations so the field offsets don't drift
 * between builds (cleanup walks NULL when JIT is disabled).
 */

#ifndef MINO_BC_JIT_H
#define MINO_BC_JIT_H

#include "../../runtime/internal.h"
#include "internal.h"

/* Hot threshold: a fn warming past this count under the interpreter
 * triggers a single JIT compile attempt. The number is intentionally
 * conservative; subsequent releases tune it through measurement.
 * Embedders override per-state via mino_state_set_jit_hot_threshold. */
#define MINO_JIT_THRESHOLD 100u

/* MINO_CPJIT_HOST_DETECTED: 1 when this build was compiled with
 * MINO_CPJIT AND the host arch / OS is supported by the JIT module.
 * Mirrors the detection cascade in eval/bc/jit/internal.h; exposed
 * here so non-JIT translation units (state.c, the capability query)
 * can branch on the build-time host check without pulling in the
 * full JIT private header. */
#if defined(MINO_CPJIT) && (                                           \
    (defined(__aarch64__) && defined(__APPLE__))                       \
 || (defined(__aarch64__) && defined(__linux__)                        \
        && defined(MINO_CPJIT_ARM64_LINUX))                            \
 || (defined(__x86_64__) && defined(__linux__)                         \
        && defined(MINO_CPJIT_X86_64_LINUX))                           \
 || (defined(__x86_64__) && defined(__APPLE__)                         \
        && defined(MINO_CPJIT_X86_64_DARWIN))                          \
 || (defined(__x86_64__) && defined(_WIN32)                            \
        && defined(MINO_CPJIT_X86_64_WINDOWS)))
#define MINO_CPJIT_HOST_DETECTED 1
#else
#define MINO_CPJIT_HOST_DETECTED 0
#endif

/* Deopt model.
 *
 * A JIT'd region's `native_gen` is the `S->ic_gen` snapshot at the
 * time the region was emitted. Anything that bumps `ic_gen`
 * (def / ns-unmap / var_set_root / var_unintern) renders the
 * region's globally-cached resolutions stale; the dispatch path
 * in apply_callable detects the mismatch on the next invocation
 * and drops the runtime-visible `native` and `native_pc_offsets`
 * pointers so the call falls through to the interpreter. The
 * backing buffers stay owned by the state's jit_regions list until
 * `mino_state_free` reaps them at teardown.
 *
 * The hot counter resets to zero on deopt; the fn must warm to
 * `MINO_JIT_THRESHOLD` again before another compile attempt fires.
 * This is the steady-state pressure-relief valve for code that
 * pingpongs between definitions.
 *
 * Mid-execution invalidation (ic_gen bumped while a JIT'd fn is
 * still running on a stack frame) is not yet handled and is moot
 * for v0.187.0 because the only stencils that exist
 * (MOVE / LOAD_K / RETURN) cannot call back into mino-land and
 * therefore cannot observe a mid-frame `def`. When stencils that
 * call into user code arrive, each safe-point inside them will
 * snapshot ic_gen and bail to the interpreter on mismatch. */

/* One JIT'd code region. Carried in a per-state linked list so
 * mino_state_free can munmap every page on teardown. The aux_ptr
 * slot holds any malloc'd auxiliary data the JIT compile attached
 * to the region (currently the per-pc native-offset side table);
 * the free path calls `free(aux_ptr)` before munmap'ing `ptr`.
 * The structure lives in jit.c when MINO_CPJIT is defined; the type
 * stays declared here unconditionally so mino_state_t's pointer
 * field is well-typed. */
struct mino_jit_region {
    void                   *ptr;
    size_t                  size;
    void                   *aux_ptr;
    struct mino_jit_region *next;
};

/* One JIT slab: a host-page-sized RW/RX-cycled buffer that bump-
 * allocates [code|tramps|pool] slots for multiple small fns. The
 * legacy `mino_jit_region` path mmaps one page per fn, leaving
 * 95%+ of each page dead for sub-KB bodies; the slab pool packs
 * those bodies together so the per-fn JIT memory footprint drops
 * to roughly slot density × page size.
 *
 * `live_slots` is the refcount of bc records that still own a slot
 * inside `page`; once it reaches zero, the next sweep can munmap
 * the page. The bump cursor never reuses freed slot bytes within a
 * slab -- new compiles always extend forward. */
struct mino_jit_slab {
    void                 *page;        /* mmap'd page, RX-sealed between compiles */
    size_t                page_size;   /* host page size at slab creation */
    size_t                bump_offset; /* next free byte (16-aligned) */
    unsigned              live_slots;  /* count of bc records owning a slot here */
    struct mino_jit_slab *next;
};

#ifdef MINO_CPJIT

/* Walk the bc and decide whether every opcode has a stencil and the
 * fn has the shape the runtime currently supports (single arity, no
 * captures, no IC slots). Returns 1 when the fn is JIT-eligible and 0
 * otherwise. */
int mino_jit_eligible(const mino_bc_fn_t *bc);

/* Materialise the JIT'd code for `fn` into an RWX page; populate
 * bc->native, bc->native_size, bc->native_gen on success. Returns 0
 * on success, -1 when the fn is ineligible or any system call fails.
 * Idempotent: re-compile attempts on a fn that already has native
 * return 0 without recompiling. */
int mino_jit_compile(struct mino_state *S, struct mino_val *fn);

/* Invoke a JIT'd fn. The runtime sets up the register window before
 * calling; the native code reads regs/consts and returns the OP_RETURN
 * value in x0. Returns NULL when the runtime detects a stale
 * native_gen, signalling a deopt back to the interpreter. */
struct mino_val *mino_jit_invoke(struct mino_state *S,
                                  struct mino_bc_fn *bc,
                                  struct mino_val **regs,
                                  struct mino_val **consts,
                                  struct mino_env *env);

/* Tear down every region in the state's jit_regions list. Called from
 * mino_state_free. */
void mino_jit_free_all(struct mino_state *S);

/* Release one slot's claim on a slab. Decrements live_slots; on the
 * last claim, unlinks the slab from S->jit_slabs and munmaps the
 * page. Called from mino_jit_invalidate when a slab-allocated bc
 * record gives up its native slot. */
void mino_jit_slab_release(struct mino_state *S, struct mino_jit_slab *slab);

/* Reverse-lookup: which bytecode pc owns the stencil whose bytes cover
 * `native_off` in bc->native? Returns -1 when the offset is out of
 * range or the fn has no offset table. Cold path; intended for stack
 * trace formatting and debugger introspection. */
long mino_jit_offset_to_pc(const mino_bc_fn_t *bc, unsigned native_off);

/* Drop the runtime-visible JIT state for `fn` without unmapping the
 * underlying region. The next call falls through to the interpreter;
 * the fn's hot counter resets so a fresh compile is gated by the
 * full threshold again. The mmap'd code and offset table stay owned
 * by the state's jit_regions list and live until state teardown.
 *
 * This is the public deopt primitive. The runtime calls it on ic_gen
 * mismatch from apply_callable. A future breakpoint mechanism, a
 * profiler that wants to re-instrument a hot fn, or any other client
 * that needs to take a JIT'd fn off the native path calls it too.
 * Safe on a fn without a native region; reduces to a no-op. */
void mino_jit_invalidate(struct mino_state *S, struct mino_val *fn);

#else

/* Stubs so callers in fn.c / state.c don't need to wrap their call
 * sites in #ifdef. None of them allocate or call into the JIT path
 * when MINO_CPJIT is disabled. */
static inline int  mino_jit_eligible(const mino_bc_fn_t *bc) { (void)bc; return 0; }
static inline int  mino_jit_compile(struct mino_state *S, struct mino_val *fn) {
    (void)S; (void)fn; return -1;
}
static inline struct mino_val *mino_jit_invoke(struct mino_state *S,
                                                struct mino_bc_fn *bc,
                                                struct mino_val **regs,
                                                struct mino_val **consts)
{
    (void)S; (void)bc; (void)regs; (void)consts; return NULL;
}
static inline void mino_jit_free_all(struct mino_state *S) { (void)S; }
static inline void mino_jit_slab_release(struct mino_state *S,
                                          struct mino_jit_slab *slab) {
    (void)S; (void)slab;
}
static inline long mino_jit_offset_to_pc(const mino_bc_fn_t *bc, unsigned off) {
    (void)bc; (void)off; return -1;
}
static inline void mino_jit_invalidate(struct mino_state *S, struct mino_val *fn) {
    (void)S; (void)fn;
}

#endif /* MINO_CPJIT */

#endif /* MINO_BC_JIT_H */
