/*
 * prim_stateful.c -- atom primitives and watch/validator support.
 *
 * Primitives: atom, deref, reset!, swap!, atom?, add-watch, remove-watch,
 *             set-validator!, get-validator, swap-vals!, reset-vals!.
 */

#include "prim/internal.h"
#include "runtime/host_threads.h"

/* mino_tx_ref_deref is declared in mino.h; pulled in via prim/internal.h.
 * deref of a ref needs to consult the active transaction to return the
 * in-tx tentative value if any, otherwise the committed value. */

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
    if (!mino_is_truthy(result)) {
        prim_throw_classified(S, "eval/contract", "MCT001", "Invalid reference state");
        return -1;
    }
    return 0;
}

/* Notify all watches after a state change.  Callback signature:
 * (fn key atom old-state new-state).  Returns -1 if any watch threw
 * (the exception propagates per Clojure JVM semantics), 0 otherwise. */
static int atom_notify_watches(mino_state_t *S, mino_val_t *atom,
                               mino_val_t *old_val, mino_val_t *new_val,
                               mino_env_t *env)
{
    mino_val_t *watches = atom->as.atom.watches;
    size_t i, len;
    if (watches == NULL || mino_type_of(watches) != MINO_MAP || watches->as.map.len == 0)
        return 0;
    len = watches->as.map.len;
    for (i = 0; i < len; i++) {
        mino_val_t *key = vec_nth(watches->as.map.key_order, i);
        mino_val_t *fn  = map_get_val(watches, key);
        mino_val_t *wargs;
        if (fn == NULL) continue;
        wargs = mino_cons(S, key,
                  mino_cons(S, atom,
                    mino_cons(S, old_val,
                      mino_cons(S, new_val, mino_nil(S)))));
        if (mino_call(S, fn, wargs, env) == NULL) return -1;
    }
    return 0;
}

/* Validate, commit, and notify.  Returns 0 on success, -1 if validator
 * rejects.  On success the atom's val is set to new_val and watches fire. */
static int atom_set(mino_state_t *S, mino_val_t *atom,
                    mino_val_t *old_val, mino_val_t *new_val,
                    mino_env_t *env)
{
    if (atom_validate(S, atom, new_val, env) != 0) return -1;
    gc_write_barrier(S, atom, atom->as.atom.val, new_val);
    atom->as.atom.val = new_val;
    return atom_notify_watches(S, atom, old_val, new_val, env);
}

/* Atomic load of an atom's current value.  In single-threaded mode this
 * is a plain pointer read; once S->multi_threaded flips the read goes
 * through __atomic_load_n with acquire ordering so swap! sees a
 * coherent snapshot of writes from other workers. */
static mino_val_t *atom_load(mino_state_t *S, mino_val_t *atom)
{
    if (S->multi_threaded) {
        return __atomic_load_n(&atom->as.atom.val, __ATOMIC_ACQUIRE);
    }
    return atom->as.atom.val;
}

/* Compare-and-exchange the atom's value pointer.  Returns 1 on success
 * (current value matched expected and was replaced with new_val), 0 on
 * failure.  In single-threaded mode this is a pointer-compare + plain
 * write; in multi-threaded mode it goes through __atomic_compare_exchange_n
 * with release/relaxed ordering so a successful winner publishes new_val
 * before any other worker can observe it. The GC write barrier and watch
 * notification are the caller's responsibility — atom_cas is the raw
 * publish step only. */
static int atom_cas_ptr(mino_state_t *S, mino_val_t *atom,
                        mino_val_t *expected, mino_val_t *new_val)
{
    if (S->multi_threaded) {
        return __atomic_compare_exchange_n(&atom->as.atom.val,
                                           &expected, new_val,
                                           0,  /* not weak */
                                           __ATOMIC_RELEASE,
                                           __ATOMIC_RELAXED) ? 1 : 0;
    }
    if (atom->as.atom.val != expected) return 0;
    atom->as.atom.val = new_val;
    return 1;
}

