/*
 * src/eval/bc/jit/helpers_loop.c -- loop-variant slow-path helpers.
 *
 * Factored out of helpers.c to keep each TU under the 1100-line
 * limit.  Every helper here mirrors a specific loop opcode's slow
 * path (cons-spine + prim dispatch + regs refresh after possible GC)
 * and honours the same contract as the helpers in helpers.c:
 *
 *   - regs is the live register-window base on entry.
 *   - return value is the (possibly relocated) regs base on success,
 *     NULL on hard failure; loop helpers additionally return
 *     (regs | 1) to signal that the loop should exit rather than
 *     iterate, and the deopt_exit helper returns mino_val * NULL
 *     with side effects on S.
 *
 * The loop-exit tagging convention: all loop slow-path helpers share
 * loop_tag_exit(), which sets the low bit of the regs pointer.  The
 * stencil caller tests the low bit before dereferencing.
 *
 * Non-loop slow-path helpers (binop, unop, call, collection, closure,
 * protocol, env, etc.) live in helpers.c.
 */

#include "internal.h"

#ifdef MINO_CPJIT_HOST

#include <stddef.h>
#include <stdint.h>

#include "../../../prim/internal.h"

/* Helper for the OP_LOOP_INT_LT exit-signal convention. Tags the low
 * bit of a regs pointer to signal "loop exits" to the caller; the
 * caller masks the bit off before dereferencing. */
static mino_val **loop_tag_exit(mino_val **regs)
{
    return (mino_val **)((uintptr_t)regs | (uintptr_t)1);
}

/* Slow path for OP_LOOP_INT_LT. Mirrors the interpreter's
 * cons + prim_lt + prim_inc dance. Returns:
 *   - NULL on cons OOM (caller propagates).
 *   - regs (low-bit-clear) if the loop should continue (back-jump):
 *     the counter has been incremented through prim_inc and written
 *     back to regs[a].
 *   - regs | 1 if the loop should exit (fall through to next stencil). */
mino_val **mino_jit_loop_int_lt_slow(mino_state *S, mino_val **regs,
                                       unsigned a, unsigned b)
{
    ptrdiff_t base = regs - S->bc.bc_regs;
    mino_val *list = mino_nil(S);
    if (list == NULL) return NULL;
    list = mino_cons(S, S->bc.bc_regs[base + b], list);
    if (list == NULL) return NULL;
    list = mino_cons(S, S->bc.bc_regs[base + a], list);
    if (list == NULL) return NULL;
    mino_val *ltv = prim_lt(S, list, NULL);
    if (ltv == NULL) return NULL;
    regs = S->bc.bc_regs + base;
    if (!mino_is_truthy_inline(ltv)) {
        return loop_tag_exit(regs);
    }
    mino_val *list2 = mino_nil(S);
    if (list2 == NULL) return NULL;
    list2 = mino_cons(S, regs[a], list2);
    if (list2 == NULL) return NULL;
    mino_val *incv = prim_inc(S, list2, NULL);
    if (incv == NULL) return NULL;
    regs = S->bc.bc_regs + base;
    regs[a] = incv;
    return regs;  /* low-bit-clear: continue */
}

/* Slow path for OP_LOOP_INT_DEC. Mirrors the interpreter's
 * cons + prim_zero_p + prim_dec dance. Same exit-signal convention as
 * mino_jit_loop_int_lt_slow. */
mino_val **mino_jit_loop_int_dec_slow(mino_state *S, mino_val **regs,
                                        unsigned a)
{
    ptrdiff_t base = regs - S->bc.bc_regs;
    mino_val *list = mino_nil(S);
    if (list == NULL) return NULL;
    list = mino_cons(S, S->bc.bc_regs[base + a], list);
    if (list == NULL) return NULL;
    mino_val *zp = prim_zero_p(S, list, NULL);
    if (zp == NULL) return NULL;
    regs = S->bc.bc_regs + base;
    if (mino_is_truthy_inline(zp)) {
        return loop_tag_exit(regs);
    }
    mino_val *list2 = mino_nil(S);
    if (list2 == NULL) return NULL;
    list2 = mino_cons(S, regs[a], list2);
    if (list2 == NULL) return NULL;
    mino_val *decv = prim_dec(S, list2, NULL);
    if (decv == NULL) return NULL;
    regs = S->bc.bc_regs + base;
    regs[a] = decv;
    return regs;
}

