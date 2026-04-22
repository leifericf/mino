/*
 * prim_stateful.c -- atom primitives and watch/validator support.
 *
 * Primitives: atom, deref, reset!, swap!, atom?, add-watch, remove-watch,
 *             set-validator!, get-validator, swap-vals!, reset-vals!.
 */

#include "prim_internal.h"

/* ---- shared helpers ---------------------------------------------------- */

/* Validate new_val against atom's validator.  Returns 0 on success,
 * -1 if the validator rejects (throws a catchable error). */
static int atom_validate(mino_state_t *S, mino_val_t *atom,
                         mino_val_t *new_val, mino_env_t *env)
{
    mino_val_t *vfn = atom->as.atom.validator;
    mino_val_t *vargs, *result;
    if (vfn == NULL) return 0;
    vargs = mino_cons(S, new_val, mino_nil(S));
    result = mino_call(S, vfn, vargs, env);
    if (result == NULL) return -1;  /* validator threw */
    if (result->type == MINO_BOOL && result->as.b == 0) {
        prim_throw_classified(S, "eval/contract", "MCT001", "Invalid reference state");
        return -1;
    }
    return 0;
}

/* Notify all watches after a state change.  Callback signature:
 * (fn key atom old-state new-state).  Exceptions in watches are ignored:
 * each callback runs inside its own try frame so throws don't escape. */
static void atom_notify_watches(mino_state_t *S, mino_val_t *atom,
                                mino_val_t *old_val, mino_val_t *new_val,
                                mino_env_t *env)
{
    mino_val_t *watches = atom->as.atom.watches;
    size_t i, len;
    if (watches == NULL || watches->type != MINO_MAP || watches->as.map.len == 0)
        return;
    len = watches->as.map.len;
    for (i = 0; i < len; i++) {
        mino_val_t *key = vec_nth(watches->as.map.key_order, i);
        mino_val_t *fn  = map_get_val(watches, key);
        mino_val_t *wargs;
        int saved_try;
        if (fn == NULL) continue;
        wargs = mino_cons(S, key,
                  mino_cons(S, atom,
                    mino_cons(S, old_val,
                      mino_cons(S, new_val, mino_nil(S)))));
        /* Wrap in a try frame so watch exceptions don't propagate. */
        saved_try = S->try_depth;
        if (S->try_depth < MAX_TRY_DEPTH) {
            S->try_stack[S->try_depth].exception = NULL;
            if (setjmp(S->try_stack[S->try_depth].buf) == 0) {
                S->try_depth++;
                (void)mino_call(S, fn, wargs, env);
                S->try_depth = saved_try;
            } else {
                /* Watch threw -- swallow and continue. */
                S->try_depth = saved_try;
            }
        }
    }
}

/* Validate, commit, and notify.  Returns 0 on success, -1 if validator
 * rejects.  On success the atom's val is set to new_val and watches fire. */
static int atom_set(mino_state_t *S, mino_val_t *atom,
                    mino_val_t *old_val, mino_val_t *new_val,
                    mino_env_t *env)
{
    if (atom_validate(S, atom, new_val, env) != 0) return -1;
    gc_write_barrier(S, atom, new_val);
    atom->as.atom.val = new_val;
    atom_notify_watches(S, atom, old_val, new_val, env);
    return 0;
}

/* Build the call-args list for swap: (cur extra1 extra2 ...). */
static mino_val_t *swap_build_args(mino_state_t *S, mino_val_t *cur,
                                   mino_val_t *extra)
{
    mino_val_t *call_args = mino_nil(S);
    if (extra != NULL && extra->type == MINO_CONS) {
        mino_val_t *tail = mino_nil(S);
        while (extra != NULL && extra->type == MINO_CONS) {
            tail = mino_cons(S, extra->as.cons.car, tail);
            extra = extra->as.cons.cdr;
        }
        call_args = mino_nil(S);
        while (tail != NULL && tail->type == MINO_CONS) {
            call_args = mino_cons(S, tail->as.cons.car, call_args);
            tail = tail->as.cons.cdr;
        }
    }
    return mino_cons(S, cur, call_args);
}

/* ---- primitives -------------------------------------------------------- */

mino_val_t *prim_atom(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "atom requires one argument");
    }
    return mino_atom(S, args->as.cons.car);
}

mino_val_t *prim_deref(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *a;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "deref requires one argument");
    }
    a = args->as.cons.car;
    if (a != NULL && a->type == MINO_ATOM) {
        return a->as.atom.val;
    }
    if (a != NULL && a->type == MINO_REDUCED) {
        return a->as.reduced.val;
    }
    return prim_throw_classified(S, "eval/type", "MTY001", "deref: expected an atom or reduced");
}

