/*
 * src/eval/bc/jit/helpers.c -- cold helpers stencils call into.
 *
 * Every helper here mirrors a specific interpreter op's slow path
 * (cons-spine + prim dispatch + regs refresh after possible GC) so
 * the JIT region falls through to the same Clojure-correct
 * semantics whenever the inline fast path declines (tag miss,
 * overflow, IC miss). The contract every helper honours:
 *
 *   - input regs is the live register-window base on entry.
 *   - the helper may alloc / call prims / trigger GC, so it
 *     snapshots `base = regs - S->bc.bc_regs` before any call that
 *     may relocate the regs array, then restores the pointer with
 *     `regs = S->bc.bc_regs + base` after.
 *   - return value is the (possibly relocated) regs base on success,
 *     NULL on hard failure that the stencil's caller propagates back
 *     up the JIT region as a NULL return.
 *
 * The address of each helper is exported to stencil-side machine code
 * through the extern-fn table in entry.c.
 *
 * Loop-variant slow-path helpers (mino_jit_loop_int_*_slow, loop_tag_exit,
 * and mino_jit_deopt_exit) live in helpers_loop.c, which was split out
 * to keep each TU under the 1100-line limit.
 */

#include "internal.h"

#ifdef MINO_CPJIT_HOST

#include <stddef.h>
#include <stdint.h>

#include "../../../prim/internal.h"

/* Backward-jump safepoint. Called by the OP_SAFEPOINT_POLL stencil
 * the emit pass places before every direct-emit backward OP_JMP /
 * OP_JMPIFNOT, mirroring the interpreter's poll-on-backward-jump
 * rule. The per-thread tick counter amortises the full
 * mino_bc_safepoint poll to every MINO_JIT_BACKJUMP_TICKS-th
 * traversal -- the cadence the fused loop stencils keep in a
 * register. The counter must be per-thread (ctx, not S or bc):
 * a shared counter lets one thread absorb every zero crossing and
 * starve a sibling's cancel poll. On cancel mino_bc_safepoint
 * raises through longjmp under an active try frame; with no try
 * frame on the worker -- the future-body norm -- it returns 0 and
 * this helper returns NULL, which the stencil carries out of the
 * JIT region as a real ret (never into the chain, where the next
 * stencil would dereference it). mino_bc_safepoint never
 * allocates, so no regs refresh is needed. */
mino_val **mino_jit_backjump_safepoint(mino_state *S, mino_val **regs)
{
    mino_thread_ctx_t *ctx = mino_current_ctx(S);
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_expect(--ctx->jit_backjump_ticks > 0, 1)) {
#else
    if (--ctx->jit_backjump_ticks > 0) {
#endif
        return regs;
    }
    ctx->jit_backjump_ticks = MINO_JIT_BACKJUMP_TICKS;
    if (!mino_bc_safepoint_batch(S, MINO_JIT_BACKJUMP_TICKS)) return NULL;
    return regs;
}

/* Slow path for the arith stencils. Mirrors the interpreter's OP_*_II
 * fallback: build a two-element cons list rooted in the bytecode
 * register window, dispatch to the matching numeric prim, and store
 * the result through the (possibly relocated) regs base before
 * returning that base to the caller. Returns NULL only when the prim
 * itself returns NULL -- in practice prims raise through longjmp on
 * type errors, so the NULL return is the defensive case the stencil's
 * caller propagates back up the JIT region. */
mino_val **mino_jit_binop_slow(mino_state *S, mino_val **regs,
                                 unsigned a, unsigned b, unsigned c,
                                 unsigned subop)
{
    ptrdiff_t base = regs - S->bc.bc_regs;
    mino_val *list = mino_nil(S);
    if (list == NULL) return NULL;
    /* Read regs[b] / regs[c] through the freshly-rebased pointer at
     * every step: a GC inside mino_cons can reallocate bc_regs and
     * leave the C local stale. The base offset stays valid. */
    list = mino_cons(S, S->bc.bc_regs[base + c], list);
    if (list == NULL) return NULL;
    list = mino_cons(S, S->bc.bc_regs[base + b], list);
    if (list == NULL) return NULL;
    mino_val *r;
    switch (subop) {
    case BINOP_ADD:  r = prim_add (S, list, NULL); break;
    case BINOP_SUB:  r = prim_sub (S, list, NULL); break;
    case BINOP_MUL:  r = prim_mul (S, list, NULL); break;
    case BINOP_LT:   r = prim_lt  (S, list, NULL); break;
    case BINOP_LE:   r = prim_lte (S, list, NULL); break;
    case BINOP_GT:   r = prim_gt  (S, list, NULL); break;
    case BINOP_GE:   r = prim_gte (S, list, NULL); break;
    case BINOP_EQ:   r = prim_eq  (S, list, NULL); break;
    case BINOP_MOD:  r = prim_mod (S, list, NULL); break;
    case BINOP_QUOT: r = prim_quot(S, list, NULL); break;
    case BINOP_REM:  r = prim_rem (S, list, NULL); break;
    case BINOP_BAND: r = prim_bit_and (S, list, NULL); break;
    case BINOP_BOR:  r = prim_bit_or  (S, list, NULL); break;
    case BINOP_BXOR: r = prim_bit_xor (S, list, NULL); break;
    case BINOP_SHL:  r = prim_bit_shift_left (S, list, NULL); break;
    case BINOP_SHR:  r = prim_bit_shift_right(S, list, NULL); break;
    case BINOP_USHR: r = prim_unsigned_bit_shift_right(S, list, NULL); break;
    default:         r = NULL;                     break;
    }
    if (r == NULL) return NULL;
    regs = S->bc.bc_regs + base;
    regs[a] = r;
    return regs;
}

