/*
 * diag.c -- structured diagnostic lifecycle and rendering.
 */

#include "mino_internal.h"

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static char *safe_strdup(const char *s)
{
    size_t n;
    char  *p;
    if (s == NULL) return NULL;
    n = strlen(s);
    p = (char *)malloc(n + 1);
    if (p != NULL) {
        memcpy(p, s, n + 1);
    }
    return p;
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

mino_diag_t *diag_new(const char *kind, const char *code,
                      const char *phase, const char *message)
{
    mino_diag_t *d = (mino_diag_t *)calloc(1, sizeof(*d));
    if (d == NULL) return NULL;
    d->kind    = safe_strdup(kind);
    d->code    = safe_strdup(code);
    d->phase   = safe_strdup(phase);
    d->summary = safe_strdup(message);
    d->message = safe_strdup(message);
    return d;
}

static void note_free(mino_diag_note_t *n)
{
    free(n->message);
    free(n->kind);
}

void diag_free(mino_diag_t *d)
{
    size_t i;
    if (d == NULL) return;
    free(d->kind);
    free(d->code);
    free(d->phase);
    free(d->summary);
    free(d->message);
    for (i = 0; i < d->notes_len; i++) {
        note_free(&d->notes[i]);
    }
    free(d->notes);
    free(d->secondary_spans);
    free(d->frames);
    if (d->cause != NULL) {
        diag_free(d->cause);
    }
    free(d);
}

/* ------------------------------------------------------------------------- */
/* Mutators                                                                  */
/* ------------------------------------------------------------------------- */

void diag_set_span(mino_diag_t *d, mino_span_t span)
{
    d->has_primary_span = 1;
    d->primary_span     = span;
}

void diag_add_note(mino_diag_t *d, const char *message)
{
    if (d->notes_len >= d->notes_cap) {
        size_t new_cap = d->notes_cap == 0 ? 4 : d->notes_cap * 2;
        mino_diag_note_t *p = (mino_diag_note_t *)realloc(
            d->notes, new_cap * sizeof(*p));
        if (p == NULL) return;
        d->notes     = p;
        d->notes_cap = new_cap;
    }
    memset(&d->notes[d->notes_len], 0, sizeof(d->notes[0]));
    d->notes[d->notes_len].message  = safe_strdup(message);
    d->notes[d->notes_len].kind     = safe_strdup("note");
    d->notes[d->notes_len].has_span = 0;
    d->notes_len++;
}

void diag_add_note_at(mino_diag_t *d, const char *message, mino_span_t span)
{
    diag_add_note(d, message);
    if (d->notes_len > 0) {
        d->notes[d->notes_len - 1].span     = span;
        d->notes[d->notes_len - 1].has_span = 1;
    }
}

void diag_set_data(mino_diag_t *d, mino_val_t *data)
{
    d->data = data;
}

void diag_set_cause(mino_diag_t *d, mino_diag_t *cause)
{
    if (d->cause != NULL) {
        diag_free(d->cause);
    }
    d->cause = cause;
}

void diag_capture_frames(mino_state_t *S, mino_diag_t *d)
{
    int i;
    if (S->call_depth <= 0) return;
    free(d->frames);
    d->frames = (mino_diag_frame_t *)calloc(
        (size_t)S->call_depth, sizeof(*d->frames));
    if (d->frames == NULL) {
        d->frames_len = 0;
        d->frames_cap = 0;
        return;
    }
    d->frames_len = (size_t)S->call_depth;
    d->frames_cap = (size_t)S->call_depth;
    for (i = S->call_depth - 1; i >= 0; i--) {
        size_t idx = (size_t)(S->call_depth - 1 - i);
        d->frames[idx].fn_name  = S->call_stack[i].name;
        d->frames[idx].file     = S->call_stack[i].file;
        d->frames[idx].line     = S->call_stack[i].line;
        d->frames[idx].column   = S->call_stack[i].column;
        d->frames[idx].is_macro = 0;
        d->frames[idx].is_host  = 0;
    }
}

/* ------------------------------------------------------------------------- */
/* Rendering                                                                 */
/* ------------------------------------------------------------------------- */

int diag_render_compact(const mino_diag_t *d, char *buf, size_t n)
{
    int written;
    if (d == NULL || buf == NULL || n == 0) return 0;

    if (d->has_primary_span && d->primary_span.file != NULL
        && d->primary_span.line > 0) {
        written = snprintf(buf, n, "%s:%d: %s",
                           d->primary_span.file,
                           d->primary_span.line,
                           d->message ? d->message : "error");
    } else {
        written = snprintf(buf, n, "%s",
                           d->message ? d->message : "error");
    }
    return written < 0 ? 0 : written;
}
