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
 * conservative; subsequent releases tune it through measurement. */
#define MINO_JIT_THRESHOLD 100u

/* One JIT'd code region. Carried in a per-state linked list so
 * mino_state_free can munmap every page on teardown. The structure
 * lives in jit.c when MINO_CPJIT is defined; the type stays declared
 * here unconditionally so mino_state_t's pointer field is well-typed. */
struct mino_jit_region {
    void                   *ptr;
    size_t                  size;
    struct mino_jit_region *next;
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
                                  struct mino_val **consts);

/* Tear down every region in the state's jit_regions list. Called from
 * mino_state_free. */
void mino_jit_free_all(struct mino_state *S);

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

#endif /* MINO_CPJIT */

#endif /* MINO_BC_JIT_H */