/* Build the call-args list for swap: (cur extra1 extra2 ...). */
static mino_val_t *swap_build_args(mino_state_t *S, mino_val_t *cur,
                                   mino_val_t *extra)
{
    mino_val_t *call_args = mino_nil(S);
    if (extra != NULL && mino_type_of(extra) == MINO_CONS) {
        mino_val_t *tail = mino_nil(S);
        while (extra != NULL && mino_type_of(extra) == MINO_CONS) {
            tail = mino_cons(S, extra->as.cons.car, tail);
            extra = extra->as.cons.cdr;
        }
        call_args = mino_nil(S);
        while (tail != NULL && mino_type_of(tail) == MINO_CONS) {
            call_args = mino_cons(S, tail->as.cons.car, call_args);
            tail = tail->as.cons.cdr;
        }
    }
    return mino_cons(S, cur, call_args);
}

/* ---- primitives -------------------------------------------------------- */

mino_val_t *prim_atom(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    /* (atom x) or (atom x & {:keys [:meta :validator]}) -- options
     * may be supplied as flat keyword args (:meta m :validator f) or
     * via apply'd nil pairs. Unknown keys are tolerated to match
     * Clojure (it accepts any keys, only :meta and :validator have
     * effect). */
    mino_val_t *initial;
    mino_val_t *meta      = NULL;
    mino_val_t *validator = NULL;
    mino_val_t *atom;
    mino_val_t *rest;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "atom requires one argument");
    }
    initial = args->as.cons.car;
    rest    = args->as.cons.cdr;
    while (mino_is_cons(rest)) {
        mino_val_t *k;
        mino_val_t *v;
        k = rest->as.cons.car;
        if (!mino_is_cons(rest->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001",
                "atom: option key requires a value");
        }
        v    = rest->as.cons.cdr->as.cons.car;
        rest = rest->as.cons.cdr->as.cons.cdr;
        if (k != NULL && mino_type_of(k) == MINO_KEYWORD) {
            if (strcmp(k->as.s.data, "meta") == 0) {
                if (v != NULL && mino_type_of(v) != MINO_NIL
                    && mino_type_of(v) != MINO_MAP
                    && mino_type_of(v) != MINO_SORTED_MAP) {
                    return prim_throw_classified(S, "eval/type", "MTY001",
                        "atom: :meta value must be a map or nil");
                }
                meta = (v != NULL
                        && (mino_type_of(v) == MINO_MAP || mino_type_of(v) == MINO_SORTED_MAP))
                       ? v : NULL;
            } else if (strcmp(k->as.s.data, "validator") == 0) {
                if (v != NULL && mino_type_of(v) != MINO_NIL) {
                    validator = v;
                }
            }
            /* Unknown keys silently ignored; Clojure does the same. */
        }
    }
    atom = mino_atom(S, initial);
    if (atom == NULL) return NULL;
    if (validator != NULL) {
        /* Run the validator against the initial value before installing
         * it so a bad initial value is rejected at construction time --
         * mirrors Clojure's contract for atom + :validator. */
        mino_val_t *vargs  = mino_cons(S, initial, mino_nil(S));
        mino_val_t *result = mino_call(S, validator, vargs, env);
        if (result == NULL) return NULL;
        if (!mino_is_truthy(result)) {
            return prim_throw_classified(S, "eval/contract", "MCT001",
                "Invalid reference state");
        }
        gc_write_barrier(S, atom, atom->as.atom.validator, validator);
        atom->as.atom.validator = validator;
    }
    if (meta != NULL) {
        atom->meta = meta;
    }
    return atom;
}

mino_val_t *prim_deref(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *a;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "deref requires one argument");
    }
    a = args->as.cons.car;
    if (a != NULL && mino_type_of(a) == MINO_ATOM) {
        return a->as.atom.val;
    }
    if (a != NULL && mino_type_of(a) == MINO_VOLATILE) {
        return a->as.volatile_.val;
    }
    if (a != NULL && mino_type_of(a) == MINO_REDUCED) {
        return a->as.reduced.val;
    }
    if (a != NULL && mino_type_of(a) == MINO_VAR) {
        if (!a->as.var.bound) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "deref: var is unbound");
        }
        return a->as.var.root != NULL ? a->as.var.root : mino_nil(S);
    }
    if (a != NULL && mino_type_of(a) == MINO_FUTURE) {
        return mino_future_deref(S, a);
    }
    if (a != NULL && mino_type_of(a) == MINO_TX_REF) {
        return mino_tx_ref_deref(S, a);
    }
    if (a != NULL && mino_type_of(a) == MINO_AGENT) {
        return a->as.agent.val;
    }
    return prim_throw_classified(S, "eval/type", "MTY001", "deref: expected an atom, volatile, var, future, ref, or reduced");
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
    if (a == NULL || mino_type_of(a) != MINO_ATOM) {
        return prim_throw_classified(S, "eval/type", "MTY001", "reset!: first argument must be an atom");
    }
    old = a->as.atom.val;
    if (atom_set(S, a, old, val, env) != 0) return NULL;
    return val;
}

