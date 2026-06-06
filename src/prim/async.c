/*
 * async.c -- minimal C surface for the async scheduler.
 *
 * Channels, buffers, and alts live in lib/core/channel.clj. The three
 * things that still need to touch C are:
 *
 *   1. The scheduler run queue. Callbacks for any pending channel op or
 *      timer go through this queue so the mino-level code and host
 *      embedders share a single drain surface.
 *   2. Deadline timers. Arming a callback to fire "N ms from now"
 *      needs host nanosecond time and a priority queue; both are small
 *      and naturally C-shaped.
 *   3. The (drain! / drain-loop!) public API for host embedders that
 *      drive the event loop from outside mino.
 *
 * That's it: four primitives.
 */

#include "runtime/internal.h"
#include "prim/internal.h"
#include "mino.h"
#include "async/scheduler.h"
#include "async/timer.h"
#include "async/chan.h"

static mino_val *prim_sched_enqueue(mino_state *S, mino_val *args,
                                      mino_env *env)
{
    mino_val *cb, *val;
    (void)env;

    if (args == NULL || mino_type_of(args) != MINO_CONS ||
        args->as.cons.cdr == NULL) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "eval/arity", "MAR001",
                      "async-sched-enqueue* requires callback and value");
        return NULL;
    }
    cb  = args->as.cons.car;
    val = args->as.cons.cdr->as.cons.car;

    if (cb != NULL && mino_type_of(cb) == MINO_NIL) cb = NULL;
    if (cb != NULL) async_sched_enqueue(S, cb, val);
    return mino_nil(S);
}

static mino_val *prim_timer_schedule(mino_state *S, mino_val *args,
                                       mino_env *env)
{
    double ms;
    mino_val *cb;
    (void)env;

    if (args == NULL || mino_type_of(args) != MINO_CONS ||
        args->as.cons.cdr == NULL) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "eval/arity", "MAR001",
                      "async-schedule-timer* requires ms and callback");
        return NULL;
    }
    if (!as_double(args->as.cons.car, &ms)) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "eval/type", "MTY001",
                      "async-schedule-timer* first argument must be a number");
        return NULL;
    }
    if (ms < 0) ms = 0;
    cb = args->as.cons.cdr->as.cons.car;
    if (cb != NULL && mino_type_of(cb) == MINO_NIL) cb = NULL;

    if (async_timer_schedule(S, ms, cb) != 0) return NULL;
    return mino_nil(S);
}

/* (async-next-timer-ms*) -> ms until the next pending timer fires
 * (number, >= 0) or nil when no timer is pending. Lets the blocking
 * bridges wake in time to drive timer callbacks, which only fire
 * inside scheduler drains. */
static mino_val *prim_timer_next_ms(mino_state *S, mino_val *args,
                                      mino_env *env)
{
    double rem;
    (void)args;
    (void)env;
    rem = async_timer_next_ms(S);
    if (rem < 0.0) return mino_nil(S);
    return mino_float(S, rem);
}

static mino_val *prim_drain(mino_state *S, mino_val *args,
                              mino_env *env)
{
    (void)args;
    async_sched_drain(S, env);
    return mino_nil(S);
}

/* (drain-loop! done-thunk)
 * Repeatedly drains the scheduler and checks timers until either:
 *   (a) (done-thunk) returns truthy, or
 *   (b) a full pass produces no progress (no callbacks ran, no timers
 *       fired) -- meaning further draining cannot help.
 * Returns true if done-thunk returned truthy, false if no progress. */
static mino_val *prim_drain_loop(mino_state *S, mino_val *args,
                                   mino_env *env)
{
    mino_val *done_thunk, *result;

    if (args == NULL || mino_type_of(args) != MINO_CONS) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "eval/arity", "MAR001",
                      "drain-loop! requires a done-check thunk");
        return NULL;
    }
    done_thunk = args->as.cons.car;
    gc_pin(done_thunk);

    for (;;) {
        int progress = async_sched_drain(S, env);

        result = mino_call(S, done_thunk, NULL, env);
        if (result != NULL && mino_type_of(result) != MINO_NIL &&
            !(mino_type_of(result) == MINO_BOOL && !mino_val_bool_get(result))) {
            gc_unpin(1);
            return mino_true(S);
        }

        if (!progress) {
            gc_unpin(1);
            return mino_false(S);
        }
    }
}

