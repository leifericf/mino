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

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mino.h"
#include "runtime/internal.h"       /* mino_state_t, gc_alloc_typed, GC_T_VALARR */
#include "eval/internal.h"          /* eval_impl, apply_callable */
#include "eval/bc/internal.h"
#include "collections/internal.h"   /* make_fn */

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

/* Integer fast-lane for OP_BINOP_INT. Identical dispatch shape to the
 * v0.103.0 eval-side fast path; returns NULL on type mismatch or
 * overflow so the VM can bail to the tree-walker fallback. */
static mino_val_t *binop_int_fast(mino_state_t *S, mino_val_t *lhs,
                                  mino_val_t *rhs, unsigned subop)
{
    if (lhs == NULL || rhs == NULL) return NULL;
    if (lhs->type != MINO_INT || rhs->type != MINO_INT) return NULL;
    long long a = lhs->as.i;
    long long b = rhs->as.i;
    long long r;
    switch (subop) {
    case BINOP_ADD:
#if defined(__GNUC__) || defined(__clang__)
        if (__builtin_saddll_overflow(a, b, &r)) return NULL;
#else
        r = a + b;
#endif
        return mino_int(S, r);
    case BINOP_SUB:
#if defined(__GNUC__) || defined(__clang__)
        if (__builtin_ssubll_overflow(a, b, &r)) return NULL;
#else
        r = a - b;
#endif
        return mino_int(S, r);
    case BINOP_MUL:
#if defined(__GNUC__) || defined(__clang__)
        if (__builtin_smulll_overflow(a, b, &r)) return NULL;
#else
        r = a * b;
#endif
        return mino_int(S, r);
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
    if (sym == NULL || sym->type != MINO_SYMBOL) return NULL;
    return eval_impl(S, sym, env, 0);
}

mino_val_t *mino_bc_run(mino_state_t *S, mino_val_t *fn_val,
                        mino_val_t **argv, int argc, mino_env_t *env)
{
    const mino_bc_fn_t *bc = fn_val->as.fn.bc;
    if (bc == NULL || bc->code == NULL) return NULL;
    /* Arity guard. With a rest binding the caller may pass any number
     * of args >= n_params (the overflow becomes the rest list). */
    if (bc->has_rest) {
        if (argc < bc->n_params) return NULL;
    } else {
        if (argc != bc->n_params) return NULL;
    }

    size_t base = bc_push_window(S, bc->n_regs);
    if (base == (size_t)-1) return NULL;

    for (int i = 0; i < bc->n_params; i++) {
        S->bc_regs[base + (size_t)i] = argv[i];
    }
    /* Collect overflow args into a list and place it in the slot
     * right after the fixed params. mino_cons walks back-to-front so
     * we get the values in their original order. When argc ==
     * n_params the rest binding is the empty list. */
    if (bc->has_rest) {
        mino_val_t *rest = mino_nil(S);
        for (int i = argc - 1; i >= bc->n_params; i--) {
            rest = mino_cons(S, argv[i], rest);
            if (rest == NULL) { bc_pop_window(S, base); return NULL; }
        }
        S->bc_regs[base + (size_t)bc->n_params] = rest;
    }

    /* When the body contains an inner fn literal, extend the lexical
     * env with a fresh child and publish the params into it. Any
     * OP_CLOSURE emitted by the body then captures an env that
     * already has the outer's params (and, via OP_PUSH_ENV, any
     * let-bindings) visible to the inner fn. Fns without inner fns
     * skip the env_alloc + n_params hash inserts entirely. */
    if (bc->captures) {
        env = env_child(S, env);
        if (env == NULL) { bc_pop_window(S, base); return NULL; }
        for (int i = 0; i < bc->n_params; i++) {
            mino_val_t *p = vec_nth(fn_val->as.fn.params, (size_t)i);
            if (p == NULL || p->type != MINO_SYMBOL) {
                bc_pop_window(S, base);
                return NULL;
            }
            env_bind_sym(S, env, p, argv[i]);
        }
        if (bc->has_rest) {
            mino_val_t *rest_sym = vec_nth(fn_val->as.fn.params,
                fn_val->as.fn.params->as.vec.len - 1);
            if (rest_sym != NULL && rest_sym->type == MINO_SYMBOL) {
                env_bind_sym(S, env, rest_sym,
                    S->bc_regs[base + (size_t)bc->n_params]);
            }
        }
    }

    mino_val_t **regs = S->bc_regs + base;
    const mino_bc_insn_t *code = bc->code;
    size_t pc = 0;
    mino_val_t *retval = NULL;
    int ok = 1;

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
            if (sym == NULL || sym->type != MINO_SYMBOL) {
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
            if (child == NULL || child->type != MINO_FN) {
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
            if (sym == NULL || sym->type != MINO_SYMBOL) {
                ok = 0; goto bc_done;
            }
            env_bind_sym(S, env, sym, regs[a]);
            break;
        }

        case OP_BINOP_INT: {
            unsigned a = A_OF(ins);
            unsigned b = B_OF(ins);
            unsigned c = C_OF(ins);
            unsigned subop = BINOP_OF(ins);
            mino_val_t *r = binop_int_fast(S, regs[b], regs[c], subop);
            if (r == NULL) {
                /* Fast-lane miss: bail. Phase 1's compiler does not
                 * emit BINOP_INT, so this path is reached only by
                 * hand-written / Phase-4-specialized streams. The
                 * follow-on commits add a slow-fallback variant that
                 * routes through the named prim. */
                ok = 0;
                goto bc_done;
            }
            regs[a] = r;
            break;
        }

        default:
            ok = 0;
            goto bc_done;
        }
    }

bc_done:
    bc_pop_window(S, base);
    if (!ok) return NULL;
    return retval != NULL ? retval : mino_nil(S);
}

void mino_bc_fn_mark(mino_state_t *S, const mino_bc_fn_t *bc)
{
    (void)S; (void)bc;
}
