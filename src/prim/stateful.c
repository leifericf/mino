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
static int atom_validate(mino_state *S, mino_val *atom,
                         mino_val *new_val, mino_env *env)
{
    mino_val *vfn = atom->as.atom.validator;
    mino_val *vargs, *result;
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
static int atom_notify_watches(mino_state *S, mino_val *atom,
                               mino_val *old_val, mino_val *new_val,
                               mino_env *env)
{
    mino_val *watches = atom->as.atom.watches;
    size_t i, len;
    if (watches == NULL || mino_type_of(watches) != MINO_MAP || watches->as.map.len == 0)
        return 0;
    len = watches->as.map.len;
    for (i = 0; i < len; i++) {
        mino_val *key = vec_nth(watches->as.map.key_order, i);
        mino_val *fn  = map_get_val(watches, key);
        mino_val *wargs;
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
static int atom_set(mino_state *S, mino_val *atom,
                    mino_val *old_val, mino_val *new_val,
                    mino_env *env)
{
    if (atom_validate(S, atom, new_val, env) != 0) return -1;
    gc_write_barrier(S, atom, atom->as.atom.val, new_val);
    atom->as.atom.val = new_val;
    return atom_notify_watches(S, atom, old_val, new_val, env);
}

/* Atomic load of an atom's current value.  In single-threaded mode this
 * is a plain pointer read; once S->threading.multi_threaded flips the read goes
 * through __atomic_load_n with acquire ordering so swap! sees a
 * coherent snapshot of writes from other workers. */
static mino_val *atom_load(mino_state *S, mino_val *atom)
{
    if (S->threading.multi_threaded) {
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
static int atom_cas_ptr(mino_state *S, mino_val *atom,
                        mino_val *expected, mino_val *new_val)
{
    if (S->threading.multi_threaded) {
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
static mino_val *swap_build_args(mino_state *S, mino_val *cur,
                                   mino_val *extra)
{
    mino_val *call_args = mino_nil(S);
    if (extra != NULL && mino_type_of(extra) == MINO_CONS) {
        mino_val *tail = mino_nil(S);
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

static mino_val *prim_atom(mino_state *S, mino_val *args, mino_env *env)
{
    /* (atom x) or (atom x & {:keys [:meta :validator]}) -- options
     * may be supplied as flat keyword args (:meta m :validator f) or
     * via apply'd nil pairs. Unknown keys are tolerated to match
     * Clojure (it accepts any keys, only :meta and :validator have
     * effect). */
    mino_val *initial;
    mino_val *meta      = NULL;
    mino_val *validator = NULL;
    mino_val *atom;
    mino_val *rest;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "atom requires one argument");
    }
    initial = args->as.cons.car;
    rest    = args->as.cons.cdr;
    while (mino_is_cons(rest)) {
        mino_val *k;
        mino_val *v;
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
        mino_val *vargs  = mino_cons(S, initial, mino_nil(S));
        mino_val *result = mino_call(S, validator, vargs, env);
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

static mino_val *prim_deref(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *a;
    int         argc;
    mino_val *ms_v        = NULL;
    mino_val *timeout_val = NULL;
    (void)env;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "deref requires one or three arguments");
    }
    a    = args->as.cons.car;
    argc = 1;
    if (mino_is_cons(args->as.cons.cdr)) {
        ms_v = args->as.cons.cdr->as.cons.car;
        if (!mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001", "deref requires one or three arguments");
        }
        timeout_val = args->as.cons.cdr->as.cons.cdr->as.cons.car;
        if (mino_is_cons(args->as.cons.cdr->as.cons.cdr->as.cons.cdr)) {
            return prim_throw_classified(S, "eval/arity", "MAR001", "deref requires one or three arguments");
        }
        argc = 3;
    }

    /* Delay realization on the hot path. A delay in mino is a
     * map carrying a `:delay/fn` thunk (built by the `delay`
     * macro in core.clj). Realising the delay is just invoking
     * that thunk; the thunk's own body handles the once-only
     * cache via :delay/state. Folding this into the C prim
     * removes the Clojure-side `deref` shadow that wrapped
     * every `(deref ...)` call with a per-call (delay? x)
     * check, saving ~300ns/call on hot atom/var derefs that
     * never touch a delay. */
    if (a != NULL && mino_type_of(a) == MINO_MAP) {
        mino_val *delay_fn_kw = mino_keyword(S, "delay/fn");
        mino_val *delay_fn    = map_get_val(a, delay_fn_kw);
        if (delay_fn != NULL) {
            /* Same semantics whether argc is 1 or 3: the 3-arg
             * timeout form on a delay was a no-op in the previous
             * Clojure wrapper too -- delays don't block. */
            return apply_callable_argv(S, delay_fn, NULL, 0, env);
        }
    }

    if (argc == 3) {
        long       ms;
        long long  ms_ll;
        if (!mino_val_int_p(ms_v)) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                "deref: timeout argument must be an integer (milliseconds)");
        }
        ms_ll = mino_val_int_get(ms_v);
        if (ms_ll < 0) ms_ll = 0;
        if (ms_ll > 2147483647LL) ms_ll = 2147483647LL;
        ms = (long)ms_ll;
        if (a != NULL && mino_type_of(a) == MINO_FUTURE) {
            return mino_future_deref_timed(S, a, ms, timeout_val);
        }
        return prim_throw_classified(S, "eval/type", "MTY001",
            "deref: 3-arg form only supported on blocking refs (future, promise)");
    }

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
        /* Thread binding wins over the root, per canon -- and it
         * satisfies the deref even when the root is unbound. */
        if (mino_current_ctx(S)->dyn_stack != NULL) {
            mino_val *bv = dyn_lookup_var_or_name(S, a, a->as.var.sym);
            if (bv != NULL) return bv;
        }
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

static mino_val *prim_reset_bang(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *a, *val, *old;
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
 * The retry path lights up once S->threading.multi_threaded flips after host
 * threads enter the picture. */
static mino_val *prim_swap_bang(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *a, *fn, *cur, *call_args, *result, *extra;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "swap! requires at least 2 arguments: atom and function");
    }
    a  = args->as.cons.car;
    fn = args->as.cons.cdr->as.cons.car;
    if (a == NULL || mino_type_of(a) != MINO_ATOM) {
        return prim_throw_classified(S, "eval/type", "MTY001", "swap!: first argument must be an atom");
    }
    extra = args->as.cons.cdr->as.cons.cdr;
    if (!S->threading.multi_threaded) {
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
        if (atom_cas_ptr(S, a, cur, result)) {
            /* Barrier on the success path only: a losing CAS that
             * still fires the barrier would add a non-edge to the
             * remset and keep `result` artificially alive across the
             * next mark cycle. state_lock is held across cas + barrier
             * so no other thread can advance gc.phase between them,
             * and the major-mark Dijkstra push still observes `result`
             * while phase is MAJOR_MARK. */
            gc_write_barrier(S, a, cur, result);
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
static mino_val *prim_compare_and_set_bang(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *a, *expected, *new_val;
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
    if (!atom_cas_ptr(S, a, expected, new_val)) {
        return mino_false(S);
    }
    /* Write barrier fires only after the CAS succeeds so it reflects an
     * actual store; a failed CAS must not leave a stale barrier record. */
    gc_write_barrier(S, a, expected, new_val);
    if (atom_notify_watches(S, a, expected, new_val, env) != 0) return NULL;
    return mino_true(S);
}

static mino_val *prim_atom_p(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "atom? requires one argument");
    }
    return mino_is_atom(args->as.cons.car) ? mino_true(S) : mino_false(S);
}

/* (add-watch atom key fn) -- register a watch callback. */
/* Atomically publish new_val to *slot. In single-threaded mode it is
 * a plain compare-and-write (no other writer can interpose, so the
 * implicit expected always matches *slot). In multi-threaded mode it
 * runs __atomic_compare_exchange_n against the caller's snapshot;
 * returning 0 means a concurrent writer raced ahead and the caller
 * must reload and rebuild. The barrier fires only after a successful
 * publish so a lost CAS does not leave a stale OLD->YOUNG edge on
 * the remset or a Dijkstra push for a value the slot never held. */
static int watchable_slot_cas(mino_state *S, mino_val *container,
                              mino_val **slot, mino_val *expected,
                              mino_val *new_val)
{
    if (S->threading.multi_threaded) {
        mino_val *witness = expected;
        if (__atomic_compare_exchange_n(slot, &witness, new_val, 0,
                                        __ATOMIC_RELEASE,
                                        __ATOMIC_RELAXED)) {
            gc_write_barrier(S, container, expected, new_val);
            return 1;
        }
        return 0;
    }
    gc_write_barrier(S, container, *slot, new_val);
    *slot = new_val;
    return 1;
}

/* Helper: load the slot. Multi-threaded mode uses acquire-load so the
 * caller sees a coherent snapshot of writes published from other
 * workers; single-threaded mode skips the fence. */
static mino_val *watchable_slot_load(mino_state *S, mino_val **slot)
{
    if (S->threading.multi_threaded) {
        return __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    }
    return *slot;
}

/* Accessor helpers so the watch / validator primitives can target
 * either MINO_ATOM or MINO_TX_REF without duplicating per-field
 * dispatch through every code path. Returns 0 for invalid input. */
static int watchable_get(mino_val *v, mino_val ***out_watches,
                          mino_val ***out_validator)
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
static int watchable_check_state(mino_state *S, mino_val *v)
{
    mino_state *owner = NULL;
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

static mino_val *prim_add_watch(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val       *a, *key, *fn, *watches, *new_map;
    mino_val      **watches_slot;
    mino_val      **validator_slot;
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
    /* CAS retry loop: load the slot, build the next map off it, and
     * publish via watchable_slot_cas. On a lost CAS another worker
     * installed (or removed) a watch before us; rebuild against the
     * fresh snapshot so the new key joins their map rather than
     * overwriting it. */
    for (;;) {
        mino_val *base;
        mino_val *snap = watchable_slot_load(S, watches_slot);
        watches = snap;
        if (watches == NULL || mino_type_of(watches) != MINO_MAP) {
            mino_val **noargs = NULL;
            base = mino_map(S, noargs, noargs, 0);
            if (base == NULL) return NULL;
        } else {
            base = watches;
        }
        new_map = mino_map_assoc1(S, base, key, fn);
        if (new_map == NULL) return NULL;
        if (watchable_slot_cas(S, a, watches_slot, snap, new_map)) {
            return a;
        }
    }
}

/* (remove-watch ref key) -- unregister a watch callback. */
static mino_val *prim_remove_watch(mino_state *S, mino_val *args,
                              mino_env *env)
{
    mino_val  *a, *key, *watches;
    mino_val **watches_slot;
    mino_val **validator_slot;
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
    for (;;) {
        mino_val *snap = watchable_slot_load(S, watches_slot);
        mino_val *new_map;
        watches = snap;
        if (watches == NULL || mino_type_of(watches) != MINO_MAP) return a;
        if (mino_map_lookup(watches, key) == NULL) return a;
        new_map = mino_map_dissoc1(S, watches, key);
        if (new_map == NULL) return NULL;
        if (watchable_slot_cas(S, a, watches_slot, snap, new_map)) {
            return a;
        }
    }
}

/* (set-validator! ref fn) -- set or remove a validator on an atom or ref. */
static mino_val *prim_set_validator(mino_state *S, mino_val *args,
                               mino_env *env)
{
    mino_val  *a, *fn;
    mino_val **watches_slot;
    mino_val **validator_slot;
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
    /* nil removes the validator. The CAS retry loop handles the case
     * where another worker installs or removes a validator
     * concurrently; whichever store sequence the CAS finally accepts
     * is the one whose effect outlives the contention window. */
    if (fn == NULL || mino_type_of(fn) == MINO_NIL) {
        for (;;) {
            mino_val *snap = watchable_slot_load(S, validator_slot);
            if (watchable_slot_cas(S, a, validator_slot, snap, NULL)) {
                return mino_nil(S);
            }
        }
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
    for (;;) {
        mino_val *snap = watchable_slot_load(S, validator_slot);
        if (watchable_slot_cas(S, a, validator_slot, snap, fn)) {
            return mino_nil(S);
        }
    }
}

/* (get-validator ref) -- return the current validator fn or nil. */
static mino_val *prim_get_validator(mino_state *S, mino_val *args,
                               mino_env *env)
{
    mino_val  *a;
    mino_val **watches_slot;
    mino_val **validator_slot;
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
static mino_val *prim_reset_vals(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *a, *val, *old;
    mino_val *pair[2];
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
static mino_val *prim_swap_vals(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *a, *fn, *cur, *call_args, *result;
    mino_val *pair[2];
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

/* Dynamic-binding primitives are defined in stateful_bindings.c. */
extern mino_val *prim_get_thread_bindings(mino_state *S, mino_val *args, mino_env *env);
extern mino_val *prim_set_dyn_binding(mino_state *S, mino_val *args, mino_env *env);
extern mino_val *prim_with_bindings_star(mino_state *S, mino_val *args, mino_env *env);
extern mino_val *prim_push_thread_bindings_star(mino_state *S, mino_val *args, mino_env *env);
extern mino_val *prim_pop_thread_bindings_star(mino_state *S, mino_val *args, mino_env *env);
extern mino_val *prim_thread_bound_p(mino_state *S, mino_val *args, mino_env *env);

/* Fault injection: make the n-th GC allocation fail (simulated OOM).
 * Testing only. Pass 0 to disable. */
static mino_val *prim_set_fail_alloc_at(mino_state *S, mino_val *args,
                                    mino_env *env)
{
    mino_val *n;
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
 * MINO_OPT_THREAD_LIMIT and standalone main.c grants cpu_count after
 * mino_install_all. The script-side future/promise/thread stubs in
 * core.clj consult this to distinguish "host has not granted threads"
 * from "host granted, runtime impl in flight." */
static mino_val *prim_mino_thread_limit(mino_state *S, mino_val *args,
                                   mino_env *env)
{
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "mino-thread-limit takes no arguments");
    }
    return mino_int(S, (long long)mino_get_option(S, MINO_OPT_THREAD_LIMIT));
}

/* (mino-thread-id*) — a stable identity for the calling thread's
 * runtime context (the ctx address as an integer). Used by the
 * cooperative `locking` monitor registry in core.clj to record
 * ownership; nothing dereferences the value. */
static mino_val *prim_mino_thread_id(mino_state *S, mino_val *args,
                                       mino_env *env)
{
    (void)env;
    if (mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "mino-thread-id* takes no arguments");
    }
    return mino_int(S, (long long)(intptr_t)mino_current_ctx(S));
}

/* (mino-thread-count) — return the live worker count for this state. */
static mino_val *prim_mino_thread_count(mino_state *S, mino_val *args,
                                   mino_env *env)
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
static mino_val *prim_future_call(mino_state *S, mino_val *args,
                                    mino_env *env)
{
    mino_val *thunk;
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
static mino_val *prim_promise(mino_state *S, mino_val *args,
                                mino_env *env)
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
static mino_val *prim_deliver(mino_state *S, mino_val *args,
                                mino_env *env)
{
    mino_val *promise, *value;
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
static mino_val *prim_future_cancel(mino_state *S, mino_val *args,
                                      mino_env *env)
{
    mino_val *fut;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "future-cancel expects one argument");
    }
    fut = args->as.cons.car;
    return mino_future_cancel(S, fut) ? mino_true(S) : mino_false(S);
}

/* (future-done? f) — true if the future has reached a terminal state. */
static mino_val *prim_future_done_q(mino_state *S, mino_val *args,
                                      mino_env *env)
{
    mino_val *fut;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "future-done? expects one argument");
    }
    fut = args->as.cons.car;
    return mino_future_done_p(fut) ? mino_true(S) : mino_false(S);
}

/* (future-cancelled? f) — true if the future was cancelled. */
static mino_val *prim_future_cancelled_q(mino_state *S, mino_val *args,
                                           mino_env *env)
{
    mino_val *fut;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "future-cancelled? expects one argument");
    }
    fut = args->as.cons.car;
    return mino_future_cancelled_p(fut) ? mino_true(S) : mino_false(S);
}

/* (future? x) — true if x is a future or promise. */
static mino_val *prim_future_q(mino_state *S, mino_val *args,
                                 mino_env *env)
{
    mino_val *x;
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
static mino_val *prim_future_deref(mino_state *S, mino_val *args,
                                     mino_env *env)
{
    mino_val *fut;
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

static mino_val *prim_volatile_bang(mino_state *S, mino_val *args,
                               mino_env *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "volatile! requires one argument");
    }
    return mino_volatile(S, args->as.cons.car);
}

static mino_val *prim_volatile_p(mino_state *S, mino_val *args,
                            mino_env *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "volatile? requires one argument");
    }
    return mino_is_volatile(args->as.cons.car) ? mino_true(S) : mino_false(S);
}

static mino_val *prim_vreset_bang(mino_state *S, mino_val *args,
                             mino_env *env)
{
    mino_val *v, *val;
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

static mino_val *prim_vswap_bang(mino_state *S, mino_val *args,
                            mino_env *env)
{
    mino_val *v, *fn, *cur, *call_args, *result, *extra;
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
    {"push-thread-bindings*", prim_push_thread_bindings_star,
     "(push-thread-bindings* bindings-map) — push a fresh dynamic-binding frame. Must be paired with pop-thread-bindings* in a try/finally."},
    {"pop-thread-bindings*", prim_pop_thread_bindings_star,
     "(pop-thread-bindings*) — pop and free the top dynamic-binding frame. Throws when no frame is active."},
    {"-thread-bound?", prim_thread_bound_p,
     "(-thread-bound? var) — true iff the var has a thread-local binding on the current dyn-stack. Clojure-level thread-bound? wraps this and is variadic."},
    {"set-dyn-binding!", prim_set_dyn_binding,
     "(set-dyn-binding! 'name value) — mutate the topmost active dynamic binding for `name`. Returns the value. Throws when no binding frame is active for `name`. Backs (set! *var* expr)."},
    {"set-fail-alloc-at!", prim_set_fail_alloc_at,
     "Make the n-th GC allocation fail (simulated OOM). Pass 0 to disable."},
    {"mino-thread-id*",   prim_mino_thread_id,
     "Stable identity of the calling thread's runtime context."},
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
