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

/* ------------------------------------------------------------------------- */
/* Map conversion                                                            */
/* ------------------------------------------------------------------------- */

/* Build a location map from a span. */
static mino_val_t *span_to_map(mino_state_t *S, const mino_span_t *sp)
{
    mino_val_t *keys[5], *vals[5];
    size_t n = 0;
    keys[n] = mino_keyword(S, "file");
    vals[n] = sp->file ? mino_string(S, sp->file) : mino_nil(S);
    n++;
    keys[n] = mino_keyword(S, "line");
    vals[n] = mino_int(S, sp->line);
    n++;
    keys[n] = mino_keyword(S, "column");
    vals[n] = mino_int(S, sp->column);
    n++;
    keys[n] = mino_keyword(S, "end-line");
    vals[n] = mino_int(S, sp->end_line);
    n++;
    keys[n] = mino_keyword(S, "end-column");
    vals[n] = mino_int(S, sp->end_column);
    n++;
    return mino_map(S, keys, vals, n);
}

/* Build a frame map. */
static mino_val_t *frame_to_map(mino_state_t *S, const mino_diag_frame_t *f)
{
    mino_val_t *keys[4], *vals[4];
    size_t n = 0;
    keys[n] = mino_keyword(S, "fn");
    vals[n] = f->fn_name ? mino_string(S, f->fn_name) : mino_nil(S);
    n++;
    keys[n] = mino_keyword(S, "file");
    vals[n] = f->file ? mino_string(S, f->file) : mino_nil(S);
    n++;
    keys[n] = mino_keyword(S, "line");
    vals[n] = mino_int(S, f->line);
    n++;
    keys[n] = mino_keyword(S, "column");
    vals[n] = mino_int(S, f->column);
    n++;
    return mino_map(S, keys, vals, n);
}

mino_val_t *diag_to_map(mino_state_t *S, mino_diag_t *d)
{
    mino_val_t *keys[11], *vals[11];
    size_t n = 0;

    if (d == NULL) return mino_nil(S);

    /* Return cached map if available. */
    if (d->cached_map != NULL) return d->cached_map;

    /* :mino/kind */
    keys[n] = mino_keyword(S, "mino/kind");
    vals[n] = d->kind ? mino_keyword(S, d->kind) : mino_nil(S);
    n++;

    /* :mino/code */
    keys[n] = mino_keyword(S, "mino/code");
    vals[n] = d->code ? mino_string(S, d->code) : mino_nil(S);
    n++;

    /* :mino/phase */
    keys[n] = mino_keyword(S, "mino/phase");
    vals[n] = d->phase ? mino_keyword(S, d->phase) : mino_nil(S);
    n++;

    /* :mino/message */
    keys[n] = mino_keyword(S, "mino/message");
    vals[n] = d->message ? mino_string(S, d->message) : mino_nil(S);
    n++;

    /* :mino/summary */
    keys[n] = mino_keyword(S, "mino/summary");
    vals[n] = d->summary ? mino_string(S, d->summary) : mino_nil(S);
    n++;

    /* :mino/expected? */
    keys[n] = mino_keyword(S, "mino/expected?");
    vals[n] = d->expected ? mino_true(S) : mino_false(S);
    n++;

    /* :mino/location */
    keys[n] = mino_keyword(S, "mino/location");
    if (d->has_primary_span) {
        vals[n] = span_to_map(S, &d->primary_span);
    } else {
        vals[n] = mino_nil(S);
    }
    n++;

    /* :mino/notes */
    keys[n] = mino_keyword(S, "mino/notes");
    {
        mino_val_t **note_items = NULL;
        size_t i;
        if (d->notes_len > 0) {
            note_items = (mino_val_t **)gc_alloc_typed(
                S, GC_T_VALARR, d->notes_len * sizeof(*note_items));
            for (i = 0; i < d->notes_len; i++) {
                note_items[i] = d->notes[i].message
                    ? mino_string(S, d->notes[i].message) : mino_nil(S);
            }
            vals[n] = mino_vector(S, note_items, d->notes_len);
        } else {
            vals[n] = mino_vector(S, NULL, 0);
        }
    }
    n++;

    /* :mino/trace */
    keys[n] = mino_keyword(S, "mino/trace");
    {
        mino_val_t **frame_items = NULL;
        size_t i;
        if (d->frames_len > 0) {
            frame_items = (mino_val_t **)gc_alloc_typed(
                S, GC_T_VALARR, d->frames_len * sizeof(*frame_items));
            for (i = 0; i < d->frames_len; i++) {
                frame_items[i] = frame_to_map(S, &d->frames[i]);
            }
            vals[n] = mino_vector(S, frame_items, d->frames_len);
        } else {
            vals[n] = mino_vector(S, NULL, 0);
        }
    }
    n++;

    /* :mino/data */
    keys[n] = mino_keyword(S, "mino/data");
    vals[n] = d->data ? d->data : mino_nil(S);
    n++;

    /* :mino/cause */
    keys[n] = mino_keyword(S, "mino/cause");
    vals[n] = d->cause ? diag_to_map(S, d->cause) : mino_nil(S);
    n++;

    d->cached_map = mino_map(S, keys, vals, n);
    return d->cached_map;
}
