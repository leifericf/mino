/*
 * regex.c -- regex primitives: re-find, re-matches.
 */

#include "prim/internal.h"
#include "regex/re.h"

/* (re-find pattern text) -- find first match of pattern in text.
 * Returns the matched substring, or nil if no match. */
mino_val_t *prim_re_find(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *pat_val, *text_val;
    int match_len = 0;
    int match_idx;
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
    match_idx = re_match(pat_val->as.s.data, text_val->as.s.data, &match_len);
    if (match_idx == -1) {
        return mino_nil(S);
    }
    return mino_string_n(S, text_val->as.s.data + match_idx, (size_t)match_len);
}

/* (re-matches pattern text) -- true if the entire text matches pattern. */
mino_val_t *prim_re_matches(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *pat_val, *text_val;
    int match_len = 0;
    int match_idx;
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
    match_idx = re_match(pat_val->as.s.data, text_val->as.s.data, &match_len);
    if (match_idx == 0 && (size_t)match_len == text_val->as.s.len) {
        return text_val;
    }
    return mino_nil(S);
}

const mino_prim_def k_prims_regex[] = {
    {"re-find",    prim_re_find,
     "Returns the first regex match in the string, or nil."},
    {"re-matches", prim_re_matches,
     "Returns the match if the entire string matches the regex, or nil."},
};

const size_t k_prims_regex_count =
    sizeof(k_prims_regex) / sizeof(k_prims_regex[0]);
