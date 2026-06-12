/*
 * error.c -- error reporting, call stack traces, type labels.
 */

#include "runtime/internal.h"
#include "eval/bc/internal.h"

/* ------------------------------------------------------------------------- */
/* Error reporting                                                           */
/* ------------------------------------------------------------------------- */

const char *mino_last_error(mino_state *S)
{
    return mino_current_ctx(S)->error_buf[0] ? mino_current_ctx(S)->error_buf : NULL;
}

const mino_diag *mino_last_diag(mino_state *S)
{
    return mino_current_ctx(S)->last_diag;
}

mino_val *mino_last_error_map(mino_state *S)
{
    if (mino_current_ctx(S)->last_diag == NULL) return mino_nil(S);
    return diag_to_map(S, mino_current_ctx(S)->last_diag);
}

const char *mino_error_kind(mino_state *S)
{
    const mino_diag *d = mino_current_ctx(S)->last_diag;
    return (d != NULL) ? d->kind : NULL;
}

const char *mino_error_code(mino_state *S)
{
    const mino_diag *d = mino_current_ctx(S)->last_diag;
    return (d != NULL) ? d->code : NULL;
}

void mino_clear_error(mino_state *S)
{
    clear_error(S);
}

/* Install a fully-built diagnostic as the current error. Takes ownership
 * of d. Renders a compact form into error_buf for backward compat. */
void set_diag(mino_state *S, mino_diag *d)
{
    if (mino_current_ctx(S)->last_diag != NULL) {
        diag_free(mino_current_ctx(S)->last_diag);
    }
    mino_current_ctx(S)->last_diag = d;
    if (d != NULL) {
        diag_render_compact(d, mino_current_ctx(S)->error_buf, sizeof(mino_current_ctx(S)->error_buf));
    } else {
        mino_current_ctx(S)->error_buf[0] = '\0';
    }
}

void set_error(mino_state *S, const char *msg)
{
    mino_diag *d;
    size_t n = strlen(msg);
    if (n >= sizeof(mino_current_ctx(S)->error_buf)) {
        n = sizeof(mino_current_ctx(S)->error_buf) - 1;
    }
    memcpy(mino_current_ctx(S)->error_buf, msg, n);
    mino_current_ctx(S)->error_buf[n] = '\0';

    /* Build a minimal unclassified diagnostic alongside. */
    d = diag_new("internal", "MIN001", "eval", msg);
    if (mino_current_ctx(S)->last_diag != NULL) diag_free(mino_current_ctx(S)->last_diag);
    mino_current_ctx(S)->last_diag = d;
}

void clear_error(mino_state *S)
{
    mino_current_ctx(S)->error_buf[0] = '\0';
    if (mino_current_ctx(S)->last_diag != NULL) {
        diag_free(mino_current_ctx(S)->last_diag);
        mino_current_ctx(S)->last_diag = NULL;
    }
}

/* Location-aware error: prepend file:line when the form has source info. */
void set_error_at(mino_state *S, const mino_val *form, const char *msg)
{
    if (form != NULL && mino_type_of(form) == MINO_CONS
        && form->as.cons.file != NULL && form->as.cons.line > 0) {
        char buf[2048];
        mino_span_t span;
        snprintf(buf, sizeof(buf), "%s:%d: %s",
                 form->as.cons.file, form->as.cons.line, msg);
        set_error(S, buf);
        /* Enrich the diagnostic with source span. */
        if (mino_current_ctx(S)->last_diag != NULL) {
            memset(&span, 0, sizeof(span));
            span.file   = form->as.cons.file;
            span.line   = form->as.cons.line;
            span.column = form->as.cons.column;
            diag_set_span(mino_current_ctx(S)->last_diag, span);
        }
    } else {
        set_error(S, msg);
    }
}

/* Classified eval-phase diagnostic from a form with source info. */
void set_eval_diag(mino_state *S, const mino_val *form,
                   const char *kind, const char *code, const char *msg)
{
    set_eval_diag_with_data(S, form, kind, code, msg, NULL, NULL);
}

/* Extended variant: attach a data payload and an optional note. Used
 * by the capability-aware MNS002 diagnostic so the unbound-symbol
 * error carries `{:capability ... :symbol ... :reason :not-installed
 * :enable-via "..."}` and a follow-on hint pointing at the C install
 * entry point. Falls back to set_eval_diag's behaviour when the
 * extension fields are NULL. */