/* (swap! atom f & args) -- applies (f current-val args...) and sets result.
 *
 * Single-threaded path is a straight read-compute-publish.  Multi-threaded
 * path runs the canonical retry loop: load, compute, CAS, retry on loss.
 * The retry path lights up once S->multi_threaded flips after host
 * threads enter the picture. */
mino_val_t *prim_swap_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *a, *fn, *cur, *call_args, *result, *extra;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "swap! requires at least 2 arguments: atom and function");
    }
    a  = args->as.cons.car;
    fn = args->as.cons.cdr->as.cons.car;
    if (a == NULL || mino_type_of(a) != MINO_ATOM) {
        return prim_throw_classified(S, "eval/type", "MTY001", "swap!: first argument must be an atom");
    }
    extra = args->as.cons.cdr->as.cons.cdr;
    if (!S->multi_threaded) {
        cur = a->as.atom.val;
        call_args = swap_build_args(S, cur, extra);
        result = mino_call(S, fn, call_args, env);
        if (result == NULL) return NULL;
        if (atom_set(S, a, cur, result, env) != 0) return NULL;
        return result;
    }
    /* Multi-threaded retry loop. */
    for (;;) {
        cur = atom_load(S, a);
        call_args = swap_build_args(S, cur, extra);
        result = mino_call(S, fn, call_args, env);
        if (result == NULL) return NULL;
        if (atom_validate(S, a, result, env) != 0) return NULL;
        gc_write_barrier(S, a, cur, result);
        if (atom_cas_ptr(S, a, cur, result)) {
            if (atom_notify_watches(S, a, cur, result, env) != 0) return NULL;
            return result;
        }
        /* Lost the race; another worker won.  Try again with the new
         * current value.  Watches do not fire for losers; the winner
         * publishes the user-visible state change. */
    }
}

/* (compare-and-set! atom expected new-val) -- single CAS attempt.
 *
 * Compares the current value with `expected` by pointer identity (matching
 * canon Clojure semantics for atom CAS, which uses .equals on the JVM but
 * pointer-eq is the ground truth here).  On match, the value is replaced
 * with new-val and watches fire; otherwise the atom is left alone and
 * false is returned.  Single-threaded mode short-circuits via plain read
 * + write; multi-threaded mode goes through __atomic_compare_exchange_n.
 *
 * Note: Clojure's compare-and-set! actually uses identical? semantics
 * (pointer eq) for the comparison, not value-equality, because that is
 * what a CAS instruction can express.  Older mino used mino_eq here,
 * which is value-equality and surprised users hitting the multi-threaded
 * path; aligning with canon both fixes that and matches what the CAS
 * primitive can deliver. */
mino_val_t *prim_compare_and_set_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *a, *expected, *new_val;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || !mino_is_cons(args->as.cons.cdr->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "compare-and-set! requires three arguments: atom, expected, new-val");
    }
    a        = args->as.cons.car;
    expected = args->as.cons.cdr->as.cons.car;
    new_val  = args->as.cons.cdr->as.cons.cdr->as.cons.car;
    if (a == NULL || mino_type_of(a) != MINO_ATOM) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "compare-and-set!: first argument must be an atom");
    }
    if (atom_validate(S, a, new_val, env) != 0) return NULL;
    gc_write_barrier(S, a, expected, new_val);
    if (!atom_cas_ptr(S, a, expected, new_val)) {
        return mino_false(S);
    }
    if (atom_notify_watches(S, a, expected, new_val, env) != 0) return NULL;
    return mino_true(S);
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
/* Accessor helpers so the watch / validator primitives can target
 * either MINO_ATOM or MINO_TX_REF without duplicating per-field
 * dispatch through every code path. Returns 0 for invalid input. */
