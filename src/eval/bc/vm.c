/*
 * eval/bc/vm.c -- register-based bytecode VM dispatch.
 *
 * Switch-based interpreter. Each compiled fn carries its own
 * instruction stream and constant pool; mino_bc_run pushes a register
 * window onto S->bc_regs for the call and pops it on return. The
 * window is a slice into a single per-state stack so the GC can walk
 * every live register slot in one pass.
 *
 * Var-indirection discipline (see plan): OP_GETGLOBAL resolves through
 * the var registry every time. No call site closes over a fn value at
 * compile time; redefinition stays visible.
 */

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mino.h"
#include "runtime/internal.h"       /* mino_state_t, gc_alloc_typed, GC_T_VALARR */
#include "eval/internal.h"          /* eval_impl, apply_callable */
#include "eval/special_internal.h"  /* normalize_exception for OP_PUSHCATCH */
#include "eval/bc/internal.h"
#include "collections/internal.h"   /* make_fn */
#include "prim/internal.h"          /* binary arith prim_* on bc fast-lane miss */

extern mino_val_t *mino_nil(mino_state_t *S);

/* Grow S->bc_regs to hold an additional `n` slots and return the base
 * index of the new window. Returns (size_t)-1 on allocation failure. */
static size_t bc_push_window(mino_state_t *S, int n)
{
    if (n < 0) return (size_t)-1;
    size_t need = S->bc_top + (size_t)n;
    if (need > S->bc_regs_cap) {
        size_t new_cap = S->bc_regs_cap == 0 ? 256 : S->bc_regs_cap * 2;
        while (new_cap < need) new_cap *= 2;
        mino_val_t **grown = (mino_val_t **)gc_alloc_typed(
            S, GC_T_VALARR, new_cap * sizeof(*grown));
        if (grown == NULL) return (size_t)-1;
        if (S->bc_regs != NULL && S->bc_top > 0) {
            memcpy(grown, S->bc_regs, S->bc_top * sizeof(*grown));
        }
        for (size_t i = S->bc_top; i < new_cap; i++) grown[i] = NULL;
        S->bc_regs     = grown;
        S->bc_regs_cap = new_cap;
    }
    size_t base = S->bc_top;
    /* The window is left uninitialized: bc_pop_window zeroes every
     * slot before the next push lands on it, and fresh bc_regs growth
     * paths zero the new tail explicitly above. Skipping the per-slot
     * NULL loop here trims a hot per-call cost. The GC root walk
     * (gc_mark_roots) only scans [0, bc_top), and the body's compiler
     * emits a write to every register it later reads -- there is no
     * GC point in mino_bc_run between fn entry and the body's writes
     * to its own registers, because all dispatch ops that may collect
     * (OP_CALL, OP_GETGLOBAL_CACHED, OP_CLOSURE, ...) come after the
     * regs[a] := producer step. */
    S->bc_top = need;
    return base;
}

static void bc_pop_window(mino_state_t *S, size_t base)
{
    while (S->bc_top > base) {
        S->bc_top--;
        S->bc_regs[S->bc_top] = NULL;
    }
}

/* Build a cons list head-first from `argc` register slots starting at
 * `base`. Used by OP_CALL / OP_TAILCALL to hand off arguments to
 * apply_callable, which still consumes the cons-spine ABI. The new
 * cells are reachable through the GC root walk because the register
 * slots in [base, base+argc) keep their referents alive, and the new
 * head cells are themselves stored into a temporary that the
 * conservative stack scan covers. */
static mino_val_t *args_from_regs(mino_state_t *S, mino_val_t **regs,
                                  unsigned argc)
{
    mino_val_t *list = mino_nil(S);
    if (list == NULL) return NULL;
    for (int i = (int)argc - 1; i >= 0; i--) {
        mino_val_t *cell = mino_cons(S, regs[i], list);
        if (cell == NULL) return NULL;
        list = cell;
    }
    return list;
}

/* Encode a 61-bit signed `r` as a tagged int. Falls back to the boxed
 * constructor for the narrow band beyond MINO_INT_MAX where the tag
 * would lose precision (in practice unreachable from the +/-/inc/dec
 * fast lanes: their operands are both already in 61-bit range and the
 * overflow check prior to encoding caught LLONG_MAX-class wraps). */
static inline mino_val_t *tag_or_box_int(mino_state_t *S, long long r)
{
#ifdef MINO_BC_PROFILE_COUNTS
    S->bc_int_make_count++;
#endif
    if (r >= MINO_INT_MIN && r <= MINO_INT_MAX) {
#ifdef MINO_BC_PROFILE_COUNTS
        S->bc_int_alloc_avoided++;
#endif
        return MINO_MAKE_INT(r);
    }
    return mino_int(S, r);
}

/* Integer fast-lane for unary inc / dec / zero?. The Phase D rewrite
 * skips mino_val_int_p / mino_val_int_get -- both inputs are required
 * to be inline-tagged ints, so the helper functions' NULL + tag + type
 * three-step check is replaced with a single MINO_IS_INT tag-bit test
 * and MINO_INT_VAL inline decode. The boxed-int slow path falls
 * through to the prim via the same NULL-return-bails-to-fallback
 * contract the binop lane uses. */
static mino_val_t *unop_int_fast(mino_state_t *S, mino_val_t *v,
                                 unsigned subop)
{
    long long a, r;
    if (!MINO_IS_INT(v)) return NULL;
    a = MINO_INT_VAL(v);
    switch (subop) {
    case UNOP_INC:
#if defined(__GNUC__) || defined(__clang__)
        if (__builtin_saddll_overflow(a, 1, &r)) return NULL;
#else
        r = a + 1;
#endif
        return tag_or_box_int(S, r);
    case UNOP_DEC:
#if defined(__GNUC__) || defined(__clang__)
        if (__builtin_ssubll_overflow(a, 1, &r)) return NULL;
#else
        r = a - 1;
#endif
        return tag_or_box_int(S, r);
    case UNOP_ZERO_P:
        return (a == 0) ? mino_true(S) : mino_false(S);
    case UNOP_POS_P:
        return (a >  0) ? mino_true(S) : mino_false(S);
    case UNOP_NEG_P:
        return (a <  0) ? mino_true(S) : mino_false(S);
    case UNOP_EVEN_P:
        return ((a & 1) == 0) ? mino_true(S) : mino_false(S);
    case UNOP_ODD_P:
        return ((a & 1) != 0) ? mino_true(S) : mino_false(S);
    case UNOP_BNOT:
        /* ~a == -a - 1; always fits in the tagged range when a does. */
        return tag_or_box_int(S, ~a);
    default:
        return NULL;
    }
}

