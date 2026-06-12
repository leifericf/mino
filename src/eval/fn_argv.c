/*
 * fn_argv.c -- argv-ABI callable invocation.
 *
 * Extracted from fn.c to keep each translation unit under the 1100-line
 * limit.  Provides argv_to_cons, invoke_bc_fn_argv (static inline),
 * mino_apply_known_bc_fn_argv, and apply_callable_argv.
 *
 * Declarations for the public functions live in eval/internal.h.
 */

#include <limits.h>

#include "eval/special_internal.h"
#include "eval/bc/internal.h"
#include "eval/bc/jit.h"

/* Helper: build a cons-spine list from argv[0..argc). Used by the slow
 * paths of apply_callable_argv that need to delegate back to
 * apply_callable's cons-list-based dispatch. Returns NULL on alloc
 * failure. */
static mino_val *argv_to_cons(mino_state *S, mino_val **argv, int argc)
{
    mino_val *list = mino_nil(S);
    if (list == NULL) return NULL;
    for (int i = argc - 1; i >= 0; i--) {
        mino_val *cell = mino_cons(S, argv[i], list);
        if (cell == NULL) return NULL;
        list = cell;
    }
    return list;
}

/* Shared bc-fn invocation. Called from apply_callable_argv's MINO_FN
 * branch and from the JIT's known-callee fast path. Pre-condition:
 * `fn` is a MINO_FN with `as.fn.bc != NULL` and `MINO_BC_RUNNABLE(fn)`
 * (caller checks). The implementation is the only place that handles
 * lazy-recompile-on-fold-staleness, hot-counter bumping, JIT
 * invalidation, push/pop frame, defining_ns scope, and the tail-call
 * trampoline. Returns the final value or NULL on error.
 *
 * always_inline forced so the refactor doesn't add a function-call
 * layer to either caller — apply_callable_argv's MINO_FN branch and
 * mino_apply_known_bc_fn_argv both inline the body. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline))
#endif
static inline mino_val *invoke_bc_fn_argv(mino_state *S, mino_val *fn,
                                            mino_val **argv, int argc,
                                            mino_env *env)
{
    /* The two staleness checks (NULL bc + fold-staleness recompile)
     * still run because the JIT path's IC slot can predate a fold
     * promotion (compile_ic_gen advances independently of S->ns_vars.ic_gen,
     * via mino_jit_compile). Skipping them risks running stale bc. */
    if (fn->as.fn.bc == NULL) {
        (void)mino_bc_compile_fn(S, fn);
    }
    if (fn->as.fn.bc != NULL
        && fn->as.fn.bc != &mino_bc_declined
        && fn->as.fn.bc->has_folds
        && fn->as.fn.bc->compile_ic_gen != S->ns_vars.ic_gen) {
        /* Template-aware recompile: closures share their template's
         * bc, so the recompile fires once on the template and every
         * sibling closure inherits the fresh bc through the back-
         * pointer. Without this, every closure with stale folds would
         * rebuild its own bc per call, defeating dedup. */
        mino_val *tmpl = fn->as.fn.template_fn;
        if (tmpl != NULL && tmpl->as.fn.bc == fn->as.fn.bc) {
            tmpl->as.fn.bc = NULL;
            (void)mino_bc_compile_fn(S, tmpl);
            fn->as.fn.bc = tmpl->as.fn.bc;
        } else {
            fn->as.fn.bc = NULL;
            (void)mino_bc_compile_fn(S, fn);
        }
    }
    mino_bc_check_require(S, fn);
    if (!MINO_BC_RUNNABLE(fn)) {
        /* Compile declined post-recompile: fall back to cons-form
         * apply_callable for full multi-arity dispatch. */
        mino_val *args = argv_to_cons(S, argv, argc);
        if (args == NULL) return NULL;
        return apply_callable(S, fn, args, env);
    }
    mino_bc_fn_t *bc_rec = fn->as.fn.bc;
    if (bc_rec->native != NULL && bc_rec->native_gen != S->ns_vars.ic_gen) {
        mino_jit_invalidate(S, fn);
    }
    if (bc_rec->native == NULL
        && bc_rec->hot_counter < (unsigned)-1
        && S->jit.jit_mode != (int)MINO_JIT_MODE_OFF) {
        bc_rec->hot_counter++;
        /* Adaptive tiering: a callee invoked from inside a JIT'd
         * region (jit_invoke_depth > 0) is on the hot path of
         * something already paying compile cost, so the threshold
         * collapses to 1. AUTO without a JIT'd caller keeps the
         * state's hot-threshold setting; ON mode threshold stays at
         * 1 unconditionally. */
        unsigned thresh;
        if (S->jit.jit_mode == (int)MINO_JIT_MODE_ON) {
            thresh = 1u;
        } else if (mino_current_ctx(S)->jit_invoke_depth > 0) {
            thresh = 1u;
        } else {
            thresh = S->jit.jit_hot_threshold;
        }
        if (bc_rec->hot_counter >= thresh) {
            if (mino_jit_compile(S, fn) < 0) {
                bc_rec->hot_counter = (unsigned)-1;
            }
        }
    }
    mino_val **call_argv = argv;
    int          call_argc = argc;
    int          cap       = argc;
    int          heap      = 0;
    const char  *file      = NULL;
    int          line      = 0;
    int          col       = 0;
    const char  *saved_ns      = S->ns_vars.current_ns;
    const char  *saved_ambient = S->ns_vars.fn_ambient_ns;
    mino_val  *result;
    if (fn->as.fn.defining_ns != NULL) {
        S->ns_vars.current_ns    = fn->as.fn.defining_ns;
        S->ns_vars.fn_ambient_ns = fn->as.fn.defining_ns;
    }
    if (mino_current_ctx(S)->eval_current_form != NULL
        && mino_type_of(mino_current_ctx(S)->eval_current_form) == MINO_CONS) {
        file = mino_current_ctx(S)->eval_current_form->as.cons.file;
        line = mino_current_ctx(S)->eval_current_form->as.cons.line;
        col  = mino_current_ctx(S)->eval_current_form->as.cons.column;
    }
    push_frame(S, "fn", file, line, col);
    for (;;) {
        result = mino_bc_run(S, fn, call_argv, call_argc,
                             fn->as.fn.env);
        if (result == NULL) {
            S->ns_vars.current_ns    = saved_ns;
            S->ns_vars.fn_ambient_ns = saved_ambient;
            return NULL;
        }
        if (mino_type_of(result) != MINO_TAIL_CALL) break;
        mino_val *next_fn   = result->as.tail_call.fn;
        mino_val *next_args = result->as.tail_call.args;
        if (next_fn != NULL && mino_type_of(next_fn) == MINO_FN
            && next_fn->as.fn.bc == NULL) {
            (void)mino_bc_compile_fn(S, next_fn);
        }
        if (next_fn != NULL && mino_type_of(next_fn) == MINO_FN
            && MINO_BC_RUNNABLE(next_fn)
            && next_fn->as.fn.params != NULL) {
            int new_argc = 0;
            mino_val *cur = next_args;
            while (mino_is_cons(cur)) {
                if (new_argc >= cap || !heap) {
                    /* Guard against signed overflow in cap * 2.  An int
                     * cap above INT_MAX/2 means we already hold an
                     * unreasonably large argv; treat it as an OOM. */
                    if (cap > INT_MAX / 2) {
                        S->ns_vars.current_ns    = saved_ns;
                        S->ns_vars.fn_ambient_ns = saved_ambient;
                        set_eval_diag(S,
                            mino_current_ctx(S)->eval_current_form,
                            "limit", "MLM003",
                            "argv buffer too large: tail-call arity overflow");
                        return NULL;
                    }
                    int new_cap = (new_argc + 1) < (cap * 2)
                        ? (cap * 2) : (new_argc + 8);
                    mino_val **grown = (mino_val **)gc_alloc_typed(
                        S, GC_T_VALARR,
                        (size_t)new_cap * sizeof(*grown));
                    if (grown == NULL) {
                        S->ns_vars.current_ns    = saved_ns;
                        S->ns_vars.fn_ambient_ns = saved_ambient;
                        return NULL;
                    }
                    memcpy(grown, call_argv,
                           (size_t)new_argc * sizeof(*grown));
                    call_argv = grown;
                    cap       = new_cap;
                    heap      = 1;
                }
                if (heap) {
                    gc_valarr_set(S, call_argv, (size_t)new_argc,
                                  cur->as.cons.car);
                    new_argc++;
                } else {
                    call_argv[new_argc++] = cur->as.cons.car;
                }
                cur = cur->as.cons.cdr;
            }
            call_argc = new_argc;
            fn        = next_fn;
            if (fn->as.fn.defining_ns != NULL) {
                S->ns_vars.current_ns    = fn->as.fn.defining_ns;
                S->ns_vars.fn_ambient_ns = fn->as.fn.defining_ns;
            }
            continue;
        }
        pop_frame(S);
        S->ns_vars.current_ns    = saved_ns;
        S->ns_vars.fn_ambient_ns = saved_ambient;
        return apply_callable(S, next_fn, next_args, env);
    }
    pop_frame(S);
    S->ns_vars.current_ns    = saved_ns;
    S->ns_vars.fn_ambient_ns = saved_ambient;
    return result;
}

