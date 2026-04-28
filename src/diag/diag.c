/*
 * diag.c -- structured diagnostic lifecycle and rendering.
 *
 * Diagnostic kinds vs internal severity classes (see diag_contract.h):
 *
 *   :eval/...      -> MINO_ERR_RECOVERABLE   (catchable user errors)
 *   :type/...      -> MINO_ERR_RECOVERABLE
 *   :arity/...     -> MINO_ERR_RECOVERABLE
 *   :user/...      -> MINO_ERR_RECOVERABLE   (raised by `throw`)
 *   :limit/steps   -> MINO_ERR_HOST          (host-set step budget)
 *   :limit/heap    -> MINO_ERR_HOST          (host-set heap budget)
 *   :host/...      -> MINO_ERR_HOST          (capability rejected the call)
 *   :io/...        -> MINO_ERR_HOST          (file / OS failure)
 *   :internal/...  -> MINO_ERR_HOST          (OOM with try frame, etc.)
 *
 * MINO_ERR_CORRUPT does not surface here -- those paths abort before
 * a diagnostic can be constructed.  See per-subsystem internal.h
 * "Error classes emitted" blocks for the abort sites.
 */

#include "runtime/internal.h"

/* ------------------------------------------------------------------------- */
/* Source cache                                                              */
/* ------------------------------------------------------------------------- */

void source_cache_store(mino_state_t *S, const char *file,
                        const char *text, size_t len)
{
    int slot = -1;
    int i;
    /* Find existing slot for this file or pick the first empty one. */
    for (i = 0; i < MINO_SOURCE_CACHE_SIZE; i++) {
        if (S->source_cache[i].file == file) {
            slot = i;
            break;
        }
        if (S->source_cache[i].file == NULL && slot < 0) {
            slot = i;
        }
    }
    /* Evict slot 0 if full. */
    if (slot < 0) {
        free(S->source_cache[0].text);
        slot = 0;
    } else {
        free(S->source_cache[slot].text);
    }
    S->source_cache[slot].file = file;
    /* len + 1 must not wrap — pathological file size would otherwise
     * malloc(0) and then memcpy SIZE_MAX bytes. */
    if (len >= SIZE_MAX) {
        S->source_cache[slot].text = NULL;
        S->source_cache[slot].file = NULL;
        S->source_cache[slot].len  = 0;
        return;
    }
    S->source_cache[slot].text = (char *)malloc(len + 1);
    if (S->source_cache[slot].text != NULL) {
        memcpy(S->source_cache[slot].text, text, len);
        S->source_cache[slot].text[len] = '\0';
        S->source_cache[slot].len = len;
    } else {
        S->source_cache[slot].file = NULL;
        S->source_cache[slot].len = 0;
    }
}

const char *source_cache_get_line(mino_state_t *S, const char *file,
                                  int line, size_t *out_len)
{
    int i;
    const char *p, *end, *line_start;
    int cur;
    for (i = 0; i < MINO_SOURCE_CACHE_SIZE; i++) {
        if (S->source_cache[i].file == file && S->source_cache[i].text != NULL) {
            p = S->source_cache[i].text;
            end = p + S->source_cache[i].len;
            cur = 1;
            while (p < end && cur < line) {
                if (*p == '\n') cur++;
                p++;
            }
            if (cur != line) return NULL;
            line_start = p;
            while (p < end && *p != '\n') p++;
            *out_len = (size_t)(p - line_start);
            return line_start;
        }
    }
    return NULL;
}

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

void diag_set_data(mino_diag_t *d, mino_val_t *data)
{
    d->data = data;
}