/* Integer fast-lane for OP_BINOP_INT. Same Phase D tag-extract shape
 * as unop_int_fast: a single MINO_IS_INT check per operand replaces
 * mino_val_int_p's NULL + tag + type chain, and MINO_INT_VAL decodes
 * inline without the boxed-fallback branch. Overflow stays on the
 * __builtin_*_overflow intrinsics; the encoded result rides through
 * tag_or_box_int. Returns NULL on a tag miss or overflow so the
 * dispatcher bails to the cons-spine prim. */
static mino_val_t *binop_int_fast(mino_state_t *S, mino_val_t *lhs,
                                  mino_val_t *rhs, unsigned subop)
{
    long long a, b, r;
    if (!MINO_IS_INT(lhs) || !MINO_IS_INT(rhs)) return NULL;
    a = MINO_INT_VAL(lhs);
    b = MINO_INT_VAL(rhs);
    switch (subop) {
    case BINOP_ADD:
#if defined(__GNUC__) || defined(__clang__)
        if (__builtin_saddll_overflow(a, b, &r)) return NULL;
#else
        r = a + b;
#endif
        return tag_or_box_int(S, r);
    case BINOP_SUB:
#if defined(__GNUC__) || defined(__clang__)
        if (__builtin_ssubll_overflow(a, b, &r)) return NULL;
#else
        r = a - b;
#endif
        return tag_or_box_int(S, r);
    case BINOP_MUL:
#if defined(__GNUC__) || defined(__clang__)
        if (__builtin_smulll_overflow(a, b, &r)) return NULL;
#else
        r = a * b;
#endif
        return tag_or_box_int(S, r);
    case BINOP_LT: return (a <  b) ? mino_true(S) : mino_false(S);
    case BINOP_LE: return (a <= b) ? mino_true(S) : mino_false(S);
    case BINOP_GT: return (a >  b) ? mino_true(S) : mino_false(S);
    case BINOP_GE: return (a >= b) ? mino_true(S) : mino_false(S);
    case BINOP_EQ: return (a == b) ? mino_true(S) : mino_false(S);
    case BINOP_MOD:
    case BINOP_QUOT:
    case BINOP_REM:
        /* Bail on b==0 (prim throws division-by-zero) or on the
         * MINO_INT_MIN / -1 corner where the quotient escapes the
         * tagged range and the prim's bigint-promote path is the
         * Clojure-correct answer. */
        if (b == 0) return NULL;
        if (a == MINO_INT_MIN && b == -1) return NULL;
        if (subop == BINOP_QUOT) return tag_or_box_int(S, a / b);
        r = a % b;
        if (subop == BINOP_MOD && r != 0 && ((r < 0) != (b < 0))) r += b;
        return tag_or_box_int(S, r);
    case BINOP_BAND: return tag_or_box_int(S, a & b);
    case BINOP_BOR:  return tag_or_box_int(S, a | b);
    case BINOP_BXOR: return tag_or_box_int(S, a ^ b);
    case BINOP_SHL:
        /* Shift amount must be in [0, 63]; route through unsigned so
         * that bit-shift-left of negative values matches the prim's
         * wrap-around result (and stays clear of signed-overflow UB). */
        if (b < 0 || b >= 64) return NULL;
        return tag_or_box_int(S, (long long)((unsigned long long)a << b));
    case BINOP_SHR:
        if (b < 0 || b >= 64) return NULL;
        return tag_or_box_int(S, a >> b);
    case BINOP_USHR:
        if (b < 0 || b >= 64) return NULL;
        return tag_or_box_int(S, (long long)((unsigned long long)a >> b));
    default:       return NULL;
    }
}

/* Resolve a global symbol through eval_impl. Goes through the same
 * dyn/lexical/ns/ambient cascade as eval_symbol; respects redefinition
 * because the lookup hits the var cell live, not a cached value. */
static mino_val_t *resolve_global(mino_state_t *S, mino_val_t *sym,
                                  mino_env_t *env)
{
    if (sym == NULL || mino_type_of(sym) != MINO_SYMBOL) return NULL;
    return eval_impl(S, sym, env, 0);
}