/* Slow path for the OP_*_IK stencils. The rhs is the pre-tagged
 * immediate (carried in a stencil pool slot), so the helper conses it
 * directly onto the spine -- there is no register-window read for it.
 * The lhs still comes through regs[b]; the result lands in regs[a]
 * after any GC-driven base relocation. */
mino_val **mino_jit_binop_k_slow(mino_state *S, mino_val **regs,
                                   unsigned a, unsigned b,
                                   mino_val *kimm, unsigned subop)
{
    ptrdiff_t base = regs - S->bc.bc_regs;
    mino_val *list = mino_nil(S);
    if (list == NULL) return NULL;
    list = mino_cons(S, kimm, list);
    if (list == NULL) return NULL;
    list = mino_cons(S, S->bc.bc_regs[base + b], list);
    if (list == NULL) return NULL;
    mino_val *r;
    switch (subop) {
    case BINOP_ADD: r = prim_add(S, list, NULL); break;
    case BINOP_SUB: r = prim_sub(S, list, NULL); break;
    case BINOP_LT:  r = prim_lt (S, list, NULL); break;
    case BINOP_LE:  r = prim_lte(S, list, NULL); break;
    case BINOP_EQ:  r = prim_eq (S, list, NULL); break;
    default:        r = NULL;                    break;
    }
    if (r == NULL) return NULL;
    regs = S->bc.bc_regs + base;
    regs[a] = r;
    return regs;
}

/* Slow path for OP_NTH_VEC. Mirrors the interpreter's fast lane:
 * if regs[b] is a vector and regs[c] is a tagged int in range, write
 * vec_nth(coll, idx) into regs[a] without consing. Otherwise build a
 * two-element cons spine and dispatch through prim_nth. The cons
 * fallback handles lazy seqs, lists, maps-with-int-key, etc. */
mino_val **mino_jit_nth_vec_slow(mino_state *S, mino_val **regs,
                                   unsigned a, unsigned b, unsigned c)
{
    ptrdiff_t   base  = regs - S->bc.bc_regs;
    mino_val *coll  = S->bc.bc_regs[base + b];
    mino_val *idx_v = S->bc.bc_regs[base + c];
    if (coll != NULL && mino_type_of(coll) == MINO_VECTOR
        && idx_v != NULL && MINO_IS_INT(idx_v)) {
        long long idx = MINO_INT_VAL(idx_v);
        if (idx >= 0 && (size_t)idx < coll->as.vec.len) {
            regs    = S->bc.bc_regs + base;
            regs[a] = vec_nth(coll, (size_t)idx);
            return regs;
        }
    }
    gc_pin(coll);
    gc_pin(idx_v);
    mino_val *list = mino_nil(S);
    if (list == NULL) { gc_unpin(2); return NULL; }
    list = mino_cons(S, idx_v, list);
    if (list == NULL) { gc_unpin(2); return NULL; }
    list = mino_cons(S, coll, list);
    gc_unpin(2);
    if (list == NULL) return NULL;
    mino_val *r = prim_nth(S, list, NULL);
    if (r == NULL) return NULL;
    regs    = S->bc.bc_regs + base;
    regs[a] = r;
    return regs;
}

/* Slow path for OP_FIRST_VEC. Mirrors the interpreter's fast lane:
 * if regs[b] is a vector, regs[a] := nil (empty) | vec_nth(coll, 0)
 * (non-empty). Otherwise cons-and-prim_first for lazy seqs, lists,
 * strings, maps, etc. */
mino_val **mino_jit_first_vec_slow(mino_state *S, mino_val **regs,
                                     unsigned a, unsigned b)
{
    ptrdiff_t   base = regs - S->bc.bc_regs;
    mino_val *coll = S->bc.bc_regs[base + b];
    if (coll != NULL && mino_type_of(coll) == MINO_VECTOR) {
        regs    = S->bc.bc_regs + base;
        regs[a] = coll->as.vec.len == 0 ? mino_nil(S) : vec_nth(coll, 0);
        return regs;
    }
    mino_val *list = mino_nil(S);
    if (list == NULL) return NULL;
    list = mino_cons(S, coll, list);
    if (list == NULL) return NULL;
    mino_val *r = prim_first(S, list, NULL);
    if (r == NULL) return NULL;
    regs    = S->bc.bc_regs + base;
    regs[a] = r;
    return regs;
}

/* Slow path for OP_ASSOC. Mirrors the interpreter's 3-arg shape
 * with the [coll, k, v] triple sitting at regs[b..b+2] and the dst at
 * regs[a]. MINO_VECTOR + tagged-int k (in-range or len-equal for
 * append) routes to vec_assoc1; MINO_MAP k routes to mino_map_assoc1.
 * Other shapes (sorted-map, record, transient, non-int vec key, idx
 * out of range, variadic forms) fall through to prim_assoc. */
mino_val **mino_jit_assoc_slow(mino_state *S, mino_val **regs,
                                 unsigned a, unsigned b)
{
    ptrdiff_t   base = regs - S->bc.bc_regs;
    mino_val *coll = S->bc.bc_regs[base + b];
    mino_val *k    = S->bc.bc_regs[base + b + 1];
    mino_val *v    = S->bc.bc_regs[base + b + 2];
    if (coll != NULL && k != NULL) {
        int t = mino_type_of(coll);
        if (t == MINO_VECTOR && MINO_IS_INT(k)) {
            long long idx = MINO_INT_VAL(k);
            if (idx >= 0 && (size_t)idx <= coll->as.vec.len) {
                mino_val *r = vec_assoc1(S, coll, (size_t)idx, v);
                if (r == NULL) return NULL;
                regs    = S->bc.bc_regs + base;
                regs[a] = r;
                return regs;
            }
        }
        if (t == MINO_MAP) {
            mino_val *r = mino_map_assoc1(S, coll, k, v);
            if (r == NULL) return NULL;
            regs    = S->bc.bc_regs + base;
            regs[a] = r;
            return regs;
        }
    }
    mino_val *list = mino_nil(S);
    if (list == NULL) return NULL;
    list = mino_cons(S, v, list);
    if (list == NULL) return NULL;
    list = mino_cons(S, k, list);
    if (list == NULL) return NULL;
    list = mino_cons(S, coll, list);
    if (list == NULL) return NULL;
    mino_val *r = prim_assoc(S, list, NULL);
    if (r == NULL) return NULL;
    regs    = S->bc.bc_regs + base;
    regs[a] = r;
    return regs;
}