static int watchable_get(mino_val_t *v, mino_val_t ***out_watches,
                          mino_val_t ***out_validator)
{
    if (v == NULL) return 0;
    if (mino_type_of(v) == MINO_ATOM) {
        *out_watches   = &v->as.atom.watches;
        *out_validator = &v->as.atom.validator;
        return 1;
    }
    if (mino_type_of(v) == MINO_TX_REF) {
        *out_watches   = &v->as.tx_ref.watches;
        *out_validator = &v->as.tx_ref.validator;
        return 1;
    }
    if (mino_type_of(v) == MINO_VAR) {
        *out_watches   = &v->as.var.watches;
        *out_validator = &v->as.var.validator;
        return 1;
    }
    if (mino_type_of(v) == MINO_AGENT) {
        *out_watches   = &v->as.agent.watches;
        *out_validator = &v->as.agent.validator;
        return 1;
    }
    return 0;
}

/* For watchable types whose state-of-allocation is tracked
 * (refs and agents), throw MST007 if v belongs to a different state.
 * Atoms and vars don't carry an owning_state slot today; for those
 * types this returns 0 without checking. Returns 0 on success
 * (host owns the value, or no check applies), 1 on mismatch
 * (caller must propagate NULL after the throw). */
static int watchable_check_state(mino_state_t *S, mino_val_t *v)
{
    mino_state_t *owner = NULL;
    if (v == NULL) return 0;
    if (mino_type_of(v) == MINO_TX_REF) owner = v->as.tx_ref.owning_state;
    else if (mino_type_of(v) == MINO_AGENT) owner = v->as.agent.owning_state;
    if (owner != NULL && owner != S) {
        prim_throw_classified(S, "eval/state", "MST007",
            "reference from foreign state");
        return 1;
    }
    return 0;
}

mino_val_t *prim_add_watch(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t       *a, *key, *fn, *watches, *new_map;
    mino_val_t      **watches_slot;
    mino_val_t      **validator_slot;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || !mino_is_cons(args->as.cons.cdr->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "add-watch requires three arguments: reference key fn");
    }
    a   = args->as.cons.car;
    key = args->as.cons.cdr->as.cons.car;
    fn  = args->as.cons.cdr->as.cons.cdr->as.cons.car;
    if (!watchable_get(a, &watches_slot, &validator_slot)) {
        return prim_throw_classified(S, "eval/type", "MTY001", "add-watch: first argument must be an atom or ref");
    }
    if (watchable_check_state(S, a)) return NULL;
    /* The watch fn is invoked as (fn key ref old-value new-value) on
     * every committed state change. Storing a non-callable here just
     * defers the failure to the dispatch site, which is far from the
     * install. Reject at install. */
    if (fn == NULL || (mino_type_of(fn) != MINO_FN
                        && mino_type_of(fn) != MINO_PRIM
                        && mino_type_of(fn) != MINO_MACRO)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "add-watch: watch fn must be a fn");
    }
    watches = *watches_slot;
    if (watches == NULL || mino_type_of(watches) != MINO_MAP) {
        mino_val_t **noargs = NULL;
        watches = mino_map(S, noargs, noargs, 0);
        if (watches == NULL) return NULL;
    }
    new_map = mino_map_assoc1(S, watches, key, fn);
    if (new_map == NULL) return NULL;
    gc_write_barrier(S, a, *watches_slot, new_map);
    *watches_slot = new_map;
    return a;
}

/* (remove-watch ref key) -- unregister a watch callback. */
mino_val_t *prim_remove_watch(mino_state_t *S, mino_val_t *args,
                              mino_env_t *env)
{
    mino_val_t  *a, *key, *watches;
    mino_val_t **watches_slot;
    mino_val_t **validator_slot;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "remove-watch requires two arguments: reference key");
    }
    a   = args->as.cons.car;
    key = args->as.cons.cdr->as.cons.car;
    if (!watchable_get(a, &watches_slot, &validator_slot)) {
        return prim_throw_classified(S, "eval/type", "MTY001", "remove-watch: first argument must be an atom or ref");
    }
    if (watchable_check_state(S, a)) return NULL;
    watches = *watches_slot;
    if (watches == NULL || mino_type_of(watches) != MINO_MAP) return a;
    if (mino_map_lookup(watches, key) != NULL) {
        mino_val_t *new_map = mino_map_dissoc1(S, watches, key);
        if (new_map == NULL) return NULL;
        gc_write_barrier(S, a, *watches_slot, new_map);
        *watches_slot = new_map;
    }
    return a;
}

