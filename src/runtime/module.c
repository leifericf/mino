/*
 * module.c -- shared module-resolution helpers used by the ns
 * special form (eval/defs.c) and the require primitive
 * (prim/module.c). Both call sites share the same dotted-name-to-
 * path conversion and alias-table mutation here so the ns and
 * require paths cannot drift on duplicate-alias handling, malloc
 * failure semantics, or temp-string lifetime.
 */

#include "runtime/internal.h"

/* Convert a dotted module symbol like "some.lib" into a slash
 * separated path like "some/lib" in BUF (size BUFSIZE). Returns 0 on
 * success, -1 if the name is empty, too long, or starts/ends with a
 * dot. Dashes inside segments are preserved as underscores to match
 * the on-disk filename convention. */
int runtime_module_dotted_to_path(const char *name, size_t nlen,
                                  char *buf, size_t bufsize)
{
    size_t i;
    if (nlen == 0 || nlen >= bufsize) return -1;
    if (name[0] == '.' || name[nlen - 1] == '.') return -1;
    for (i = 0; i < nlen; i++) {
        if      (name[i] == '.')  buf[i] = '/';
        else if (name[i] == '-')  buf[i] = '_';
        else                      buf[i] = name[i];
    }
    buf[nlen] = '\0';
    return 0;
}

static char *dup_str(const char *s, size_t n)
{
    char *d = (char *)malloc(n + 1);
    if (d == NULL) return NULL;
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

/* Add or update an ns alias entry: "alias" -> "full.module.name" in
 * the current namespace. ALIAS and FULL are NUL-terminated byte
 * strings. If an entry for the same (current_ns, alias) already
 * exists its full_name is replaced. Returns 0 on success and -1 on
 * OOM; callers should surface failures via the standard diagnostic
 * path so OOM during alias add is catchable. */
int runtime_module_add_alias(mino_state_t *S,
                             const char *alias, const char *full)
{
    size_t i;
    size_t alen = strlen(alias);
    size_t flen = strlen(full);
    const char *owner = S->current_ns != NULL ? S->current_ns : "user";
    size_t olen = strlen(owner);
    char  *o;
    char  *a;
    char  *f;

    for (i = 0; i < S->ns_alias_len; i++) {
        if (S->ns_aliases[i].owning_ns != NULL
            && strcmp(S->ns_aliases[i].owning_ns, owner) == 0
            && strcmp(S->ns_aliases[i].alias, alias) == 0) {
            char *replaced = dup_str(full, flen);
            if (replaced == NULL) return -1;
            free(S->ns_aliases[i].full_name);
            S->ns_aliases[i].full_name = replaced;
            return 0;
        }
    }

    if (S->ns_alias_len == S->ns_alias_cap) {
        size_t new_cap = S->ns_alias_cap == 0 ? 8 : S->ns_alias_cap * 2;
        ns_alias_t *nb = (ns_alias_t *)realloc(
            S->ns_aliases, new_cap * sizeof(*nb));
        if (nb == NULL) return -1;
        S->ns_aliases   = nb;
        S->ns_alias_cap = new_cap;
    }

    o = dup_str(owner, olen);
    a = dup_str(alias, alen);
    f = dup_str(full,  flen);
    if (o == NULL || a == NULL || f == NULL) {
        free(o);
        free(a);
        free(f);
        return -1;
    }
    S->ns_aliases[S->ns_alias_len].owning_ns = o;
    S->ns_aliases[S->ns_alias_len].alias     = a;
    S->ns_aliases[S->ns_alias_len].full_name = f;
    S->ns_alias_len++;
    return 0;
}
