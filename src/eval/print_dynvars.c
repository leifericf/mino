/*
 * print_dynvars.c -- *print-* dynamic variable resolution and restore.
 *
 * Extracted from print.c to keep each translation unit under the 1100-line
 * limit.  The two public functions are declared in prim/internal.h so every
 * caller (prim/io.c, prim/pr.c, etc.) already finds them through that header.
 */

#include "runtime/internal.h"
#include "prim/internal.h"

/* Resolve a print-side dynvar by name. Checks the dynamic binding
 * stack first under the clojure.core var's identity (where (binding
 * [*print-length* N] ...) lands regardless of spelling, with a text
 * probe for var-less boot-time bindings), then the lexical env, then
 * the var root in clojure.core. Returns the raw mino_val* (NULL
 * when truly unset). The integer / boolean adapters below interpret
 * the result. */
static mino_val *resolve_print_dynvar(mino_state *S, mino_env *env,
                                       const char *name)
{
    mino_val *v = NULL;
    mino_val *var;
    if (mino_current_ctx(S)->dyn_stack != NULL) {
        var = var_find(S, "clojure.core", name);
        v = (var != NULL) ? dyn_lookup_var_or_name(S, var, name)
                          : dyn_lookup(S, name);
    }
    if (v == NULL && env != NULL) {
        v = mino_env_get(env, name);
    }
    if (v == NULL) {
        var = var_find(S, "clojure.core", name);
        if (var != NULL && mino_type_of(var) == MINO_VAR
            && var->as.var.bound) {
            v = var->as.var.root;
        }
    }
    return v;
}

/* Mirrors the resolve_io_sink pattern in prim/io.c. Non-integer
 * values silently become "unset" (-1) so a misconfigured dynvar
 * doesn't crash the printer. */
static int resolve_print_int_dynvar(mino_state *S, mino_env *env,
                                    const char *name)
{
    mino_val *v = resolve_print_dynvar(S, env, name);
    long long n;
    if (v == NULL || mino_type_of(v) == MINO_NIL) return -1;
    if (!as_long(v, &n)) return -1;
    if (n < 0) return -1;
    if (n > 0x7fffffff) return 0x7fffffff;
    return (int)n;
}

/* Resolve a boolean print-side dynvar. Returns 1 for truthy
 * (anything but nil / false), 0 for falsy, fallback when truly
 * unset. mino doesn't conflate nil with "unset" here -- a binding
 * to nil explicitly means "treat as false" per JVM Clojure. */
static int resolve_print_bool_dynvar(mino_state *S, mino_env *env,
                                     const char *name, int fallback)
{
    mino_val *v = resolve_print_dynvar(S, env, name);
    if (v == NULL) return fallback;
    if (mino_type_of(v) == MINO_NIL) return 0;
    if (mino_type_of(v) == MINO_BOOL) return mino_val_bool_get(v) ? 1 : 0;
    /* Any non-nil, non-false value is truthy in Clojure semantics. */
    return 1;
}

void print_dynvars_resolve(mino_state *S, mino_env *env,
                           print_dynvars_saved_t *saved)
{
    saved->length   = S->print_length_limit;
    saved->level    = S->print_level_limit;
    saved->readably = S->print_readably_flag;
    saved->meta     = S->print_meta_flag;
    saved->dup      = S->print_dup_flag;
    saved->ns_maps  = S->print_namespace_maps_flag;
    saved->flush_nl = S->flush_on_newline_flag;
    S->print_length_limit = resolve_print_int_dynvar(S, env, "*print-length*");
    S->print_level_limit  = resolve_print_int_dynvar(S, env, "*print-level*");
    /* Defaults match JVM Clojure: *print-readably* true (pr/prn
     * default; print/println sites pass readably=0 explicitly),
     * *print-meta* false, *print-dup* false,
     * *print-namespace-maps* false, *flush-on-newline* true. */
    S->print_readably_flag       = resolve_print_bool_dynvar(S, env, "*print-readably*", 1);
    S->print_meta_flag           = resolve_print_bool_dynvar(S, env, "*print-meta*", 0);
    S->print_dup_flag            = resolve_print_bool_dynvar(S, env, "*print-dup*", 0);
    S->print_namespace_maps_flag = resolve_print_bool_dynvar(S, env, "*print-namespace-maps*", 0);
    S->flush_on_newline_flag     = resolve_print_bool_dynvar(S, env, "*flush-on-newline*", 1);
}

void print_dynvars_restore(mino_state *S, const print_dynvars_saved_t *saved)
{
    S->print_length_limit        = saved->length;
    S->print_level_limit         = saved->level;
    S->print_readably_flag       = saved->readably;
    S->print_meta_flag           = saved->meta;
    S->print_dup_flag            = saved->dup;
    S->print_namespace_maps_flag = saved->ns_maps;
    S->flush_on_newline_flag     = saved->flush_nl;
}
