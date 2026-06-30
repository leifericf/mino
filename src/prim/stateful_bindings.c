/*
 * stateful_bindings.c -- dynamic-binding primitives split from stateful.c.
 *
 * Primitives: get-thread-bindings, with-bindings*, push-thread-bindings*,
 *             pop-thread-bindings*, -thread-bound?, set-dyn-binding!
 * Also exports: mino_snapshot_thread_bindings (declared in runtime/env_api.h).
 *
 * Internal to the prim subsystem; embedders should only use mino.h.
 */

#include "prim/internal.h"
/* Prototypes for all non-static functions in this TU. */
mino_val *mino_snapshot_thread_bindings(mino_state *S);
mino_val *prim_get_thread_bindings(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_set_dyn_binding(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_with_bindings_star(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_push_thread_bindings_star(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_pop_thread_bindings_star(mino_state *S, mino_val *args, mino_env *env);
mino_val *prim_thread_bound_p(mino_state *S, mino_val *args, mino_env *env);

/* Snapshot the current thread's dyn_stack into a map keyed by binding-
 * name symbols. Var-backed bindings key on the fully qualified ns/name
 * symbol so a replay (with-bindings* / convey-to-worker) resolves the
 * exact var regardless of the namespace it runs in; var-less names
 * keep their literal spelling. Inner frames shadow outer frames; the
 * first occurrence walking newest-first wins. Returns nil when the
 * stack is empty. */
mino_val *mino_snapshot_thread_bindings(mino_state *S)
{
    dyn_frame_t   *f;
    dyn_binding_t *b;
    size_t         cap = 0, len = 0;
    mino_val   **keys = NULL;
    mino_val   **vals = NULL;
    if (mino_current_ctx(S)->dyn_stack == NULL) return mino_nil(S);

    for (f = mino_current_ctx(S)->dyn_stack; f != NULL; f = f->prev) {
        if (f->building) continue;
        for (b = f->bindings; b != NULL; b = b->next) {
            char        qbuf[512];
            char       *heap_key = NULL;
            const char *key_name = b->name;
            size_t i;
            int    dup = 0;
            if (b->var != NULL && b->var->as.var.ns != NULL) {
                int qn = snprintf(qbuf, sizeof(qbuf), "%s/%s",
                                  b->var->as.var.ns, b->var->as.var.sym);
                if (qn > 0 && (size_t)qn < sizeof(qbuf)) {
                    key_name = qbuf;
                } else if (qn >= (int)sizeof(qbuf)) {
                    /* Qualified name overflows the stack buffer; heap-allocate. */
                    heap_key = (char *)malloc((size_t)qn + 1);
                    if (heap_key == NULL)
                        gc_oom_throw(S, "snapshot_thread_bindings: key name");
                    snprintf(heap_key, (size_t)qn + 1, "%s/%s",
                             b->var->as.var.ns, b->var->as.var.sym);
                    key_name = heap_key;
                }
            }
            for (i = 0; i < len; i++) {
                if (strcmp(keys[i]->as.s.data, key_name) == 0) {
                    dup = 1;
                    break;
                }
            }
            if (dup) { free(heap_key); continue; }
            if (len == cap) {
                size_t       new_cap;
                mino_val **nk;
                mino_val **nv;
                size_t       j;
                if (cap == 0) {
                    new_cap = 8;
                } else if (!checked_double_sz(cap, &new_cap)) {
                    free(heap_key);
                    gc_oom_throw(S, "snapshot_thread_bindings: capacity overflow");
                }
                /* Pin old arrays before any allocation can trigger GC. */
                if (keys != NULL) gc_pin((mino_val *)keys);
                if (vals != NULL) gc_pin((mino_val *)vals);
                nk = (mino_val **)gc_alloc_typed(
                    S, GC_T_VALARR, new_cap * sizeof(*nk));
                /* Pin nk before the second allocation can trigger GC. */
                gc_pin((mino_val *)nk);
                nv = (mino_val **)gc_alloc_typed(
                    S, GC_T_VALARR, new_cap * sizeof(*nv));
                gc_unpin(1);
                if (vals != NULL) gc_unpin(1);
                if (keys != NULL) gc_unpin(1);
                for (j = 0; j < len; j++) {
                    gc_valarr_set(S, nk, j, keys[j]);
                    gc_valarr_set(S, nv, j, vals[j]);
                }
                keys = nk;
                vals = nv;
                cap  = new_cap;
            }
            /* mino_symbol can trigger GC; pin keys and vals across the call. */
            gc_pin((mino_val *)keys);
            gc_pin((mino_val *)vals);
            gc_valarr_set(S, keys, len, mino_symbol(S, key_name));
            gc_unpin(2);
            gc_valarr_set(S, vals, len, b->val);
            len++;
            free(heap_key);
        }
    }
    if (len == 0) return mino_nil(S);
    return mino_map(S, keys, vals, len);
}

/* (get-thread-bindings) -- snapshot the current dyn_stack into a map
 * suitable as input to with-bindings*. */
mino_val *prim_get_thread_bindings(mino_state *S, mino_val *args,
                                    mino_env *env)
{
    (void)args;
    (void)env;
    return mino_snapshot_thread_bindings(S);
}

/* (set-dyn-binding! 'name value) -- mutate the topmost active dynamic
 * binding for `name` to `value`. Returns the new value. Throws when
 * there is no active binding frame for the name (matches Clojure's
 * contract: set! on a dynamic var without an enclosing binding form
 * raises "Can't change/establish root binding"). Used by the
 * (set! *var* expr) macro to back the JVM-Clojure dynamic-var mutation
 * shape. */
mino_val *prim_set_dyn_binding(mino_state *S, mino_val *args,
                                mino_env *env)
{
    mino_val    *name_sym;
    mino_val    *new_val;
    const char    *name;
    dyn_frame_t   *f;
    dyn_binding_t *b;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "set-dyn-binding! requires two arguments: name value");
    }
    name_sym = args->as.cons.car;
    new_val  = args->as.cons.cdr->as.cons.car;
    if (name_sym == NULL || mino_type_of(name_sym) != MINO_SYMBOL) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "set-dyn-binding!: first argument must be a symbol");
    }
    name = name_sym->as.s.data;
    /* Match by canonical var identity when the name resolves to a
     * var (with the var-less text criterion the lookup paths use);
     * pure dynamic-scope names match by text alone. */
    {
        mino_val *var = dyn_resolve_var(S, name, name_sym->as.s.len);
        const char *bare = (var != NULL) ? var->as.var.sym : name;
        for (f = mino_current_ctx(S)->dyn_stack; f != NULL; f = f->prev) {
            if (f->building) continue;
            for (b = f->bindings; b != NULL; b = b->next) {
                if ((var != NULL && b->var == var)
                    || (b->var == NULL && bare != NULL
                        && strcmp(b->name, bare) == 0)) {
                    b->val = new_val;
                    return new_val;
                }
            }
        }
    }
    {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "Can't change/establish root binding of: %s with set!", name);
        return prim_throw_classified(S, "eval/contract", "MCT001", msg);
    }
}

