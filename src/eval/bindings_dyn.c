/*
 * bindings_dyn.c -- dynamic binding special form (binding) and public C API
 *                   (mino_push_bindings / mino_pop_bindings).
 *
 * Extracted from bindings.c to keep each translation unit under the
 * 1100-line limit.  eval_binding is declared in eval/special_internal.h;
 * mino_push_bindings / mino_pop_bindings are declared in mino.h.
 */

#include "eval/special_internal.h"
#include "eval/internal.h"
#include "collections/internal.h"

/* push_dyn_binding -- validate sym_v as a binding name, resolve it to
 * its canonical var (the dyn stack is keyed by var identity so any
 * spelling of a var's name reaches the same binding), eval val_form
 * in env, and prepend a dyn_binding to frame->bindings. The frame is
 * already on the dyn stack with `building` set: the GC root walk
 * keeps the freshly evaluated values alive while later value forms
 * run, but lookups skip the frame so the bindings install in
 * parallel, matching `binding`'s contract (unlike let). On any
 * failure sets a diag and returns 0; the caller pops and frees the
 * frame. */
static int push_dyn_binding(mino_state *S, mino_val *form,
                            mino_val *sym_v, mino_val *val_form,
                            mino_env *env, dyn_frame_t *frame)
{
    mino_val    *val;
    mino_val    *var;
    dyn_binding_t *b;
    if (sym_v == NULL || mino_type_of(sym_v) != MINO_SYMBOL) {
        set_eval_diag(S, form, "syntax", "MSY003",
            "binding: names must be symbols");
        return 0;
    }
    /* Reject rebinding a non-dynamic var: JVM Clojure throws here, and
     * silently allowing it lets a bug compile clean on mino and blow
     * up in production on the JVM. The resolve probes the same
     * cascade a read walks (alias-aware qualified, then current ns /
     * defining ns / clojure.core for bare names) and only fires when
     * a var actually exists -- pure lexical names (which the macro
     * layer often introduces via gensym) have no var so fall through
     * to the var-less dynamic-scope push. */
    var = dyn_resolve_var(S, sym_v->as.s.data, sym_v->as.s.len);
    if (var != NULL && !var->as.var.dynamic) {
        char msg[300];
        snprintf(msg, sizeof(msg),
            "Can't dynamically bind non-dynamic var: %s/%s",
            var->as.var.ns != NULL ? var->as.var.ns : "?",
            var->as.var.sym != NULL ? var->as.var.sym : sym_v->as.s.data);
        set_eval_diag(S, form, "eval/binding", "MBN001", msg);
        return 0;
    }
    /* Snapshot current_ns once per frame when the frame binds
     * clojure.core/*ns*. *ns* reads return current_ns directly, so the
     * frame must restore it on teardown to give `binding` its
     * thread-local scope over ns mutations (in-ns / ns inside the
     * body). dyn_frame_restore_ns consumes the snapshot at every pop
     * site. Taken before the value form evaluates so any in-ns there
     * is also scoped. */
    if (frame->saved_ns == NULL
        && var != NULL && var->as.var.dynamic
        && var->as.var.ns != NULL && strcmp(var->as.var.ns, "clojure.core") == 0
        && var->as.var.sym != NULL && strcmp(var->as.var.sym, "*ns*") == 0) {
        frame->saved_ns = S->ns_vars.current_ns;
    }
    val = eval(S, val_form, env);
    if (val == NULL) return 0;
    b = (dyn_binding_t *)malloc(sizeof(*b));
    if (b == NULL) {
        set_eval_diag(S, form, "syntax", "MSY003",
            "binding: out of memory");
        return 0;
    }
    b->name = (var != NULL) ? var->as.var.sym
                            : sym_v->as.s.data;  /* interned via reader */
    b->var  = var;
    b->val  = val;
    b->next = frame->bindings;
    frame->bindings = b;
    return 1;
}

