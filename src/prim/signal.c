/*
 * signal.c -- signal traps and process exit hooks: on-signal, at-exit.
 *
 * A delivered signal does one async-signal-safe thing: it writes 1 to a
 * volatile sig_atomic_t flag in g_signal_pending. Nothing else runs in
 * signal context -- no allocation, no mino call, no lock. The registered
 * mino handler runs later, at the interpreter safepoint that eval_impl
 * already polls (mino_signal_deliver_pending), so a handler is ordinary
 * mino code with the full runtime available, never reentrant with libc.
 *
 * Handlers and at-exit thunks are held in fixed C-side slots on the
 * mino_state and rooted by the collector (gc_mark_runtime_globals), so a
 * trapped fn survives every collection between registration and delivery.
 *
 * at-exit thunks run last-registered-first in mino_signal_run_atexit,
 * driven from the single teardown seam mino_state_free, so both a plain
 * (exit n) and falling off the end of a script run them exactly once,
 * with the exit code untouched.
 *
 * The five trappable signals are the portable job-control set: :int
 * (SIGINT), :term (SIGTERM), :hup (SIGHUP), :usr1 / :usr2. On Windows,
 * where only SIGINT and SIGTERM exist in the C runtime, the other three
 * throw a classified not-supported error rather than silently doing
 * nothing.
 */

#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE 1
#endif

#include "prim/internal.h"
#include "mino.h"

#include <signal.h>
#include <string.h>

/* Internal slot per trappable signal; also indexes the state's handler
 * and the g_signal_pending flag array. */
enum sig_slot {
    SIG_SLOT_INT = 0,
    SIG_SLOT_TERM,
    SIG_SLOT_HUP,
    SIG_SLOT_USR1,
    SIG_SLOT_USR2,
    SIG_SLOT_COUNT
};

/* Set from the async-signal-safe handler, cleared at the safepoint.
 * volatile sig_atomic_t is the only object a handler may touch per the C
 * standard; nothing else in this file runs in signal context. */
static volatile sig_atomic_t g_signal_pending[SIG_SLOT_COUNT];

/* Aggregate of g_signal_pending: non-zero when any signal is pending.
 * The eval safepoint reads only this one flag on its hot path, and calls
 * into mino_signal_deliver_pending only when it is set, so a script with
 * no trap installed pays a single predictably-not-taken branch. */
volatile sig_atomic_t mino_signal_any;

/* Map a slot to its OS signal number, or -1 when the platform lacks it. */
static int slot_signo(enum sig_slot slot)
{
    switch (slot) {
    case SIG_SLOT_INT:  return SIGINT;
    case SIG_SLOT_TERM: return SIGTERM;
#ifdef SIGHUP
    case SIG_SLOT_HUP:  return SIGHUP;
#else
    case SIG_SLOT_HUP:  return -1;
#endif
#ifdef SIGUSR1
    case SIG_SLOT_USR1: return SIGUSR1;
#else
    case SIG_SLOT_USR1: return -1;
#endif
#ifdef SIGUSR2
    case SIG_SLOT_USR2: return SIGUSR2;
#else
    case SIG_SLOT_USR2: return -1;
#endif
    default:            return -1;
    }
}

/* Map a :int/:term/:hup/:usr1/:usr2 keyword to its slot, or -1. */
static int slot_for_keyword(const mino_val *kw)
{
    const char *name;
    if (kw == NULL || mino_type_of(kw) != MINO_KEYWORD) return -1;
    name = kw->as.s.data;
    if (name == NULL) return -1;
    if (strcmp(name, "int")  == 0) return SIG_SLOT_INT;
    if (strcmp(name, "term") == 0) return SIG_SLOT_TERM;
    if (strcmp(name, "hup")  == 0) return SIG_SLOT_HUP;
    if (strcmp(name, "usr1") == 0) return SIG_SLOT_USR1;
    if (strcmp(name, "usr2") == 0) return SIG_SLOT_USR2;
    return -1;
}

/* The async-signal-safe handler: record the pending signal and return.
 * Only the volatile flag write is standard-safe here. */
