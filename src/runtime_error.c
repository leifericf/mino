/*
 * runtime_error.c -- error reporting, call stack traces, type labels.
 *
 * Extracted from mino.c. No behavior change.
 */

#include "mino_internal.h"

/* ------------------------------------------------------------------------- */
/* Error reporting                                                           */
/* ------------------------------------------------------------------------- */

const char *mino_last_error(mino_state_t *S)
{
    return error_buf[0] ? error_buf : NULL;
}

void set_error(mino_state_t *S, const char *msg)
{
    size_t n = strlen(msg);
    if (n >= sizeof(error_buf)) {
        n = sizeof(error_buf) - 1;
    }
    memcpy(error_buf, msg, n);
    error_buf[n] = '\0';
}

void clear_error(mino_state_t *S)
{
    error_buf[0] = '\0';
}

/* Location-aware error: prepend file:line when the form has source info. */
void set_error_at(mino_state_t *S, const mino_val_t *form, const char *msg)
{
    if (form != NULL && form->type == MINO_CONS
        && form->as.cons.file != NULL && form->as.cons.line > 0) {
        char buf[2048];
        snprintf(buf, sizeof(buf), "%s:%d: %s",
                 form->as.cons.file, form->as.cons.line, msg);
        set_error(S, buf);
    } else {
        set_error(S, msg);
    }
}

/* Return a short human-readable label for a value's type. */
const char *type_tag_str(const mino_val_t *v)
{
    if (v == NULL) return "nil";
    switch (v->type) {
    case MINO_NIL:     return "nil";
    case MINO_BOOL:    return "bool";
    case MINO_INT:     return "int";
    case MINO_FLOAT:   return "float";
    case MINO_STRING:  return "string";
    case MINO_SYMBOL:  return "symbol";
    case MINO_KEYWORD: return "keyword";
    case MINO_CONS:    return "list";
    case MINO_VECTOR:  return "vector";
    case MINO_MAP:     return "map";
    case MINO_SET:     return "set";
    case MINO_PRIM:    return "fn";
    case MINO_FN:      return "fn";
    case MINO_MACRO:   return "macro";
    case MINO_HANDLE:  return "handle";
    case MINO_ATOM:    return "atom";
    case MINO_LAZY:    return "lazy-seq";
    case MINO_RECUR:     return "recur";
    case MINO_TAIL_CALL: return "tail-call";
    case MINO_REDUCED:   return "reduced";
    }
    return "unknown";
}

/* ------------------------------------------------------------------------- */
/* Call stack (for stack traces on error)                                     */
/* ------------------------------------------------------------------------- */

void push_frame(mino_state_t *S, const char *name, const char *file, int line)
{
    if (call_depth < MAX_CALL_DEPTH) {
        call_stack[call_depth].name = name;
        call_stack[call_depth].file = file;
        call_stack[call_depth].line = line;
        call_depth++;
    }
}

void pop_frame(mino_state_t *S)
{
    if (call_depth > 0) {
        call_depth--;
    }
}

/* Append the current call stack to error_buf. Called once per error. */
void append_trace(mino_state_t *S)
{
    size_t pos;
    int    i;
    if (trace_added || call_depth == 0) {
        return;
    }
    trace_added = 1;
    pos = strlen(error_buf);
    for (i = call_depth - 1; i >= 0 && pos + 80 < sizeof(error_buf); i--) {
        pos += (size_t)snprintf(
            error_buf + pos, sizeof(error_buf) - pos, "\n  in %s",
            call_stack[i].name ? call_stack[i].name : "<fn>");
        if (call_stack[i].file != NULL && pos + 40 < sizeof(error_buf)) {
            pos += (size_t)snprintf(
                error_buf + pos, sizeof(error_buf) - pos, " (%s:%d)",
                call_stack[i].file, call_stack[i].line);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Metadata                                                                  */
/* ------------------------------------------------------------------------- */

meta_entry_t *meta_find(mino_state_t *S, const char *name)
{
    size_t i;
    for (i = 0; i < meta_table_len; i++) {
        if (strcmp(meta_table[i].name, name) == 0) {
            return &meta_table[i];
        }
    }
    return NULL;
}

void meta_set(mino_state_t *S, const char *name, const char *doc,
              size_t doc_len, mino_val_t *source)
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
    if (meta_table_len == meta_table_cap) {
        size_t new_cap = meta_table_cap == 0 ? 32 : meta_table_cap * 2;
        meta_entry_t *ne = (meta_entry_t *)realloc(
            meta_table, new_cap * sizeof(*ne));
        if (ne == NULL) { return; }
        meta_table = ne;
        meta_table_cap = new_cap;
    }
    {
        size_t nlen = strlen(name);
        meta_table[meta_table_len].name = (char *)malloc(nlen + 1);
        if (meta_table[meta_table_len].name == NULL) { return; }
        memcpy(meta_table[meta_table_len].name, name, nlen + 1);
    }
    meta_table[meta_table_len].docstring = NULL;
    if (doc != NULL) {
        meta_table[meta_table_len].docstring = (char *)malloc(doc_len + 1);
        if (meta_table[meta_table_len].docstring != NULL) {
            memcpy(meta_table[meta_table_len].docstring, doc, doc_len);
            meta_table[meta_table_len].docstring[doc_len] = '\0';
        }
    }
    meta_table[meta_table_len].source = source;
    meta_table_len++;
}
