/*
 * error_diag.h -- runtime error reporting, structured diagnostics,
 * call-frame stack, source cache, and the metadata table API.
 *
 * Bodies live in runtime/error.c. Internal to the runtime;
 * embedders should only use mino.h.
 */

#ifndef RUNTIME_ERROR_DIAG_H
#define RUNTIME_ERROR_DIAG_H

#include "mino_internal.h"
#include "diag.h"
#include "runtime/runtime_types.h"   /* meta_entry_t */

#include <stddef.h>

/* set_error / set_error_at copy msg into the current ctx's error_buf; msg is borrowed. */
void        set_error(mino_state_t *S, const char *msg);          /* msg: borrowed */
void        set_error_at(mino_state_t *S, const mino_val_t *form, /* form: borrowed */
                         const char *msg);                         /* msg: borrowed */
void        clear_error(mino_state_t *S);
void        set_diag(mino_state_t *S, mino_diag_t *d);           /* d: consumed */
void        source_cache_store(mino_state_t *S, const char *file,
                               const char *text, size_t len);
const char *source_cache_get_line(mino_state_t *S, const char *file,
                                  int line, size_t *out_len);
void        set_eval_diag(mino_state_t *S, const mino_val_t *form,
                          const char *kind, const char *code,
                          const char *msg);
/* Extended variant of set_eval_diag that also attaches a `:mino/data`
 * payload (GC-owned; the runtime keeps it alive while the diag is
 * live) and an optional note. Pass NULL for either to skip. */
void        set_eval_diag_with_data(mino_state_t *S, const mino_val_t *form,
                                    const char *kind, const char *code,
                                    const char *msg, mino_val_t *data,
                                    const char *note);
const char *type_tag_str(const mino_val_t *v);                    /* static string */
void        push_frame(mino_state_t *S, const char *name,     /* name: borrowed */
                       const char *file, int line,            /* file: borrowed */
                       int column);
void        pop_frame(mino_state_t *S);
void        append_trace(mino_state_t *S);
meta_entry_t *meta_find(mino_state_t *S, const char *name);   /* borrowed into meta_table */
void meta_set(mino_state_t *S, const char *name,              /* name: borrowed (copied) */
              const char *doc, size_t doc_len,                 /* doc: borrowed (copied) */
              mino_val_t *source);                             /* source: GC-owned, retained */
/* meta_set_capability tags a registered binding with its install-group
 * label. Borrows; copies. NULL clears. */
void meta_set_capability(mino_state_t *S, const char *name,
                         const char *capability);

#endif /* RUNTIME_ERROR_DIAG_H */