/* Slow path for OP_ASSOC_BANG. Mirrors the interpreter's transient
 * fast lane: a valid MINO_TRANSIENT routes to mino_assoc_bang
 * directly; anything else (invalidated transient, persistent coll,
 * variadic arity through a forced-rewrite path) falls through to
 * prim_assoc_bang for the Clojure-canonical diagnostic. */
mino_val **mino_jit_assoc_bang_slow(mino_state *S, mino_val **regs,
                                      unsigned a, unsigned b)
{
    ptrdiff_t   base = regs - S->bc.bc_regs;
    mino_val *coll = S->bc.bc_regs[base + b];
    mino_val *k    = S->bc.bc_regs[base + b + 1];
    mino_val *v    = S->bc.bc_regs[base + b + 2];
    if (coll != NULL
        && mino_type_of(coll) == MINO_TRANSIENT
        && coll->as.transient.valid) {
        mino_val *r = mino_assoc_bang(S, coll, k, v);
        if (r == NULL) return NULL;
        regs    = S->bc.bc_regs + base;
        regs[a] = r;
        return regs;
    }
    mino_val *list = mino_nil(S);
    if (list == NULL) return NULL;
    list = mino_cons(S, v, list);
    if (list == NULL) return NULL;
    list = mino_cons(S, k, list);
    if (list == NULL) return NULL;
    list = mino_cons(S, coll, list);
    if (list == NULL) return NULL;
    mino_val *r = prim_assoc_bang(S, list, NULL);
    if (r == NULL) return NULL;
    regs    = S->bc.bc_regs + base;
    regs[a] = r;
    return regs;
}

/* Slow path for OP_CONJ_BANG. Mirrors the interpreter's transient fast
 * lane: valid MINO_TRANSIENT routes to mino_conj_bang; anything else
 * falls through to prim_conj_bang. */
mino_val **mino_jit_conj_bang_slow(mino_state *S, mino_val **regs,
                                     unsigned a, unsigned b, unsigned c)
{
    ptrdiff_t   base = regs - S->bc.bc_regs;
    mino_val *coll = S->bc.bc_regs[base + b];
    mino_val *item = S->bc.bc_regs[base + c];
    if (coll != NULL
        && mino_type_of(coll) == MINO_TRANSIENT
        && coll->as.transient.valid) {
        mino_val *r = mino_conj_bang(S, coll, item);
        if (r == NULL) return NULL;
        regs    = S->bc.bc_regs + base;
        regs[a] = r;
        return regs;
    }
    mino_val *list = mino_nil(S);
    if (list == NULL) return NULL;
    list = mino_cons(S, item, list);
    if (list == NULL) return NULL;
    list = mino_cons(S, coll, list);
    if (list == NULL) return NULL;
    mino_val *r = prim_conj_bang(S, list, NULL);
    if (r == NULL) return NULL;
    regs    = S->bc.bc_regs + base;
    regs[a] = r;
    return regs;
}

/* Slow path for OP_DISSOC_BANG. */
mino_val **mino_jit_dissoc_bang_slow(mino_state *S, mino_val **regs,
                                       unsigned a, unsigned b, unsigned c)
{
    ptrdiff_t   base = regs - S->bc.bc_regs;
    mino_val *coll = S->bc.bc_regs[base + b];
    mino_val *key  = S->bc.bc_regs[base + c];
    if (coll != NULL
        && mino_type_of(coll) == MINO_TRANSIENT
        && coll->as.transient.valid) {
        mino_val *r = mino_dissoc_bang(S, coll, key);
        if (r == NULL) return NULL;
        regs    = S->bc.bc_regs + base;
        regs[a] = r;
        return regs;
    }
    mino_val *list = mino_nil(S);
    if (list == NULL) return NULL;
    list = mino_cons(S, key, list);
    if (list == NULL) return NULL;
    list = mino_cons(S, coll, list);
    if (list == NULL) return NULL;
    mino_val *r = prim_dissoc_bang(S, list, NULL);
    if (r == NULL) return NULL;
    regs    = S->bc.bc_regs + base;
    regs[a] = r;
    return regs;
}

/* Slow path for OP_DISJ_BANG. */
mino_val **mino_jit_disj_bang_slow(mino_state *S, mino_val **regs,
                                     unsigned a, unsigned b, unsigned c)
{
    ptrdiff_t   base = regs - S->bc.bc_regs;
    mino_val *coll = S->bc.bc_regs[base + b];
    mino_val *item = S->bc.bc_regs[base + c];
    if (coll != NULL
        && mino_type_of(coll) == MINO_TRANSIENT
        && coll->as.transient.valid) {
        mino_val *r = mino_disj_bang(S, coll, item);
        if (r == NULL) return NULL;
        regs    = S->bc.bc_regs + base;
        regs[a] = r;
        return regs;
    }
    mino_val *list = mino_nil(S);
    if (list == NULL) return NULL;
    list = mino_cons(S, item, list);
    if (list == NULL) return NULL;
    list = mino_cons(S, coll, list);
    if (list == NULL) return NULL;
    mino_val *r = prim_disj_bang(S, list, NULL);
    if (r == NULL) return NULL;
    regs    = S->bc.bc_regs + base;
    regs[a] = r;
    return regs;
}

/* Slow path for OP_DISSOC. MINO_MAP fast lane via mino_map_dissoc1
 * (allocates -- regs base may relocate); other coll types fall through
 * to prim_dissoc which raises a type diagnostic. Mirrors the
 * interpreter's OP_DISSOC cold-op handler. */