/* Channel primitives. Each is a thin shim over async/chan.c. The
 * user-facing surface (chan, offer!, poll!, put!, take!, close!,
 * closed?) lives in lib/clojure/core/async.clj; that script-side layer
 * wraps these to add transducer / ex-handler / alts! arbitration. */

static int as_chan(mino_state *S, mino_val *v, const char *fn,
                   mino_val **out)
{
    if (v == NULL || mino_type_of(v) != MINO_CHAN) {
        char msg[120];
        snprintf(msg, sizeof(msg), "%s requires a channel", fn);
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                      "eval/type", "MTY001", msg);
        return 0;
    }
    *out = v;
    return 1;
}

static mino_val *prim_chan_star(mino_state *S, mino_val *args, mino_env *env)
{
    long long buf_kind_l = 0;
    long long buf_cap_l  = 0;
    mino_val *kind_v, *cap_v, *xform_v, *exh_v;
    (void)env;
    if (args == NULL || mino_type_of(args) != MINO_CONS
        || args->as.cons.cdr == NULL || args->as.cons.cdr->as.cons.cdr == NULL
        || args->as.cons.cdr->as.cons.cdr->as.cons.cdr == NULL) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                      "eval/arity", "MAR001",
                      "chan* requires (buf-kind buf-cap xform ex-handler)");
        return NULL;
    }
    kind_v  = args->as.cons.car;
    cap_v   = args->as.cons.cdr->as.cons.car;
    xform_v = args->as.cons.cdr->as.cons.cdr->as.cons.car;
    exh_v   = args->as.cons.cdr->as.cons.cdr->as.cons.cdr->as.cons.car;
    if (!mino_to_int(kind_v, &buf_kind_l)
        || !mino_to_int(cap_v, &buf_cap_l)) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                      "eval/type", "MTY001",
                      "chan* requires integer buf-kind and buf-cap");
        return NULL;
    }
    if (buf_cap_l < 0) buf_cap_l = 0;
    if (xform_v != NULL && mino_type_of(xform_v) == MINO_NIL) xform_v = NULL;
    if (exh_v != NULL && mino_type_of(exh_v) == MINO_NIL) exh_v = NULL;
    return mino_chan_new(S, (int)buf_kind_l, (size_t)buf_cap_l,
                         xform_v, exh_v);
}

static mino_val *prim_chan_p(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *v;
    (void)env;
    if (args == NULL || mino_type_of(args) != MINO_CONS) return mino_false(S);
    v = args->as.cons.car;
    return (v != NULL && mino_type_of(v) == MINO_CHAN)
           ? mino_true(S) : mino_false(S);
}

static mino_val *prim_chan_offer(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *ch, *val;
    int accepted = 0;
    (void)env;
    if (args == NULL || mino_type_of(args) != MINO_CONS
        || args->as.cons.cdr == NULL) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                      "eval/arity", "MAR001",
                      "chan-offer* requires (chan val)");
        return NULL;
    }
    if (!as_chan(S, args->as.cons.car, "chan-offer*", &ch)) return NULL;
    val = args->as.cons.cdr->as.cons.car;
    if (val == NULL || mino_type_of(val) == MINO_NIL) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                      "eval/type", "MTY001",
                      "cannot put nil on a channel");
        return NULL;
    }
    if (mino_chan_offer(S, ch, val, &accepted) != 0) return NULL;
    return accepted ? mino_true(S) : mino_false(S);
}

static mino_val *prim_chan_poll(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *ch;
    (void)env;
    if (args == NULL || mino_type_of(args) != MINO_CONS) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                      "eval/arity", "MAR001",
                      "chan-poll* requires (chan)");
        return NULL;
    }
    if (!as_chan(S, args->as.cons.car, "chan-poll*", &ch)) return NULL;
    return mino_chan_poll(S, ch);
}