void set_eval_diag_with_data(mino_state *S, const mino_val *form,
                             const char *kind, const char *code,
                             const char *msg, mino_val *data,
                             const char *note)
{
    /* Inside a try block, convert diagnostics to thrown exceptions so
     * they are catchable by the surrounding catch clause. Build a
     * diagnostic map with classification, message, optional data, and
     * a :mino/location span when the form or the bc cursor knows the
     * source position. Mirrors the shape prim_throw_classified emits
     * so catch handlers see one consistent diagnostic schema. */
    if (mino_current_ctx(S)->try_depth > 0) {
        const char *loc_file = NULL;
        int         loc_line = 0;
        int         loc_col  = 0;
        if (form != NULL && mino_type_of(form) == MINO_CONS
            && form->as.cons.file != NULL && form->as.cons.line > 0) {
            loc_file = form->as.cons.file;
            loc_line = form->as.cons.line;
            loc_col  = form->as.cons.column;
        } else {
            const mino_bc_fn_t *cur_bc = mino_current_ctx(S)->bc_current_bc;
            size_t              cur_pc = mino_current_ctx(S)->bc_current_pc;
            if (cur_bc != NULL) {
                (void)mino_bc_source_lookup(cur_bc, cur_pc,
                                            &loc_file, &loc_line, &loc_col);
            }
        }
        mino_val *keys[7], *vals[7];
        size_t      n = 0;
        int         npin = 0; /* tracks live gc_pin count for gc_unpin */
        /* Each mino_keyword/mino_string/mino_int/mino_map call can trigger
         * GC.  Pin every key/val as it is produced so earlier entries are
         * not swept while later ones are being allocated.  All pins are
         * released with a single gc_unpin just before mino_map consumes
         * the completed arrays. */
        keys[n] = mino_keyword(S, "mino/kind");    gc_pin(keys[n]); npin++;
        vals[n] = mino_keyword(S, kind);            gc_pin(vals[n]); npin++;
        n++;
        keys[n] = mino_keyword(S, "mino/code");    gc_pin(keys[n]); npin++;
        vals[n] = mino_string(S, code);             gc_pin(vals[n]); npin++;
        n++;
        keys[n] = mino_keyword(S, "mino/phase");   gc_pin(keys[n]); npin++;
        vals[n] = mino_keyword(S, "eval");          gc_pin(vals[n]); npin++;
        n++;
        keys[n] = mino_keyword(S, "mino/message"); gc_pin(keys[n]); npin++;
        vals[n] = mino_string(S, msg);              gc_pin(vals[n]); npin++;
        n++;
        keys[n] = mino_keyword(S, "mino/data");    gc_pin(keys[n]); npin++;
        vals[n] = data != NULL ? data : mino_nil(S); gc_pin(vals[n]); npin++;
        n++;
        if (loc_file != NULL && loc_line > 0) {
            mino_val *lkeys[3], *lvals[3];
            lkeys[0] = mino_keyword(S, "file");    gc_pin(lkeys[0]);
            lvals[0] = mino_string(S, loc_file);   gc_pin(lvals[0]);
            lkeys[1] = mino_keyword(S, "line");    gc_pin(lkeys[1]);
            lvals[1] = mino_int(S, loc_line);      gc_pin(lvals[1]);
            lkeys[2] = mino_keyword(S, "column");  gc_pin(lkeys[2]);
            lvals[2] = mino_int(S, loc_col);       gc_pin(lvals[2]);
            keys[n] = mino_keyword(S, "mino/location"); gc_pin(keys[n]);
            vals[n] = mino_map(S, lkeys, lvals, 3);
            gc_unpin(7); /* lkeys[0..2], lvals[0..2], keys[n] */
            gc_pin(vals[n]); npin++;
            n++;
        }
        gc_unpin(npin); /* all keys[] and vals[] entries */
        mino_val *ex = mino_map(S, keys, vals, n);
        (void)note;
        mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth - 1].exception = ex;
        longjmp(mino_current_ctx(S)->try_stack[mino_current_ctx(S)->try_depth - 1].buf, 1);
    }
    {
        mino_diag *d = diag_new(kind, code, "eval", msg);
        if (d != NULL && form != NULL && mino_type_of(form) == MINO_CONS
            && form->as.cons.file != NULL && form->as.cons.line > 0) {
            mino_span_t span;
            memset(&span, 0, sizeof(span));
            span.file   = form->as.cons.file;
            span.line   = form->as.cons.line;
            span.column = form->as.cons.column;
            diag_set_span(d, span);
        } else if (d != NULL && !d->has_primary_span) {
            /* Form lacked source info -- fall back to the bc cursor.
             * Resolves the precise pc that was executing when the
             * diagnostic was raised, regardless of whether the bc
             * frame's enclosing eval_current_form had been refreshed
             * yet. Native tiers populate the same cursor so JIT'd
             * errors inherit this attribution path. */
            const mino_bc_fn_t *cur_bc = mino_current_ctx(S)->bc_current_bc;
            size_t              cur_pc = mino_current_ctx(S)->bc_current_pc;
            const char         *file   = NULL;
            int                 line   = 0;
            int                 column = 0;
            if (cur_bc != NULL
                && mino_bc_source_lookup(cur_bc, cur_pc,
                                         &file, &line, &column)) {
                mino_span_t span;
                memset(&span, 0, sizeof(span));
                span.file   = file;
                span.line   = line;
                span.column = column;
                diag_set_span(d, span);
            }
        }
        if (d != NULL && data != NULL) {
            diag_set_data(d, data);
        }
        if (d != NULL && note != NULL && note[0] != '\0') {
            diag_add_note(d, note);
        }
        set_diag(S, d);
    }
}