/* (with-bindings* bindings-map fn) -- pushes a fresh dynamic-binding
 * frame from the map's entries (symbol-or-string keys), invokes fn
 * with no arguments, pops the frame, and returns the result. Used by
 * bound-fn* to replay a captured binding context around a thunk. The
 * malloc'd binding chain is freed on both the success and the
 * error/longjmp path because this primitive runs inside any active
 * try frame's setjmp window the caller already established. */
mino_val *prim_with_bindings_star(mino_state *S, mino_val *args,
                                   mino_env *env)
{
    mino_val    *map_arg;
    mino_val    *fn;
    mino_val    *result;
    /* Heap-allocated so the pointer remains valid if a throw inside
     * fn unwinds past the cleanup. control.c's longjmp handler walks
     * mino_current_ctx(S)->dyn_stack to free each frame; a stack-local frame would
     * leave a dangling read on Windows where popped stack memory is
     * not preserved as eagerly as on POSIX. */
    dyn_frame_t   *frame;
    dyn_binding_t *bhead = NULL;
    dyn_binding_t *b;
    size_t         i;

    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "with-bindings* requires two arguments: bindings-map fn");
    }
    map_arg = args->as.cons.car;
    fn      = args->as.cons.cdr->as.cons.car;

    /* nil bindings: just call fn with no extra frame. */
    if (map_arg == NULL || mino_type_of(map_arg) == MINO_NIL) {
        return mino_call(S, fn, mino_nil(S), env);
    }
    if (mino_type_of(map_arg) != MINO_MAP) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "with-bindings*: bindings argument must be a map or nil");
    }

    for (i = 0; i < map_arg->as.map.len; i++) {
        mino_val *key = vec_nth(map_arg->as.map.key_order, i);
        mino_val *val = map_get_val(map_arg, key);
        if (key == NULL
            || (mino_type_of(key) != MINO_VAR
                && mino_type_of(key) != MINO_SYMBOL
                && mino_type_of(key) != MINO_STRING)) {
            dyn_binding_list_free(bhead);
            return prim_throw_classified(S, "eval/type", "MTY001",
                "with-bindings*: keys must be vars, symbols, or strings");
        }
        /* Clojure-canon: with-bindings/push-thread-bindings map is
         * keyed by vars. dyn_binding_make keys var entries by var
         * identity and resolves symbol/string keys to their canonical
         * var so any read spelling of the var sees the binding. */
        b = dyn_binding_make(S, key, val, bhead);
        if (b == NULL) {
            dyn_binding_list_free(bhead);
            return prim_throw_classified(S, "eval/contract", "MIN001",
                "with-bindings*: out of memory");
        }
        bhead = b;
    }

    frame = (dyn_frame_t *)calloc(1, sizeof(*frame));
    if (frame == NULL) {
        dyn_binding_list_free(bhead);
        return prim_throw_classified(S, "eval/contract", "MIN001",
            "with-bindings*: out of memory");
    }
    frame->bindings = bhead;
    frame->building = 0;
    frame->prev     = mino_current_ctx(S)->dyn_stack;
    mino_current_ctx(S)->dyn_stack    = frame;
    result          = mino_call(S, fn, mino_nil(S), env);
    mino_current_ctx(S)->dyn_stack    = frame->prev;
    dyn_binding_list_free(bhead);
    free(frame);
    return result;
}

