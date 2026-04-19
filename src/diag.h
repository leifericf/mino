/*
 * diag.h -- structured diagnostic types for error reporting.
 *
 * Internal header included by mino_internal.h. Not part of the public API.
 * The public API exposes mino_diag_t as an opaque struct through mino.h.
 */

#ifndef MINO_DIAG_H
#define MINO_DIAG_H

#include <stddef.h>

/* This header is included from mino_internal.h, after mino.h.
 * mino_val_t, mino_state_t, and mino_diag_t are typedef'd in mino.h. */

/* ------------------------------------------------------------------------- */
/* Source span                                                               */
/* ------------------------------------------------------------------------- */

typedef struct {
    const char *file;       /* borrowed: interned filename or static string */
    int         line;
    int         column;
    int         end_line;
    int         end_column;
} mino_span_t;

/* ------------------------------------------------------------------------- */
/* Stack frame                                                               */
/* ------------------------------------------------------------------------- */

typedef struct {
    const char *fn_name;    /* borrowed: function name */
    const char *file;       /* borrowed: source file */
    int         line;
    int         column;
    unsigned char is_macro;
    unsigned char is_host;
} mino_diag_frame_t;

/* ------------------------------------------------------------------------- */
/* Note                                                                      */
/* ------------------------------------------------------------------------- */

typedef struct {
    char       *message;    /* malloc-owned */
    char       *kind;       /* malloc-owned: "hint", "note", etc. */
    mino_span_t span;
    unsigned char has_span;
} mino_diag_note_t;

/* ------------------------------------------------------------------------- */
/* Diagnostic                                                                */
/* ------------------------------------------------------------------------- */

struct mino_diag {
    char       *kind;           /* malloc-owned: "reader", "eval/type", etc. */
    char       *code;           /* malloc-owned: "MRE001", etc. */
    char       *phase;          /* malloc-owned: "read", "eval", etc. */
    char       *summary;        /* malloc-owned: short message */
    char       *message;        /* malloc-owned: full message */
    unsigned char expected;     /* 1 = expected/normal failure */

    unsigned char       has_primary_span;
    mino_span_t         primary_span;

    mino_diag_note_t   *notes;          /* malloc-owned array */
    size_t              notes_len;
    size_t              notes_cap;

    mino_span_t        *secondary_spans; /* malloc-owned array */
    size_t              secondary_spans_len;

    mino_diag_frame_t  *frames;         /* malloc-owned array */
    size_t              frames_len;
    size_t              frames_cap;

    mino_val_t         *data;           /* GC-owned payload (may be NULL) */
    struct mino_diag   *cause;          /* malloc-owned chained cause */
    mino_val_t         *cached_map;     /* GC-owned lazily-built map */
};

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

/* Create a new diagnostic. All strings are copied. Returns NULL on OOM. */
mino_diag_t *diag_new(const char *kind, const char *code,
                      const char *phase, const char *message);

/* Free a diagnostic and all owned memory (recurses into cause). */
void diag_free(mino_diag_t *d);

/* ------------------------------------------------------------------------- */
/* Mutators                                                                  */
/* ------------------------------------------------------------------------- */

/* Set the primary source span. */
void diag_set_span(mino_diag_t *d, mino_span_t span);

/* Append a note. message is copied. */
void diag_add_note(mino_diag_t *d, const char *message);

/* Append a note with a span. message is copied. */
void diag_add_note_at(mino_diag_t *d, const char *message, mino_span_t span);

/* Set the user data payload (GC-owned, caller must ensure it is pinned). */
void diag_set_data(mino_diag_t *d, mino_val_t *data);

/* Set the chained cause (takes ownership of cause). */
void diag_set_cause(mino_diag_t *d, mino_diag_t *cause);

/* Copy call stack frames from the runtime state into the diagnostic. */
void diag_capture_frames(mino_state_t *S, mino_diag_t *d);

/* ------------------------------------------------------------------------- */
/* Rendering                                                                 */
/* ------------------------------------------------------------------------- */

/* Render a compact one-line form into buf. Returns bytes written (excl NUL).
 * Output is truncated if it exceeds n-1 bytes. */
int diag_render_compact(const mino_diag_t *d, char *buf, size_t n);

/* Render a pretty multi-line diagnostic with source snippet and caret. */
int diag_render_pretty(mino_state_t *S, const mino_diag_t *d,
                       char *buf, size_t n);

/* ------------------------------------------------------------------------- */
/* Map conversion                                                            */
/* ------------------------------------------------------------------------- */

/* Convert a diagnostic to a mino map with canonical :mino/kind etc. keys.
 * The result is GC-owned and cached on d->cached_map. */
mino_val_t *diag_to_map(mino_state_t *S, mino_diag_t *d);

#endif /* MINO_DIAG_H */