mino_val_t *prim_reset_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *a, *val, *old;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "reset! requires two arguments");
    }
    a   = args->as.cons.car;
    val = args->as.cons.cdr->as.cons.car;
    if (a == NULL || a->type != MINO_ATOM) {
        return prim_throw_classified(S, "eval/type", "MTY001", "reset!: first argument must be an atom");
    }
    old = a->as.atom.val;
    if (atom_set(S, a, old, val, env) != 0) return NULL;
    return val;
}

/* (swap! atom f & args) -- applies (f current-val args...) and sets result. */
mino_val_t *prim_swap_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *a, *fn, *cur, *call_args, *result;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "swap! requires at least 2 arguments: atom and function");
    }
    a  = args->as.cons.car;
    fn = args->as.cons.cdr->as.cons.car;
    if (a == NULL || a->type != MINO_ATOM) {
        return prim_throw_classified(S, "eval/type", "MTY001", "swap!: first argument must be an atom");
    }
    cur = a->as.atom.val;
    call_args = swap_build_args(S, cur, args->as.cons.cdr->as.cons.cdr);
    result = mino_call(S, fn, call_args, env);
    if (result == NULL) return NULL;
    if (atom_set(S, a, cur, result, env) != 0) return NULL;
    return result;
}

mino_val_t *prim_atom_p(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "atom? requires one argument");
    }
    return mino_is_atom(args->as.cons.car) ? mino_true(S) : mino_false(S);
}

/* (add-watch atom key fn) -- register a watch callback. */
mino_val_t *prim_add_watch(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t       *a, *key, *fn, *watches, *new_map;
    mino_hamt_node_t *root;
    mino_val_t       *order;
    size_t            len;
    hamt_entry_t     *e;
    uint32_t          h;
    int               replaced = 0;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || !mino_is_cons(args->as.cons.cdr->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "add-watch requires three arguments: atom key fn");
    }
    a   = args->as.cons.car;
    key = args->as.cons.cdr->as.cons.car;
    fn  = args->as.cons.cdr->as.cons.cdr->as.cons.car;
    if (a == NULL || a->type != MINO_ATOM) {
        return prim_throw_classified(S, "eval/type", "MTY001", "add-watch: first argument must be an atom");
    }
    watches = a->as.atom.watches;
    if (watches == NULL || watches->type != MINO_MAP) {
        root  = NULL;
        order = mino_vector(S, NULL, 0);
        len   = 0;
    } else {
        root  = watches->as.map.root;
        order = watches->as.map.key_order;
        len   = watches->as.map.len;
    }
    e = hamt_entry_new(S, key, fn);
    h = hash_val(key);
    root = hamt_assoc(S, root, e, h, 0u, &replaced);
    if (!replaced) {
        order = vec_conj1(S, order, key);
        len++;
    }
    new_map = alloc_val(S, MINO_MAP);
    new_map->as.map.root      = root;
    new_map->as.map.key_order = order;
    new_map->as.map.len       = len;
    gc_write_barrier(S, a, new_map);
    a->as.atom.watches = new_map;
    return a;
}

/* (remove-watch atom key) -- unregister a watch callback. */
mino_val_t *prim_remove_watch(mino_state_t *S, mino_val_t *args,
                              mino_env_t *env)
{
    mino_val_t       *a, *key, *watches;
    uint32_t          h;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "remove-watch requires two arguments: atom key");
    }
    a   = args->as.cons.car;
    key = args->as.cons.cdr->as.cons.car;
    if (a == NULL || a->type != MINO_ATOM) {
        return prim_throw_classified(S, "eval/type", "MTY001", "remove-watch: first argument must be an atom");
    }
    watches = a->as.atom.watches;
    if (watches == NULL || watches->type != MINO_MAP) return a;
    h = hash_val(key);
    if (hamt_get(watches->as.map.root, key, h, 0u) != NULL) {
        mino_val_t       *new_map = alloc_val(S, MINO_MAP);
        mino_val_t       *order   = mino_vector(S, NULL, 0);
        mino_hamt_node_t *root    = NULL;
        size_t            new_len = 0;
        size_t            i;
        for (i = 0; i < watches->as.map.len; i++) {
            mino_val_t *k = vec_nth(watches->as.map.key_order, i);
            if (!mino_eq(k, key)) {
                mino_val_t   *v  = map_get_val(watches, k);
                hamt_entry_t *e2 = hamt_entry_new(S, k, v);
                uint32_t      h2 = hash_val(k);
                int rep = 0;
                root = hamt_assoc(S, root, e2, h2, 0u, &rep);
                order = vec_conj1(S, order, k);
                new_len++;
            }
        }
        new_map->as.map.root      = root;
        new_map->as.map.key_order = order;
        new_map->as.map.len       = new_len;
        gc_write_barrier(S, a, new_map);
        a->as.atom.watches = new_map;
    }
    return a;
}