mino_val **mino_jit_dissoc_slow(mino_state *S, mino_val **regs,
                                   unsigned a, unsigned b, unsigned c)
{
    ptrdiff_t   base = regs - S->bc.bc_regs;
    mino_val *coll = S->bc.bc_regs[base + b];
    mino_val *key  = S->bc.bc_regs[base + c];
    if (coll != NULL && mino_type_of(coll) == MINO_MAP) {
        mino_val *r = mino_map_dissoc1(S, coll, key);
        if (r == NULL) return NULL;
        regs    = S->bc.bc_regs + base;
        regs[a] = r;
        return regs;
    }
    mino_val *list = mino_nil(S);
    if (list == NULL) return NULL;
    list = mino_cons(S, key, list);
    if (list == NULL) return NULL;
    list = mino_cons(S, coll, list);
    if (list == NULL) return NULL;
    mino_val *r = prim_dissoc(S, list, NULL);
    if (r == NULL) return NULL;
    regs    = S->bc.bc_regs + base;
    regs[a] = r;
    return regs;
}

/* Slow path for OP_CONJ_VEC. MINO_VECTOR fast lane via vec_conj1
 * (allocates -- regs base may relocate); other coll types fall
 * through to prim_conj which handles lists, sorted-colls, sets,
 * maps, transients, etc. */
mino_val **mino_jit_conj_vec_slow(mino_state *S, mino_val **regs,
                                    unsigned a, unsigned b, unsigned c)
{
    ptrdiff_t   base = regs - S->bc.bc_regs;
    mino_val *coll = S->bc.bc_regs[base + b];
    mino_val *item = S->bc.bc_regs[base + c];
    if (coll != NULL && mino_type_of(coll) == MINO_VECTOR) {
        mino_val *r = vec_conj1(S, coll, item);
        if (r == NULL) return NULL;
        regs    = S->bc.bc_regs + base;
        regs[a] = r;
        return regs;
    }
    mino_val *list = mino_nil(S);
    if (list == NULL) return NULL;
    list = mino_cons(S, item, list);
    if (list == NULL) return NULL;
    list = mino_cons(S, coll, list);
    if (list == NULL) return NULL;
    mino_val *r = prim_conj(S, list, NULL);
    if (r == NULL) return NULL;
    regs    = S->bc.bc_regs + base;
    regs[a] = r;
    return regs;
}

/* Slow path for OP_GET_KW_MAP. Mirrors the interpreter handler:
 * MINO_MAP fast lane via map_get_val; MINO_RECORD + MINO_KEYWORD
 * fast lane via record_field_index; MINO_TRANSIENT unwraps to its
 * backing collection and reuses the MAP / VECTOR inline path;
 * everything else falls through to prim_get's full semantics. */
mino_val **mino_jit_get_kw_map_slow(mino_state *S, mino_val **regs,
                                      unsigned a, unsigned b, unsigned c)
{
    ptrdiff_t   base = regs - S->bc.bc_regs;
    mino_val *coll = S->bc.bc_regs[base + b];
    mino_val *key  = S->bc.bc_regs[base + c];
    if (coll != NULL && key != NULL) {
        int t = mino_type_of(coll);
        if (t == MINO_TRANSIENT && coll->as.transient.valid) {
            mino_val *inner = coll->as.transient.current;
            if (inner != NULL) {
                int it = mino_type_of(inner);
                if (it == MINO_MAP) {
                    mino_val *v = map_get_val(inner, key);
                    regs    = S->bc.bc_regs + base;
                    regs[a] = v == NULL ? mino_nil(S) : v;
                    return regs;
                }
                if (it == MINO_VECTOR && mino_val_int_p(key)) {
                    long long idx = mino_val_int_get(key);
                    regs = S->bc.bc_regs + base;
                    if (idx >= 0 && (size_t)idx < inner->as.vec.len) {
                        regs[a] = vec_nth(inner, (size_t)idx);
                    } else {
                        regs[a] = mino_nil(S);
                    }
                    return regs;
                }
            }
        }
        if (t == MINO_MAP) {
            mino_val *v = map_get_val(coll, key);
            regs    = S->bc.bc_regs + base;
            regs[a] = v == NULL ? mino_nil(S) : v;
            return regs;
        }
        if (t == MINO_RECORD && mino_type_of(key) == MINO_KEYWORD) {
            int idx = record_field_index(coll, key);
            if (idx >= 0) {
                regs    = S->bc.bc_regs + base;
                regs[a] = coll->as.record.vals[idx];
                return regs;
            }
            /* Keyword not a declared field: fall through to prim_get
             * which consults the optional ext-map and returns nil for
             * an absent key. */
        }
    }
    mino_val *list = mino_nil(S);
    if (list == NULL) return NULL;
    list = mino_cons(S, key, list);
    if (list == NULL) return NULL;
    list = mino_cons(S, coll, list);
    if (list == NULL) return NULL;
    mino_val *r = prim_get(S, list, NULL);
    if (r == NULL) return NULL;
    regs    = S->bc.bc_regs + base;
    regs[a] = r;
    return regs;
}

/* Slow path for OP_COUNT_VEC. Vector fast lane returns vec.len as a
 * tagged int (or boxed for the rare overflow). Any other coll type
 * (lazy seq, string, map, set, ...) falls through to prim_count. */
