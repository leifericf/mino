/*
 * regex.c -- regex primitives: re-find, re-matches.
 */

#include "prim/internal.h"
#include "regex/re.h"

/* Build the [whole g1 g2 ...] vector for a successful match. The
 * pattern's group ids start at 1; missing/unmatched groups surface as
 * nil. The whole-match offset is the absolute start position in
 * text->as.s.data. */
static mino_val_t *match_vector(mino_state_t *S, mino_val_t *text_val,
                                int match_idx, int match_len,
                                const re_groups_t *g)
{
    mino_val_t *items[1 + RE_MAX_GROUPS];
    size_t      n = 1;
    int         i;
    items[0] = mino_string_n(S, text_val->as.s.data + match_idx,
                             (size_t)match_len);
    for (i = 0; i < g->n; i++) {
        if (g->starts[i] < 0 || g->ends[i] < 0
         || g->ends[i] < g->starts[i]) {
            items[n++] = mino_nil(S);
        } else {
            items[n++] = mino_string_n(S,
                text_val->as.s.data + g->starts[i],
                (size_t)(g->ends[i] - g->starts[i]));
        }
    }
    return mino_vector(S, items, n);
}

/* (re-find pattern text) -- find first match of pattern in text.
 * Returns the matched substring (no groups) or [whole g1 g2 ...]
 * (when the pattern has capture groups), or nil if no match. */
mino_val_t *prim_re_find(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *pat_val, *text_val;
    re_t        compiled;
    re_groups_t groups;
    int         match_len = 0;
    int         match_idx;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "re-find requires two arguments");
    }
    pat_val  = args->as.cons.car;
    text_val = args->as.cons.cdr->as.cons.car;
    if (pat_val == NULL || pat_val->type != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "re-find: first argument must be a pattern string");
    }
    /* Real Clojure (re-find re nil) returns nil rather than throwing. */
    if (text_val == NULL || text_val->type == MINO_NIL) return mino_nil(S);
    if (text_val->type != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "re-find: second argument must be a string");
    }
    compiled = re_compile(pat_val->as.s.data);
    if (compiled == NULL) {
        return prim_throw_classified(S, "eval/contract", "MCT001",
            "re-find: invalid regex pattern");
    }
    match_idx = re_matchp_groups(compiled, text_val->as.s.data,
                                 &match_len, &groups);
    re_free(compiled);
    if (match_idx == -1) {
        return mino_nil(S);
    }
    if (groups.n > 0) {
        return match_vector(S, text_val, match_idx, match_len, &groups);
    }
    return mino_string_n(S, text_val->as.s.data + match_idx, (size_t)match_len);
}

/* (re-matches pattern text) -- match anchored to whole string.
 * Returns the matched substring (no groups) or [whole g1 g2 ...]
 * (with groups), or nil. */
mino_val_t *prim_re_matches(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *pat_val, *text_val;
    re_t        compiled;
    re_groups_t groups;
    int         match_len = 0;
    int         match_idx;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "re-matches requires two arguments");
    }
    pat_val  = args->as.cons.car;
    text_val = args->as.cons.cdr->as.cons.car;
    if (pat_val == NULL || pat_val->type != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "re-matches: first argument must be a pattern string");
    }
    if (text_val == NULL || text_val->type == MINO_NIL) return mino_nil(S);
    if (text_val->type != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "re-matches: second argument must be a string");
    }
    compiled = re_compile(pat_val->as.s.data);
    if (compiled == NULL) {
        return prim_throw_classified(S, "eval/contract", "MCT001",
            "re-matches: invalid regex pattern");
    }
    match_idx = re_matchp_groups(compiled, text_val->as.s.data,
                                 &match_len, &groups);
    re_free(compiled);
    if (match_idx != 0 || (size_t)match_len != text_val->as.s.len) {
        return mino_nil(S);
    }
    if (groups.n > 0) {
        return match_vector(S, text_val, match_idx, match_len, &groups);
    }
    return text_val;
}

const mino_prim_def k_prims_regex[] = {
    {"re-find",    prim_re_find,
     "Returns the first regex match in the string, or nil."},
    {"re-matches", prim_re_matches,
     "Returns the match if the entire string matches the regex, or nil."},
};

const size_t k_prims_regex_count =
    sizeof(k_prims_regex) / sizeof(k_prims_regex[0]);