/* (set-validator! ref fn) -- set or remove a validator on an atom or ref. */
mino_val_t *prim_set_validator(mino_state_t *S, mino_val_t *args,
                               mino_env_t *env)
{
    mino_val_t  *a, *fn;
    mino_val_t **watches_slot;
    mino_val_t **validator_slot;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "set-validator! requires two arguments: reference fn");
    }
    a  = args->as.cons.car;
    fn = args->as.cons.cdr->as.cons.car;
    if (!watchable_get(a, &watches_slot, &validator_slot)) {
        return prim_throw_classified(S, "eval/type", "MTY001", "set-validator!: first argument must be an atom or ref");
    }
    if (watchable_check_state(S, a)) return NULL;
    /* nil removes the validator. */
    if (fn == NULL || mino_type_of(fn) == MINO_NIL) {
        gc_write_barrier(S, a, *validator_slot, NULL);
        *validator_slot = NULL;
        return mino_nil(S);
    }
    /* The validator is invoked as (fn new-value) on every state
     * transition. Storing a non-callable just defers the failure to
     * the next mutation, far from this install site -- reject loudly
     * here so user typos surface immediately. */
    if (mino_type_of(fn) != MINO_FN
        && mino_type_of(fn) != MINO_PRIM
        && mino_type_of(fn) != MINO_MACRO) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "set-validator!: validator must be a fn or nil");
    }
    /* JVM Clojure does not validate the current value at install time; only
     * subsequent state transitions are checked. Match canon: install fn
     * unconditionally, even if the current value would fail it. */
    gc_write_barrier(S, a, *validator_slot, fn);
    *validator_slot = fn;
    return mino_nil(S);
}