mino_val **mino_jit_count_vec_slow(mino_state *S, mino_val **regs,
                                     unsigned a, unsigned b)
{
    ptrdiff_t   base = regs - S->bc.bc_regs;
    mino_val *coll = S->bc.bc_regs[base + b];
    if (coll != NULL && mino_type_of(coll) == MINO_VECTOR) {
        mino_val *r = tag_or_box_int(S, (long long)coll->as.vec.len);
        if (r == NULL) return NULL;
        regs    = S->bc.bc_regs + base;
        regs[a] = r;
        return regs;
    }
    mino_val *list = mino_nil(S);
    if (list == NULL) return NULL;
    list = mino_cons(S, coll, list);
    if (list == NULL) return NULL;
    mino_val *r = prim_count(S, list, NULL);
    if (r == NULL) return NULL;
    regs    = S->bc.bc_regs + base;
    regs[a] = r;
    return regs;
}

/* Slow path for OP_EMPTY_VEC. Vector fast lane returns true iff
 * vec.len == 0. Other coll types fall through to prim_empty_p. */
mino_val **mino_jit_empty_vec_slow(mino_state *S, mino_val **regs,
                                     unsigned a, unsigned b)
{
    ptrdiff_t   base = regs - S->bc.bc_regs;
    mino_val *coll = S->bc.bc_regs[base + b];
    if (coll != NULL && mino_type_of(coll) == MINO_VECTOR) {
        regs    = S->bc.bc_regs + base;
        regs[a] = coll->as.vec.len == 0 ? mino_true(S) : mino_false(S);
        return regs;
    }
    mino_val *list = mino_nil(S);
    if (list == NULL) return NULL;
    list = mino_cons(S, coll, list);
    if (list == NULL) return NULL;
    mino_val *r = prim_empty_p(S, list, NULL);
    if (r == NULL) return NULL;
    regs    = S->bc.bc_regs + base;
    regs[a] = r;
    return regs;
}

/* Continue-marker stubs. Both functions exist only so the linker
 * resolves the symbol references each stencil emits:
 *
 *   loop_continue_marker  -- fused-loop stencils emit a `b` against
 *     this symbol (inline asm); the JIT runtime detects the BRANCH26
 *     reloc during emit_stencil and rewrites it to target the
 *     stencil instance's own start (the loop back-jump).
 *
 *   chain_continue_marker -- every non-final stencil ends each
 *     return path with `__attribute__((musttail)) return
 *     mino_jit_chain_continue_marker(regs, consts, S)`. clang
 *     lowers the musttail to a `b` (ARM64) or `jmp` (x86_64); the
 *     JIT's post-emit pass walks each non-final stencil's reloc
 *     table, finds every relocation against this marker, and
 *     patches the branch offset to point at the next stencil
 *     instance's start.
 *
 * Neither function is ever executed by the JIT region; the bytes
 * the linker emits for the no-op body just satisfy the symbol
 * definition requirement. */
void mino_jit_loop_continue_marker(void) { /* never called */ }
void mino_jit_chain_continue_marker(mino_val **regs,
                                    mino_val **consts,
                                    mino_state *S)
{
    (void)regs; (void)consts; (void)S;
}

/* Slow path for the unary OP_INC_I / OP_DEC_I / OP_ZERO_INT_P stencils.
 * Builds a one-element cons spine and dispatches to the matching
 * prim; mirrors the interpreter's unary handler. */
mino_val **mino_jit_unop_slow(mino_state *S, mino_val **regs,
                                unsigned a, unsigned b, unsigned subop)
{
    ptrdiff_t base = regs - S->bc.bc_regs;
    mino_val *list = mino_nil(S);
    if (list == NULL) return NULL;
    list = mino_cons(S, S->bc.bc_regs[base + b], list);
    if (list == NULL) return NULL;
    mino_val *r;
    switch (subop) {
    case UNOP_INC:    r = prim_inc    (S, list, NULL); break;
    case UNOP_DEC:    r = prim_dec    (S, list, NULL); break;
    case UNOP_ZERO_P: r = prim_zero_p (S, list, NULL); break;
    case UNOP_POS_P:  r = prim_pos_p  (S, list, NULL); break;
    case UNOP_NEG_P:  r = prim_neg_p  (S, list, NULL); break;
    case UNOP_EVEN_P: r = prim_even_p (S, list, NULL); break;
    case UNOP_ODD_P:  r = prim_odd_p  (S, list, NULL); break;
    case UNOP_BNOT:   r = prim_bit_not(S, list, NULL); break;
    default:          r = NULL;                        break;
    }
    if (r == NULL) return NULL;
    regs = S->bc.bc_regs + base;
    regs[a] = r;
    return regs;
}

/* OP_GETGLOBAL_CACHED slow helper. Routes through the public
 * `mino_bc_ic_global_load`, which mirrors the interpreter's handler
 * exactly: bounds-check the slot index, run the dyn-then-env-then-
 * cached-then-resolve cascade, refill the slot under the GC write
 * barrier on a fresh resolve. env is passed NULL by the JIT path --
 * JIT-eligible fns have `captures == 0`, so their bodies never bind
 * names through env. dyn_active is read from the current thread ctx
 * so a live `(binding ...)` form shadows the cached var the same way
 * the interpreter does.
 *
 * GC-safe regs base refresh: bc_regs may relocate if the resolve path
 * triggers an allocation (cons / write-barrier / map_get cascade). */
mino_val **mino_jit_getglobal_cached_slow(mino_state *S,
                                            mino_val **regs,
                                            unsigned a,
                                            mino_bc_fn_t *bc,
                                            unsigned slot_idx)
{
    ptrdiff_t          base       = regs - S->bc.bc_regs;
    mino_thread_ctx_t *ctx        = mino_current_ctx(S);
    int                dyn_active = (ctx->dyn_stack != NULL);
    /* env: published by mino_jit_invoke from the bc_run frame's env
     * parameter. Captured-local symbol references compile to
     * OP_GETGLOBAL_CACHED slots whose resolution path runs through
     * env first; without env, those slots would fail to resolve and
     * surface as spurious "unbound symbol" diagnostics. */
    mino_env *env = ctx->jit_invoke_env;
    mino_val *v   = mino_bc_ic_global_load(S, bc, (int)slot_idx,
                                             env, dyn_active);
    if (v == NULL) return NULL;
    regs    = S->bc.bc_regs + base;
    regs[a] = v;
    return regs;
}