/* Pop `frame` (which must be the top of the dyn stack) and free it
 * with its binding chain. */
static void pop_dyn_frame(mino_state *S, dyn_frame_t *frame)
{
    mino_current_ctx(S)->dyn_stack = frame->prev;
    dyn_frame_restore_ns(S, frame);
    dyn_binding_list_free(frame->bindings);
    free(frame);
}

mino_val *eval_binding(mino_state *S, mino_val *form,
                         mino_val *args, mino_env *env, int tail)
{
    mino_val *pairs, *body, *result;
    /* Frame is heap-allocated so the pointer remains valid even if a
     * throw inside body unwinds this C frame past the cleanup at the
     * end. The src/eval/control.c longjmp handler walks
     * mino_current_ctx(S)->dyn_stack and frees each frame's bindings;
     * if frame were stack-local, that walk would read popped stack memory.
     *
     * The frame goes onto the dyn stack BEFORE the value forms run,
     * marked `building`: the GC root walk is the only thing keeping
     * already-evaluated binding values alive while a later value form
     * allocates, and a throw from a value form lets the longjmp
     * unwinder free the partial frame instead of leaking it. Lookups
     * skip building frames, preserving parallel-install semantics. */
    dyn_frame_t   *frame;
    (void)tail;
    if (!mino_is_cons(args)) {
        set_eval_diag(S, form, "syntax", "MSY001", "binding requires a binding list and body");
        return NULL;
    }
    pairs = args->as.cons.car;
    body  = args->as.cons.cdr;
    if ((pairs == NULL || mino_type_of(pairs) != MINO_VECTOR)
        && !mino_is_cons(pairs)) {
        set_eval_diag(S, form, "syntax", "MSY001", "binding requires a binding list and body");
        return NULL;
    }
    frame = (dyn_frame_t *)calloc(1, sizeof(*frame));
    if (frame == NULL) {
        set_eval_diag(S, form, "internal", "MIN001", "binding: out of memory");
        return NULL;
    }
    frame->bindings = NULL;
    frame->building = 1;
    frame->prev     = mino_current_ctx(S)->dyn_stack;
    mino_current_ctx(S)->dyn_stack = frame;
    if (mino_type_of(pairs) == MINO_VECTOR) {
        /* Vector binding form: [sym val sym val ...] */
        size_t vlen = pairs->as.vec.len;
        size_t vi;
        if (vlen % 2 != 0) {
            set_eval_diag(S, form, "syntax", "MSY003",
                "binding: odd number of forms in binding vector");
            pop_dyn_frame(S, frame);
            return NULL;
        }
        for (vi = 0; vi < vlen; vi += 2) {
            if (!push_dyn_binding(S, form, vec_nth(pairs, vi),
                                  vec_nth(pairs, vi + 1), env, frame)) {
                pop_dyn_frame(S, frame);
                return NULL;
            }
        }
    } else {
        /* Legacy list binding form: (sym val sym val ...) */
        while (pairs != NULL && mino_type_of(pairs) == MINO_CONS) {
            mino_val *sym_v    = pairs->as.cons.car;
            mino_val *val_form;
            pairs = pairs->as.cons.cdr;
            if (pairs == NULL || mino_type_of(pairs) != MINO_CONS) {
                set_eval_diag(S, form, "syntax", "MSY003",
                    "binding: odd number of forms in binding list");
                pop_dyn_frame(S, frame);
                return NULL;
            }
            val_form = pairs->as.cons.car;
            pairs    = pairs->as.cons.cdr;
            if (!push_dyn_binding(S, form, sym_v, val_form, env, frame)) {
                pop_dyn_frame(S, frame);
                return NULL;
            }
        }
    }
    /* All values evaluated: make the frame visible to lookups. */
    frame->building = 0;
    result = eval_implicit_do(S, body, env);
    pop_dyn_frame(S, frame);
    return result;
}