/* (get-validator ref) -- return the current validator fn or nil. */
mino_val_t *prim_get_validator(mino_state_t *S, mino_val_t *args,
                               mino_env_t *env)
{
    mino_val_t  *a;
    mino_val_t **watches_slot;
    mino_val_t **validator_slot;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "get-validator requires one argument");
    }
    a = args->as.cons.car;
    if (!watchable_get(a, &watches_slot, &validator_slot)) {
        return prim_throw_classified(S, "eval/type", "MTY001", "get-validator: argument must be an atom or ref");
    }
    if (watchable_check_state(S, a)) return NULL;
    return *validator_slot != NULL ? *validator_slot : mino_nil(S);
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
    if (a == NULL || mino_type_of(a) != MINO_ATOM) {
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
    if (a == NULL || mino_type_of(a) != MINO_ATOM) {
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

/* Snapshot the current thread's dyn_stack into a map keyed by binding-
 * name symbols. Inner frames shadow outer frames; the first occurrence
 * walking newest-first wins. Returns nil when the stack is empty. */
mino_val_t *mino_snapshot_thread_bindings(mino_state_t *S)
{
    dyn_frame_t   *f;
    dyn_binding_t *b;
    size_t         cap = 0, len = 0;
    mino_val_t   **keys = NULL;
    mino_val_t   **vals = NULL;
    if (mino_current_ctx(S)->dyn_stack == NULL) return mino_nil(S);

    for (f = mino_current_ctx(S)->dyn_stack; f != NULL; f = f->prev) {
        for (b = f->bindings; b != NULL; b = b->next) {
            size_t i;
            int    dup = 0;
            for (i = 0; i < len; i++) {
                if (strcmp(keys[i]->as.s.data, b->name) == 0) {
                    dup = 1;
                    break;
                }
            }
            if (dup) continue;
            if (len == cap) {
                size_t       new_cap = cap == 0 ? 8 : cap * 2;
                mino_val_t **nk      = (mino_val_t **)gc_alloc_typed(
                    S, GC_T_VALARR, new_cap * sizeof(*nk));
                mino_val_t **nv      = (mino_val_t **)gc_alloc_typed(
                    S, GC_T_VALARR, new_cap * sizeof(*nv));
                size_t       j;
                for (j = 0; j < len; j++) {
                    gc_valarr_set(S, nk, j, keys[j]);
                    gc_valarr_set(S, nv, j, vals[j]);
                }
                keys = nk;
                vals = nv;
                cap  = new_cap;
            }
            gc_valarr_set(S, keys, len, mino_symbol(S, b->name));
            gc_valarr_set(S, vals, len, b->val);
            len++;
        }
    }
    if (len == 0) return mino_nil(S);
    return mino_map(S, keys, vals, len);
}

/* (get-thread-bindings) -- snapshot the current dyn_stack into a map
 * suitable as input to with-bindings*. */
mino_val_t *prim_get_thread_bindings(mino_state_t *S, mino_val_t *args,
                                     mino_env_t *env)
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
mino_val_t *prim_set_dyn_binding(mino_state_t *S, mino_val_t *args,
                                 mino_env_t *env)
{
    mino_val_t    *name_sym;
    mino_val_t    *new_val;
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
    for (f = mino_current_ctx(S)->dyn_stack; f != NULL; f = f->prev) {
        for (b = f->bindings; b != NULL; b = b->next) {
            if (strcmp(b->name, name) == 0) {
                b->val = new_val;
                return new_val;
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
mino_val_t *prim_with_bindings_star(mino_state_t *S, mino_val_t *args,
                                    mino_env_t *env)
{
    mino_val_t    *map_arg;
    mino_val_t    *fn;
    mino_val_t    *result;
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
        mino_val_t *key = vec_nth(map_arg->as.map.key_order, i);
        mino_val_t *val = map_get_val(map_arg, key);
        const char *name_str;
        if (key == NULL
            || (mino_type_of(key) != MINO_SYMBOL && mino_type_of(key) != MINO_STRING)) {
            dyn_binding_list_free(bhead);
            return prim_throw_classified(S, "eval/type", "MTY001",
                "with-bindings*: keys must be symbols or strings");
        }
        /* Intern the name string through mino_symbol so it shares
         * storage with the binding-form path at bindings.c. */
        name_str = mino_symbol(S, key->as.s.data)->as.s.data;
        b = (dyn_binding_t *)malloc(sizeof(*b));
        if (b == NULL) {
            dyn_binding_list_free(bhead);
            return prim_throw_classified(S, "eval/contract", "MIN001",
                "with-bindings*: out of memory");
        }
        b->name = name_str;
        b->val  = val;
        b->next = bhead;
        bhead   = b;
    }

    frame = (dyn_frame_t *)malloc(sizeof(*frame));
    if (frame == NULL) {
        dyn_binding_list_free(bhead);
        return prim_throw_classified(S, "eval/contract", "MIN001",
            "with-bindings*: out of memory");
    }
    frame->bindings = bhead;
    frame->prev     = mino_current_ctx(S)->dyn_stack;
    mino_current_ctx(S)->dyn_stack    = frame;
    result          = mino_call(S, fn, mino_nil(S), env);
    mino_current_ctx(S)->dyn_stack    = frame->prev;
    dyn_binding_list_free(bhead);
    free(frame);
    return result;
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
    if (n == NULL || !mino_val_int_p(n)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "set-fail-alloc-at!: argument must be an integer");
    }
    mino_set_fail_alloc_at(S, (long)mino_val_int_get(n));
    return mino_nil(S);
}

/* (mino-thread-limit) — return the host-granted thread limit for this
 * state. Default is 1 (single-threaded); embedders raise this via
 * mino_set_thread_limit and standalone main.c grants cpu_count after
 * mino_install_all. The script-side future/promise/thread stubs in
 * core.clj consult this to distinguish "host has not granted threads"
 * from "host granted, runtime impl in flight." */
mino_val_t *prim_mino_thread_limit(mino_state_t *S, mino_val_t *args,
                                   mino_env_t *env)
{
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "mino-thread-limit takes no arguments");
    }
    return mino_int(S, mino_get_thread_limit(S));
}

/* (mino-thread-count) — return the live worker count for this state. */
mino_val_t *prim_mino_thread_count(mino_state_t *S, mino_val_t *args,
                                   mino_env_t *env)
{
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "mino-thread-count takes no arguments");
    }
    return mino_int(S, mino_thread_count(S));
}

/* ------------------------------------------------------------------------- */
/* Host-thread futures                                                       */
/* ------------------------------------------------------------------------- */

/* (future-call thunk) — spawn a worker thread that evaluates (thunk)
 * and returns a future. */
static mino_val_t *prim_future_call(mino_state_t *S, mino_val_t *args,
                                    mino_env_t *env)
{
    mino_val_t *thunk;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "future-call expects exactly one argument");
    }
    thunk = args->as.cons.car;
    if (thunk == NULL || (mino_type_of(thunk) != MINO_FN
                          && mino_type_of(thunk) != MINO_PRIM
                          && mino_type_of(thunk) != MINO_MACRO)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "future-call expects a function");
    }
    return mino_future_spawn(S, thunk, env);
}