/* OP_CALL slow helper -- uncached call site. Reads the callee from
 * regs[fn_reg], invokes the callable with argv at
 * regs[fn_reg + 1..fn_reg + argc], stores the result at regs[dst].
 * env reaches the callable via the same `jit_invoke_env` publish
 * point the cached variants use.
 *
 * Routes through `mino_apply_known_bc_fn_argv` so the common
 * MINO_FN case skips apply_callable_argv's outer var-deref and
 * type-of switch: invoke_bc_fn_argv is forced-inlined inside the
 * known-bc helper, so the MINO_FN fast lane is a single inlined
 * dispatch. Non-MINO_FN callables (PRIM, MINO_VAR with non-fn
 * root, macros, slow-path callables) cost one extra C call hop
 * via the known-bc helper's fallback into apply_callable_argv;
 * for the targeted callback workloads (user fns passed to map /
 * reduce / filter / sort comparators) the MINO_FN lane fires
 * every iteration. */
mino_val **mino_jit_call_slow(mino_state *S, mino_val **regs,
                                unsigned fn_reg, unsigned argc, unsigned dst)
{
    ptrdiff_t          base = regs - S->bc.bc_regs;
    mino_thread_ctx_t *ctx  = mino_current_ctx(S);
    mino_env        *env  = ctx->jit_invoke_env;
    mino_val        *callee = S->bc.bc_regs[base + fn_reg];
    mino_val        *r = mino_apply_known_bc_fn_argv(
        S, callee, S->bc.bc_regs + base + fn_reg + 1, (int)argc, env);
    if (r == NULL) return NULL;
    regs      = S->bc.bc_regs + base;
    regs[dst] = r;
    return regs;
}

/* OP_CLOSURE slow helper. Reads the child fn from bc->consts[bx],
 * builds a fresh closure value over the current jit_invoke_env, and
 * stores it at regs[a]. Mirrors the interpreter's OP_CLOSURE
 * handler. */
mino_val **mino_jit_closure_slow(mino_state *S, mino_val **regs,
                                    unsigned a, mino_bc_fn_t *bc, unsigned bx)
{
    ptrdiff_t          base = regs - S->bc.bc_regs;
    mino_thread_ctx_t *ctx  = mino_current_ctx(S);
    if (bx >= bc->consts_len) return NULL;
    mino_val *child = bc->consts[bx];
    if (child == NULL || mino_type_of(child) != MINO_FN) return NULL;
    gc_pin(child);
    mino_val *closure = make_fn(S, child->as.fn.params,
                                   child->as.fn.body,
                                   ctx->jit_invoke_env);
    gc_unpin(1);
    if (closure == NULL) return NULL;
    closure->as.fn.defining_ns = child->as.fn.defining_ns;
    *(const mino_bc_fn_t **)&closure->as.fn.bc = child->as.fn.bc;
    closure->as.fn.shape = child->as.fn.shape;
    closure->as.fn.template_fn = child;
    regs    = S->bc.bc_regs + base;
    regs[a] = closure;
    return regs;
}

/* OP_PUSH_ENV slow helper. Extends the JIT-invoke env with a fresh
 * child frame; subsequent OP_ENV_BIND helpers bind into it and
 * resolve-cascade lookups walk through it before reaching the
 * caller-supplied env. */
mino_val **mino_jit_push_env_slow(mino_state *S, mino_val **regs)
{
    mino_thread_ctx_t *ctx   = mino_current_ctx(S);
    mino_env        *child = env_child(S, ctx->jit_invoke_env);
    if (child == NULL) return NULL;
    ctx->jit_invoke_env = child;
    return regs;
}

/* OP_POP_ENV slow helper. Walks the JIT-invoke env up one frame.
 * NULL parent (already at the root) is a malformed bc; surface as a
 * slow-path failure rather than corrupting the chain. */
mino_val **mino_jit_pop_env_slow(mino_state *S, mino_val **regs)
{
    mino_thread_ctx_t *ctx = mino_current_ctx(S);
    mino_env        *env = ctx->jit_invoke_env;
    if (env == NULL || env->parent == NULL) return NULL;
    ctx->jit_invoke_env = env->parent;
    return regs;
}

/* OP_ENV_BIND slow helper. Publishes regs[a] under the symbol at
 * bc->consts[bx] into the current JIT-invoke env. Mirrors the
 * interpreter's OP_ENV_BIND so inner fn literals captured at
 * OP_CLOSURE time pick up the same names the interpreter would. */
mino_val **mino_jit_env_bind_slow(mino_state *S, mino_val **regs,
                                     unsigned a, mino_bc_fn_t *bc,
                                     unsigned bx)
{
    ptrdiff_t          base = regs - S->bc.bc_regs;
    mino_thread_ctx_t *ctx  = mino_current_ctx(S);
    if (bx >= bc->consts_len) return NULL;
    mino_val *sym = bc->consts[bx];
    if (sym == NULL || mino_type_of(sym) != MINO_SYMBOL) return NULL;
    env_bind_sym(S, ctx->jit_invoke_env, sym, S->bc.bc_regs[base + a]);
    return regs;
}

/* OP_TAILCALL slow helper. Builds the args cons list head-first
 * from regs[fn_reg + 1..fn_reg + argc] and publishes (callee, args)
 * on `S->tail_call_sentinel`. Returns the sentinel pointer so the
 * stencil's natural ret hands it back as the JIT region's return
 * value; the trampoline in apply_callable picks up the new
 * (fn, args) without growing the C stack. */
