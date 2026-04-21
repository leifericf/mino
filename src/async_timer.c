/*
 * async_timer.c -- timeout channels for async operations.
 */

#include "async_timer.h"
#include "async_channel.h"
#include "mino_internal.h"

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

mino_val_t *async_timeout(mino_state_t *S, double ms)
{
    mino_val_t *ch;
    timer_entry_t *entry;
    mino_async_buf_t *buf;

    /* Create a buffered channel (1) for the timeout. */
    buf = async_buf_create(S, ASYNC_BUF_FIXED, 1);
    if (buf == NULL) {
        set_eval_diag(S, S->eval_current_form, "internal", "MIN001", "out of memory creating timeout buffer");
        return NULL;
    }
    ch = async_chan_create(S, buf);
    if (ch == NULL) return NULL;

    entry = calloc(1, sizeof(*entry));
    if (entry == NULL) {
        set_eval_diag(S, S->eval_current_form, "internal", "MIN001", "out of memory creating timer entry");
        return NULL;
    }
    entry->deadline_ms = now_ms() + ms;
    entry->chan_handle  = ch;
    entry->chan_ref     = mino_ref(S, ch);
    entry->next         = NULL;

    /* Insert sorted by deadline (earliest first). */
    if (S->async_timers == NULL ||
        entry->deadline_ms <=
            S->async_timers->deadline_ms) {
        entry->next = S->async_timers;
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

    return ch;
}

void async_timers_check(mino_state_t *S)
{
    double t = now_ms();
    timer_entry_t *entry;

    while (S->async_timers != NULL) {
        entry = S->async_timers;
        if (entry->deadline_ms > t) break;

        S->async_timers = entry->next;

        /* Close the timeout channel. */
        {
            mino_async_chan_t *ch = async_chan_get(entry->chan_handle);
            if (ch != NULL && !async_chan_closed(ch))
                async_chan_close(S, ch);
        }

        if (entry->chan_ref) mino_unref(S, entry->chan_ref);
        free(entry);
    }
}

void async_timers_free(mino_state_t *S)
{
    timer_entry_t *entry = S->async_timers;
    while (entry != NULL) {
        timer_entry_t *next = entry->next;
        if (entry->chan_ref) mino_unref(S, entry->chan_ref);
        free(entry);
        entry = next;
    }
    S->async_timers = NULL;
}

void async_timers_mark(mino_state_t *S)
{
    timer_entry_t *entry;
    for (entry = S->async_timers; entry != NULL;
         entry = entry->next) {
        gc_mark_interior(S, entry->chan_handle);
    }
}