/* (promise) — return a fresh promise. */
static mino_val_t *prim_promise(mino_state_t *S, mino_val_t *args,
                                mino_env_t *env)
{
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "promise takes no arguments");
    }
    return mino_promise_new(S);
}

/* (deliver promise value) — deliver value to the promise. Returns the
 * promise on success, nil if the promise was already realized. */
static mino_val_t *prim_deliver(mino_state_t *S, mino_val_t *args,
                                mino_env_t *env)
{
    mino_val_t *promise, *value;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "deliver expects two arguments: promise value");
    }
    promise = args->as.cons.car;
    value   = args->as.cons.cdr->as.cons.car;
    if (promise == NULL || mino_type_of(promise) != MINO_FUTURE) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "deliver expects a promise");
    }
    return mino_promise_deliver(S, promise, value) ? promise : mino_nil(S);
}

/* (future-cancel f) — cancel a future. Returns true if newly cancelled. */
static mino_val_t *prim_future_cancel(mino_state_t *S, mino_val_t *args,
                                      mino_env_t *env)
{
    mino_val_t *fut;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "future-cancel expects one argument");
    }
    fut = args->as.cons.car;
    return mino_future_cancel(S, fut) ? mino_true(S) : mino_false(S);
}

/* (future-done? f) — true if the future has reached a terminal state. */
static mino_val_t *prim_future_done_q(mino_state_t *S, mino_val_t *args,
                                      mino_env_t *env)
{
    mino_val_t *fut;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "future-done? expects one argument");
    }
    fut = args->as.cons.car;
    return mino_future_done_p(fut) ? mino_true(S) : mino_false(S);
}

/* (future-cancelled? f) — true if the future was cancelled. */
static mino_val_t *prim_future_cancelled_q(mino_state_t *S, mino_val_t *args,
                                           mino_env_t *env)
{
    mino_val_t *fut;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "future-cancelled? expects one argument");
    }
    fut = args->as.cons.car;
    return mino_future_cancelled_p(fut) ? mino_true(S) : mino_false(S);
}

/* (future? x) — true if x is a future or promise. */
static mino_val_t *prim_future_q(mino_state_t *S, mino_val_t *args,
                                 mino_env_t *env)
{
    mino_val_t *x;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "future? expects one argument");
    }
    x = args->as.cons.car;
    return (x != NULL && mino_type_of(x) == MINO_FUTURE) ? mino_true(S) : mino_false(S);
}

/* (future-deref f) — block until the future is realized; return result
 * (or rethrow exception, or throw :mino/cancelled). The user-facing
 * `deref` in core.clj routes futures to this primitive. */
static mino_val_t *prim_future_deref(mino_state_t *S, mino_val_t *args,
                                     mino_env_t *env)
{
    mino_val_t *fut;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "future-deref expects one argument");
    }
    fut = args->as.cons.car;
    if (fut == NULL || mino_type_of(fut) != MINO_FUTURE) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "future-deref expects a future");
    }
    return mino_future_deref(S, fut);
}

/* ---- volatile primitives ----------------------------------------------- */

mino_val_t *prim_volatile_bang(mino_state_t *S, mino_val_t *args,
                               mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "volatile! requires one argument");
    }
    return mino_volatile(S, args->as.cons.car);
}

mino_val_t *prim_volatile_p(mino_state_t *S, mino_val_t *args,
                            mino_env_t *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "volatile? requires one argument");
    }
    return mino_is_volatile(args->as.cons.car) ? mino_true(S) : mino_false(S);
}

mino_val_t *prim_vreset_bang(mino_state_t *S, mino_val_t *args,
                             mino_env_t *env)
{
    mino_val_t *v, *val;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "vreset! requires two arguments");
    }
    v   = args->as.cons.car;
    val = args->as.cons.cdr->as.cons.car;
    if (v == NULL || mino_type_of(v) != MINO_VOLATILE) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "vreset!: first argument must be a volatile");
    }
    gc_write_barrier(S, v, v->as.volatile_.val, val);
    v->as.volatile_.val = val;
    return val;
}