static mino_val *prim_chan_put(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *ch, *val, *cb;
    (void)env;
    if (args == NULL || mino_type_of(args) != MINO_CONS
        || args->as.cons.cdr == NULL
        || args->as.cons.cdr->as.cons.cdr == NULL) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                      "eval/arity", "MAR001",
                      "chan-put* requires (chan val cb-or-nil)");
        return NULL;
    }
    if (!as_chan(S, args->as.cons.car, "chan-put*", &ch)) return NULL;
    val = args->as.cons.cdr->as.cons.car;
    cb  = args->as.cons.cdr->as.cons.cdr->as.cons.car;
    if (val == NULL || mino_type_of(val) == MINO_NIL) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                      "eval/type", "MTY001",
                      "cannot put nil on a channel");
        return NULL;
    }
    if (cb != NULL && mino_type_of(cb) == MINO_NIL) cb = NULL;
    if (mino_chan_put(S, ch, val, cb) < 0) return NULL;
    return mino_nil(S);
}

static mino_val *prim_chan_take(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *ch, *cb;
    (void)env;
    if (args == NULL || mino_type_of(args) != MINO_CONS
        || args->as.cons.cdr == NULL) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                      "eval/arity", "MAR001",
                      "chan-take* requires (chan cb-or-nil)");
        return NULL;
    }
    if (!as_chan(S, args->as.cons.car, "chan-take*", &ch)) return NULL;
    cb = args->as.cons.cdr->as.cons.car;
    if (cb != NULL && mino_type_of(cb) == MINO_NIL) cb = NULL;
    if (mino_chan_take(S, ch, cb) < 0) return NULL;
    return mino_nil(S);
}

static mino_val *prim_chan_close(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *ch;
    (void)env;
    if (args == NULL || mino_type_of(args) != MINO_CONS) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                      "eval/arity", "MAR001",
                      "chan-close* requires (chan)");
        return NULL;
    }
    if (!as_chan(S, args->as.cons.car, "chan-close*", &ch)) return NULL;
    mino_chan_close(S, ch);
    return mino_nil(S);
}

static mino_val *prim_chan_closed_p(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *ch;
    (void)env;
    if (args == NULL || mino_type_of(args) != MINO_CONS) return mino_false(S);
    if (!as_chan(S, args->as.cons.car, "chan-closed?*", &ch)) return NULL;
    return mino_chan_closed_p(ch) ? mino_true(S) : mino_false(S);
}

static mino_val *prim_chan_buf_count(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *ch;
    (void)env;
    if (args == NULL || mino_type_of(args) != MINO_CONS) return mino_int(S, 0);
    if (!as_chan(S, args->as.cons.car, "chan-buf-count*", &ch)) return NULL;
    return mino_int(S, mino_chan_buf_count(ch));
}

static mino_val *prim_chan_buf_full_p(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *ch;
    (void)env;
    if (args == NULL || mino_type_of(args) != MINO_CONS) return mino_true(S);
    if (!as_chan(S, args->as.cons.car, "chan-buf-full?*", &ch)) return NULL;
    return mino_chan_buf_full_p(ch) ? mino_true(S) : mino_false(S);
}

static mino_val *prim_chan_put_alts(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *ch, *val, *cb, *flag;
    (void)env;
    if (args == NULL || mino_type_of(args) != MINO_CONS
        || args->as.cons.cdr == NULL
        || args->as.cons.cdr->as.cons.cdr == NULL
        || args->as.cons.cdr->as.cons.cdr->as.cons.cdr == NULL) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                      "eval/arity", "MAR001",
                      "chan-put-alts* requires (chan val cb flag)");
        return NULL;
    }
    if (!as_chan(S, args->as.cons.car, "chan-put-alts*", &ch)) return NULL;
    val  = args->as.cons.cdr->as.cons.car;
    cb   = args->as.cons.cdr->as.cons.cdr->as.cons.car;
    flag = args->as.cons.cdr->as.cons.cdr->as.cons.cdr->as.cons.car;
    if (val == NULL || mino_type_of(val) == MINO_NIL) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                      "eval/type", "MTY001",
                      "cannot put nil on a channel");
        return NULL;
    }
    if (cb != NULL && mino_type_of(cb) == MINO_NIL) cb = NULL;
    if (flag != NULL && mino_type_of(flag) == MINO_NIL) flag = NULL;
    if (mino_chan_put_alts(S, ch, val, cb, flag) < 0) return NULL;
    return mino_nil(S);
}

