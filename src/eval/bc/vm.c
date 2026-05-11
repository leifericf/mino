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
    for (int i = 0; i < n; i++) S->bc_regs[base + (size_t)i] = NULL;
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
    S->bc_int_make_count++;
    if (r >= MINO_INT_MIN && r <= MINO_INT_MAX) {
        S->bc_int_alloc_avoided++;
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
            mino_val_t *args   = args_from_regs(S, regs + a + 1, argn);
            if (args == NULL) { ok = 0; goto bc_done; }
            mino_val_t *r = apply_callable(S, callee, args, env);
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
             * without growing the C stack. */
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
        case OP_EQ_II: {
            /* Speculative int+int fast lanes for the eight binary
             * arith / compare ops. On a type miss we fall through to
             * the corresponding prim with the same argv ABI as a
             * regular OP_CALL. */
            unsigned a = A_OF(ins);
            unsigned b = B_OF(ins);
            unsigned c = C_OF(ins);
            unsigned subop;
            mino_val_t *(*fallback)(mino_state_t *, mino_val_t *, mino_env_t *);
            switch (op) {
            case OP_ADD_II: subop = BINOP_ADD; fallback = prim_add; break;
            case OP_SUB_II: subop = BINOP_SUB; fallback = prim_sub; break;
            case OP_MUL_II: subop = BINOP_MUL; fallback = prim_mul; break;
            case OP_LT_II:  subop = BINOP_LT;  fallback = prim_lt;  break;
            case OP_LE_II:  subop = BINOP_LE;  fallback = prim_lte; break;
            case OP_GT_II:  subop = BINOP_GT;  fallback = prim_gt;  break;
            case OP_GE_II:  subop = BINOP_GE;  fallback = prim_gte; break;
            case OP_EQ_II:  subop = BINOP_EQ;  fallback = prim_eq;  break;
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
        case OP_ZERO_INT_P: {
            unsigned a = A_OF(ins);
            unsigned b = B_OF(ins);
            unsigned subop;
            mino_val_t *(*fallback)(mino_state_t *, mino_val_t *, mino_env_t *);
            switch (op) {
            case OP_INC_I:      subop = UNOP_INC;    fallback = prim_inc;    break;
            case OP_DEC_I:      subop = UNOP_DEC;    fallback = prim_dec;    break;
            case OP_ZERO_INT_P: subop = UNOP_ZERO_P; fallback = prim_zero_p; break;
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
             * post-setjmp writes need to survive the longjmp. */
            ctx->bc_catch_stack[ctx->bc_catch_depth].handler_pc        = hpc;
            ctx->bc_catch_stack[ctx->bc_catch_depth].reg_window_base   = base;
            ctx->bc_catch_stack[ctx->bc_catch_depth].try_depth_at_push = td;
            ctx->bc_catch_stack[ctx->bc_catch_depth].ex_reg            = a;
            ctx->bc_catch_stack[ctx->bc_catch_depth].env_at_push       = env;
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
                int d = --ctx->bc_catch_depth;
                mino_val_t *ex = ctx->try_stack[td].exception;
                S->current_ns    = ctx->try_stack[td].saved_ns;
                S->fn_ambient_ns = ctx->try_stack[td].saved_ambient;
                load_stack_truncate(S, ctx->try_stack[td].saved_load_len);
                ctx->try_depth = ctx->bc_catch_stack[d].try_depth_at_push;
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
    bc_pop_window(S, base);
    if (!ok) return NULL;
    return retval != NULL ? retval : mino_nil(S);
}

void mino_bc_fn_mark(mino_state_t *S, const mino_bc_fn_t *bc)
{
    (void)S; (void)bc;
}
