/*
 * regex.c -- regex primitives: re-pattern, re-find, re-matches.
 */

#include "prim/internal.h"
#include "mino.h"
#include "regex/re.h"

/* Construct a MINO_REGEX wrapping the given source string. The source
 * is held by reference (no copy) -- callers must pass an interned or
 * already GC-owned string. */
mino_val *mino_regex_from_source(mino_state *S, mino_val *source)
{
    mino_val *v;
    if (source == NULL || mino_type_of(source) != MINO_STRING) return NULL;
    v = alloc_val(S, MINO_REGEX);
    if (v == NULL) return NULL;
    v->as.regex.source = source;
    return v;
}

/* Pull out the pattern bytes from either a MINO_REGEX (canonical) or
 * a MINO_STRING (callers that pass a pattern source directly, such as
 * (re-find #"..." s) shorthand variants). Returns 0 if the argument
 * is neither. */
static int regex_source_view(const mino_val *v, const char **data, size_t *len)
{
    if (v == NULL) return 0;
    if (mino_type_of(v) == MINO_STRING) {
        *data = v->as.s.data;
        *len  = v->as.s.len;
        return 1;
    }
    if (mino_type_of(v) == MINO_REGEX && v->as.regex.source != NULL
        && mino_type_of(v->as.regex.source) == MINO_STRING) {
        *data = v->as.regex.source->as.s.data;
        *len  = v->as.regex.source->as.s.len;
        return 1;
    }
    return 0;
}

/* (re-pattern s) -- compile a regex from a source string, returning a
 * MINO_REGEX. Accepts a MINO_REGEX too (re-returned identically) so
 * `(re-pattern existing-pattern)` is a no-op. */
mino_val *prim_re_pattern(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *x;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "re-pattern requires one argument");
    }
    x = args->as.cons.car;
    if (x != NULL && mino_type_of(x) == MINO_REGEX) return x;
    if (x != NULL && mino_type_of(x) == MINO_STRING) {
        return mino_regex_from_source(S, x);
    }
    return prim_throw_classified(S, "eval/type", "MTY001",
        "re-pattern: argument must be a string or regex");
}

/* Build the [whole g1 g2 ...] vector for a successful match. The
 * pattern's group ids start at 1; missing/unmatched groups surface as
 * nil. The whole-match offset is the absolute start position in
 * text->as.s.data. */