void diag_capture_frames(mino_state_t *S, mino_diag_t *d)
{
    int i;
    if (mino_current_ctx(S)->call_depth <= 0) return;
    free(d->frames);
    d->frames = (mino_diag_frame_t *)calloc(
        (size_t)mino_current_ctx(S)->call_depth, sizeof(*d->frames));
    if (d->frames == NULL) {
        d->frames_len = 0;
        d->frames_cap = 0;
        return;
    }
    d->frames_len = (size_t)mino_current_ctx(S)->call_depth;
    d->frames_cap = (size_t)mino_current_ctx(S)->call_depth;
    for (i = mino_current_ctx(S)->call_depth - 1; i >= 0; i--) {
        size_t idx = (size_t)(mino_current_ctx(S)->call_depth - 1 - i);
        d->frames[idx].fn_name  = mino_current_ctx(S)->call_stack[i].name;
        d->frames[idx].file     = mino_current_ctx(S)->call_stack[i].file;
        d->frames[idx].line     = mino_current_ctx(S)->call_stack[i].line;
        d->frames[idx].column   = mino_current_ctx(S)->call_stack[i].column;
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

int diag_render_pretty(mino_state_t *S, const mino_diag_t *d,
                       char *buf, size_t n)
{
    size_t pos = 0;
    size_t i;
    int w;
    if (d == NULL || buf == NULL || n == 0) return 0;

    /* Header: error[CODE]: message */
    w = snprintf(buf + pos, n - pos, "error[%s]: %s\n",
                 d->code ? d->code : "???",
                 d->message ? d->message : "error");
    if (w > 0) pos += (size_t)w;

    /* Location pointer */
    if (d->has_primary_span && d->primary_span.file != NULL
        && d->primary_span.line > 0 && pos < n) {
        const mino_span_t *sp = &d->primary_span;
        w = snprintf(buf + pos, n - pos, "  --> %s:%d:%d\n",
                     sp->file, sp->line, sp->column);
        if (w > 0) pos += (size_t)w;

        /* Source snippet with caret */
        if (S != NULL && pos < n) {
            size_t line_len = 0;
            const char *line_text = source_cache_get_line(
                S, sp->file, sp->line, &line_len);
            if (line_text != NULL) {
                int gutter_w = snprintf(buf + pos, n - pos, "   |\n");
                if (gutter_w > 0) pos += (size_t)gutter_w;

                /* Source line */
                w = snprintf(buf + pos, n - pos, "%3d | %.*s\n",
                             sp->line, (int)line_len, line_text);
                if (w > 0) pos += (size_t)w;

                /* Caret line */
                if (sp->column > 0 && pos < n) {
                    int pad = sp->column - 1;
                    w = snprintf(buf + pos, n - pos, "    | ");
                    if (w > 0) pos += (size_t)w;
                    while (pad > 0 && pos < n - 1) {
                        buf[pos++] = ' ';
                        pad--;
                    }
                    if (pos < n - 1) buf[pos++] = '^';
                    if (pos < n - 1) buf[pos++] = '\n';
                }
            }
        }
    }

    /* Notes */
    for (i = 0; i < d->notes_len && pos < n; i++) {
        w = snprintf(buf + pos, n - pos, "note: %s\n",
                     d->notes[i].message ? d->notes[i].message : "");
        if (w > 0) pos += (size_t)w;
    }

    /* Stack trace */
    if (d->frames_len > 0 && pos < n) {
        w = snprintf(buf + pos, n - pos, "stack trace:\n");
        if (w > 0) pos += (size_t)w;
        for (i = 0; i < d->frames_len && pos < n; i++) {
            const mino_diag_frame_t *f = &d->frames[i];
            if (f->file != NULL) {
                w = snprintf(buf + pos, n - pos, "  at %s (%s:%d)\n",
                             f->fn_name ? f->fn_name : "<fn>",
                             f->file, f->line);
            } else {
                w = snprintf(buf + pos, n - pos, "  at %s\n",
                             f->fn_name ? f->fn_name : "<fn>");
            }
            if (w > 0) pos += (size_t)w;
        }
    }

    if (pos < n) buf[pos] = '\0';
    else if (n > 0) buf[n - 1] = '\0';
    return (int)pos;
}

/* Public rendering dispatcher. */
int mino_render_diag(mino_state_t *S, const mino_diag_t *d,
                     int mode, char *buf, size_t n)
{
    if (mode == MINO_DIAG_RENDER_PRETTY)
        return diag_render_pretty(S, d, buf, n);
    return diag_render_compact(d, buf, n);
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
