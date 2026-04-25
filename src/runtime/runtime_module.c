/*
 * runtime_module.c -- shared module-resolution helpers.
 *
 * Two call sites -- eval_special_defs.c (the ns special form) and
 * prim_module.c (the require primitive with a vector spec) -- used to
 * carry their own copies of dotted-name-to-path conversion and alias
 * table mutation. They drifted: the require-side appended aliases
 * without checking for duplicates, silently swallowed malloc failures
 * mid-record, and had a subtly different lifetime for its temporary
 * strings. This file holds the canonical implementation that both
 * call sites share.
 */

#include "runtime_internal.h"

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

/* Add or update an ns alias entry: "alias" -> "full.module.name".
 * ALIAS and FULL are NUL-terminated byte strings. If an entry for
 * ALIAS already exists its full_name is replaced. On OOM the call
 * is silently a no-op (matches the prior behavior at both sites). */
void runtime_module_add_alias(mino_state_t *S,
                              const char *alias, const char *full)
{
    size_t i;
    size_t alen = strlen(alias);
    size_t flen = strlen(full);
    char  *a;
    char  *f;

    for (i = 0; i < S->ns_alias_len; i++) {
        if (strcmp(S->ns_aliases[i].alias, alias) == 0) {
            char *replaced = dup_str(full, flen);
            if (replaced == NULL) return;
            free(S->ns_aliases[i].full_name);
            S->ns_aliases[i].full_name = replaced;
            return;
        }
    }

    if (S->ns_alias_len == S->ns_alias_cap) {
        size_t new_cap = S->ns_alias_cap == 0 ? 8 : S->ns_alias_cap * 2;
        ns_alias_t *nb = (ns_alias_t *)realloc(
            S->ns_aliases, new_cap * sizeof(*nb));
        if (nb == NULL) return;
        S->ns_aliases   = nb;
        S->ns_alias_cap = new_cap;
    }

    a = dup_str(alias, alen);
    f = dup_str(full,  flen);
    if (a == NULL || f == NULL) {
        free(a);
        free(f);
        return;
    }
    S->ns_aliases[S->ns_alias_len].alias     = a;
    S->ns_aliases[S->ns_alias_len].full_name = f;
    S->ns_alias_len++;
}