static mino_val *prim_chan_buf_add(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *ch, *val;
    (void)env;
    if (args == NULL || mino_type_of(args) != MINO_CONS
        || args->as.cons.cdr == NULL) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                      "eval/arity", "MAR001",
                      "chan-buf-add* requires (chan val)");
        return NULL;
    }
    if (!as_chan(S, args->as.cons.car, "chan-buf-add*", &ch)) return NULL;
    val = args->as.cons.cdr->as.cons.car;
    if (val == NULL || mino_type_of(val) == MINO_NIL) return mino_nil(S);
    mino_chan_buf_add(S, ch, val);
    return mino_nil(S);
}

static mino_val *prim_chan_set_xform(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *ch, *rf, *exh;
    (void)env;
    if (args == NULL || mino_type_of(args) != MINO_CONS
        || args->as.cons.cdr == NULL
        || args->as.cons.cdr->as.cons.cdr == NULL) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                      "eval/arity", "MAR001",
                      "chan-set-xform* requires (chan rf ex-handler)");
        return NULL;
    }
    if (!as_chan(S, args->as.cons.car, "chan-set-xform*", &ch)) return NULL;
    rf  = args->as.cons.cdr->as.cons.car;
    exh = args->as.cons.cdr->as.cons.cdr->as.cons.car;
    if (rf != NULL && mino_type_of(rf) == MINO_NIL) rf = NULL;
    if (exh != NULL && mino_type_of(exh) == MINO_NIL) exh = NULL;
    mino_chan_set_xform(S, ch, rf, exh);
    return mino_nil(S);
}

static mino_val *prim_chan_get_xform(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *ch, *rf;
    (void)env;
    if (args == NULL || mino_type_of(args) != MINO_CONS) return mino_nil(S);
    if (!as_chan(S, args->as.cons.car, "chan-get-xform*", &ch)) return NULL;
    rf = mino_chan_get_xform(ch);
    return (rf != NULL) ? rf : mino_nil(S);
}

static mino_val *prim_chan_get_ex_handler(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *ch, *exh;
    (void)env;
    if (args == NULL || mino_type_of(args) != MINO_CONS) return mino_nil(S);
    if (!as_chan(S, args->as.cons.car, "chan-get-ex-handler*", &ch)) return NULL;
    exh = mino_chan_get_ex_handler(ch);
    return (exh != NULL) ? exh : mino_nil(S);
}

static mino_val *prim_chan_flush_buf_to_takers(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *ch;
    (void)env;
    if (args == NULL || mino_type_of(args) != MINO_CONS) return mino_nil(S);
    if (!as_chan(S, args->as.cons.car, "chan-flush*", &ch)) return NULL;
    mino_chan_flush_buf_to_takers(S, ch);
    return mino_nil(S);
}

static mino_val *prim_chan_has_pending_taker_p(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *ch;
    (void)env;
    if (args == NULL || mino_type_of(args) != MINO_CONS) return mino_false(S);
    if (!as_chan(S, args->as.cons.car, "chan-has-pending-taker?*", &ch)) return NULL;
    return mino_chan_has_pending_taker_p(S, ch) ? mino_true(S) : mino_false(S);
}

static mino_val *prim_chan_has_pending_putter_p(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *ch;
    (void)env;
    if (args == NULL || mino_type_of(args) != MINO_CONS) return mino_false(S);
    if (!as_chan(S, args->as.cons.car, "chan-has-pending-putter?*", &ch)) return NULL;
    return mino_chan_has_pending_putter_p(S, ch) ? mino_true(S) : mino_false(S);
}