/* ------------------------------------------------------------------------- */
/* Public binding push/pop (mino.h)                                          */
/* ------------------------------------------------------------------------- */

/* Push a dynamic-binding frame from C. Returns an opaque handle the
 * embedder pops with mino_pop_bindings; nests correctly with script-
 * side `(binding [...] ...)` frames. */
mino_binding_frame *mino_push_bindings(mino_state *S,
                                       mino_val **vars,
                                       mino_val **vals,
                                       size_t n)
{
    dyn_binding_t *bhead = NULL;
    dyn_frame_t   *frame;
    size_t         i;
    if (S == NULL) return NULL;
    if (n > 0 && (vars == NULL || vals == NULL)) return NULL;
    for (i = 0; i < n; i++) {
        mino_val *sym = vars[i];
        dyn_binding_t *b;
        if (sym == NULL || mino_type_of(sym) != MINO_SYMBOL) {
            dyn_binding_list_free(bhead);
            return NULL;
        }
        /* dyn_binding_make resolves the symbol to its canonical var so
         * script-side reads of the var (under any spelling) see the
         * embedder's binding. */
        b = dyn_binding_make(S, sym, vals[i], bhead);
        if (b == NULL) { dyn_binding_list_free(bhead); return NULL; }
        bhead = b;
    }
    frame = (dyn_frame_t *)calloc(1, sizeof(*frame));
    if (frame == NULL) { dyn_binding_list_free(bhead); return NULL; }
    frame->bindings = bhead;
    frame->building = 0;
    frame->prev     = mino_current_ctx(S)->dyn_stack;
    /* Mirror eval_binding: if this frame binds clojure.core/*ns*,
     * snapshot current_ns so mino_pop_bindings can restore it (reads
     * of *ns* return current_ns directly). */
    if (S->ns_vars.current_ns != NULL) {
        dyn_binding_t *b;
        for (b = bhead; b != NULL; b = b->next) {
            if (b->var != NULL
                && b->var->as.var.ns != NULL
                && strcmp(b->var->as.var.ns, "clojure.core") == 0
                && b->var->as.var.sym != NULL
                && strcmp(b->var->as.var.sym, "*ns*") == 0) {
                frame->saved_ns = S->ns_vars.current_ns;
                break;
            }
        }
    }
    mino_current_ctx(S)->dyn_stack = frame;
    return (mino_binding_frame *)frame;
}

void mino_pop_bindings(mino_state *S, mino_binding_frame *frame)
{
    dyn_frame_t *df = (dyn_frame_t *)frame;
    if (S == NULL || df == NULL) return;
    /* Tolerate script-side `throw` having already unwound through us:
     * if the current dyn_stack is BELOW our frame, this pop is a
     * no-op. Otherwise we splice df out by popping frames until df is
     * the top, then unlink. */
    {
        dyn_frame_t *cur = mino_current_ctx(S)->dyn_stack;
        while (cur != NULL && cur != df) cur = cur->prev;
        if (cur == NULL) return;  /* Already unwound. */
    }
    /* df is somewhere in the dyn stack: pop everything down to and
     * including df. The runtime's normal unwinder frees frame
     * bindings via dyn_binding_list_free on unwind, but a clean pop
     * from C has to do it here. */
    while (mino_current_ctx(S)->dyn_stack != NULL
           && mino_current_ctx(S)->dyn_stack != df) {
        dyn_frame_t *top = mino_current_ctx(S)->dyn_stack;
        mino_current_ctx(S)->dyn_stack = top->prev;
        dyn_frame_restore_ns(S, top);
        dyn_binding_list_free(top->bindings);
        free(top);
    }
    if (mino_current_ctx(S)->dyn_stack == df) {
        mino_current_ctx(S)->dyn_stack = df->prev;
        dyn_frame_restore_ns(S, df);
        dyn_binding_list_free(df->bindings);
        free(df);
    }
}