/* Slow path for OP_LOOP_INT_LT_INC. Mirrors the interpreter's
 * cons + prim_lt + prim_inc + prim_inc dance for the two-binding
 * counted-loop shape. Returns the same low-bit-tagged signal as the
 * single-binding LT helper. */
mino_val **mino_jit_loop_int_lt_inc_slow(mino_state *S, mino_val **regs,
                                            unsigned a, unsigned b,
                                            unsigned c)
{
    ptrdiff_t base = regs - S->bc.bc_regs;
    mino_val *list = mino_nil(S);
    if (list == NULL) return NULL;
    list = mino_cons(S, S->bc.bc_regs[base + b], list);
    if (list == NULL) return NULL;
    list = mino_cons(S, S->bc.bc_regs[base + a], list);
    if (list == NULL) return NULL;
    mino_val *ltv = prim_lt(S, list, NULL);
    if (ltv == NULL) return NULL;
    regs = S->bc.bc_regs + base;
    if (!mino_is_truthy_inline(ltv)) {
        return loop_tag_exit(regs);
    }
    mino_val *list2 = mino_nil(S);
    if (list2 == NULL) return NULL;
    list2 = mino_cons(S, regs[a], list2);
    if (list2 == NULL) return NULL;
    mino_val *incv = prim_inc(S, list2, NULL);
    if (incv == NULL) return NULL;
    regs = S->bc.bc_regs + base;
    mino_val *list3 = mino_nil(S);
    if (list3 == NULL) return NULL;
    list3 = mino_cons(S, regs[c], list3);
    if (list3 == NULL) return NULL;
    mino_val *incv2 = prim_inc(S, list3, NULL);
    if (incv2 == NULL) return NULL;
    regs = S->bc.bc_regs + base;
    regs[a] = incv;
    regs[c] = incv2;
    return regs;
}

/* Slow path for OP_LOOP_INT_DEC_INC. Mirrors the interpreter's
 * cons + prim_zero_p + prim_dec + prim_inc dance for the two-binding
 * reverse-counted loop shape. Returns the same low-bit-tagged signal
 * as the other loop helpers: NULL on OOM, regs|1 on exit, regs on
 * continue. */
mino_val **mino_jit_loop_int_dec_inc_slow(mino_state *S,
                                             mino_val **regs,
                                             unsigned a, unsigned b)
{
    ptrdiff_t base = regs - S->bc.bc_regs;
    mino_val *list = mino_nil(S);
    if (list == NULL) return NULL;
    list = mino_cons(S, S->bc.bc_regs[base + a], list);
    if (list == NULL) return NULL;
    mino_val *zp = prim_zero_p(S, list, NULL);
    if (zp == NULL) return NULL;
    regs = S->bc.bc_regs + base;
    if (mino_is_truthy_inline(zp)) {
        return loop_tag_exit(regs);
    }
    mino_val *list2 = mino_nil(S);
    if (list2 == NULL) return NULL;
    list2 = mino_cons(S, regs[a], list2);
    if (list2 == NULL) return NULL;
    mino_val *decv = prim_dec(S, list2, NULL);
    if (decv == NULL) return NULL;
    regs = S->bc.bc_regs + base;
    mino_val *list3 = mino_nil(S);
    if (list3 == NULL) return NULL;
    list3 = mino_cons(S, regs[b], list3);
    if (list3 == NULL) return NULL;
    mino_val *incv = prim_inc(S, list3, NULL);
    if (incv == NULL) return NULL;
    regs = S->bc.bc_regs + base;
    regs[a] = decv;
    regs[b] = incv;
    return regs;
}

/* Slow path for OP_LOOP_INT_LT_ACC: forward-counted loop where the
 * accumulator's step is (+ acc <reg-d>). Mirrors the interpreter's
 * cons + prim_lt + prim_inc + prim_add dance. Returns the same
 * low-bit-tagged signal as the other loop helpers. */
