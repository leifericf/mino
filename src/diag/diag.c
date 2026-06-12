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
#include <limits.h>             /* INT_MAX for precision clamping */

/* ------------------------------------------------------------------------- */
/* Source cache                                                              */
/* ------------------------------------------------------------------------- */

void source_cache_store(mino_state *S, const char *file,
                        const char *text, size_t len)
{
    int slot = -1;
    int i;
    /* Find existing slot for this file or pick the first empty one. */
    for (i = 0; i < MINO_SOURCE_CACHE_SIZE; i++) {
        if (S->reader.source_cache[i].file == file) {
            slot = i;
            break;
        }
        if (S->reader.source_cache[i].file == NULL && slot < 0) {
            slot = i;
        }
    }
    /* Evict slot 0 if full. */
    if (slot < 0) {
        free(S->reader.source_cache[0].text);
        slot = 0;
    } else {
        free(S->reader.source_cache[slot].text);
    }
    S->reader.source_cache[slot].file = file;
    /* len + 1 must not wrap — pathological file size would otherwise
     * malloc(0) and then memcpy SIZE_MAX bytes. */
    if (len >= SIZE_MAX) {
        S->reader.source_cache[slot].text = NULL;
        S->reader.source_cache[slot].file = NULL;
        S->reader.source_cache[slot].len  = 0;
        return;
    }
    S->reader.source_cache[slot].text = (char *)malloc(len + 1);
    if (S->reader.source_cache[slot].text != NULL) {
        memcpy(S->reader.source_cache[slot].text, text, len);
        S->reader.source_cache[slot].text[len] = '\0';
        S->reader.source_cache[slot].len = len;
    } else {
        S->reader.source_cache[slot].file = NULL;
        S->reader.source_cache[slot].len = 0;
    }
}