mino_val_t *prim_vswap_bang(mino_state_t *S, mino_val_t *args,
                            mino_env_t *env)
{
    mino_val_t *v, *fn, *cur, *call_args, *result, *extra;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "vswap! requires at least 2 arguments: volatile and function");
    }
    v  = args->as.cons.car;
    fn = args->as.cons.cdr->as.cons.car;
    if (v == NULL || mino_type_of(v) != MINO_VOLATILE) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "vswap!: first argument must be a volatile");
    }
    extra     = args->as.cons.cdr->as.cons.cdr;
    cur       = v->as.volatile_.val;
    call_args = swap_build_args(S, cur, extra);
    result    = mino_call(S, fn, call_args, env);
    if (result == NULL) return NULL;
    gc_write_barrier(S, v, cur, result);
    v->as.volatile_.val = result;
    return result;
}

const mino_prim_def k_prims_stateful[] = {
    {"atom",           prim_atom,
     "Creates an atom with the given initial value."},
    {"deref",          prim_deref,
     "Returns the current value of a reference (atom, delay, etc.)."},
    {"reset!",         prim_reset_bang,
     "Sets the value of an atom to newval and returns newval."},
    {"swap!",          prim_swap_bang,
     "Atomically applies f to the current value of the atom and any additional args."},
    {"compare-and-set!", prim_compare_and_set_bang,
     "Atomically sets the atom to new-val if its current value equals expected. Returns true on swap, false otherwise."},
    {"atom?",          prim_atom_p,
     "Returns true if x is an atom."},
    {"volatile!",      prim_volatile_bang,
     "Creates a volatile cell with the given initial value. A volatile is a single-slot mutable reference with no watches, validators, or atomic publish — intended for transducer state where the reducing fn already implies single-thread access."},
    {"volatile?",      prim_volatile_p,
     "Returns true if x is a volatile."},
    {"vreset!",        prim_vreset_bang,
     "Sets the value of a volatile to newval and returns newval. No watches, no validators, no atomicity."},
    {"vswap!",         prim_vswap_bang,
     "Applies f to the current value of the volatile and any args, sets the result, and returns it. No retry loop — single-thread only."},
    {"add-watch",      prim_add_watch,
     "Adds a watch function to an atom, called on state changes."},
    {"remove-watch",   prim_remove_watch,
     "Removes a watch function from an atom by key."},
    {"set-validator!", prim_set_validator,
     "Sets a validator function on an atom."},
    {"get-validator",  prim_get_validator,
     "Returns the validator function of an atom, or nil."},
    {"reset-vals!",    prim_reset_vals,
     "Sets the value of an atom and returns [old new]."},
    {"swap-vals!",     prim_swap_vals,
     "Atomically applies f to the atom and returns [old new]."},
    {"get-thread-bindings", prim_get_thread_bindings,
     "Returns a map of symbol->value for the active dynamic bindings, or nil if no binding frames are active."},
    {"with-bindings*", prim_with_bindings_star,
     "(with-bindings* bindings-map fn) — pushes the bindings as a dynamic frame and invokes fn with no args."},
    {"set-dyn-binding!", prim_set_dyn_binding,
     "(set-dyn-binding! 'name value) — mutate the topmost active dynamic binding for `name`. Returns the value. Throws when no binding frame is active for `name`. Backs (set! *var* expr)."},
    {"set-fail-alloc-at!", prim_set_fail_alloc_at,
     "Make the n-th GC allocation fail (simulated OOM). Pass 0 to disable."},
    {"mino-thread-limit", prim_mino_thread_limit,
     "Return the host-granted thread limit for this state. 1 means single-threaded; >1 means the host has granted that many concurrent worker threads."},
    {"mino-thread-count", prim_mino_thread_count,
     "Return the live host-thread count for this state."},
    {"future-call",         prim_future_call,
     "Spawn a worker thread to evaluate the given thunk; return a future."},
    {"promise",             prim_promise,
     "Return a fresh promise that can be deliver'd a value once."},
    {"deliver",             prim_deliver,
     "Deliver a value to a promise. Returns the promise on success, nil if already realized."},
    {"future-cancel",       prim_future_cancel,
     "Cancel a pending future. Returns true if the future was newly cancelled."},
    {"future-done?",        prim_future_done_q,
     "Return true if the future has reached a terminal state (resolved/failed/cancelled)."},
    {"future-cancelled?",   prim_future_cancelled_q,
     "Return true if the future was cancelled."},
    {"future?",             prim_future_q,
     "Return true if x is a future or promise."},
    {"future-deref",        prim_future_deref,
     "Block until the future is realized; return result, rethrow exception, or throw :mino/cancelled."},
};

const size_t k_prims_stateful_count =
    sizeof(k_prims_stateful) / sizeof(k_prims_stateful[0]);
