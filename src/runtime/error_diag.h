/*
 * error_diag.h -- runtime error reporting, structured diagnostics,
 * call-frame stack, source cache, and the metadata table API.
 *
 * Bodies live in state/error.c. Internal to the runtime;
 * embedders should only use mino.h.
 */

#ifndef RUNTIME_ERROR_DIAG_H
#define RUNTIME_ERROR_DIAG_H

#include "mino_internal.h"
#include "diag.h"
#include "runtime/runtime_types.h"   /* meta_entry_t */

#include <stddef.h>

/* set_error copies msg into the current ctx's error_buf; msg is borrowed. */
void        set_error(mino_state *S, const char *msg);          /* msg: borrowed */
void        clear_error(mino_state *S);
void        set_diag(mino_state *S, mino_diag *d);           /* d: consumed */
void        source_cache_store(mino_state *S, const char *file,
                               const char *text, size_t len);
void        set_eval_diag(mino_state *S, const mino_val *form,
                          const char *kind, const char *code,
                          const char *msg);
/* Extended variant of set_eval_diag that also attaches a `:mino/data`
 * payload (GC-owned; the runtime keeps it alive while the diag is
 * live) and an optional note. Pass NULL for either to skip. */
void        set_eval_diag_with_data(mino_state *S, const mino_val *form,
                                    const char *kind, const char *code,
                                    const char *msg, mino_val *data,
                                    const char *note);
/* Throw a classified catchable exception (kind + code + msg, optional
 * :mino/data payload in the _data form). Inside a try frame this does
 * NOT return: the diagnostic map is delivered to the matching catch
 * via longjmp. Outside any try frame it sets the diagnostic, appends
 * the trace, and returns NULL for the caller to propagate. Any layer
 * may raise through these; they live beside the error machinery. */
mino_val *throw_classified(mino_state *S, const char *kind,
                           const char *code, const char *msg);
mino_val *throw_classified_data(mino_state *S, const char *kind,
                                const char *code, const char *msg,
                                mino_val *data);
const char *type_tag_str(const mino_val *v);                    /* static string */
void        push_frame(mino_state *S, const char *name,     /* name: borrowed */
                       const char *file, int line,            /* file: borrowed */
                       int column);
void        pop_frame(mino_state *S);
void        append_trace(mino_state *S);
meta_entry_t *meta_find(mino_state *S, const char *name);   /* borrowed into meta_table */
void meta_set(mino_state *S, const char *name,              /* name: borrowed (copied) */
              const char *doc, size_t doc_len,                 /* doc: borrowed (copied) */
              mino_val *source);                             /* source: GC-owned, retained */
/* meta_set_capability tags a registered binding with its install-group
 * label. Borrows; copies. NULL clears. */
void meta_set_capability(mino_state *S, const char *name,
                         const char *capability);

#endif /* RUNTIME_ERROR_DIAG_H */