const char *source_cache_get_line(mino_state *S, const char *file,
                                  int line, size_t *out_len)
{
    int i;
    const char *p, *end, *line_start;
    int cur;
    for (i = 0; i < MINO_SOURCE_CACHE_SIZE; i++) {
        if (S->reader.source_cache[i].file == file && S->reader.source_cache[i].text != NULL) {
            p = S->reader.source_cache[i].text;
            end = p + S->reader.source_cache[i].len;
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

mino_diag *diag_new(const char *kind, const char *code,
                      const char *phase, const char *message)
{
    mino_diag *d = (mino_diag *)calloc(1, sizeof(*d));
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

void diag_free(mino_diag *d)
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

void diag_set_span(mino_diag *d, mino_span_t span)
{
    d->has_primary_span = 1;
    d->primary_span     = span;
}

void diag_add_note(mino_diag *d, const char *message)
{
    if (d->notes_len >= d->notes_cap) {
        size_t new_cap = d->notes_cap == 0 ? 4
                       : (d->notes_cap > SIZE_MAX / 2 ? SIZE_MAX : d->notes_cap * 2);
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

void diag_set_data(mino_diag *d, mino_val *data)
{
    d->data = data;
}

void diag_capture_frames(mino_state *S, mino_diag *d)
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

int diag_render_compact(const mino_diag *d, char *buf, size_t n)
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

/* Advance pos by w bytes from snprintf, clamped so pos never exceeds n-1.
 * snprintf returns the number of bytes that *would* be written (may exceed
 * the available space), so naively adding w to pos can push pos past n,
 * causing n-pos to wrap on the next call and smash memory. */
#define DIAG_POS_ADVANCE(pos, n, w) \
    do { if ((w) > 0) { (pos) += (size_t)(w); if ((pos) >= (n)) (pos) = (n) - 1; } } while (0)

int diag_render_pretty(mino_state *S, const mino_diag *d,
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
    DIAG_POS_ADVANCE(pos, n, w);

    /* Location pointer */
    if (d->has_primary_span && d->primary_span.file != NULL
        && d->primary_span.line > 0 && pos < n) {
        const mino_span_t *sp = &d->primary_span;
        w = snprintf(buf + pos, n - pos, "  --> %s:%d:%d\n",
                     sp->file, sp->line, sp->column);
        DIAG_POS_ADVANCE(pos, n, w);

        /* Source snippet with caret */
        if (S != NULL && pos < n) {
            size_t line_len = 0;
            const char *line_text = source_cache_get_line(
                S, sp->file, sp->line, &line_len);
            if (line_text != NULL) {
                w = snprintf(buf + pos, n - pos, "   |\n");
                DIAG_POS_ADVANCE(pos, n, w);

                /* Source line: clamp line_len to INT_MAX so the %.*s
                 * precision argument (int) does not wrap to negative. */
                {
                    int prec = (line_len > (size_t)INT_MAX) ? INT_MAX : (int)line_len;
                    w = snprintf(buf + pos, n - pos, "%3d | %.*s\n",
                                 sp->line, prec, line_text);
                    DIAG_POS_ADVANCE(pos, n, w);
                }

                /* Caret line */
                if (sp->column > 0 && pos < n) {
                    int pad = sp->column - 1;
                    w = snprintf(buf + pos, n - pos, "    | ");
                    DIAG_POS_ADVANCE(pos, n, w);
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
        DIAG_POS_ADVANCE(pos, n, w);
    }

    /* Stack trace */
    if (d->frames_len > 0 && pos < n) {
        w = snprintf(buf + pos, n - pos, "stack trace:\n");
        DIAG_POS_ADVANCE(pos, n, w);
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
            DIAG_POS_ADVANCE(pos, n, w);
        }
    }

    if (pos < n) buf[pos] = '\0';
    else if (n > 0) buf[n - 1] = '\0';
    return (int)pos;
}

#undef DIAG_POS_ADVANCE

/* Public rendering dispatcher. */
int mino_render_diag(mino_state *S, const mino_diag *d,
                     int mode, char *buf, size_t n)
{
    if (mode == MINO_DIAG_RENDER_PRETTY)
        return diag_render_pretty(S, d, buf, n);
    return diag_render_compact(d, buf, n);
}

/* ------------------------------------------------------------------------- */
/* Map conversion                                                            */
/* ------------------------------------------------------------------------- */

/* Build a location map from a span.
 * keys/vals are GC-allocated and pinned so that each allocation in the
 * body cannot collect a pointer that is only on the C stack. */
static mino_val *span_to_map(mino_state *S, const mino_span_t *sp)
{
#define SPAN_N 5
    mino_val **keys = (mino_val **)gc_alloc_typed(
        S, GC_T_VALARR, SPAN_N * sizeof(*keys));
    mino_val **vals = (mino_val **)gc_alloc_typed(
        S, GC_T_VALARR, SPAN_N * sizeof(*vals));
    mino_val *result;
    gc_pin((mino_val *)keys);
    gc_pin((mino_val *)vals);
    gc_valarr_set(S, keys, 0, mino_keyword(S, "file"));
    gc_valarr_set(S, vals, 0, sp->file ? mino_string(S, sp->file) : mino_nil(S));
    gc_valarr_set(S, keys, 1, mino_keyword(S, "line"));
    gc_valarr_set(S, vals, 1, mino_int(S, sp->line));
    gc_valarr_set(S, keys, 2, mino_keyword(S, "column"));
    gc_valarr_set(S, vals, 2, mino_int(S, sp->column));
    gc_valarr_set(S, keys, 3, mino_keyword(S, "end-line"));
    gc_valarr_set(S, vals, 3, mino_int(S, sp->end_line));
    gc_valarr_set(S, keys, 4, mino_keyword(S, "end-column"));
    gc_valarr_set(S, vals, 4, mino_int(S, sp->end_column));
    result = mino_map(S, keys, vals, SPAN_N);
    gc_unpin(2);
    return result;
#undef SPAN_N
}

/* Build a frame map.
 * keys/vals are GC-allocated and pinned so that each allocation in the
 * body cannot collect a pointer that is only on the C stack. */
static mino_val *frame_to_map(mino_state *S, const mino_diag_frame_t *f)
{
#define FRAME_N 4
    mino_val **keys = (mino_val **)gc_alloc_typed(
        S, GC_T_VALARR, FRAME_N * sizeof(*keys));
    mino_val **vals = (mino_val **)gc_alloc_typed(
        S, GC_T_VALARR, FRAME_N * sizeof(*vals));
    mino_val *result;
    gc_pin((mino_val *)keys);
    gc_pin((mino_val *)vals);
    gc_valarr_set(S, keys, 0, mino_keyword(S, "fn"));
    gc_valarr_set(S, vals, 0, f->fn_name ? mino_string(S, f->fn_name) : mino_nil(S));
    gc_valarr_set(S, keys, 1, mino_keyword(S, "file"));
    gc_valarr_set(S, vals, 1, f->file ? mino_string(S, f->file) : mino_nil(S));
    gc_valarr_set(S, keys, 2, mino_keyword(S, "line"));
    gc_valarr_set(S, vals, 2, mino_int(S, f->line));
    gc_valarr_set(S, keys, 3, mino_keyword(S, "column"));
    gc_valarr_set(S, vals, 3, mino_int(S, f->column));
    result = mino_map(S, keys, vals, FRAME_N);
    gc_unpin(2);
    return result;
#undef FRAME_N
}

mino_val *diag_to_map(mino_state *S, mino_diag *d)
{
#define DIAG_MAP_N 11
    mino_val **keys;
    mino_val **vals;
    size_t n = 0;

    if (d == NULL) return mino_nil(S);

    /* Return cached map if available. */
    if (d->cached_map != NULL) return d->cached_map;

    /* Allocate key/value staging arrays on the GC heap and pin them so
     * that the many allocating calls below cannot collect pointers that
     * have been stored but are not yet reachable through a GC root. */
    keys = (mino_val **)gc_alloc_typed(
        S, GC_T_VALARR, DIAG_MAP_N * sizeof(*keys));
    vals = (mino_val **)gc_alloc_typed(
        S, GC_T_VALARR, DIAG_MAP_N * sizeof(*vals));
    gc_pin((mino_val *)keys);
    gc_pin((mino_val *)vals);

    /* :mino/kind */
    gc_valarr_set(S, keys, n, mino_keyword(S, "mino/kind"));
    gc_valarr_set(S, vals, n, d->kind ? mino_keyword(S, d->kind) : mino_nil(S));
    n++;

    /* :mino/code */
    gc_valarr_set(S, keys, n, mino_keyword(S, "mino/code"));
    gc_valarr_set(S, vals, n, d->code ? mino_string(S, d->code) : mino_nil(S));
    n++;

    /* :mino/phase */
    gc_valarr_set(S, keys, n, mino_keyword(S, "mino/phase"));
    gc_valarr_set(S, vals, n, d->phase ? mino_keyword(S, d->phase) : mino_nil(S));
    n++;

    /* :mino/message */
    gc_valarr_set(S, keys, n, mino_keyword(S, "mino/message"));
    gc_valarr_set(S, vals, n, d->message ? mino_string(S, d->message) : mino_nil(S));
    n++;

    /* :mino/summary */
    gc_valarr_set(S, keys, n, mino_keyword(S, "mino/summary"));
    gc_valarr_set(S, vals, n, d->summary ? mino_string(S, d->summary) : mino_nil(S));
    n++;

    /* :mino/expected? */
    gc_valarr_set(S, keys, n, mino_keyword(S, "mino/expected?"));
    gc_valarr_set(S, vals, n, d->expected ? mino_true(S) : mino_false(S));
    n++;

    /* :mino/location */
    gc_valarr_set(S, keys, n, mino_keyword(S, "mino/location"));
    if (d->has_primary_span) {
        gc_valarr_set(S, vals, n, span_to_map(S, &d->primary_span));
    } else {
        gc_valarr_set(S, vals, n, mino_nil(S));
    }
    n++;

    /* :mino/notes */
    gc_valarr_set(S, keys, n, mino_keyword(S, "mino/notes"));
    {
        size_t i;
        if (d->notes_len > 0) {
            mino_val **note_items;
            if (d->notes_len > SIZE_MAX / sizeof(*note_items)) {
                gc_unpin(2);
                gc_oom_throw(S, "diag notes array size overflow");
            }
            note_items = (mino_val **)gc_alloc_typed(
                S, GC_T_VALARR, d->notes_len * sizeof(*note_items));
            gc_pin((mino_val *)note_items);
            for (i = 0; i < d->notes_len; i++) {
                gc_valarr_set(S, note_items, i,
                    d->notes[i].message
                        ? mino_string(S, d->notes[i].message) : mino_nil(S));
            }
            gc_valarr_set(S, vals, n, mino_vector(S, note_items, d->notes_len));
            gc_unpin(1);
        } else {
            gc_valarr_set(S, vals, n, mino_vector(S, NULL, 0));
        }
    }
    n++;

    /* :mino/trace */
    gc_valarr_set(S, keys, n, mino_keyword(S, "mino/trace"));
    {
        size_t i;
        if (d->frames_len > 0) {
            mino_val **frame_items;
            if (d->frames_len > SIZE_MAX / sizeof(*frame_items)) {
                gc_unpin(2);
                gc_oom_throw(S, "diag frames array size overflow");
            }
            frame_items = (mino_val **)gc_alloc_typed(
                S, GC_T_VALARR, d->frames_len * sizeof(*frame_items));
            gc_pin((mino_val *)frame_items);
            for (i = 0; i < d->frames_len; i++) {
                gc_valarr_set(S, frame_items, i,
                    frame_to_map(S, &d->frames[i]));
            }
            gc_valarr_set(S, vals, n, mino_vector(S, frame_items, d->frames_len));
            gc_unpin(1);
        } else {
            gc_valarr_set(S, vals, n, mino_vector(S, NULL, 0));
        }
    }
    n++;

    /* :mino/data */
    gc_valarr_set(S, keys, n, mino_keyword(S, "mino/data"));
    gc_valarr_set(S, vals, n, d->data ? d->data : mino_nil(S));
    n++;

    /* :mino/cause */
    gc_valarr_set(S, keys, n, mino_keyword(S, "mino/cause"));
    gc_valarr_set(S, vals, n,
        d->cause ? diag_to_map(S, d->cause) : mino_nil(S));
    n++;

    d->cached_map = mino_map(S, keys, vals, n);
    gc_unpin(2);
    return d->cached_map;
#undef DIAG_MAP_N
}