static mino_val *match_vector(mino_state *S, mino_val *text_val,
                                int match_idx, int match_len,
                                const re_groups_t *g)
{
    mino_val *items[1 + RE_MAX_GROUPS];
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
mino_val *prim_re_find(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *pat_val, *text_val;
    re_t        compiled;
    re_groups_t groups;
    int         match_len = 0;
    int         match_idx;
    const char *pat_data;
    size_t      pat_len;
    (void)env;
    (void)pat_len;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "re-find requires two arguments");
    }
    pat_val  = args->as.cons.car;
    text_val = args->as.cons.cdr->as.cons.car;
    if (!regex_source_view(pat_val, &pat_data, &pat_len)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "re-find: first argument must be a pattern (regex or string)");
    }
    /* Real Clojure (re-find re nil) returns nil rather than throwing. */
    if (text_val == NULL || mino_type_of(text_val) == MINO_NIL) return mino_nil(S);
    if (mino_type_of(text_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "re-find: second argument must be a string");
    }
    /* The regex engine uses file-static globals (re_flags +
     * re_g_state) for match state, so every caller must serialize
     * through state_lock. The debug-build assert surfaces a missing
     * lock at the offending call site instead of silently corrupting
     * the next match's capture spans. */
    MINO_ASSERT_STATE_SAFE(S);
    compiled = re_compile(pat_data);
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
mino_val *prim_re_matches(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *pat_val, *text_val;
    re_t        compiled;
    re_groups_t groups;
    int         match_len = 0;
    int         match_idx;
    const char *pat_data;
    size_t      pat_len;
    (void)env;
    (void)pat_len;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr) ||
        mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "re-matches requires two arguments");
    }
    pat_val  = args->as.cons.car;
    text_val = args->as.cons.cdr->as.cons.car;
    if (!regex_source_view(pat_val, &pat_data, &pat_len)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "re-matches: first argument must be a pattern (regex or string)");
    }
    if (text_val == NULL || mino_type_of(text_val) == MINO_NIL) return mino_nil(S);
    if (mino_type_of(text_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001", "re-matches: second argument must be a string");
    }
    MINO_ASSERT_STATE_SAFE(S);
    compiled = re_compile(pat_data);
    if (compiled == NULL) {
        return prim_throw_classified(S, "eval/contract", "MCT001",
            "re-matches: invalid regex pattern");
    }
    match_idx = re_matchp_groups_anchored(compiled, text_val->as.s.data,
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

/* (re-find-from pattern text cp-start) -- like re-find, but starts
 * the scan at codepoint index cp-start and reports where the match
 * landed: nil or [match cp-start cp-end] with codepoint indices into
 * the full text. Backs re-seq and the stateful matcher so they
 * advance by the real match position instead of re-locating the
 * matched text by substring search (which picks the wrong site when
 * the same text occurs earlier, and cannot represent a zero-width
 * match). */
mino_val *prim_re_find_from(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *pat_val, *text_val, *pos_val;
    re_t        compiled;
    re_groups_t groups;
    int         match_len = 0;
    int         match_idx;
    const char *pat_data;
    size_t      pat_len;
    long long   cp_start;
    size_t      byte_start;
    long long   total_cps;
    (void)env;
    (void)pat_len;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || !mino_is_cons(args->as.cons.cdr->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "re-find-from requires three arguments");
    }
    pat_val  = args->as.cons.car;
    text_val = args->as.cons.cdr->as.cons.car;
    pos_val  = args->as.cons.cdr->as.cons.cdr->as.cons.car;
    if (!regex_source_view(pat_val, &pat_data, &pat_len)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "re-find-from: first argument must be a pattern (regex or string)");
    }
    if (text_val == NULL || mino_type_of(text_val) == MINO_NIL) return mino_nil(S);
    if (mino_type_of(text_val) != MINO_STRING) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "re-find-from: second argument must be a string");
    }
    if (pos_val == NULL || !mino_val_int_p(pos_val)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "re-find-from: start must be an integer");
    }
    cp_start = mino_val_int_get(pos_val);
    if (cp_start < 0) return mino_nil(S);
    total_cps = utf8_codepoint_count(text_val->as.s.data, text_val->as.s.len);
    if (cp_start > total_cps) return mino_nil(S);
    byte_start = utf8_skip_codepoints(text_val->as.s.data,
                                      text_val->as.s.len, 0, cp_start);
    MINO_ASSERT_STATE_SAFE(S);
    compiled = re_compile(pat_data);
    if (compiled == NULL) {
        return prim_throw_classified(S, "eval/contract", "MCT001",
            "re-find-from: invalid regex pattern");
    }
    match_idx = re_matchp_groups(compiled, text_val->as.s.data + byte_start,
                                 &match_len, &groups);
    re_free(compiled);
    if (match_idx == -1) {
        return mino_nil(S);
    }
    {
        const char *base = text_val->as.s.data + byte_start;
        long long   cp_m_start = cp_start
            + utf8_codepoint_count(base, (size_t)match_idx);
        long long   cp_m_end = cp_m_start
            + utf8_codepoint_count(base + match_idx, (size_t)match_len);
        mino_val *match;
        if (groups.n > 0) {
            /* Group offsets are relative to the scan base; build the
             * vector against that base rather than the text start. */
            mino_val *items[1 + RE_MAX_GROUPS];
            size_t      gn = 1;
            int         i;
            items[0] = mino_string_n(S, base + match_idx, (size_t)match_len);
            for (i = 0; i < groups.n; i++) {
                if (groups.starts[i] < 0 || groups.ends[i] < 0
                 || groups.ends[i] < groups.starts[i]) {
                    items[gn++] = mino_nil(S);
                } else {
                    items[gn++] = mino_string_n(S, base + groups.starts[i],
                        (size_t)(groups.ends[i] - groups.starts[i]));
                }
            }
            match = mino_vector(S, items, gn);
        } else {
            match = mino_string_n(S, base + match_idx, (size_t)match_len);
        }
        if (match == NULL) return NULL;
        {
            mino_val *items[3];
            items[0] = match;
            items[1] = mino_int(S, cp_m_start);
            items[2] = mino_int(S, cp_m_end);
            return mino_vector(S, items, 3);
        }
    }
}

const mino_prim_def k_prims_regex[] = {
    {"re-pattern", prim_re_pattern,
     "Returns a regex from a string pattern (no-op on an existing regex)."},
    {"re-find",    prim_re_find,
     "Returns the first regex match in the string, or nil."},
    {"re-matches", prim_re_matches,
     "Returns the match if the entire string matches the regex, or nil."},
    {"re-find-from", prim_re_find_from,
     "Internal: finds the first match at or after a codepoint index; returns [match start end] or nil."},
};

const size_t k_prims_regex_count =
    sizeof(k_prims_regex) / sizeof(k_prims_regex[0]);

void mino_install_regex(mino_state *S, mino_env *env)
{
    mino_env *core_env = ns_env_ensure(S, "clojure.core");
    (void)env;
    prim_install_table_with_capability(S, core_env, "clojure.core",
                                       k_prims_regex, k_prims_regex_count,
                                       "regex");
    S->caps_installed |= MINO_CAP_REGEX;
}
