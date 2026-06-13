/*
 * special_host.c -- host-interop syntax sugar dispatch.
 *
 * Extracted from special.c to keep each translation unit under the
 * 1100-line limit.  eval_try_host_syntax is declared in
 * eval/special_internal.h.
 *
 * Recognises the four interop shapes and rewrites them into the
 * corresponding host/... primitive call:
 *
 *   (.method target args...)         -> (host/call target :method args...)
 *   (.-field target)                 -> (host/get  target :field)
 *   (new TypeName args...)           -> (host/new :TypeName args...)
 *   (TypeName/staticMethod args...)  -> (host/static-call :TypeName :staticMethod args...)
 *
 * Returns 1 with `*out` set when the form was a host shape (success
 * or error path; *out reflects the call result, NULL on diag).
 * Returns 0 when the form is not a host shape, leaving *out untouched.
 */

#include "eval/special_internal.h"
#include "eval/internal.h"
#include "prim/internal.h"
#include "mino.h"

int eval_try_host_syntax(mino_state *S, mino_val *form,
                         mino_val *head, mino_val *args,
                         mino_env *env, mino_val **out)
{
    const char *hname;
    size_t      hlen;
    if (head == NULL || mino_type_of(head) != MINO_SYMBOL || head->as.s.len == 0) {
        return 0;
    }
    hname = head->as.s.data;
    hlen  = head->as.s.len;

    /* Host primitives live under literal "host/<op>" names inside
     * clojure.core. The interop special forms need to find them
     * regardless of the caller's current namespace; do that by going
     * straight to clojure.core rather than walking the alias table.
     */
#define HOST_PRIM_LOOKUP(_var, _name) do {                                    \
        mino_env *_core = ns_env_lookup(S, "clojure.core");                 \
        env_binding_t *_b =                                                   \
            (_core != NULL) ? env_find_here(_core, (_name)) : NULL;           \
        (_var) = (_b != NULL) ? _b->val : NULL;                               \
    } while (0)

    /* (.method target args...) -> (host/call target :method args...)
     * (.-field target)         -> (host/get  target :field) */
    if (hname[0] == '.' && hlen > 1) {
        int         is_getter = (hlen > 2 && hname[1] == '-');
        const char *member    = hname + (is_getter ? 2 : 1);
        mino_val *kw        = mino_keyword(S, member);
        gc_pin(kw);
        if (is_getter) {
            mino_val *target_form;
            mino_val *target_val;
            mino_val *prim;
            if (!mino_is_cons(args)) {
                gc_unpin(1);
                set_eval_diag(S, form, "syntax", "MSY001",
                              ".-field requires a target");
                *out = NULL;
                return 1;
            }
            target_form = args->as.cons.car;
            target_val  = eval_value(S, target_form, env);
            if (target_val == NULL) { gc_unpin(1); *out = NULL; return 1; }
            gc_pin(target_val);
            HOST_PRIM_LOOKUP(prim, "host/get");
            if (prim == NULL) {
                gc_unpin(2);
                set_eval_diag(S, form, "name", "MNS001", "host/get not bound");
                *out = NULL;
                return 1;
            }
            {
                mino_val *a = mino_cons(S, kw, mino_nil(S));
                gc_pin(a);
                a = mino_cons(S, target_val, a);
                gc_unpin(3);
                *out = apply_callable(S, prim, a, env);
                return 1;
            }
        } else {
            mino_val *target_form;
            mino_val *target_val;
            mino_val *prim;
            mino_val *rest;
            mino_val *evaled_rest;
            if (!mino_is_cons(args)) {
                gc_unpin(1);
                set_eval_diag(S, form, "syntax", "MSY001",
                              ".method requires a target");
                *out = NULL;
                return 1;
            }
            target_form = args->as.cons.car;
            rest        = args->as.cons.cdr;
            target_val  = eval_value(S, target_form, env);
            if (target_val == NULL) { gc_unpin(1); *out = NULL; return 1; }
            gc_pin(target_val);
            evaled_rest = eval_args(S, rest, env);
            if (evaled_rest == NULL && mino_is_cons(rest)) {
                gc_unpin(2); *out = NULL; return 1;
            }
            gc_pin(evaled_rest);
            HOST_PRIM_LOOKUP(prim, "host/call");
            if (prim == NULL) {
                gc_unpin(3);
                set_eval_diag(S, form, "name", "MNS001",
                              "host/call not bound");
                *out = NULL;
                return 1;
            }
            {
                mino_val *a = mino_cons(S, kw, evaled_rest);
                gc_pin(a);
                a = mino_cons(S, target_val, a);
                gc_unpin(4);
                *out = apply_callable(S, prim, a, env);
                return 1;
            }
        }
    }

    /* (new TypeName args...) -> (host/new :TypeName args...) */
    if (hlen == 3 && memcmp(hname, "new", 3) == 0 && mino_is_cons(args)) {
        mino_val *type_sym = args->as.cons.car;
        if (type_sym != NULL && mino_type_of(type_sym) == MINO_SYMBOL) {
            mino_val *kw   = mino_keyword(S, type_sym->as.s.data);
            mino_val *rest = args->as.cons.cdr;
            mino_val *evaled_rest;
            mino_val *prim;
            gc_pin(kw);
            evaled_rest = eval_args(S, rest, env);
            if (evaled_rest == NULL && mino_is_cons(rest)) {
                gc_unpin(1); *out = NULL; return 1;
            }
            gc_pin(evaled_rest);
            HOST_PRIM_LOOKUP(prim, "host/new");
            if (prim == NULL) {
                gc_unpin(2);
                set_eval_diag(S, form, "name", "MNS001",
                              "host/new not bound");
                *out = NULL;
                return 1;
            }
            {
                mino_val *a = mino_cons(S, kw, evaled_rest);
                gc_pin(a);        /* pin a before releasing kw/evaled_rest */
                gc_unpin(2);      /* release kw and evaled_rest */
                *out = apply_callable(S, prim, a, env);
                gc_unpin(1);      /* release a */
                return 1;
            }
        }
    }

    /* (Foo. args...) -> (->Foo args...) when Foo resolves to a
     * MINO_TYPE (i.e. a defrecord/deftype). This is the trailing-dot
     * constructor syntax from JVM Clojure; on mino it just dispatches
     * to the existing positional factory generated by defrecord.
     * Resolution mirrors the lexical -> current-ns -> ambient-ns
     * lookup chain used by eval_symbol so vars defined at top level
     * resolve here too. */
    if (hlen > 1 && hname[hlen - 1] == '.') {
        char       stem_buf[256];
        size_t     stem_len = hlen - 1;
        if (stem_len < sizeof(stem_buf)) {
            mino_val *type_val = NULL;
            mino_env *ns_env;
            memcpy(stem_buf, hname, stem_len);
            stem_buf[stem_len] = '\0';
            type_val = mino_env_get(env, stem_buf);
            if (type_val == NULL) {
                ns_env = current_ns_env(S);
                if (ns_env != NULL) type_val = mino_env_get(ns_env, stem_buf);
            }
            if (type_val == NULL && S->ns_vars.fn_ambient_ns != NULL
                && (S->ns_vars.current_ns == NULL
                    || strcmp(S->ns_vars.fn_ambient_ns, S->ns_vars.current_ns) != 0)) {
                mino_env *amb = ns_env_lookup(S, S->ns_vars.fn_ambient_ns);
                if (amb != NULL) type_val = mino_env_get(amb, stem_buf);
            }
            if (type_val != NULL && mino_type_of(type_val) == MINO_TYPE) {
                char        ctor_buf[256 + 2];
                mino_val *ctor = NULL;
                size_t      cn = stem_len + 2;
                if (cn < sizeof(ctor_buf)) {
                    ctor_buf[0] = '-'; ctor_buf[1] = '>';
                    memcpy(ctor_buf + 2, stem_buf, stem_len);
                    ctor_buf[cn] = '\0';
                    ctor = mino_env_get(env, ctor_buf);
                    if (ctor == NULL) {
                        ns_env = current_ns_env(S);
                        if (ns_env != NULL) ctor = mino_env_get(ns_env, ctor_buf);
                    }
                    if (ctor == NULL && S->ns_vars.fn_ambient_ns != NULL
                        && (S->ns_vars.current_ns == NULL
                            || strcmp(S->ns_vars.fn_ambient_ns, S->ns_vars.current_ns) != 0)) {
                        mino_env *amb = ns_env_lookup(S, S->ns_vars.fn_ambient_ns);
                        if (amb != NULL) ctor = mino_env_get(amb, ctor_buf);
                    }
                    if (ctor != NULL) {
                        mino_val *evaled_rest = eval_args(S, args, env);
                        if (evaled_rest == NULL && mino_is_cons(args)) {
                            *out = NULL; return 1;
                        }
                        *out = apply_callable(S, ctor, evaled_rest, env);
                        return 1;
                    }
                }
            }
        }
    }

    /* (TypeName/staticMethod args...)
     * -> (host/static-call :TypeName :staticMethod args...)
     * Only if the namespace part matches a registered host type
     * and the full name is not a literal env binding. */
    {
        const char *sl = (hlen > 1) ? memchr(hname, '/', hlen) : NULL;
        if (sl != NULL) {
            char        tbuf[256];
            size_t      tlen  = (size_t)(sl - hname);
            const char *mname = sl + 1;
            if (tlen < sizeof(tbuf) && tlen > 0 && *mname != '\0'
                && mino_env_get(env, hname) == NULL) {
                host_type_t *ht;
                memcpy(tbuf, hname, tlen);
                tbuf[tlen] = '\0';
                ht = host_type_find(S, tbuf);
                if (ht != NULL) {
                    mino_val *tkw = mino_keyword(S, tbuf);
                    mino_val *mkw = mino_keyword(S, mname);
                    mino_val *evaled_rest;
                    mino_val *prim;
                    gc_pin(tkw);
                    gc_pin(mkw);
                    evaled_rest = eval_args(S, args, env);
                    if (evaled_rest == NULL && mino_is_cons(args)) {
                        gc_unpin(2); *out = NULL; return 1;
                    }
                    gc_pin(evaled_rest);
                    HOST_PRIM_LOOKUP(prim, "host/static-call");
                    if (prim == NULL) {
                        gc_unpin(3);
                        set_eval_diag(S, form, "name", "MNS001",
                                      "host/static-call not bound");
                        *out = NULL;
                        return 1;
                    }
                    {
                        mino_val *a = mino_cons(S, mkw, evaled_rest);
                        gc_pin(a);
                        a = mino_cons(S, tkw, a);
                        gc_unpin(4);
                        *out = apply_callable(S, prim, a, env);
                        return 1;
                    }
                }
            }
        }
    }
    return 0;
#undef HOST_PRIM_LOOKUP
}