/* Return a short human-readable label for a value's type. */
const char *type_tag_str(const mino_val *v)
{
    if (v == NULL) return "nil";
    switch (mino_type_of(v)) {
    case MINO_NIL:     return "nil";
    case MINO_BOOL:    return "bool";
    case MINO_INT:     return "int";
    case MINO_FLOAT:   return "float";
    case MINO_FLOAT32: return "float32";
    case MINO_CHAR:    return "char";
    case MINO_STRING:  return "string";
    case MINO_SYMBOL:  return "symbol";
    case MINO_KEYWORD:    return "keyword";
    case MINO_EMPTY_LIST: return "list";
    case MINO_CONS:       return "list";
    case MINO_VECTOR:  return "vector";
    case MINO_MAP:     return "map";
    case MINO_SET:        return "set";
    case MINO_SORTED_MAP: return "sorted-map";
    case MINO_SORTED_SET: return "sorted-set";
    case MINO_PRIM:    return "fn";
    case MINO_FN:      return "fn";
    case MINO_MACRO:   return "macro";
    case MINO_HANDLE:  return "handle";
    case MINO_ATOM:    return "atom";
    case MINO_VOLATILE: return "volatile";
    case MINO_LAZY:    return "lazy-seq";
    case MINO_CHUNK:   return "chunk";
    case MINO_CHUNKED_CONS: return "list";
    case MINO_RECUR:     return "recur";
    case MINO_TAIL_CALL: return "tail-call";
    case MINO_REDUCED:   return "reduced";
    case MINO_VAR:       return "var";
    case MINO_TRANSIENT: return "transient";
    case MINO_BIGINT:    return "bigint";
    case MINO_RATIO:     return "ratio";
    case MINO_BIGDEC:    return "bigdec";
    case MINO_TYPE:      return "record-type";
    case MINO_RECORD:    return "record";
    case MINO_FUTURE:    return "future";
    case MINO_UUID:      return "uuid";
    case MINO_REGEX:     return "regex";
    case MINO_HOST_ARRAY: return "host-array";
    case MINO_MAP_ENTRY: return "map-entry";
    case MINO_TX_REF:    return "ref";
    case MINO_AGENT:     return "agent";
    case MINO_CHAN:      return "chan";
    case MINO_QUEUE:     return "queue";
    case MINO_BYTES:     return "bytes";
    }
    return "unknown";
}

/* ------------------------------------------------------------------------- */
/* Call stack (for stack traces on error)                                     */
/* ------------------------------------------------------------------------- */

void push_frame(mino_state *S, const char *name, const char *file,
                int line, int column)
{
    if (mino_current_ctx(S)->call_depth < MAX_CALL_DEPTH) {
        mino_current_ctx(S)->call_stack[mino_current_ctx(S)->call_depth].name   = name;
        mino_current_ctx(S)->call_stack[mino_current_ctx(S)->call_depth].file   = file;
        mino_current_ctx(S)->call_stack[mino_current_ctx(S)->call_depth].line   = line;
        mino_current_ctx(S)->call_stack[mino_current_ctx(S)->call_depth].column = column;
        mino_current_ctx(S)->call_depth++;
    }
}

