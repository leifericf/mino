/*
 * async_timer.c -- deadline-scheduled callbacks.
 *
 * The timer queue is a singly-linked list sorted by deadline; insertion
 * is O(n) in the current queue size. When a deadline passes the
 * callback is handed to the async scheduler, which invokes it on the
 * next drain. This keeps all fire-ordering and GC pinning concerns
 * inside the scheduler, not the timer subsystem.
 */

#include "async/timer.h"
#include "async/scheduler.h"
#include "runtime/internal.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__) || defined(__unix__) || defined(__linux__)
#include <sys/time.h>
#endif

/* Get current wall-clock time in milliseconds. */
static double now_ms(void)
{
#if defined(_WIN32)
    LARGE_INTEGER freq, ctr;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&ctr);
    return (double)ctr.QuadPart / (double)freq.QuadPart * 1000.0;
#elif defined(__APPLE__) || defined(__unix__) || defined(__linux__)
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
#else
    /* Fallback: ANSI C time() gives seconds resolution. */
    return (double)time(NULL) * 1000.0;
#endif
}

int async_timer_schedule(mino_state_t *S, double ms, mino_val_t *callback)
{
    timer_entry_t *entry;

    if (callback == NULL || callback->type == MINO_NIL) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "eval/contract", "MCT001",
                      "async-schedule-timer* requires a non-nil callback");
        return -1;
    }

    entry = calloc(1, sizeof(*entry));
    if (entry == NULL) {
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form, "internal", "MIN001",
                      "out of memory creating timer entry");
        return -1;
    }
    entry->deadline_ms = now_ms() + ms;
    entry->callback    = callback;
    entry->cb_ref      = mino_ref(S, callback);
    entry->next        = NULL;

    /* Insert sorted by deadline (earliest first). */
    if (S->async_timers == NULL ||
        entry->deadline_ms <= S->async_timers->deadline_ms) {
        entry->next     = S->async_timers;
        S->async_timers = entry;
    } else {
        timer_entry_t *cur = S->async_timers;
        while (cur->next != NULL &&
               cur->next->deadline_ms < entry->deadline_ms) {
            cur = cur->next;
        }
        entry->next = cur->next;
        cur->next   = entry;
    }

    return 0;
}

void async_timers_check(mino_state_t *S)
{
    double t = now_ms();
    timer_entry_t *entry;

    while (S->async_timers != NULL) {
        entry = S->async_timers;
        if (entry->deadline_ms > t) break;

        S->async_timers = entry->next;

        /* Hand the callback to the scheduler; it runs on the next drain
         * pass and receives nil as its argument. */
        async_sched_enqueue(S, entry->callback, mino_nil(S));

        if (entry->cb_ref) mino_unref(S, entry->cb_ref);
        free(entry);
    }
}

void async_timers_free(mino_state_t *S)
{
    timer_entry_t *entry = S->async_timers;
    while (entry != NULL) {
        timer_entry_t *next = entry->next;
        if (entry->cb_ref) mino_unref(S, entry->cb_ref);
        free(entry);
        entry = next;
    }
    S->async_timers = NULL;
}

void async_timers_mark(mino_state_t *S)
{
    timer_entry_t *entry;
    for (entry = S->async_timers; entry != NULL; entry = entry->next) {
        if (entry->callback) gc_mark_interior(S, entry->callback);
    }
}