/* (push-thread-bindings* bindings-map) — push a fresh dynamic-binding
 * frame whose entries come from the map; symbols-or-strings as keys.
 * The frame stays live until a matching pop-thread-bindings* runs (or
 * the state is freed). Callers must pair push/pop in a try/finally
 * shape to keep the dyn_stack consistent across throws — the catch
 * unwinder in control.c does free trailing frames pushed during the
 * try body, so the worst-case path is "throw before pop" → frame is
 * freed automatically. */
mino_val *prim_push_thread_bindings_star(mino_state *S, mino_val *args,
                                         mino_env *env)
{
    mino_val    *map_arg;
    dyn_frame_t   *frame;
    dyn_binding_t *bhead = NULL;
    dyn_binding_t *b;
    size_t         i;
    (void)env;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "push-thread-bindings* requires one argument: bindings-map");
    }
    map_arg = args->as.cons.car;

    /* Build an empty frame for nil/empty bindings so a paired pop
     * still has something to remove. */
    if (map_arg != NULL && mino_type_of(map_arg) != MINO_NIL
        && mino_type_of(map_arg) != MINO_MAP) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "push-thread-bindings*: bindings argument must be a map or nil");
    }
    if (map_arg != NULL && mino_type_of(map_arg) == MINO_MAP) {
        for (i = 0; i < map_arg->as.map.len; i++) {
            mino_val *key = vec_nth(map_arg->as.map.key_order, i);
            mino_val *val = map_get_val(map_arg, key);
            if (key == NULL
                || (mino_type_of(key) != MINO_VAR
                    && mino_type_of(key) != MINO_SYMBOL
                    && mino_type_of(key) != MINO_STRING)) {
                dyn_binding_list_free(bhead);
                return prim_throw_classified(S, "eval/type", "MTY001",
                    "push-thread-bindings*: keys must be vars, symbols, or strings");
            }
            b = dyn_binding_make(S, key, val, bhead);
            if (b == NULL) {
                dyn_binding_list_free(bhead);
                return prim_throw_classified(S, "eval/contract", "MIN001",
                    "push-thread-bindings*: out of memory");
            }
            bhead = b;
        }
    }
    frame = (dyn_frame_t *)calloc(1, sizeof(*frame));
    if (frame == NULL) {
        dyn_binding_list_free(bhead);
        return prim_throw_classified(S, "eval/contract", "MIN001",
            "push-thread-bindings*: out of memory");
    }
    frame->bindings = bhead;
    frame->building = 0;
    frame->prev     = mino_current_ctx(S)->dyn_stack;
    mino_current_ctx(S)->dyn_stack = frame;
    return mino_nil(S);
}

/* (pop-thread-bindings*) — pop and free the top dynamic-binding frame.
 * Throws when the dyn-stack is empty. Returns nil. */
mino_val *prim_pop_thread_bindings_star(mino_state *S, mino_val *args,
                                        mino_env *env)
{
    dyn_frame_t *frame;
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "pop-thread-bindings* takes no arguments");
    }
    frame = mino_current_ctx(S)->dyn_stack;
    if (frame == NULL) {
        return prim_throw_classified(S, "eval/state", "MST005",
            "pop-thread-bindings*: no active binding frame");
    }
    mino_current_ctx(S)->dyn_stack = frame->prev;
    dyn_binding_list_free(frame->bindings);
    free(frame);
    return mino_nil(S);
}

/* (thread-bound? v) — true iff v is a var that has a thread-local
 * binding active on the current dyn_stack. Used by Clojure's
 * thread-bound? wrapper, which is variadic and ANDs over all args. */
mino_val *prim_thread_bound_p(mino_state *S, mino_val *args,
                               mino_env *env)
{
    mino_val    *v;
    dyn_frame_t   *f;
    dyn_binding_t *b;
    const char    *name;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "thread-bound? requires one argument");
    }
    v = args->as.cons.car;
    if (v == NULL || mino_type_of(v) != MINO_VAR) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "thread-bound?: expected a var");
    }
    name = v->as.var.sym;
    if (name == NULL) return mino_false(S);
    /* Var identity first; the text fallback covers var-less entries
     * pushed before the var existed (embedder boot order). */
    for (f = mino_current_ctx(S)->dyn_stack; f != NULL; f = f->prev) {
        if (f->building) continue;
        for (b = f->bindings; b != NULL; b = b->next) {
            if (b->var == v
                || (b->var == NULL && b->name != NULL
                    && strcmp(b->name, name) == 0)) {
                return mino_true(S);
            }
        }
    }
    return mino_false(S);
}
