/*
 * runtime/reader_printer_state.h -- per-state reader + printer state.
 *
 * Reader position, dialect / read-cond knobs, the filename and
 * var-name intern tables, and the small source cache used by
 * diagnostic rendering. Also carries the printer's recursion-depth
 * counter.
 *
 * Internal to the runtime; embedders should only use mino.h.
 */

#ifndef RUNTIME_READER_PRINTER_STATE_H
#define RUNTIME_READER_PRINTER_STATE_H

#include "mino_internal.h"

#include <stddef.h>

#define MINO_SOURCE_CACHE_SIZE 4

typedef struct reader_printer_state {
    /* Reader. print_depth stays inline in mino_state so it packs with
     * caps_installed and the sub-struct starts at an 8-aligned offset
     * with its first pointer field (reader_file). */
    const char     *reader_file;
    int             reader_line;
    int             reader_col;
    const char     *reader_dialect;   /* "mino" */
    /* :read-cond mode for reader conditionals.
     *   0 = allow (default; match reader_dialect / default)
     *   1 = preserve (return a reader-conditional record)
     *   2 = disallow (error on any #? or #?@) */
    int             reader_cond_mode;
    /* Transient flag: set by `#?(...)` when no branch matched so the
     * read returned NULL silently. */
    int             reader_last_cond_empty;
    /* Nonzero while inside a #(...) body; nesting them is a reader
     * error because the inner form's % slots would be ambiguous. */
    int             reader_in_anon_fn;

    /* Filename intern table. Strings are malloc-owned, freed at state
     * teardown. Held here (not process-global) so two runtimes on two
     * threads don't race on a shared table. */
    const char    **interned_files;
    size_t          interned_files_len;
    size_t          interned_files_cap;

    /* Var-name intern table (ns + name for MINO_VAR). Same rationale
     * as interned_files: strings outlive the state's vars but not the
     * state itself. */
    const char    **interned_var_strs;
    size_t          interned_var_strs_len;
    size_t          interned_var_strs_cap;
    /* Open-addressing hash mirror over interned_var_strs, keyed on the
     * string contents. Each slot stores the canonical pointer; NULL
     * marks an empty slot. cap is always a power of two; resize when
     * load factor exceeds 0.7. */
    const char    **interned_var_strs_hash;
    size_t          interned_var_strs_hash_cap;

    /* Source cache for diagnostic rendering. */
    struct {
        const char *file;   /* interned filename */
        char       *text;   /* malloc-owned full source text */
        size_t      len;    /* length of text */
    } source_cache[MINO_SOURCE_CACHE_SIZE];
} reader_printer_state_t;

#endif /* RUNTIME_READER_PRINTER_STATE_H */