static void signal_trap(int signo)
{
    int i;
    for (i = 0; i < SIG_SLOT_COUNT; i++) {
        if (slot_signo((enum sig_slot)i) == signo) {
            g_signal_pending[i] = 1;
            mino_signal_any     = 1;
            return;
        }
    }
}

/* Install signal_trap for a slot's signal (sigaction on POSIX so the
 * handler is not reset per delivery and SA_RESTART resumes blocked
 * syscalls). Returns 0 on success, -1 when the platform lacks the
 * signal. */
static int slot_install_trap(enum sig_slot slot)
{
    int signo = slot_signo(slot);
    if (signo < 0) return -1;
#ifdef _WIN32
    if (signal(signo, signal_trap) == SIG_ERR) return -1;
#else
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = signal_trap;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        if (sigaction(signo, &sa, NULL) != 0) return -1;
    }
#endif
    return 0;
}

/* Reset a slot's signal to a fixed disposition (SIG_DFL or SIG_IGN). */
static int slot_set_disposition(enum sig_slot slot, void (*disp)(int))
{
    int signo = slot_signo(slot);
    if (signo < 0) return -1;
#ifdef _WIN32
    if (signal(signo, disp) == SIG_ERR) return -1;
#else
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = disp;
        sigemptyset(&sa.sa_mask);
        if (sigaction(signo, &sa, NULL) != 0) return -1;
    }
#endif
    return 0;
}

/* (on-signal sig handler) -- trap sig with a zero-arg fn, or reshape the
 * disposition with :default / :ignore. Returns nil. */
static mino_val *prim_on_signal(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *sig_val;
    mino_val *handler;
    int       slot;
    (void)env;

    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "on-signal requires two arguments: a "
                                     "signal keyword and a handler");
    }
    sig_val = args->as.cons.car;
    handler = args->as.cons.cdr->as.cons.car;

    slot = slot_for_keyword(sig_val);
    if (slot < 0) {
        return prim_throw_classified(S, "eval/contract", "MCT001",
                                     "on-signal: signal must be one of "
                                     ":int :term :hup :usr1 :usr2");
    }
    if (slot_signo((enum sig_slot)slot) < 0) {
        return prim_throw_classified(S, "host/unsupported", "MHU001",
                                     "on-signal: this signal is not "
                                     "available on this platform");
    }

    /* The keyword data forms :default and :ignore reshape the OS
     * disposition and drop any fn handler for this signal. */
    if (mino_type_of(handler) == MINO_KEYWORD && handler->as.s.data != NULL
        && strcmp(handler->as.s.data, "default") == 0) {
        S->signal_handlers[slot] = NULL;
        g_signal_pending[slot]   = 0;
        if (slot_set_disposition((enum sig_slot)slot, SIG_DFL) != 0) {
            return prim_throw_classified(S, "host", "MHO001",
                                         "on-signal: failed to restore the "
                                         "default disposition");
        }
        return mino_nil(S);
    }
    if (mino_type_of(handler) == MINO_KEYWORD && handler->as.s.data != NULL
        && strcmp(handler->as.s.data, "ignore") == 0) {
        S->signal_handlers[slot] = NULL;
        g_signal_pending[slot]   = 0;
        if (slot_set_disposition((enum sig_slot)slot, SIG_IGN) != 0) {
            return prim_throw_classified(S, "host", "MHO001",
                                         "on-signal: failed to ignore the "
                                         "signal");
        }
        return mino_nil(S);
    }
    if (!mino_is_fn(handler)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "on-signal: handler must be a function "
                                     "or the keyword :default / :ignore");
    }

    S->signal_handlers[slot] = handler;
    if (slot_install_trap((enum sig_slot)slot) != 0) {
        S->signal_handlers[slot] = NULL;
        return prim_throw_classified(S, "host", "MHO001",
                                     "on-signal: failed to install the "
                                     "signal handler");
    }
    return mino_nil(S);
}

/* (at-exit thunk) -- register a zero-arg fn to run at process exit.
 * Returns nil. Hooks run last-registered-first. */