/* (set-validator! atom fn) -- set or remove a validator on an atom. */
mino_val_t *prim_set_validator(mino_state_t *S, mino_val_t *args,
                               mino_env_t *env)
{
    mino_val_t *a, *fn;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "set-validator! requires two arguments: atom fn");
    }
    a  = args->as.cons.car;
    fn = args->as.cons.cdr->as.cons.car;
    if (a == NULL || a->type != MINO_ATOM) {
        return prim_throw_classified(S, "eval/type", "MTY001", "set-validator!: first argument must be an atom");
    }
    /* nil removes the validator. */
    if (fn == NULL || fn->type == MINO_NIL) {
        gc_write_barrier(S, a, NULL);
        a->as.atom.validator = NULL;
        return mino_nil(S);
    }
    /* Validate current value with the new validator before installing.
     * Cannot use atom_validate here because prim_throw_error longjmps
     * and would skip the revert of the validator field. */
    {
        mino_val_t *vargs  = mino_cons(S, a->as.atom.val, mino_nil(S));
        mino_val_t *result = mino_call(S, fn, vargs, env);
        if (result == NULL) return NULL;  /* validator threw */
        if (result->type == MINO_BOOL && result->as.b == 0) {
            prim_throw_classified(S, "eval/contract", "MCT001", "Invalid reference state");
            return NULL;
        }
    }
    gc_write_barrier(S, a, fn);
    a->as.atom.validator = fn;
    return mino_nil(S);
}

/* (get-validator atom) -- return the current validator fn or nil. */
mino_val_t *prim_get_validator(mino_state_t *S, mino_val_t *args,
                               mino_env_t *env)
{
    mino_val_t *a;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "get-validator requires one argument");
    }
    a = args->as.cons.car;
    if (a == NULL || a->type != MINO_ATOM) {
        return prim_throw_classified(S, "eval/type", "MTY001", "get-validator: argument must be an atom");
    }
    return a->as.atom.validator != NULL ? a->as.atom.validator : mino_nil(S);
}

/* (reset-vals! atom val) -- like reset! but returns [old new]. */
mino_val_t *prim_reset_vals(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *a, *val, *old;
    mino_val_t *pair[2];
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "reset-vals! requires two arguments");
    }
    a   = args->as.cons.car;
    val = args->as.cons.cdr->as.cons.car;
    if (a == NULL || a->type != MINO_ATOM) {
        return prim_throw_classified(S, "eval/type", "MTY001", "reset-vals!: first argument must be an atom");
    }
    old = a->as.atom.val;
    if (atom_set(S, a, old, val, env) != 0) return NULL;
    pair[0] = old;
    pair[1] = val;
    return mino_vector(S, pair, 2);
}

/* (swap-vals! atom f & args) -- like swap! but returns [old new]. */
mino_val_t *prim_swap_vals(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *a, *fn, *cur, *call_args, *result;
    mino_val_t *pair[2];
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "swap-vals! requires at least 2 arguments: atom and function");
    }
    a  = args->as.cons.car;
    fn = args->as.cons.cdr->as.cons.car;
    if (a == NULL || a->type != MINO_ATOM) {
        return prim_throw_classified(S, "eval/type", "MTY001", "swap-vals!: first argument must be an atom");
    }
    cur = a->as.atom.val;
    call_args = swap_build_args(S, cur, args->as.cons.cdr->as.cons.cdr);
    result = mino_call(S, fn, call_args, env);
    if (result == NULL) return NULL;
    if (atom_set(S, a, cur, result, env) != 0) return NULL;
    pair[0] = cur;
    pair[1] = result;
    return mino_vector(S, pair, 2);
}

/* Fault injection: make the n-th GC allocation fail (simulated OOM).
 * Testing only. Pass 0 to disable. */
mino_val_t *prim_set_fail_alloc_at(mino_state_t *S, mino_val_t *args,
                                    mino_env_t *env)
{
    mino_val_t *n;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "set-fail-alloc-at! requires 1 argument");
    }
    n = args->as.cons.car;
    if (n == NULL || n->type != MINO_INT) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "set-fail-alloc-at!: argument must be an integer");
    }
    mino_set_fail_alloc_at(S, (long)n->as.i);
    return mino_nil(S);
}