/* JIT entry point: invoke a pre-resolved MINO_FN bc-runnable callable
 * without going through apply_callable_argv's var-unwrap + type-of
 * dispatch switch. The caller (mino_jit_call_known_fn_slow) only routes
 * here when the IC slot's cached_callable_kind is MINO_FN_BC_SINGLE
 * and a basic arity match held. Defensive: re-resolves a stale Var
 * pointer and falls back to apply_callable_argv if the callee's shape
 * drifted out from under the cache. */
mino_val *mino_apply_known_bc_fn_argv(mino_state *S, mino_val *fn,
                                        mino_val **argv, int argc,
                                        mino_env *env)
{
    if (fn != NULL && mino_type_of(fn) == MINO_VAR) {
        if (!fn->as.var.bound || fn->as.var.root == NULL) {
            return apply_callable_argv(S, fn, argv, argc, env);
        }
        fn = fn->as.var.root;
    }
    if (fn == NULL || mino_type_of(fn) != MINO_FN) {
        return apply_callable_argv(S, fn, argv, argc, env);
    }
    return invoke_bc_fn_argv(S, fn, argv, argc, env);
}

mino_val *apply_callable_argv(mino_state *S, mino_val *fn,
                                mino_val **argv, int argc,
                                mino_env *env)
{
    if (fn == NULL) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                      "eval/type", "MTY002", "cannot apply null");
        return NULL;
    }
    /* Var deref. A var bound to a callable just unwraps; matches
     * apply_callable's first action. */
    if (mino_type_of(fn) == MINO_VAR) {
        if (!fn->as.var.bound) {
            char msg[256];
            snprintf(msg, sizeof(msg), "var is unbound: %s/%s",
                     fn->as.var.ns ? fn->as.var.ns : "?",
                     fn->as.var.sym ? fn->as.var.sym : "?");
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                          "eval/type", "MTY002", msg);
            return NULL;
        }
        fn = fn->as.var.root;
        if (fn == NULL) {
            set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                          "eval/type", "MTY002", "var has nil root");
            return NULL;
        }
    }

    /* Fast path #1: argv-ABI prim. The hot case for arithmetic, IO,
     * and collection prims after the install_stdlib migration. No
     * cons-spine traffic. */
    if (mino_type_of(fn) == MINO_PRIM && fn->as.prim.fn2 != NULL) {
        const char *file = NULL;
        int         line = 0;
        int         col  = 0;
        mino_val *result;
        if (mino_current_ctx(S)->eval_current_form != NULL
            && mino_type_of(mino_current_ctx(S)->eval_current_form) == MINO_CONS) {
            file = mino_current_ctx(S)->eval_current_form->as.cons.file;
            line = mino_current_ctx(S)->eval_current_form->as.cons.line;
            col  = mino_current_ctx(S)->eval_current_form->as.cons.column;
        }
        push_frame(S, fn->as.prim.name, file, line, col);
        result = fn->as.prim.fn2(S, argv, argc, env);
        if (result == NULL) return NULL; /* leave frame for trace */
        pop_frame(S);
        return result;
    }

    /* Fast path #2: bc-runnable FN. invoke_bc_fn_argv handles lazy
     * compile, staleness check, tier promotion, and the tail-call
     * trampoline. */
    if (mino_type_of(fn) == MINO_FN) {
        return invoke_bc_fn_argv(S, fn, argv, argc, env);
    }

    /* Slow paths: PRIM with fn1 ABI, MINO_MACRO, non-fn callables.
     * Build the cons-spine and delegate. */
    {
        mino_val *args = argv_to_cons(S, argv, argc);
        if (args == NULL) return NULL;
        return apply_callable(S, fn, args, env);
    }
}