static mino_val *prim_at_exit(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val  *thunk;
    mino_val **grown;
    (void)env;

    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
                                     "at-exit requires one argument: a "
                                     "zero-argument function");
    }
    thunk = args->as.cons.car;
    if (!mino_is_fn(thunk)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
                                     "at-exit: the hook must be a function");
    }

    if (S->atexit_len == S->atexit_cap) {
        size_t new_cap = (S->atexit_cap == 0) ? 8 : S->atexit_cap * 2;
        if (new_cap > SIZE_MAX / sizeof(mino_val *)) {
            return prim_throw_classified(S, "eval/bounds", "MBD001",
                                         "at-exit: too many hooks");
        }
        grown = (mino_val **)realloc(S->atexit_hooks,
                                     new_cap * sizeof(mino_val *));
        if (grown == NULL) {
            return prim_throw_classified(S, "internal", "MIN001",
                                         "at-exit: out of memory");
        }
        S->atexit_hooks = grown;
        S->atexit_cap   = new_cap;
    }
    S->atexit_hooks[S->atexit_len++] = thunk;
    return mino_nil(S);
}

/* Run every pending mino signal handler at the interpreter safepoint.
 * Each flag is cleared before its handler runs so a signal delivered
 * during the handler is coalesced into a single later delivery, never a
 * reentrant loop. Called from eval_check_limits, so this is ordinary
 * eval depth: mino_call recurses into eval_impl exactly as a normal
 * call would. A handler that throws propagates like any thrown value. */
void mino_signal_deliver_pending(mino_state *S)
{
    int i;
    if (S == NULL) return;
    /* Clear the aggregate before draining so a signal delivered while a
     * handler runs re-arms it and is picked up on the next safepoint. */
    mino_signal_any = 0;
    for (i = 0; i < SIG_SLOT_COUNT; i++) {
        mino_val *handler;
        if (!g_signal_pending[i]) continue;
        g_signal_pending[i] = 0;
        handler = S->signal_handlers[i];
        if (handler == NULL) continue;
        (void)mino_call(S, handler, mino_nil(S), NULL);
    }
}

/* Run the at-exit hooks last-registered-first, exactly once. A hook's
 * thrown value is swallowed (mino_pcall) so one failing hook does not
 * abort the rest of teardown or leak a longjmp past the exit path.
 * Called from the single teardown seam so every exit route runs it. */
void mino_signal_run_atexit(mino_state *S)
{
    size_t i;
    if (S == NULL || S->atexit_running) return;
    S->atexit_running = 1;
    i = S->atexit_len;
    while (i > 0) {
        mino_val *thunk = S->atexit_hooks[--i];
        mino_val *out   = NULL;
        if (thunk != NULL) {
            (void)mino_pcall(S, thunk, mino_nil(S), NULL, &out, NULL);
        }
    }
    S->atexit_len = 0;
    free(S->atexit_hooks);
    S->atexit_hooks = NULL;
    S->atexit_cap   = 0;
}

const mino_prim_def k_prims_signal[] = {
    {"on-signal", prim_on_signal,
     "Traps signal sig (one of :int :term :hup :usr1 :usr2) with handler, "
     "a zero-argument function that runs at the interpreter safepoint "
     "after the signal is delivered, never in signal context. Pass "
     ":default to restore the OS default disposition or :ignore to drop "
     "the signal. Returns nil."},
    {"at-exit", prim_at_exit,
     "Registers a zero-argument function to run when the process exits. "
     "Hooks run last-registered-first, on a plain (exit n) and when a "
     "script ends, and after a trapped signal's handler calls (exit n). "
     "The exit code is preserved. Returns nil."},
};

const size_t k_prims_signal_count =
    sizeof(k_prims_signal) / sizeof(k_prims_signal[0]);

/* signal rides its own MINO_CAP_SIGNAL bit (in MINO_CAP_DEFAULT). The
 * bit is set here so a direct mino_install_signal sets it too, matching
 * the codec/digest/random install. */
void mino_install_signal(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_signal, k_prims_signal_count,
                                       "signal");
    S->caps_installed |= MINO_CAP_SIGNAL;
}
