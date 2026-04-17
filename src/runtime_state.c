/*
 * runtime_state.c -- state lifecycle, refs, public eval entry points,
 *                    execution limits, fault injection, interrupt.
 *
 * Extracted from mino.c. No behavior change.
 */

#include "mino_internal.h"

/* ------------------------------------------------------------------------- */
/* State lifecycle                                                           */
/* ------------------------------------------------------------------------- */

static void state_init(mino_state_t *S)
{
    memset(S, 0, sizeof(*S));
    gc_threshold        = 1u << 20;
    gc_stress           = -1;
    nil_singleton.type  = MINO_NIL;
    true_singleton.type = MINO_BOOL;
    true_singleton.as.b = 1;
    false_singleton.type = MINO_BOOL;
    reader_line         = 1;
}

mino_state_t *mino_state_new(void)
{
    mino_state_t *st = (mino_state_t *)calloc(1, sizeof(*st));
    if (st == NULL) {
        abort(); /* unrecoverable: no state to report error through */
    }
    state_init(st);
    return st;
}

void mino_state_free(mino_state_t *S)
{
    root_env_t *r;
    root_env_t *rnext;
    gc_hdr_t   *h;
    gc_hdr_t   *hnext;
    size_t      i;
    if (S == NULL) {
        return;
    }
    for (r = gc_root_envs; r != NULL; r = rnext) {
        rnext = r->next;
        free(r);
    }
    {
        mino_ref_t *ref = S->ref_roots;
        mino_ref_t *rnxt;
        while (ref != NULL) {
            rnxt = ref->next;
            free(ref);
            ref = rnxt;
        }
    }
    for (i = 0; i < module_cache_len; i++) {
        free(module_cache[i].name);
    }
    free(module_cache);
    for (i = 0; i < meta_table_len; i++) {
        free(meta_table[i].name);
        free(meta_table[i].docstring);
    }
    free(meta_table);
    free(sym_intern.entries);
    free(kw_intern.entries);
    free(gc_ranges);
    free(S->core_forms);
    for (h = gc_all; h != NULL; h = hnext) {
        hnext = h->next;
        if (h->type_tag == GC_T_VAL) {
            mino_val_t *v = (mino_val_t *)(h + 1);
            if (v->type == MINO_HANDLE && v->as.handle.finalizer != NULL) {
                v->as.handle.finalizer(v->as.handle.ptr, v->as.handle.tag);
            }
        }
        free(h);
    }
    free(S);
}

/* ------------------------------------------------------------------------- */
/* Value retention (refs)                                                    */
/* ------------------------------------------------------------------------- */

mino_ref_t *mino_ref(mino_state_t *S, mino_val_t *val)
{
    mino_ref_t *r = (mino_ref_t *)calloc(1, sizeof(*r));
    if (r == NULL) {
        return NULL;
    }
    r->val  = val;
    r->prev = NULL;
    r->next = S->ref_roots;
    if (S->ref_roots != NULL) {
        S->ref_roots->prev = r;
    }
    S->ref_roots = r;
    return r;
}

mino_val_t *mino_deref(const mino_ref_t *ref)
{
    if (ref == NULL) {
        return NULL;
    }
    return ref->val;
}

void mino_unref(mino_state_t *S, mino_ref_t *ref)
{
    if (ref == NULL) {
        return;
    }
    if (ref->prev != NULL) {
        ref->prev->next = ref->next;
    } else {
        S->ref_roots = ref->next;
    }
    if (ref->next != NULL) {
        ref->next->prev = ref->prev;
    }
    free(ref);
}

/* ------------------------------------------------------------------------- */
/* Public eval entry points                                                  */
/* ------------------------------------------------------------------------- */

mino_val_t *mino_eval(mino_state_t *S, mino_val_t *form, mino_env_t *env)
{
    volatile char probe = 0;
    mino_val_t   *v;
    int           saved_try = try_depth;
    gc_note_host_frame(S, (void *)&probe);
    (void)probe;
    eval_steps     = 0;
    limit_exceeded = 0;
    interrupted    = 0;
    trace_added    = 0;
    call_depth     = 0;

    /* Top-level try frame so that OOM and unhandled throw during eval
     * surface as a NULL return instead of aborting the process. */
    if (try_depth < MAX_TRY_DEPTH) {
        try_stack[try_depth].exception = NULL;
        if (setjmp(try_stack[try_depth].buf) != 0) {
            /* Landed here from longjmp (OOM or uncaught throw). */
            try_depth = saved_try;
            if (mino_last_error(S) == NULL) {
                set_error(S, "unhandled exception");
            }
            call_depth = 0;
            return NULL;
        }
        try_depth++;
    }

    v = eval(S, form, env);
    try_depth = saved_try;
    if (v == NULL) {
        append_trace(S);
        call_depth = 0;
        return NULL;
    }
    if (v->type == MINO_RECUR) {
        set_error(S, "recur must be in tail position");
        call_depth = 0;
        return NULL;
    }
    if (v->type == MINO_TAIL_CALL) {
        set_error(S, "tail call escaped to top level");
        call_depth = 0;
        return NULL;
    }
    call_depth = 0;
    return v;
}