mino_val *mino_jit_tailcall_slow(mino_state *S, mino_val **regs,
                                    unsigned fn_reg, unsigned argc)
{
    ptrdiff_t   base   = regs - S->bc.bc_regs;
    mino_val *callee = S->bc.bc_regs[base + fn_reg];
    gc_pin(callee);
    mino_val *args   = mino_nil(S);
    if (args == NULL) { gc_unpin(1); return NULL; }
    for (int i = (int)argc - 1; i >= 0; i--) {
        mino_val *cell = mino_cons(S,
                                     S->bc.bc_regs[base + fn_reg + 1 + i],
                                     args);
        if (cell == NULL) { gc_unpin(1); return NULL; }
        args = cell;
    }
    gc_unpin(1);
    S->tail_call_sentinel.as.tail_call.fn   = callee;
    S->tail_call_sentinel.as.tail_call.args = args;
    return &S->tail_call_sentinel;
}

/* OP_CALL_CACHED slow helper. Resolves the callee through the same
 * IC cascade as the read-only OP_GETGLOBAL_CACHED variant, then
 * dispatches via `apply_callable_argv` so all callable kinds (PRIM
 * argv, FN bc, FN tree-walker, multi-arity dispatch, etc.) reach
 * their correct entry. The argv slice is taken straight from the
 * bytecode register window at regs[arg_base..arg_base + argc - 1];
 * `apply_callable_argv` reads from this slice without writing back,
 * so the live regs base is stable for the duration of the call's
 * argv reads (any GC inside the callee may relocate the window
 * afterwards, which is why the helper refreshes from S on return). */
mino_val **mino_jit_call_cached_slow(mino_state *S, mino_val **regs,
                                       unsigned arg_base, unsigned argc,
                                       unsigned dst,
                                       mino_bc_fn_t *bc, unsigned slot_idx)
{
    ptrdiff_t          base       = regs - S->bc.bc_regs;
    mino_thread_ctx_t *ctx        = mino_current_ctx(S);
    int                dyn_active = (ctx->dyn_stack != NULL);
    mino_env        *env        = ctx->jit_invoke_env;
    mino_val        *callee     = mino_bc_ic_global_load(S, bc,
                                                            (int)slot_idx,
                                                            env, dyn_active);
    if (callee == NULL) return NULL;
    mino_val *r = apply_callable_argv(S, callee,
                                        S->bc.bc_regs + base + arg_base,
                                        (int)argc, env);
    if (r == NULL) return NULL;
    regs      = S->bc.bc_regs + base;
    regs[dst] = r;
    return regs;
}

/* Inlined-resolve fast complement of mino_jit_call_cached_slow. The
 * stencil's inline path verified the IC slot is hot (cached !=NULL,
 * gen match, no dyn binding active), so it hands the resolved callee
 * to this helper directly and skips the second IC lookup. Same
 * dispatch + regs-refresh contract as the cached_slow path. */
mino_val **mino_jit_call_resolved_slow(mino_state *S, mino_val **regs,
                                          mino_val *callee,
                                          unsigned arg_base, unsigned argc,
                                          unsigned dst)
{
    ptrdiff_t          base = regs - S->bc.bc_regs;
    mino_thread_ctx_t *ctx  = mino_current_ctx(S);
    mino_env        *env  = ctx->jit_invoke_env;
    mino_val *r = apply_callable_argv(S, callee,
                                        S->bc.bc_regs + base + arg_base,
                                        (int)argc, env);
    if (r == NULL) return NULL;
    regs      = S->bc.bc_regs + base;
    regs[dst] = r;
    return regs;
}

/* Inline-cached-known-callee fast complement. Stencil's inline path
 * additionally verified that slot->cached_callable_kind ==
 * MINO_FN_BC_SINGLE and that argc == cached_fn_n_params (so no
 * arity-clause walk inside mino_bc_run is needed). The helper skips
 * apply_callable_argv's var-unwrap / type-of dispatch switch entirely
 * by entering at mino_apply_known_bc_fn_argv. Same dispatch +
 * regs-refresh contract as call_resolved_slow. */
mino_val **mino_jit_call_known_fn_slow(mino_state *S, mino_val **regs,
                                         mino_val *callee,
                                         unsigned arg_base, unsigned argc,
                                         unsigned dst)
{
    ptrdiff_t          base = regs - S->bc.bc_regs;
    mino_thread_ctx_t *ctx  = mino_current_ctx(S);
    mino_env        *env  = ctx->jit_invoke_env;
    mino_val *r = mino_apply_known_bc_fn_argv(S, callee,
                                                S->bc.bc_regs + base + arg_base,
                                                (int)argc, env);
    if (r == NULL) return NULL;
    regs      = S->bc.bc_regs + base;
    regs[dst] = r;
    return regs;
}

/* Inline-cached-known-PRIM_ARGV complement. Stencil's inline path
 * verified that slot->cached_callable_kind ==
 * MINO_IC_CALLABLE_PRIM_ARGV, so the callee is a MINO_PRIM with
 * fn2 set. Skips apply_callable_argv's dispatch switch and invokes
 * the prim directly with the live regs slice as argv. Mirrors the
 * apply_callable_argv PRIM-fast-path body including push_frame for
 * stack-trace attribution. Defensive: if the cached value's shape
 * has drifted (slot caches a stale Var, or the sym was redefined
 * between resolves), the slot's gen check would already have fired;
 * the type-of check here is the second line of defence. */