static mino_val *prim_chan_take_alts(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *ch, *cb, *flag;
    (void)env;
    if (args == NULL || mino_type_of(args) != MINO_CONS
        || args->as.cons.cdr == NULL
        || args->as.cons.cdr->as.cons.cdr == NULL) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                      "eval/arity", "MAR001",
                      "chan-take-alts* requires (chan cb flag)");
        return NULL;
    }
    if (!as_chan(S, args->as.cons.car, "chan-take-alts*", &ch)) return NULL;
    cb   = args->as.cons.cdr->as.cons.car;
    flag = args->as.cons.cdr->as.cons.cdr->as.cons.car;
    if (cb != NULL && mino_type_of(cb) == MINO_NIL) cb = NULL;
    if (flag != NULL && mino_type_of(flag) == MINO_NIL) flag = NULL;
    if (mino_chan_take_alts(S, ch, cb, flag) < 0) return NULL;
    return mino_nil(S);
}

const mino_prim_def k_prims_async[] = {
    {"async-sched-enqueue*",  prim_sched_enqueue,
     "Enqueue a callback on the async scheduler run queue."},
    {"async-schedule-timer*", prim_timer_schedule,
     "Schedule a callback to fire after ms milliseconds."},
    {"async-next-timer-ms*",  prim_timer_next_ms,
     "Milliseconds until the next pending timer, or nil when none."},
    {"drain!",                prim_drain,
     "Drain the async run queue once."},
    {"drain-loop!",           prim_drain_loop,
     "Drain until done-thunk returns truthy or no progress."},
    /* Channel C primitives. Names intentionally have no trailing `*`
     * because the script-side `clojure.core.async` namespace exposes
     * the `*`-suffixed surface (chan*, chan-put*, chan-take*,
     * chan-close*, ...) that the go macro emits; the unstarred names
     * are the C-level bindings the script-side wraps. */
    {"chan-new",              prim_chan_star,
     "Construct a channel: (chan-new buf-kind buf-cap xform ex-handler)."},
    {"chan-instance?",        prim_chan_p,
     "True if x is a channel (MINO_CHAN tag). Public chan? lives in clojure.core.async to avoid shadowing on :refer :all."},
    {"chan-offer",            prim_chan_offer,
     "Non-blocking put: (chan-offer ch val). Returns true/false."},
    {"chan-poll",             prim_chan_poll,
     "Non-blocking take: (chan-poll ch). Returns value or nil."},
    {"chan-put",              prim_chan_put,
     "Async put: (chan-put ch val cb-or-nil)."},
    {"chan-take",             prim_chan_take,
     "Async take: (chan-take ch cb-or-nil)."},
    {"chan-close",            prim_chan_close,
     "Close channel: (chan-close ch)."},
    {"chan-closed?",          prim_chan_closed_p,
     "True if channel is closed."},
    {"chan-buf-count",        prim_chan_buf_count,
     "Number of buffered values."},
    {"chan-buf-full?",        prim_chan_buf_full_p,
     "True if buffer is full (or unbuffered/promise-set)."},
    {"chan-put-alts",         prim_chan_put_alts,
     "alts-flavoured put: (chan-put-alts ch val cb flag)."},
    {"chan-take-alts",        prim_chan_take_alts,
     "alts-flavoured take: (chan-take-alts ch cb flag)."},
    {"chan-buf-add",          prim_chan_buf_add,
     "Direct buffer push: (chan-buf-add ch val). Used by xform rf."},
    {"chan-set-xform",        prim_chan_set_xform,
     "Install transducer rf: (chan-set-xform ch rf ex-handler)."},
    {"chan-get-xform",        prim_chan_get_xform,
     "Read installed transducer rf, or nil if none."},
    {"chan-get-ex-handler",   prim_chan_get_ex_handler,
     "Read installed ex-handler, or nil if none."},
    {"chan-flush-buf-to-takers", prim_chan_flush_buf_to_takers,
     "Wake every parked taker with a buffered value handoff."},
    {"chan-has-pending-taker?",  prim_chan_has_pending_taker_p,
     "True if any non-committed taker is parked."},
    {"chan-has-pending-putter?", prim_chan_has_pending_putter_p,
     "True if any non-committed putter is parked."},
};

const size_t k_prims_async_count =
    sizeof(k_prims_async) / sizeof(k_prims_async[0]);

void mino_install_async(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_async, k_prims_async_count,
                                       "async");
    S->caps_installed |= MINO_CAP_ASYNC;
}