void pop_frame(mino_state *S)
{
    if (mino_current_ctx(S)->call_depth > 0) {
        mino_current_ctx(S)->call_depth--;
    }
}

/* Append the current call stack to error_buf. Called once per error. */
void append_trace(mino_state *S)
{
    size_t pos;
    int    i;
    if (mino_current_ctx(S)->trace_added || mino_current_ctx(S)->call_depth == 0) {
        return;
    }
    mino_current_ctx(S)->trace_added = 1;
    pos = strlen(mino_current_ctx(S)->error_buf);
    for (i = mino_current_ctx(S)->call_depth - 1; i >= 0 && pos + 80 < sizeof(mino_current_ctx(S)->error_buf); i--) {
        pos += (size_t)snprintf(
            mino_current_ctx(S)->error_buf + pos, sizeof(mino_current_ctx(S)->error_buf) - pos, "\n  in %s",
            mino_current_ctx(S)->call_stack[i].name ? mino_current_ctx(S)->call_stack[i].name : "<fn>");
        if (mino_current_ctx(S)->call_stack[i].file != NULL && pos + 40 < sizeof(mino_current_ctx(S)->error_buf)) {
            pos += (size_t)snprintf(
                mino_current_ctx(S)->error_buf + pos, sizeof(mino_current_ctx(S)->error_buf) - pos, " (%s:%d)",
                mino_current_ctx(S)->call_stack[i].file, mino_current_ctx(S)->call_stack[i].line);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Metadata                                                                  */
/* ------------------------------------------------------------------------- */

meta_entry_t *meta_find(mino_state *S, const char *name)
{
    size_t i;
    for (i = 0; i < S->module.meta_table_len; i++) {
        if (strcmp(S->module.meta_table[i].name, name) == 0) {
            return &S->module.meta_table[i];
        }
    }
    return NULL;
}

void meta_set(mino_state *S, const char *name, const char *doc,
              size_t doc_len, mino_val *source)
{
    meta_entry_t *e = meta_find(S, name);
    if (e != NULL) {
        free(e->docstring);
        e->docstring = NULL;
        if (doc != NULL) {
            e->docstring = (char *)malloc(doc_len + 1);
            if (e->docstring != NULL) {
                memcpy(e->docstring, doc, doc_len);
                e->docstring[doc_len] = '\0';
            }
        }
        e->source = source;
        return;
    }
    if (S->module.meta_table_len == S->module.meta_table_cap) {
        size_t new_cap = S->module.meta_table_cap == 0 ? 32 : S->module.meta_table_cap * 2;
        meta_entry_t *ne = (meta_entry_t *)realloc(
            S->module.meta_table, new_cap * sizeof(*ne));
        if (ne == NULL) { return; }
        S->module.meta_table = ne;
        S->module.meta_table_cap = new_cap;
    }
    {
        size_t nlen = strlen(name);
        S->module.meta_table[S->module.meta_table_len].name = (char *)malloc(nlen + 1);
        if (S->module.meta_table[S->module.meta_table_len].name == NULL) { return; }
        memcpy(S->module.meta_table[S->module.meta_table_len].name, name, nlen + 1);
    }
    S->module.meta_table[S->module.meta_table_len].docstring = NULL;
    if (doc != NULL) {
        S->module.meta_table[S->module.meta_table_len].docstring = (char *)malloc(doc_len + 1);
        if (S->module.meta_table[S->module.meta_table_len].docstring != NULL) {
            memcpy(S->module.meta_table[S->module.meta_table_len].docstring, doc, doc_len);
            S->module.meta_table[S->module.meta_table_len].docstring[doc_len] = '\0';
        }
    }
    S->module.meta_table[S->module.meta_table_len].capability = NULL;
    S->module.meta_table[S->module.meta_table_len].source = source;
    S->module.meta_table_len++;
}

void meta_set_capability(mino_state *S, const char *name,
                         const char *capability)
{
    meta_entry_t *e = meta_find(S, name);
    if (e == NULL) { return; }
    free(e->capability);
    e->capability = NULL;
    if (capability != NULL) {
        size_t cl = strlen(capability);
        e->capability = (char *)malloc(cl + 1);
        if (e->capability != NULL) {
            memcpy(e->capability, capability, cl + 1);
        }
    }
}