mino_val_t *mino_eval_string(mino_state_t *S, const char *src, mino_env_t *env)
{
    volatile char   probe = 0;
    mino_val_t     *last  = mino_nil(S);
    const char     *saved_file = reader_file;
    int             saved_line = reader_line;
    int             saved_try  = try_depth;
    gc_note_host_frame(S, (void *)&probe);
    (void)probe;
    eval_steps     = 0;
    limit_exceeded = 0;
    interrupted    = 0;
    if (reader_file == NULL) {
        reader_file = intern_filename("<string>");
    }
    reader_line = 1;

    /* Top-level try frame so that OOM during read or eval surfaces as a
     * NULL return instead of aborting the process. */
    if (try_depth < MAX_TRY_DEPTH) {
        try_stack[try_depth].exception = NULL;
        if (setjmp(try_stack[try_depth].buf) != 0) {
            try_depth   = saved_try;
            reader_file = saved_file;
            reader_line = saved_line;
            if (mino_last_error(S) == NULL) {
                set_error(S, "unhandled exception");
            }
            call_depth = 0;
            return NULL;
        }
        try_depth++;
    }

    while (*src != '\0') {
        const char *end  = NULL;
        mino_val_t *form = mino_read(S, src, &end);
        if (form == NULL) {
            if (mino_last_error(S) != NULL) {
                try_depth   = saved_try;
                reader_file = saved_file;
                reader_line = saved_line;
                return NULL;
            }
            break; /* EOF */
        }
        last = mino_eval(S, form, env);
        if (last == NULL) {
            try_depth   = saved_try;
            reader_file = saved_file;
            reader_line = saved_line;
            return NULL;
        }
        src = end;
    }
    try_depth   = saved_try;
    reader_file = saved_file;
    reader_line = saved_line;
    return last;
}

mino_val_t *mino_load_file(mino_state_t *S, const char *path, mino_env_t *env)
{
    FILE  *f;
    char  *buf;
    long   sz;
    size_t rd;
    mino_val_t    *result;
    const char    *saved_file;
    if (path == NULL || env == NULL) {
        set_error(S, "mino_load_file: NULL argument");
        return NULL;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        char msg[300];
        snprintf(msg, sizeof(msg), "cannot open file: %s", path);
        set_error(S, msg);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        set_error(S, "cannot seek to end of file");
        return NULL;
    }
    sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        set_error(S, "cannot determine file size");
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        set_error(S, "cannot seek to start of file");
        return NULL;
    }
    buf = (char *)malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(f);
        set_error(S, "out of memory loading file");
        return NULL;
    }
    rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) {
        free(buf);
        set_error(S, "short read loading file");
        return NULL;
    }
    buf[rd] = '\0';
    saved_file  = reader_file;
    reader_file = intern_filename(path);
    result = mino_eval_string(S, buf, env);
    reader_file = saved_file;
    free(buf);
    return result;
}

mino_env_t *mino_new(mino_state_t *S)
{
    mino_env_t *env = mino_env_new(S);
    mino_install_core(S, env);
    mino_install_io(S, env);
    return env;
}

void mino_register_fn(mino_state_t *S, mino_env_t *env, const char *name, mino_prim_fn fn)
{
    mino_env_set(S, env, name, mino_prim(S, name, fn));
}

mino_val_t *mino_call(mino_state_t *S, mino_val_t *fn, mino_val_t *args, mino_env_t *env)
{
    volatile char probe = 0;
    gc_note_host_frame(S, (void *)&probe);
    (void)probe;
    return apply_callable(S, fn, args, env);
}

int mino_pcall(mino_state_t *S, mino_val_t *fn, mino_val_t *args, mino_env_t *env,
               mino_val_t **out)
{
    int saved_try = try_depth;
    mino_val_t *result;

    if (try_depth >= MAX_TRY_DEPTH) {
        if (out != NULL) {
            *out = NULL;
        }
        return -1;
    }

    try_stack[try_depth].exception = NULL;
    if (setjmp(try_stack[try_depth].buf) != 0) {
        /* Landed here from longjmp -- error was thrown. */
        mino_val_t *ex = try_stack[saved_try].exception;
        try_depth = saved_try;
        /* Populate last_error from the exception value so the host
         * can inspect it via mino_last_error(). */
        if (mino_last_error(S) == NULL || mino_last_error(S)[0] == '\0') {
            const char *s = NULL;
            size_t slen = 0;
            if (ex != NULL && mino_to_string(ex, &s, &slen)) {
                set_error(S, s);
            } else {
                set_error(S, "unhandled exception");
            }
        }
        if (out != NULL) {
            *out = NULL;
        }
        return -1;
    }
    try_depth++;

    result = mino_call(S, fn, args, env);
    try_depth = saved_try;

    if (out != NULL) {
        *out = result;
    }
    return result == NULL ? -1 : 0;
}