mino_val_t *mino_bc_run(mino_state_t *S, mino_val_t *fn_val,
                        mino_val_t **argv, int argc, mino_env_t *env)
{
    const mino_bc_fn_t *bc = fn_val->as.fn.bc;
    if (bc == NULL || bc->code == NULL) return NULL;
    if (bc->n_clauses <= 0 || bc->clauses == NULL) return NULL;

    /* Select a clause whose arity matches argc. Prefer fixed-arity
     * matches over variadic ones (Clojure semantics: the most-specific
     * clause wins). If two clauses share the same min arity we pick
     * the first in source order. */
    const mino_bc_clause_t *match = NULL;
    for (int i = 0; i < bc->n_clauses; i++) {
        const mino_bc_clause_t *cl = &bc->clauses[i];
        if (!cl->has_rest && cl->n_params == argc) { match = cl; break; }
    }
    if (match == NULL) {
        for (int i = 0; i < bc->n_clauses; i++) {
            const mino_bc_clause_t *cl = &bc->clauses[i];
            if (cl->has_rest && argc >= cl->n_params) { match = cl; break; }
        }
    }
    if (match == NULL) return NULL;

    size_t base = bc_push_window(S, bc->n_regs);
    if (base == (size_t)-1) return NULL;

    for (int i = 0; i < match->n_params; i++) {
        S->bc_regs[base + (size_t)i] = argv[i];
    }
    /* Collect overflow args into a list and place it in the slot
     * right after the fixed params. mino_cons walks back-to-front so
     * we get the values in their original order. When argc ==
     * n_params the rest binding is the empty list. */
    if (match->has_rest) {
        mino_val_t *rest = mino_nil(S);
        for (int i = argc - 1; i >= match->n_params; i--) {
            rest = mino_cons(S, argv[i], rest);
            if (rest == NULL) { bc_pop_window(S, base); return NULL; }
        }
        S->bc_regs[base + (size_t)match->n_params] = rest;
    }

    /* When the body contains an inner fn literal or a (lazy-seq ...),
     * extend the lexical env with a fresh child and publish the
     * matched clause's params into it. */
    if (bc->captures) {
        env = env_child(S, env);
        if (env == NULL) { bc_pop_window(S, base); return NULL; }
        for (int i = 0; i < match->n_params; i++) {
            mino_val_t *p = vec_nth(match->params_vec, (size_t)i);
            if (p == NULL || mino_type_of(p) != MINO_SYMBOL) {
                bc_pop_window(S, base);
                return NULL;
            }
            env_bind_sym(S, env, p, argv[i]);
        }
        if (match->has_rest) {
            mino_val_t *rest_sym = vec_nth(match->params_vec,
                match->params_vec->as.vec.len - 1);
            if (rest_sym != NULL && mino_type_of(rest_sym) == MINO_SYMBOL) {
                env_bind_sym(S, env, rest_sym,
                    S->bc_regs[base + (size_t)match->n_params]);
            }
        }
    }

    mino_val_t **regs = S->bc_regs + base;
    const mino_bc_insn_t *code = bc->code;
    size_t pc = (size_t)match->entry_pc;
    mino_val_t *retval = NULL;
    int ok = 1;

    /* Save the try-state snapshot at fn entry so any abnormal exit
     * (early goto bc_done while a PUSHCATCH is still live, or a fn
     * that body-faults inside a try) rolls bc_catch_depth and
     * try_depth back to where they were before this fn ran. A
     * leaked frame would leave a stale setjmp landing pad pointing
     * into this stack frame after we return, and the next longjmp
     * up the chain would jump to garbage. */
    mino_thread_ctx_t *ctx = mino_current_ctx(S);
    int saved_try_depth        = ctx->try_depth;
    int saved_bc_catch_depth   = ctx->bc_catch_depth;
    /* Anchor the dyn_stack at fn entry so bc_done can unwind any
     * OP_PUSHDYN frames that survived an early exit (NULL return /
     * tail-call sentinel / catch landing). The body either matches
     * each PUSHDYN with a POPDYN -- the normal path -- or one of
     * those error paths kicks in and the cleanup below frees the
     * orphaned frames. Mirrors the longjmp-unwind loop in
     * control.c's eval_try. */
    dyn_frame_t       *saved_dyn_stack    = ctx->dyn_stack;

    while (pc < bc->code_len) {
        /* Refresh the window pointer every cycle. Any op that can
         * trigger user code (OP_CALL/TAILCALL via apply_callable,
         * OP_GETGLOBAL via eval_impl, OP_CLOSURE via make_fn that
         * may collect, OP_SETGLOBAL via var_intern) can cascade into
         * a recursive mino_bc_run that grows S->bc_regs and frees
         * the prior buffer. Recomputing from base on each iteration
         * keeps the window pointer correct without per-op clutter. */
        regs = S->bc_regs + base;
        mino_bc_insn_t ins = code[pc++];
        unsigned op = OP_OF(ins);
        switch (op) {
        case OP_NOP:
            break;

        case OP_MOVE: {
            unsigned a = A_OF(ins);
            unsigned b = B_OF(ins);
            regs[a] = regs[b];
            break;
        }

        case OP_LOAD_K: {
            unsigned a  = A_OF(ins);
            unsigned bx = Bx_OF(ins);
            if (bx >= bc->consts_len) { ok = 0; goto bc_done; }
            regs[a] = bc->consts[bx];
            break;
        }

        case OP_GETGLOBAL: {
            unsigned a  = A_OF(ins);
            unsigned bx = Bx_OF(ins);
            if (bx >= bc->consts_len) { ok = 0; goto bc_done; }
            mino_val_t *sym = bc->consts[bx];
            mino_val_t *v   = resolve_global(S, sym, env);
            if (v == NULL) { ok = 0; goto bc_done; }
            regs[a] = v;
            break;
        }

        case OP_GETGLOBAL_CACHED: {
            /* Inline cache for global symbol resolution. The slot is
             * filled on first miss with (cached, gen=S->ic_gen) and
             * re-read while the gen still matches. Bumps to ic_gen
             * (def / ns-unmap / var_set_root / var_unintern) invalidate
             * naturally. With dynamic bindings active the cache is
             * skipped on miss so `(binding [*x* ...] ...)` doesn't
             * mask the dyn-shadowed value with a stale var root.
             *
             * Closure free vars resolve through the env chain at
             * runtime, but the bc record (and thus the IC slot array)
             * is shared across all closures built from one template.
             * Two closures of `(fn [i] (fn [] i))` therefore share the
             * IC slot for `i`, even though each carries its own captured
             * env where `i` is bound to a different value. Probe dyn
             * and then env (matching eval_symbol's order) and use the
             * found value directly without caching; the cache only
             * fires for symbols that neither dyn nor env shadow. */
            unsigned a  = A_OF(ins);
            unsigned bx = Bx_OF(ins);
            if ((int)bx >= bc->ic_slots_len) { ok = 0; goto bc_done; }
            mino_bc_ic_slot_t *slot = &bc->ic_slots[bx];
            int dyn_active = (ctx->dyn_stack != NULL);
            if (dyn_active) {
                mino_val_t *dyn_v = dyn_lookup(S, slot->sym->as.s.data);
                if (dyn_v != NULL) {
                    regs[a] = dyn_v;
                    break;
                }
            }
            if (env != NULL) {
                mino_val_t *env_v = mino_env_get_sym(env, slot->sym);
                if (env_v != NULL) {
                    regs[a] = env_v;
                    break;
                }
            }
            if (!dyn_active
                && slot->cached != NULL
                && slot->gen == S->ic_gen) {
                regs[a] = slot->cached;
                break;
            }
            mino_val_t *v = resolve_global(S, slot->sym, env);
            if (v == NULL) { ok = 0; goto bc_done; }
            if (!dyn_active) {
                /* slot array may be OLD after a minor cycle; v may be
                 * a freshly resolved var-root in YOUNG. The barrier
                 * keeps the next minor's remset honest. */
                gc_write_barrier(S, bc->ic_slots, slot->cached, v);
                slot->cached = v;
                slot->gen    = S->ic_gen;
            }
            regs[a] = v;
            break;
        }

        case OP_SETGLOBAL: {
            unsigned a  = A_OF(ins);
            unsigned bx = Bx_OF(ins);
            if (bx >= bc->consts_len) { ok = 0; goto bc_done; }
            mino_val_t *sym = bc->consts[bx];
            if (sym == NULL || mino_type_of(sym) != MINO_SYMBOL) {
                ok = 0;
                goto bc_done;
            }
            mino_val_t *v   = regs[a];
            mino_val_t *var = var_intern(S, S->current_ns, sym->as.s.data);
            if (var == NULL) { ok = 0; goto bc_done; }
            var_set_root(S, var, v);
            S->ic_gen++;     /* invalidate cached call sites */
            regs[a] = v;
            break;
        }

        case OP_JMP: {
            int off = sBx_OF(ins);
            pc = (size_t)((long)pc + off);
            break;
        }

        case OP_JMPIFNOT: {
            unsigned a = A_OF(ins);
            int off = sBx_OF(ins);
            if (!mino_is_truthy_inline(regs[a])) {
                pc = (size_t)((long)pc + off);
            }
            break;
        }

        case OP_CALL: {
            unsigned a    = A_OF(ins);
            unsigned argn = B_OF(ins);
            unsigned ret  = C_OF(ins);
            mino_val_t *callee = regs[a];
            /* argv ABI: hand the caller's register slice straight to
             * apply_callable_argv. For PRIM-fn2 and bc-FN callees the
             * cons-spine is never built; legacy callees fall back to a
             * cons rebuild inside apply_callable_argv. */
            mino_val_t *r = apply_callable_argv(S, callee, regs + a + 1,
                                                (int)argn, env);
            if (r == NULL) { ok = 0; goto bc_done; }
            S->bc_regs[base + ret] = r;
            break;
        }

        case OP_TAILCALL: {
            unsigned a    = A_OF(ins);
            unsigned argn = B_OF(ins);
            mino_val_t *callee = regs[a];
            mino_val_t *args   = args_from_regs(S, regs + a + 1, argn);
            if (args == NULL) { ok = 0; goto bc_done; }
            /* Hand off via the MINO_TAIL_CALL sentinel; the outer
             * apply_callable trampoline picks up the new (fn, args)
             * without growing the C stack. The sentinel's args field
             * stays in cons-format for legacy callers that read it
             * directly; the trampoline inside apply_callable_argv
             * walks it back to argv for bc-FN targets. */
            S->tail_call_sentinel.as.tail_call.fn   = callee;
            S->tail_call_sentinel.as.tail_call.args = args;
            retval = &S->tail_call_sentinel;
            goto bc_done;
        }

        case OP_RETURN: {
            unsigned a = A_OF(ins);
            retval = regs[a];
            goto bc_done;
        }

        case OP_CLOSURE: {
            unsigned a  = A_OF(ins);
            unsigned bx = Bx_OF(ins);
            if (bx >= bc->consts_len) { ok = 0; goto bc_done; }
            mino_val_t *child = bc->consts[bx];
            if (child == NULL || mino_type_of(child) != MINO_FN) {
                ok = 0;
                goto bc_done;
            }
            /* Build a fresh closure that captures the current env.
             * The child fn template carries the params/body/bc; copy
             * those over and rebind the env to the calling frame's
             * lexical environment. */
            mino_val_t *closure = make_fn(S, child->as.fn.params,
                                          child->as.fn.body, env);
            if (closure == NULL) { ok = 0; goto bc_done; }
            closure->as.fn.defining_ns = child->as.fn.defining_ns;
            /* Cast away const: the bc field is exposed as const for
             * embedders, but the runtime owns it and shares it
             * across closures of the same template. */
            *(const mino_bc_fn_t **)&closure->as.fn.bc = child->as.fn.bc;
            closure->as.fn.shape = child->as.fn.shape;
            regs[a] = closure;
            break;
        }

        case OP_MAKE_LAZY: {
            unsigned a  = A_OF(ins);
            unsigned bx = Bx_OF(ins);
            if (bx >= bc->consts_len) { ok = 0; goto bc_done; }
            mino_val_t *body = bc->consts[bx];
            mino_val_t *lz = alloc_val(S, MINO_LAZY);
            if (lz == NULL) { ok = 0; goto bc_done; }
            lz->as.lazy.body     = body;
            lz->as.lazy.env      = env;
            lz->as.lazy.cached   = NULL;
            lz->as.lazy.realized = 0;
            regs[a] = lz;
            break;
        }

        case OP_PUSH_ENV: {
            mino_env_t *child = env_child(S, env);
            if (child == NULL) { ok = 0; goto bc_done; }
            env = child;
            break;
        }

        case OP_POP_ENV: {
            if (env == NULL || env->parent == NULL) {
                /* Can't happen if the compiler emits matched
                 * PUSH/POP pairs around every let scope; defensive. */
                ok = 0; goto bc_done;
            }
            env = env->parent;
            break;
        }

        case OP_ENV_BIND: {
            unsigned a  = A_OF(ins);
            unsigned bx = Bx_OF(ins);
            if (bx >= bc->consts_len) { ok = 0; goto bc_done; }
            mino_val_t *sym = bc->consts[bx];
            if (sym == NULL || mino_type_of(sym) != MINO_SYMBOL) {
                ok = 0; goto bc_done;
            }
            env_bind_sym(S, env, sym, regs[a]);
            break;
        }

        case OP_ADD_II:
        case OP_SUB_II:
        case OP_MUL_II:
        case OP_LT_II:
        case OP_LE_II:
        case OP_GT_II:
        case OP_GE_II:
        case OP_EQ_II:
        case OP_MOD_II:
        case OP_QUOT_II:
        case OP_REM_II:
        case OP_BAND_II:
        case OP_BOR_II:
        case OP_BXOR_II:
        case OP_SHL_II:
        case OP_SHR_II:
        case OP_USHR_II: {
            /* Speculative int+int fast lanes for the binary arith /
             * compare / bitwise / div-class ops. On a type miss or a
             * bail (div-by-zero, shift-out-of-range, MIN/-1 overflow)
             * we fall through to the corresponding prim with the same
             * argv ABI as a regular OP_CALL so the prim raises the
             * Clojure-correct diagnostic or promotes through the
             * numeric tower. */
            unsigned a = A_OF(ins);
            unsigned b = B_OF(ins);
            unsigned c = C_OF(ins);
            unsigned subop;
            mino_val_t *(*fallback)(mino_state_t *, mino_val_t *, mino_env_t *);
            switch (op) {
            case OP_ADD_II:  subop = BINOP_ADD;  fallback = prim_add; break;
            case OP_SUB_II:  subop = BINOP_SUB;  fallback = prim_sub; break;
            case OP_MUL_II:  subop = BINOP_MUL;  fallback = prim_mul; break;
            case OP_LT_II:   subop = BINOP_LT;   fallback = prim_lt;  break;
            case OP_LE_II:   subop = BINOP_LE;   fallback = prim_lte; break;
            case OP_GT_II:   subop = BINOP_GT;   fallback = prim_gt;  break;
            case OP_GE_II:   subop = BINOP_GE;   fallback = prim_gte; break;
            case OP_EQ_II:   subop = BINOP_EQ;   fallback = prim_eq;  break;
            case OP_MOD_II:  subop = BINOP_MOD;  fallback = prim_mod; break;
            case OP_QUOT_II: subop = BINOP_QUOT; fallback = prim_quot; break;
            case OP_REM_II:  subop = BINOP_REM;  fallback = prim_rem; break;
            case OP_BAND_II: subop = BINOP_BAND; fallback = prim_bit_and; break;
            case OP_BOR_II:  subop = BINOP_BOR;  fallback = prim_bit_or;  break;
            case OP_BXOR_II: subop = BINOP_BXOR; fallback = prim_bit_xor; break;
            case OP_SHL_II:  subop = BINOP_SHL;  fallback = prim_bit_shift_left; break;
            case OP_SHR_II:  subop = BINOP_SHR;  fallback = prim_bit_shift_right; break;
            case OP_USHR_II: subop = BINOP_USHR; fallback = prim_unsigned_bit_shift_right; break;
            default: ok = 0; goto bc_done;
            }
            mino_val_t *r = binop_int_fast(S, regs[b], regs[c], subop);
            if (r == NULL) {
                mino_val_t *list = mino_nil(S);
                list = mino_cons(S, regs[c], list);
                if (list == NULL) { ok = 0; goto bc_done; }
                list = mino_cons(S, regs[b], list);
                if (list == NULL) { ok = 0; goto bc_done; }
                r = fallback(S, list, env);
                if (r == NULL) { ok = 0; goto bc_done; }
                regs = S->bc_regs + base;
            }
            regs[a] = r;
            break;
        }

        case OP_INC_I:
        case OP_DEC_I:
        case OP_ZERO_INT_P:
        case OP_POS_P_I:
        case OP_NEG_P_I:
        case OP_EVEN_P_I:
        case OP_ODD_P_I:
        case OP_BNOT_I: {
            unsigned a = A_OF(ins);
            unsigned b = B_OF(ins);
            unsigned subop;
            mino_val_t *(*fallback)(mino_state_t *, mino_val_t *, mino_env_t *);
            switch (op) {
            case OP_INC_I:      subop = UNOP_INC;    fallback = prim_inc;    break;
            case OP_DEC_I:      subop = UNOP_DEC;    fallback = prim_dec;    break;
            case OP_ZERO_INT_P: subop = UNOP_ZERO_P; fallback = prim_zero_p; break;
            case OP_POS_P_I:    subop = UNOP_POS_P;  fallback = prim_pos_p;  break;
            case OP_NEG_P_I:    subop = UNOP_NEG_P;  fallback = prim_neg_p;  break;
            case OP_EVEN_P_I:   subop = UNOP_EVEN_P; fallback = prim_even_p; break;
            case OP_ODD_P_I:    subop = UNOP_ODD_P;  fallback = prim_odd_p;  break;
            case OP_BNOT_I:     subop = UNOP_BNOT;   fallback = prim_bit_not; break;
            default: ok = 0; goto bc_done;
            }
            mino_val_t *r = unop_int_fast(S, regs[b], subop);
            if (r == NULL) {
                mino_val_t *list = mino_nil(S);
                list = mino_cons(S, regs[b], list);
                if (list == NULL) { ok = 0; goto bc_done; }
                r = fallback(S, list, env);
                if (r == NULL) { ok = 0; goto bc_done; }
                regs = S->bc_regs + base;
            }
            regs[a] = r;
            break;
        }

        case OP_LOOP_INT_DEC: {
            /* Fused counted-loop step (single binding):
             *   if regs[A] == 0: fall through (exit branch follows).
             *   else: regs[A]-- and re-fetch (pc-=1).
             * Hot path: tagged-int test, in-range decrement, single
             * back-jump. Cold paths (non-int test, MIN_INT decrement)
             * delegate to prim_zero_p / prim_dec so the user-visible
             * diagnostic ("zero? requires a number", "integer
             * overflow") fires exactly as the unfused emission
             * would have. */
            unsigned a = A_OF(ins);
            mino_val_t *v = regs[a];
            if (v != NULL && MINO_IS_INT(v)) {
                long long t = MINO_INT_VAL(v);
                if (t == 0) break;
                if (t != MINO_INT_MIN) {
                    regs[a] = MINO_MAKE_INT(t - 1);
                    pc -= 1;
                    break;
                }
                /* MIN_INT: fall through to the prim_dec slow path so
                 * the throw fires. */
            }
            /* Slow path: call prim_zero_p first to decide the branch
             * and to surface any non-number diagnostic. Then on
             * non-zero, call prim_dec which raises on overflow. */
            {
                mino_val_t *list = mino_nil(S);
                list = mino_cons(S, regs[a], list);
                if (list == NULL) { ok = 0; goto bc_done; }
                mino_val_t *zp = prim_zero_p(S, list, env);
                if (zp == NULL) { ok = 0; goto bc_done; }
                regs = S->bc_regs + base;
                if (mino_is_truthy(zp)) {
                    /* Fall through to the exit branch (no recur). */
                    break;
                }
                mino_val_t *list2 = mino_nil(S);
                list2 = mino_cons(S, regs[a], list2);
                if (list2 == NULL) { ok = 0; goto bc_done; }
                mino_val_t *decv = prim_dec(S, list2, env);
                if (decv == NULL) { ok = 0; goto bc_done; }
                regs = S->bc_regs + base;
                regs[a] = decv;
                pc -= 1;
                break;
            }
        }

        case OP_LOOP_INT_DEC_INC: {
            /* Fused counted-loop step (two bindings). Hot path is the
             * tagged-int / in-range case; everything else delegates to
             * prim_zero_p / prim_dec / prim_inc so the
             * non-number / overflow diagnostics still fire. */
            unsigned a = A_OF(ins);
            unsigned b = B_OF(ins);
            mino_val_t *vt = regs[a];
            mino_val_t *vi = regs[b];
            if (vt != NULL && vi != NULL
                && MINO_IS_INT(vt) && MINO_IS_INT(vi)) {
                long long t = MINO_INT_VAL(vt);
                if (t == 0) break;
                long long i = MINO_INT_VAL(vi);
                if (t != MINO_INT_MIN && i != MINO_INT_MAX) {
                    regs[a] = MINO_MAKE_INT(t - 1);
                    regs[b] = MINO_MAKE_INT(i + 1);
                    pc -= 1;
                    break;
                }
                /* Overflow on dec or inc: fall through to the prim
                 * slow path so the throw fires. */
            }
            {
                mino_val_t *list = mino_nil(S);
                list = mino_cons(S, regs[a], list);
                if (list == NULL) { ok = 0; goto bc_done; }
                mino_val_t *zp = prim_zero_p(S, list, env);
                if (zp == NULL) { ok = 0; goto bc_done; }
                regs = S->bc_regs + base;
                if (mino_is_truthy(zp)) break;
                mino_val_t *list2 = mino_nil(S);
                list2 = mino_cons(S, regs[a], list2);
                if (list2 == NULL) { ok = 0; goto bc_done; }
                mino_val_t *decv = prim_dec(S, list2, env);
                if (decv == NULL) { ok = 0; goto bc_done; }
                regs = S->bc_regs + base;
                mino_val_t *list3 = mino_nil(S);
                list3 = mino_cons(S, regs[b], list3);
                if (list3 == NULL) { ok = 0; goto bc_done; }
                mino_val_t *incv = prim_inc(S, list3, env);
                if (incv == NULL) { ok = 0; goto bc_done; }
                regs = S->bc_regs + base;
                regs[a] = decv;
                regs[b] = incv;
                pc -= 1;
                break;
            }
        }

        case OP_NTH_VEC: {
            /* Fast lane for (nth vec int). Falls through to prim_nth on
             * any type miss so the diagnostic (wrong type / out-of-
             * range / lazy-seq stride) stays Clojure-correct. */
            unsigned a = A_OF(ins);
            unsigned b = B_OF(ins);
            unsigned cc = C_OF(ins);
            mino_val_t *coll = regs[b];
            mino_val_t *idx_v = regs[cc];
            if (coll != NULL && mino_type_of(coll) == MINO_VECTOR
                && idx_v != NULL && MINO_IS_INT(idx_v)) {
                long long idx = MINO_INT_VAL(idx_v);
                if (idx >= 0 && (size_t)idx < coll->as.vec.len) {
                    regs[a] = vec_nth(coll, (size_t)idx);
                    break;
                }
            }
            /* Miss: cons the arg pair and call prim_nth so all of its
             * lazy-seq / chunk / nil / negative-index / out-of-range /
             * type-error paths fire exactly as the slow lane would. */
            mino_val_t *list = mino_nil(S);
            list = mino_cons(S, idx_v, list);
            list = mino_cons(S, coll, list);
            if (list == NULL) { ok = 0; goto bc_done; }
            mino_val_t *r = prim_nth(S, list, env);
            if (r == NULL) { ok = 0; goto bc_done; }
            regs = S->bc_regs + base;
            regs[a] = r;
            break;
        }

        case OP_GET_KW_MAP: {
            /* Fast lane for (get map keyword) -> map value or nil. On a
             * map+keyword pair this is a single hash + HAMT lookup, no
             * arg-list cons. Misses fall back to prim_get so non-map
             * collections / non-keyword keys / 3-arg default forms keep
             * their full semantics. */
            unsigned a = A_OF(ins);
            unsigned b = B_OF(ins);
            unsigned cc = C_OF(ins);
            mino_val_t *coll = regs[b];
            mino_val_t *key  = regs[cc];
            if (coll != NULL && mino_type_of(coll) == MINO_MAP
                && key != NULL && mino_type_of(key) == MINO_KEYWORD) {
                mino_val_t *v = map_get_val(coll, key);
                regs[a] = v == NULL ? mino_nil(S) : v;
                break;
            }
            mino_val_t *list = mino_nil(S);
            list = mino_cons(S, key, list);
            list = mino_cons(S, coll, list);
            if (list == NULL) { ok = 0; goto bc_done; }
            mino_val_t *r = prim_get(S, list, env);
            if (r == NULL) { ok = 0; goto bc_done; }
            regs = S->bc_regs + base;
            regs[a] = r;
            break;
        }

        case OP_ADD_IK:
        case OP_SUB_IK:
        case OP_LT_IK:
        case OP_LE_IK:
        case OP_EQ_IK: {
            /* Immediate-operand variants: lhs in B reg, signed 8-bit
             * imm in C. The imm is by-construction an int (compile-time
             * literal), so only the lhs register needs a tag check. On
             * a tag miss we synthesize the literal back into a tagged
             * int and reuse the existing prim fallback path. */
            unsigned a    = A_OF(ins);
            unsigned b    = B_OF(ins);
            long long imm = (long long)(int8_t)C_OF(ins);
            mino_val_t *lhs = regs[b];
            mino_val_t *r;
            if (MINO_IS_INT(lhs)) {
                long long la = MINO_INT_VAL(lhs);
                long long out;
                switch (op) {
                case OP_ADD_IK:
#if defined(__GNUC__) || defined(__clang__)
                    if (__builtin_saddll_overflow(la, imm, &out)) { r = NULL; break; }
#else
                    out = la + imm;
#endif
                    r = tag_or_box_int(S, out); break;
                case OP_SUB_IK:
#if defined(__GNUC__) || defined(__clang__)
                    if (__builtin_ssubll_overflow(la, imm, &out)) { r = NULL; break; }
#else
                    out = la - imm;
#endif
                    r = tag_or_box_int(S, out); break;
                case OP_LT_IK: r = (la <  imm) ? mino_true(S) : mino_false(S); break;
                case OP_LE_IK: r = (la <= imm) ? mino_true(S) : mino_false(S); break;
                case OP_EQ_IK: r = (la == imm) ? mino_true(S) : mino_false(S); break;
                default: ok = 0; goto bc_done;
                }
            } else {
                r = NULL;
            }
            if (r == NULL) {
                /* Fallback path: rebuild a cons-spine arg list with the
                 * literal as a freshly-tagged int and call the prim. */
                mino_val_t *(*fallback)(mino_state_t *, mino_val_t *, mino_env_t *);
                mino_val_t *list, *imv;
                switch (op) {
                case OP_ADD_IK: fallback = prim_add; break;
                case OP_SUB_IK: fallback = prim_sub; break;
                case OP_LT_IK:  fallback = prim_lt;  break;
                case OP_LE_IK:  fallback = prim_lte; break;
                case OP_EQ_IK:  fallback = prim_eq;  break;
                default: ok = 0; goto bc_done;
                }
                imv  = mino_int(S, imm);
                if (imv == NULL) { ok = 0; goto bc_done; }
                list = mino_cons(S, imv, mino_nil(S));
                if (list == NULL) { ok = 0; goto bc_done; }
                list = mino_cons(S, regs[b], list);
                if (list == NULL) { ok = 0; goto bc_done; }
                r = fallback(S, list, env);
                if (r == NULL) { ok = 0; goto bc_done; }
                regs = S->bc_regs + base;
            }
            regs[a] = r;
            break;
        }

        case OP_BINOP_INT: {
            /* The original Phase-1 generic binop with its sub-op
             * encoded in the low nibble of the instruction. The
             * encoding collides with the op byte for non-zero sub-ops;
             * the compiler now emits per-op specialised opcodes
             * instead. The handler stays for any hand-written stream
             * that uses sub-op zero (ADD). */
            unsigned a = A_OF(ins);
            unsigned b = B_OF(ins);
            unsigned c = C_OF(ins);
            unsigned subop = BINOP_OF(ins);
            mino_val_t *r = binop_int_fast(S, regs[b], regs[c], subop);
            if (r == NULL) { ok = 0; goto bc_done; }
            regs[a] = r;
            break;
        }

        case OP_PUSHCATCH: {
            unsigned a  = A_OF(ins);
            int off     = sBx_OF(ins);
            int td      = ctx->try_depth;
            size_t hpc  = (size_t)((long)pc + off);
            if (td >= MAX_TRY_DEPTH
                || ctx->bc_catch_depth >= MAX_TRY_DEPTH) {
                ok = 0; goto bc_done;
            }
            /* Record the BC-side resume state BEFORE the setjmp call.
             * The longjmp-return branch only reads from S/ctx; no
             * post-setjmp writes need to survive the longjmp. The
             * local `td` is intentionally NOT used after setjmp -- a
             * sibling PUSHCATCH frame may share its stack slot, so by
             * the time longjmp lands we cannot trust `td` to still
             * carry our value. `bc_catch_stack[d].try_depth_at_push`
             * holds the same value in heap-backed storage. */
            ctx->bc_catch_stack[ctx->bc_catch_depth].handler_pc        = hpc;
            ctx->bc_catch_stack[ctx->bc_catch_depth].reg_window_base   = base;
            ctx->bc_catch_stack[ctx->bc_catch_depth].try_depth_at_push = td;
            ctx->bc_catch_stack[ctx->bc_catch_depth].ex_reg            = a;
            ctx->bc_catch_stack[ctx->bc_catch_depth].env_at_push       = env;
            ctx->bc_catch_stack[ctx->bc_catch_depth].dyn_stack_at_push = ctx->dyn_stack;
            ctx->bc_catch_depth++;

            ctx->try_stack[td].exception      = NULL;
            ctx->try_stack[td].saved_ns       = S->current_ns;
            ctx->try_stack[td].saved_ambient  = S->fn_ambient_ns;
            ctx->try_stack[td].saved_load_len = S->load_stack_len;

            if (setjmp(ctx->try_stack[td].buf) == 0) {
                /* Normal entry: arm the try frame and run the body. */
                ctx->try_depth = td + 1;
            } else {
                /* longjmp landed here: a throw inside the body (BC or
                 * tree-walker callee) targeted our setjmp. Recover the
                 * VM state from the catch entry, drop the try frame,
                 * stash the normalized exception in ex_reg, and resume
                 * at the handler pc. Locals modified between setjmp
                 * and longjmp (pc, env, regs, retval, ok) are
                 * overwritten here; base / bc / code / match never
                 * change after fn entry so they survive untouched. */
                int d         = --ctx->bc_catch_depth;
                int my_td     = ctx->bc_catch_stack[d].try_depth_at_push;
                mino_val_t *ex = ctx->try_stack[my_td].exception;
                S->current_ns    = ctx->try_stack[my_td].saved_ns;
                S->fn_ambient_ns = ctx->try_stack[my_td].saved_ambient;
                load_stack_truncate(S, ctx->try_stack[my_td].saved_load_len);
                ctx->try_depth = my_td;
                /* Pop any dyn frames that the body PUSHDYN'd but never
                 * POPDYN'd because the throw bypassed the matching
                 * cleanup. Matches eval_try's saved_dyn unwind so a
                 * `(binding [...] (throw ...))` body doesn't leave its
                 * binding visible to the catch handler. */
                {
                    dyn_frame_t *anchor =
                        ctx->bc_catch_stack[d].dyn_stack_at_push;
                    while (ctx->dyn_stack != anchor) {
                        dyn_frame_t *f = ctx->dyn_stack;
                        if (f == NULL) break;
                        ctx->dyn_stack = f->prev;
                        dyn_binding_list_free(f->bindings);
                        free(f);
                    }
                }
                pc      = ctx->bc_catch_stack[d].handler_pc;
                env     = ctx->bc_catch_stack[d].env_at_push;
                regs    = S->bc_regs + base;
                regs[ctx->bc_catch_stack[d].ex_reg] =
                    normalize_exception(S, ex);
                retval  = NULL;
                ok      = 1;
                clear_error(S);
            }
            break;
        }

        case OP_POPCATCH: {
            if (ctx->bc_catch_depth <= saved_bc_catch_depth
                || ctx->try_depth <= saved_try_depth) {
                ok = 0; goto bc_done;
            }
            ctx->bc_catch_depth--;
            ctx->try_depth--;
            break;
        }

        case OP_PUSHDYN: {
            /* A=base_reg, Bx=names_const_idx
             *
             * names_const is a MINO_VECTOR of plain symbols; the
             * matching binding values live in regs[base..base+N).
             * Allocates a dyn_frame_t and one dyn_binding_t per name
             * via malloc -- same lifecycle as eval_binding so a
             * throw-unwind walking dyn_stack frees them by the same
             * path. */
            unsigned a  = A_OF(ins);
            unsigned bx = Bx_OF(ins);
            if (bx >= bc->consts_len) { ok = 0; goto bc_done; }
            mino_val_t *names = bc->consts[bx];
            if (names == NULL || mino_type_of(names) != MINO_VECTOR) {
                ok = 0; goto bc_done;
            }
            size_t n = names->as.vec.len;
            dyn_binding_t *bhead = NULL;
            for (size_t i = 0; i < n; i++) {
                mino_val_t *sym = vec_nth(names, i);
                if (sym == NULL || mino_type_of(sym) != MINO_SYMBOL) {
                    while (bhead != NULL) {
                        dyn_binding_t *nxt = bhead->next;
                        free(bhead); bhead = nxt;
                    }
                    ok = 0; goto bc_done;
                }
                dyn_binding_t *b = (dyn_binding_t *)malloc(sizeof(*b));
                if (b == NULL) {
                    while (bhead != NULL) {
                        dyn_binding_t *nxt = bhead->next;
                        free(bhead); bhead = nxt;
                    }
                    ok = 0; goto bc_done;
                }
                b->name = sym->as.s.data;
                b->val  = regs[a + (unsigned)i];
                b->next = bhead;
                bhead   = b;
            }
            dyn_frame_t *frame = (dyn_frame_t *)malloc(sizeof(*frame));
            if (frame == NULL) {
                while (bhead != NULL) {
                    dyn_binding_t *nxt = bhead->next;
                    free(bhead); bhead = nxt;
                }
                ok = 0; goto bc_done;
            }
            frame->bindings = bhead;
            frame->prev     = ctx->dyn_stack;
            ctx->dyn_stack  = frame;
            break;
        }

        case OP_POPDYN: {
            /* A=count: pop `count` frames off the dyn stack. The body
             * has finished and matched its PUSHDYN(s); free each
             * frame's bindings + frame itself. count is encoded so
             * the compiler can collapse multiple POPDYNs into one in
             * patterns like (binding [...] (binding [...] body)). */
            unsigned a = A_OF(ins);
            if (a == 0) a = 1;
            for (unsigned i = 0; i < a; i++) {
                dyn_frame_t *f = ctx->dyn_stack;
                if (f == NULL || f == saved_dyn_stack) {
                    ok = 0; goto bc_done;
                }
                ctx->dyn_stack = f->prev;
                dyn_binding_list_free(f->bindings);
                free(f);
            }
            break;
        }

        case OP_THROW: {
            unsigned a = A_OF(ins);
            mino_val_t *exc = regs[a];
            if (ctx->try_depth > 0) {
                ctx->try_stack[ctx->try_depth - 1].exception = exc;
                longjmp(ctx->try_stack[ctx->try_depth - 1].buf, 1);
                /* unreachable */
            }
            /* No enclosing try -- format as fatal user error and bail
             * through the standard error path. */
            prim_throw_classified(S, "user", "MUS001",
                "unhandled exception (no try)");
            ok = 0;
            goto bc_done;
        }

        default:
            ok = 0;
            goto bc_done;
        }
    }

bc_done:
    /* Roll any BC catch frames that survived back so the try_stack is
     * exactly as it was at fn entry. Normal POPCATCH paths balance the
     * count on their own; this only kicks in on error / unwind paths. */
    if (ctx->bc_catch_depth > saved_bc_catch_depth) {
        ctx->bc_catch_depth = saved_bc_catch_depth;
    }
    if (ctx->try_depth > saved_try_depth) {
        ctx->try_depth = saved_try_depth;
    }
    /* Unwind any dyn frames that the body PUSHDYN'd but didn't POP --
     * happens on NULL-return error paths, tail-call sentinel paths,
     * and catch landing pads (which restore try state but leave the
     * dyn frames pushed by the failing body). Mirrors the unwind loop
     * in control.c's eval_try longjmp branch. */
    while (ctx->dyn_stack != saved_dyn_stack) {
        dyn_frame_t *f = ctx->dyn_stack;
        if (f == NULL) break;
        ctx->dyn_stack = f->prev;
        dyn_binding_list_free(f->bindings);
        free(f);
    }
    bc_pop_window(S, base);
    if (!ok) return NULL;
    return retval != NULL ? retval : mino_nil(S);
}

void mino_bc_fn_mark(mino_state_t *S, const mino_bc_fn_t *bc)
{
    (void)S; (void)bc;
}