mino_val **mino_jit_call_known_prim_slow(mino_state *S,
                                           mino_val **regs,
                                           mino_val *callee,
                                           unsigned arg_base,
                                           unsigned argc,
                                           unsigned dst)
{
    ptrdiff_t          base = regs - S->bc.bc_regs;
    mino_thread_ctx_t *ctx  = mino_current_ctx(S);
    mino_env        *env  = ctx->jit_invoke_env;
    if (callee != NULL && mino_type_of(callee) == MINO_VAR) {
        if (!callee->as.var.bound || callee->as.var.root == NULL) {
            goto fallback;
        }
        callee = callee->as.var.root;
    }
    if (callee == NULL || mino_type_of(callee) != MINO_PRIM
        || callee->as.prim.fn2 == NULL) {
        goto fallback;
    }
    {
        const mino_val *form = mino_current_ctx(S)->eval_current_form;
        const char *file = NULL;
        int         line = 0;
        int         col  = 0;
        if (form != NULL && mino_type_of((mino_val *)form) == MINO_CONS) {
            file = form->as.cons.file;
            line = form->as.cons.line;
            col  = form->as.cons.column;
        }
        push_frame(S, callee->as.prim.name, file, line, col);
        {
            mino_val *r = callee->as.prim.fn2(S,
                S->bc.bc_regs + base + arg_base, (int)argc, env);
            if (r == NULL) return NULL; /* leave frame for trace */
            pop_frame(S);
            regs      = S->bc.bc_regs + base;
            regs[dst] = r;
            return regs;
        }
    }
fallback:
    {
        mino_val *r = apply_callable_argv(S, callee,
            S->bc.bc_regs + base + arg_base, (int)argc, env);
        if (r == NULL) return NULL;
        regs      = S->bc.bc_regs + base;
        regs[dst] = r;
        return regs;
    }
}

/* Cached-native complement. Stencil pre-resolved the callee as a
 * single-arity bc-runnable MINO_FN, pre-fetched its bc from the IC
 * slot's `cached_bc` field, and verified argc / has_rest match. This
 * helper skips the var-unwrap + type-of + bc-staleness + RUNNABLE +
 * hot-counter dance that invoke_bc_fn_argv runs on every call (the
 * IC's gen check already validated them) and enters mino_bc_run
 * directly. The ns scope dance + push_frame are kept because they
 * are semantically required for stack-trace attribution and lexical
 * resolution of inner forms.
 *
 * Tail-call sentinel handling: when mino_bc_run returns a TAIL_CALL
 * sentinel, this helper drives the cons-form apply_callable trampoline
 * to keep parity with invoke_bc_fn_argv's loop. The hot-recursion
 * case (fib, dispatch loops) does not use the sentinel, so the
 * branch is rarely-taken. Falls back to apply_callable_argv on any
 * unexpected shape so the slow path remains correct even when the
 * cached slot drifts. */
mino_val **mino_jit_call_known_native_slow(mino_state *S,
                                             mino_val **regs,
                                             mino_val *fn,
                                             mino_bc_fn_t *bc,
                                             unsigned arg_base,
                                             unsigned argc,
                                             unsigned dst)
{
    ptrdiff_t          base = regs - S->bc.bc_regs;
    mino_thread_ctx_t *ctx  = mino_current_ctx(S);
    mino_env        *env  = ctx->jit_invoke_env;
    /* Defensive: if the cached fn pointer has drifted to something
     * that is no longer an FN, bail to the generic path. The IC's
     * gen check normally screens this, but a fold-driven recompile
     * can change fn->as.fn.bc out from under the slot without
     * advancing ic_gen. */
    if (fn == NULL || mino_type_of(fn) != MINO_FN
        || fn->as.fn.bc != bc) {
        goto fallback;
    }
    const char  *saved_ns      = S->ns_vars.current_ns;
    const char  *saved_ambient = S->ns_vars.fn_ambient_ns;
    if (fn->as.fn.defining_ns != NULL) {
        S->ns_vars.current_ns    = fn->as.fn.defining_ns;
        S->ns_vars.fn_ambient_ns = fn->as.fn.defining_ns;
    }
    const mino_val *form = mino_current_ctx(S)->eval_current_form;
    const char *file = NULL;
    int         line = 0;
    int         col  = 0;
    if (form != NULL && mino_type_of((mino_val *)form) == MINO_CONS) {
        file = form->as.cons.file;
        line = form->as.cons.line;
        col  = form->as.cons.column;
    }
    push_frame(S, "fn", file, line, col);
    /* Take the JIT-call fast lane when the cached bc + the matched
     * single-arity / no-rest / captures-free / native-ready
     * preconditions all hold. mino_bc_run_known_native falls back to
     * mino_bc_run automatically when any precondition has drifted,
     * so the helper stays correct against a stale IC slot. */
    mino_val *r = mino_bc_run_known_native(S, fn,
                                              S->bc.bc_regs + base + arg_base,
                                              (int)argc, fn->as.fn.env);
    /* TAIL_CALL sentinel: rare in the hot path. Delegate to
     * apply_callable so the cons-form trampoline drives the next
     * dispatch with full correctness. Frame + ns scope are dropped
     * before delegation so the trampoline owns its own attribution. */
    if (r != NULL && mino_type_of(r) == MINO_TAIL_CALL) {
        mino_val *next_fn   = r->as.tail_call.fn;
        mino_val *next_args = r->as.tail_call.args;
        pop_frame(S);
        S->ns_vars.current_ns    = saved_ns;
        S->ns_vars.fn_ambient_ns = saved_ambient;
        r = apply_callable(S, next_fn, next_args, env);
    } else {
        pop_frame(S);
        S->ns_vars.current_ns    = saved_ns;
        S->ns_vars.fn_ambient_ns = saved_ambient;
    }
    if (r == NULL) return NULL;
    regs      = S->bc.bc_regs + base;
    regs[dst] = r;
    return regs;
fallback:
    {
        mino_val *fb_r = apply_callable_argv(S, fn,
            S->bc.bc_regs + base + arg_base, (int)argc, env);
        if (fb_r == NULL) return NULL;
        regs      = S->bc.bc_regs + base;
        regs[dst] = fb_r;
        return regs;
    }
}

#endif /* MINO_CPJIT_HOST */

/* Keep this TU non-empty under -Werror=pedantic when MINO_CPJIT_HOST
 * isn't defined for the build target. */
typedef int mino_jit_helpers_tu_marker;
