/*
 *
 * Mini regex-module inspired by Rob Pike's regex code described in:
 *
 * http://www.cs.princeton.edu/courses/archive/spr09/cos333/beautiful.html
 *
 *
 *
 * Supports:
 * ---------
 *   '.'        Dot, matches any character
 *   '^'        Start anchor, matches beginning of string
 *   '$'        End anchor, matches end of string
 *   '*'        Asterisk, match zero or more (greedy)
 *   '+'        Plus, match one or more (greedy)
 *   '?'        Question, match zero or one (non-greedy)
 *   '[abc]'    Character class, match if one of {'a', 'b', 'c'}
 *   '[^abc]'   Inverted class, match if NOT one of {'a', 'b', 'c'}
 *   '[a-zA-Z]' Character ranges, the character set of the ranges { a-z | A-Z }
 *   '\s'       Whitespace, \t \f \r \n \v and spaces
 *   '\S'       Non-whitespace
 *   '\w'       Alphanumeric, [a-zA-Z0-9_]
 *   '\W'       Non-alphanumeric
 *   '\d'       Digits, [0-9]
 *   '\D'       Non-digits
 *
 *
 */

#ifndef _TINY_REGEX_C
#define _TINY_REGEX_C


#ifndef RE_DOT_MATCHES_NEWLINE
/* Define to 0 if you DON'T want '.' to match '\r' + '\n' */
#define RE_DOT_MATCHES_NEWLINE 1
#endif

#ifdef __cplusplus
extern "C"{
#endif



/* Typedef'd pointer to get abstract datatype. */
typedef struct regex_t* re_t;


/* Compile regex string pattern to a regex_t-array.
 * Returns a heap-allocated pattern; caller must free with re_free.
 * Returns NULL on invalid pattern or allocation failure. */
re_t re_compile(const char* pattern);

/* Free a compiled pattern returned by re_compile. */
void re_free(re_t pattern);

/* Find matches of the compiled pattern inside text. */
int re_matchp(re_t pattern, const char* text, int* matchlength);


/* Find matches of the txt pattern inside text (will compile automatically first). */
int re_match(const char* pattern, const char* text, int* matchlength);

/* Capture-group support.
 *   Maximum capture groups in a pattern (Clojure indexes 9 positionally
 *   via re-groups; mino's #" reader has no group-name syntax). */
#define RE_MAX_GROUPS 16

typedef struct re_groups_s {
  int n;                      /* number of captured groups (0..RE_MAX_GROUPS) */
  int starts[RE_MAX_GROUPS];  /* offset relative to the matched text start    */
  int ends[RE_MAX_GROUPS];    /* end offset (exclusive); -1 if not matched    */
} re_groups_t;

/* Number of groups in the compiled pattern. */
int re_n_groups(re_t pattern);

/* re_matchp_groups: like re_matchp, but also fills `out` with span
 * positions for each capture group on a successful match. The spans
 * are byte offsets relative to the input `text` (so callers should add
 * them to the match start). On no match returns -1 and `out` is left
 * untouched. */
int re_matchp_groups(re_t pattern, const char* text, int* matchlength,
                     re_groups_t* out);


#ifdef __cplusplus
}
#endif

#endif /* ifndef _TINY_REGEX_C */