/* ------------------------------------------------------------------------- */
/* Execution limits, fault injection, interrupt                              */
/* ------------------------------------------------------------------------- */

void mino_set_limit(mino_state_t *S, int kind, size_t value)
{
    switch (kind) {
    case MINO_LIMIT_STEPS: limit_steps = value; break;
    case MINO_LIMIT_HEAP:  limit_heap  = value; break;
    default: break;
    }
}

void mino_set_fail_alloc_at(mino_state_t *S, long n)
{
    S->fi_alloc_countdown = n;
}

void mino_set_fail_raw_at(mino_state_t *S, long n)
{
    S->fi_raw_countdown = n;
}

int mino_fi_should_fail_raw(mino_state_t *S)
{
    if (S->fi_raw_countdown > 0) {
        S->fi_raw_countdown--;
        if (S->fi_raw_countdown == 0) {
            return 1;
        }
    }
    return 0;
}

void mino_interrupt(mino_state_t *S)
{
    /* Write directly to avoid S (may be in use by another thread). */
#undef interrupted
    S->interrupted = 1;
#define interrupted (S->interrupted)
}

/* ------------------------------------------------------------------------- */
/* In-process REPL handle                                                    */
/* ------------------------------------------------------------------------- */

struct mino_repl {
    mino_state_t *state;
    mino_env_t   *env;
    char         *buf;
    size_t        len;
    size_t        cap;
};

mino_repl_t *mino_repl_new(mino_state_t *S, mino_env_t *env)
{
    mino_repl_t *r = (mino_repl_t *)malloc(sizeof(*r));
    if (r == NULL) { return NULL; }
    r->state = S;
    r->env   = env;
    r->buf   = NULL;
    r->len   = 0;
    r->cap   = 0;
    return r;
}

static int repl_is_whitespace(const char *s)
{
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != ',') {
            return 0;
        }
    }
    return 1;
}

int mino_repl_feed(mino_repl_t *repl, const char *line, mino_val_t **out)
{
    mino_state_t  *S;
    size_t         add;
    const char    *cursor;
    const char    *end;
    mino_val_t    *form;
    mino_val_t    *result;

    if (out != NULL) { *out = NULL; }
    if (repl == NULL) { return MINO_REPL_ERROR; }
    S = repl->state;

    /* Append the line to the buffer. */
    add = (line != NULL) ? strlen(line) : 0;
    if (repl->len + add + 1 > repl->cap) {
        size_t new_cap = repl->cap == 0 ? 256 : repl->cap;
        char  *nb;
        while (new_cap < repl->len + add + 1) { new_cap *= 2; }
        nb = (char *)realloc(repl->buf, new_cap);
        if (nb == NULL) {
            set_error(S, "repl: out of memory");
            return MINO_REPL_ERROR;
        }
        repl->buf = nb;
        repl->cap = new_cap;
    }
    if (add > 0) {
        memcpy(repl->buf + repl->len, line, add);
    }
    repl->len += add;
    repl->buf[repl->len] = '\0';

    /* If buffer is only whitespace, need more input. */
    if (repl_is_whitespace(repl->buf)) {
        return MINO_REPL_MORE;
    }

    /* Try to read a form. */
    cursor = repl->buf;
    end    = repl->buf;
    form   = mino_read(S, cursor, &end);
    if (form == NULL) {
        const char *err = mino_last_error(S);
        if (err != NULL && strstr(err, "unterminated") != NULL) {
            return MINO_REPL_MORE;
        }
        /* Hard parse error -- reset buffer. */
        repl->len = 0;
        repl->buf[0] = '\0';
        return MINO_REPL_ERROR;
    }

    /* Shift remaining bytes to the front. */
    {
        size_t consumed  = (size_t)(end - repl->buf);
        size_t remaining = repl->len - consumed;
        memmove(repl->buf, end, remaining + 1);
        repl->len = remaining;
    }

    /* Evaluate the form. */
    result = mino_eval(S, form, repl->env);
    if (result == NULL) {
        return MINO_REPL_ERROR;
    }
    if (out != NULL) { *out = result; }
    return MINO_REPL_OK;
}

void mino_repl_free(mino_repl_t *repl)
{
    if (repl == NULL) { return; }
    free(repl->buf);
    free(repl);
}