mino_val **mino_jit_loop_int_lt_acc_slow(mino_state *S,
                                            mino_val **regs,
                                            unsigned a, unsigned b,
                                            unsigned c, unsigned d)
{
    ptrdiff_t base = regs - S->bc.bc_regs;
    mino_val *list = mino_nil(S);
    if (list == NULL) return NULL;
    list = mino_cons(S, S->bc.bc_regs[base + b], list);
    if (list == NULL) return NULL;
    list = mino_cons(S, S->bc.bc_regs[base + a], list);
    if (list == NULL) return NULL;
    mino_val *ltv = prim_lt(S, list, NULL);
    if (ltv == NULL) return NULL;
    regs = S->bc.bc_regs + base;
    if (!mino_is_truthy_inline(ltv)) {
        return loop_tag_exit(regs);
    }
    mino_val *list2 = mino_nil(S);
    if (list2 == NULL) return NULL;
    list2 = mino_cons(S, regs[a], list2);
    if (list2 == NULL) return NULL;
    mino_val *incv = prim_inc(S, list2, NULL);
    if (incv == NULL) return NULL;
    regs = S->bc.bc_regs + base;
    mino_val *list3 = mino_nil(S);
    if (list3 == NULL) return NULL;
    list3 = mino_cons(S, regs[d], list3);
    if (list3 == NULL) return NULL;
    list3 = mino_cons(S, regs[c], list3);
    if (list3 == NULL) return NULL;
    mino_val *addv = prim_add(S, list3, NULL);
    if (addv == NULL) return NULL;
    regs = S->bc.bc_regs + base;
    regs[a] = incv;
    regs[c] = addv;
    return regs;
}

/* Slow path for OP_LOOP_INT_DEC_ACC: reverse-counted loop where the
 * accumulator's step is (+ acc <reg-d>). Mirrors the interpreter's
 * cons + prim_zero_p + prim_dec + prim_add dance. */
mino_val **mino_jit_loop_int_dec_acc_slow(mino_state *S,
                                             mino_val **regs,
                                             unsigned a, unsigned c,
                                             unsigned d)
{
    ptrdiff_t base = regs - S->bc.bc_regs;
    mino_val *list = mino_nil(S);
    if (list == NULL) return NULL;
    list = mino_cons(S, S->bc.bc_regs[base + a], list);
    if (list == NULL) return NULL;
    mino_val *zp = prim_zero_p(S, list, NULL);
    if (zp == NULL) return NULL;
    regs = S->bc.bc_regs + base;
    if (mino_is_truthy_inline(zp)) {
        return loop_tag_exit(regs);
    }
    mino_val *list2 = mino_nil(S);
    if (list2 == NULL) return NULL;
    list2 = mino_cons(S, regs[a], list2);
    if (list2 == NULL) return NULL;
    mino_val *decv = prim_dec(S, list2, NULL);
    if (decv == NULL) return NULL;
    regs = S->bc.bc_regs + base;
    mino_val *list3 = mino_nil(S);
    if (list3 == NULL) return NULL;
    list3 = mino_cons(S, regs[d], list3);
    if (list3 == NULL) return NULL;
    list3 = mino_cons(S, regs[c], list3);
    if (list3 == NULL) return NULL;
    mino_val *addv = prim_add(S, list3, NULL);
    if (addv == NULL) return NULL;
    regs = S->bc.bc_regs + base;
    regs[a] = decv;
    regs[c] = addv;
    return regs;
}

/* Shared validation, IC resolution, and env/base capture for the two
 * OP_PROTOCOL_*_CACHED slow helpers.  Performs bounds-check on the IC
 * slot, atom-shape-check, mino_bc_ic_resolve_protocol dispatch, and
 * snapshots the env and base the caller needs to drive
 * apply_callable_argv.  Returns the resolved impl on success, NULL on
 * any validation or resolution failure. */
static mino_val *jit_protocol_resolve(mino_state *S,
                                       mino_val **regs,
                                       unsigned a,
                                       unsigned argn,
                                       mino_bc_fn_t *bc,
                                       unsigned slot_idx,
                                       mino_env **out_env,
                                       ptrdiff_t *out_base)
{
    ptrdiff_t          base = regs - S->bc.bc_regs;
    mino_thread_ctx_t *ctx  = mino_current_ctx(S);
    *out_env  = ctx->jit_invoke_env;
    *out_base = base;
    if (bc->ic_slots_len <= 0 || slot_idx >= (unsigned)bc->ic_slots_len) return NULL;
    mino_bc_ic_slot_t *slot = &bc->ic_slots[slot_idx];
    if (argn < 1 || slot->atom == NULL
        || mino_type_of(slot->atom) != MINO_ATOM) {
        return NULL;
    }
    mino_val *first_arg = S->bc.bc_regs[base + a];
    return mino_bc_ic_resolve_protocol(S, bc, slot, first_arg);
}

/* OP_PROTOCOL_CALL_CACHED slow helper. Mirrors the interpreter's
 * handler bit-for-bit: bounds-check the slot, atom-shape-check, run
 * mino_bc_ic_resolve_protocol (handles the dispatch-table lookup, IC
 * refill under write barriers, MPR001 / MPR002 diagnostics), then
 * apply_callable_argv with the args sitting at regs[a..a+argn-1]
 * (note: regs[a] IS the first arg -- protocol dispatch uses the
 * leading positional argument as the type-discriminator source).
 * Stores the return at regs[ret]. */
mino_val **mino_jit_protocol_call_cached_slow(mino_state *S,
                                                 mino_val **regs,
                                                 unsigned a,
                                                 unsigned argn,
                                                 unsigned ret,
                                                 mino_bc_fn_t *bc,
                                                 unsigned slot_idx)
{
    mino_env  *env;
    ptrdiff_t  base;
    mino_val *impl = jit_protocol_resolve(S, regs, a, argn, bc, slot_idx,
                                           &env, &base);
    if (impl == NULL) return NULL;
    mino_val *r = apply_callable_argv(S, impl,
                                         S->bc.bc_regs + base + a,
                                         (int)argn, env);
    if (r == NULL) return NULL;
    regs      = S->bc.bc_regs + base;
    regs[ret] = r;
    return regs;
}

/* OP_PROTOCOL_TAILCALL_CACHED slow helper. Resolves the impl the same
 * way as the non-tail variant, but exits the JIT region by returning
 * the impl's result directly (no register write-back). The owning
 * stencil is FINAL: its return value is the fn's return value. */
mino_val *mino_jit_protocol_tailcall_cached_slow(mino_state *S,
                                                    mino_val **regs,
                                                    unsigned a,
                                                    unsigned argn,
                                                    mino_bc_fn_t *bc,
                                                    unsigned slot_idx)
{
    mino_env  *env;
    ptrdiff_t  base;
    mino_val *impl = jit_protocol_resolve(S, regs, a, argn, bc, slot_idx,
                                           &env, &base);
    if (impl == NULL) return NULL;
    return apply_callable_argv(S, impl,
                                S->bc.bc_regs + base + a,
                                (int)argn, env);
}

/* OP_MAKE_LAZY slow helper. Mirrors the interpreter's OP_MAKE_LAZY
 * cold-op handler: read the body bc from bc->consts[bx], allocate a
 * MINO_LAZY value, fill its fields (body, captured jit-invoke env,
 * cached=NULL, realized=0), and store it at regs[a]. The "slow"
 * suffix is borrowed from the arith stencils for naming consistency
 * even though there's no fast-path counterpart -- every OP_MAKE_LAZY
 * routes through this helper. */
mino_val **mino_jit_make_lazy_slow(mino_state *S, mino_val **regs,
                                      unsigned a, mino_bc_fn_t *bc,
                                      unsigned bx)
{
    ptrdiff_t          base = regs - S->bc.bc_regs;
    mino_thread_ctx_t *ctx  = mino_current_ctx(S);
    if (bx >= bc->consts_len) return NULL;
    mino_val *body = bc->consts[bx];
    gc_pin(body);
    mino_val *lz   = alloc_val(S, MINO_LAZY);
    gc_unpin(1);
    if (lz == NULL) return NULL;
    lz->as.lazy.body     = body;
    lz->as.lazy.env      = ctx->jit_invoke_env;
    lz->as.lazy.cached   = NULL;
    lz->as.lazy.defining_ns = S->ns_vars.fn_ambient_ns != NULL
                              ? S->ns_vars.fn_ambient_ns
                              : S->ns_vars.current_ns;
    lz->as.lazy.realized = LAZY_UNREALIZED;
    regs    = S->bc.bc_regs + base;
    regs[a] = lz;
    return regs;
}

/* Side-exit runtime helper. The deopt stencil tail-calls this when the
 * native prefix reaches the first PC the JIT couldn't compile; it
 * records the resume PC on the state and returns NULL so the native
 * region's final ret carries the deopt-sentinel value back to
 * mino_jit_invoke, which then continues dispatch through the
 * interpreter at S->jit_deopt_pc. */
mino_val *mino_jit_deopt_exit(mino_state *S, size_t resume_pc)
{
    S->jit_deopt_pc      = resume_pc;
    S->jit_deopt_pending = 1;
    return NULL;
}

#endif /* MINO_CPJIT_HOST */

/* Keep this TU non-empty under -Werror=pedantic when MINO_CPJIT_HOST
 * isn't defined for the build target. */
typedef int mino_jit_helpers_loop_tu_marker;
