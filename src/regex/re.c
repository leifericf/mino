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
 *   '?'        Question, match zero or one (greedy)
 *   '[abc]'    Character class, match if one of {'a', 'b', 'c'}
 *   '[^abc]'   Inverted class, match if NOT one of {'a', 'b', 'c'}
 *   '[a-zA-Z]' Character ranges, the character set of the ranges { a-z | A-Z }
 *   '\s'       Whitespace, \t \f \r \n \v and spaces
 *   '\S'       Non-whitespace
 *   '\w'       Alphanumeric, [a-zA-Z0-9_]
 *   '\W'       Non-alphanumeric
 *   '\d'       Digits, [0-9]
 *   '\D'       Non-digits
 *   '\b' '\B'  Word boundary / non-boundary (zero-width)
 *   '\n' &c.   Control-character escapes: \n \r \t \f \a \e \0
 *              (inside a class, \b is the backspace character)
 *
 *
 * Style note. This translation unit and its header preserve the
 * upstream tinyregex-c style (Allman braces, two-space indent, dense
 * locals, fixed-size pattern arena). Per Rule 15 of the project's C
 * implementation guide, the local conventions inside src/regex/ take
 * precedence over the codebase-wide style. Do not reformat to match
 * the rest of the tree. The module reaches no other mino subsystem
 * and its public surface is the four functions in re.h; only
 * src/prim/regex.c consumes the header.
 *
 */



#include "re.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Definitions: */

#define MAX_REGEXP_OBJECTS      256   /* Max number of regex symbols in expression. */
#define MAX_CHAR_CLASS_LEN      256   /* Max length of character-class buffer in.   */


/* RE_CHAR (not CHAR): the bare `CHAR` token collides with the Win32
 * `CHAR` typedef from <windows.h>, which other TUs pull in under
 * _WIN32. In the single-file amalgamation those includes precede this
 * enum, so the unprefixed name breaks a Windows amalgam build. */
enum { UNUSED, DOT, BEGIN, END, QUESTIONMARK, STAR, PLUS, RE_CHAR, CHAR_CLASS, INV_CHAR_CLASS, DIGIT, NOT_DIGIT, ALPHA, NOT_ALPHA, WHITESPACE, NOT_WHITESPACE, GROUP_OPEN, GROUP_CLOSE, BOUNDED, SET_FLAGS, ALT, WORD_BOUNDARY, NOT_WORD_BOUNDARY, LAZY_QUESTIONMARK, LAZY_STAR, LAZY_PLUS, LAZY_BOUNDED };

/* Inline-flag bits parsed from JVM-style (?<flags>) syntax. The
 * compiler emits a SET_FLAGS slot that the matcher absorbs at its
 * GROUP_OPEN/GROUP_CLOSE skip-loop, updating a static `re_flags`
 * word. Per-slot flags so writes like (?i)abc(?-i)def behave the
 * same as JVM Pattern: each region honors the flag state in effect
 * at the time the matcher walks past it. */
#define RE_FLAG_ICASE      (1u << 0)  /* (?i) case-insensitive       */
#define RE_FLAG_DOTALL     (1u << 1)  /* (?s) dot matches newline    */
#define RE_FLAG_MULTILINE  (1u << 2)  /* (?m) ^ $ at line boundaries */
#define RE_FLAG_EXTENDED   (1u << 3)  /* (?x) strip pattern ws       */

/* Active flag word during a match. Reset to the pattern-level
 * compile-time default by re_matchp before walking the pattern.
 * Updated as the matcher absorbs SET_FLAGS slots. */
static unsigned char re_flags;

/* When set, pattern completion requires end-of-input: backs the
 * whole-string matchers (re-matches) so lazy quantifiers keep growing
 * until the input is consumed instead of stopping at the shortest
 * accepting prefix. Set only by re_matchp_groups_anchored. */
static int re_anchor_end;

typedef struct regex_t
{
  unsigned char  type;   /* RE_CHAR, STAR, etc.                      */
  union
  {
    unsigned char  ch;   /*      the character itself             */
    unsigned char* ccl;  /*  OR  a pointer to characters in class */
    unsigned char  gid;  /*  OR  the capture group id (1..15)     */
    struct {             /*  OR  the bounded-repeat min/max for   */
      unsigned char min; /*  {n}, {n,m}, {n,}. {n} stores min==max. */
      unsigned char max; /*  {n,} stores max == 0xFF (unbounded). */
    } bnd;
    struct {             /*  OR  the inline-flag op for SET_FLAGS: */
      unsigned char mask; /* the flag bits to apply               */
      unsigned char set;  /* 1 = OR mask into re_flags; 0 = clear */
    } flg;
  } u;
} regex_t;

/* Per-pattern group count is recorded in the sentinel UNUSED slot's
 * u.gid field at the end of the compiled buffer. re_n_groups reads
 * it back. */

/* Capture-group state for the active match call. The matcher walks
 * the pattern in a forward-only fashion with limited backtracking
 * inside matchstar / matchplus / matchquestion, so a successful match
 * leaves spans pointing at the final write for each group. */
static struct {
  const char *base;                 /* start of input text */
  int         n;
  int         starts[RE_MAX_GROUPS];
  int         ends[RE_MAX_GROUPS];
} re_g_state;



/* Private function declarations: */
static int re_match_width(regex_t p, const char* text);
static const char* re_back_one(regex_t p, const char* floor, const char* text);
static int matchpattern(regex_t* pattern, const char* text, int* matchlength);
static int matchcharclass(char c, const char* str);
static int matchstar(regex_t p, regex_t* pattern, const char* text, int* matchlength);
static int matchplus(regex_t p, regex_t* pattern, const char* text, int* matchlength);
static int matchone(regex_t p, char c);
static int matchdigit(char c);
static int matchalpha(char c);
static int matchwhitespace(char c);
static int matchmetachar(char c, const char* str);
static int matchrange(char c, const char* str);
static int matchdot(char c);
static int ismetachar(char c);



/* Public functions: */
int re_match(const char* pattern, const char* text, int* matchlength)
{
  int result;
  re_t compiled = re_compile(pattern);
  result = re_matchp(compiled, text, matchlength);
  re_free(compiled);
  return result;
}

static void re_g_state_reset(const char* base, int n)
{
  int i;
  re_g_state.base = base;
  re_g_state.n    = n;
  for (i = 0; i < RE_MAX_GROUPS; i++)
  {
    re_g_state.starts[i] = -1;
    re_g_state.ends[i]   = -1;
  }
}

int re_matchp_groups_anchored(re_t pattern, const char* text,
                              int* matchlength, re_groups_t* out)
{
  int idx;
  re_anchor_end = 1;
  idx = re_matchp_groups(pattern, text, matchlength, out);
  re_anchor_end = 0;
  return idx;
}

int re_matchp_groups(re_t pattern, const char* text, int* matchlength,
                     re_groups_t* out)
{
  int idx;
  int n_groups = 0;
  if (pattern != 0)
  {
    int j = 0;
    while (pattern[j].type != UNUSED) j++;
    n_groups = (int)pattern[j].u.gid;
  }
  re_g_state_reset(text, n_groups);
  idx = re_matchp(pattern, text, matchlength);
  if (out != NULL)
  {
    int i;
    out->n = n_groups;
    for (i = 0; i < RE_MAX_GROUPS; i++)
    {
      out->starts[i] = re_g_state.starts[i];
      out->ends[i]   = re_g_state.ends[i];
    }
  }
  return idx;
}

/* Walk the compiled pattern array; return the slot index of the
 * next top-level ALT (`|` not inside a group), or the UNUSED
 * sentinel index if none remains. Used by re_matchp to split a
 * pattern with top-level alternations and try each branch. */
static int next_top_alt(re_t pattern, int start)
{
  int depth = 0;
  int i;
  for (i = start; pattern[i].type != UNUSED; i++)
  {
    if (pattern[i].type == GROUP_OPEN)       depth++;
    else if (pattern[i].type == GROUP_CLOSE) { if (depth > 0) depth--; }
    else if (pattern[i].type == ALT && depth == 0) return i;
  }
  return i;
}

/* Single-branch match. Same shape as the prior re_matchp (no
 * top-level alternation awareness). re_matchp dispatches to this
 * once per alternative. */
static int re_matchp_one(re_t pattern, const char* text, int* matchlength);

int re_matchp(re_t pattern, const char* text, int* matchlength)
{
  if (pattern == 0) return -1;
  /* If the pattern has no top-level alternation, the single-branch
   * path is faithful. Detect by scanning for ALT at depth 0. */
  {
    int first_alt = next_top_alt(pattern, 0);
    if (pattern[first_alt].type != ALT)
    {
      return re_matchp_one(pattern, text, matchlength);
    }
    /* Top-level alternation: try each branch in left-to-right order
     * at the earliest matching position. JVM's regex picks the first
     * alternative that matches at the leftmost position, so we
     * iterate position outermost, branches innermost. */
    {
      int             alt_start = 0;
      int             best_idx  = -1;
      int             best_len  = 0;
      regex_t        *mp        = (regex_t *)pattern;
      while (1)
      {
        int    end       = next_top_alt(pattern, alt_start);
        int    saved     = mp[end].type;
        int    branch_ml = 0;
        int    branch_idx;
        /* Temporarily terminate the branch with UNUSED so the inner
         * matchpattern stops there. Restored after the call. */
        mp[end].type = UNUSED;
        branch_idx = re_matchp_one(&mp[alt_start], text, &branch_ml);
        mp[end].type = (unsigned char)saved;
        if (branch_idx >= 0)
        {
          if (best_idx < 0
              || branch_idx < best_idx
              || (branch_idx == best_idx && branch_ml > best_len))
          {
            best_idx = branch_idx;
            best_len = branch_ml;
          }
        }
        if (mp[end].type != ALT) break;
        alt_start = end + 1;
      }
      if (best_idx < 0) return -1;
      *matchlength = best_len;
      return best_idx;
    }
  }
}

static int re_matchp_one(re_t pattern, const char* text, int* matchlength)
{
  *matchlength = 0;
  /* Reset group spans for groupless callers too, so stale state from
   * a prior re_matchp_groups call doesn't bleed in. The base pointer
   * is set so GROUP_OPEN/CLOSE markers (if any) can record offsets. */
  if (re_g_state.base != text)
  {
    re_g_state_reset(text, 0);
  }
  /* Reset inline-flag state. The first SET_FLAGS slot inside
   * matchpattern (if any) installs the leading flags. */
  re_flags = 0;
  if (pattern != 0)
  {
    /* If the pattern starts with one or more SET_FLAGS slots followed
     * by BEGIN, absorb them before checking the BEGIN anchor so a
     * pattern like "(?i)^abc" still pins to text[0]. */
    int p0 = 0;
    while (pattern[p0].type == SET_FLAGS)
    {
      if (pattern[p0].u.flg.set) re_flags |=  pattern[p0].u.flg.mask;
      else                       re_flags &= ~pattern[p0].u.flg.mask;
      p0++;
    }
    if (pattern[p0].type == BEGIN)
    {
      /* In (?m) multiline mode, `^` matches at the start of input
       * AND at every position immediately after a `\n`. Without
       * (?m) it pins to text[0]. */
      if (re_flags & RE_FLAG_MULTILINE)
      {
        const char *base       = text;
        unsigned char leading  = re_flags;
        int           idx      = 0;
        while (1)
        {
          re_flags = leading;
          if (matchpattern(&pattern[p0+1], text, matchlength))
          {
            return idx;
          }
          /* Advance to the position right after the next newline. */
          while (*text != '\0' && *text != '\n') { text++; idx++; }
          if (*text == '\0') break;
          text++; idx++; /* skip the newline */
        }
        (void)base;
        return -1;
      }
      return ((matchpattern(&pattern[p0+1], text, matchlength)) ? 0 : -1);
    }
    else
    {
      int idx = -1;
      unsigned char leading_flags = re_flags;

      do
      {
        idx += 1;
        re_flags = leading_flags; /* restart with leading flags each try */

        if (matchpattern(&pattern[p0], text, matchlength))
        {
          /* A zero-width match at the terminal position is a real
           * match -- empty-capable patterns match the empty string
           * and the end-of-input site, as in the canonical engines. */
          return idx;
        }
      }
      while (*text++ != '\0');
    }
  }
  return -1;
}

void re_free(re_t pattern)
{
  free(pattern);
}

re_t re_compile(const char* pattern)
{
  /* Allocate compile buffers on the heap so that multiple patterns can
   * coexist and compilation is reentrant/thread-safe.  The regex_t
   * array and the char-class buffer are laid out in a single allocation:
   *   [MAX_REGEXP_OBJECTS * regex_t] [MAX_CHAR_CLASS_LEN bytes]       */
  size_t obj_bytes = MAX_REGEXP_OBJECTS * sizeof(regex_t);
  size_t total     = obj_bytes + MAX_CHAR_CLASS_LEN;
  regex_t       *re_compiled;
  unsigned char *ccl_buf;
  int ccl_bufidx = 1;

  char c;     /* current char in pattern   */
  int i = 0;  /* index into pattern        */
  int j = 0;  /* index into re_compiled    */
  int           group_count       = 0;
  unsigned char group_stack[RE_MAX_GROUPS];
  int           group_stack_depth = 0;
  /* `extended_mode` mirrors the (?x) flag at compile time so the
   * parser can skip whitespace and `#`-line comments before they
   * become RE_CHAR slots. Toggled when (?x) / (?-x) inline-flag ops
   * are parsed. */
  int           extended_mode     = 0;

  re_compiled = (regex_t *)malloc(total);
  if (re_compiled == NULL) return 0;
  memset(re_compiled, 0, total);
  ccl_buf = (unsigned char *)re_compiled + obj_bytes;

  while (pattern[i] != '\0' && (j+1 < MAX_REGEXP_OBJECTS))
  {
    c = pattern[i];

    /* Extended-mode preprocessing: skip whitespace and #-line
     * comments before tokenizing. (?x) inside the pattern toggles
     * this flag on the fly via the SET_FLAGS handler below. */
    if (extended_mode)
    {
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
      {
        i++;
        continue;
      }
      if (c == '#')
      {
        while (pattern[i] != '\0' && pattern[i] != '\n') i++;
        continue;
      }
    }

    switch (c)
    {
      /* Meta-characters: */
      case '^': {    re_compiled[j].type = BEGIN;           } break;
      case '$': {    re_compiled[j].type = END;             } break;
      case '.': {    re_compiled[j].type = DOT;             } break;
      /* A '?' right after a quantifier selects the lazy variant. */
      case '*': {    re_compiled[j].type = STAR;
                     if (pattern[i+1] == '?') { re_compiled[j].type = LAZY_STAR; i++; } } break;
      case '+': {    re_compiled[j].type = PLUS;
                     if (pattern[i+1] == '?') { re_compiled[j].type = LAZY_PLUS; i++; } } break;
      case '?': {    re_compiled[j].type = QUESTIONMARK;
                     if (pattern[i+1] == '?') { re_compiled[j].type = LAZY_QUESTIONMARK; i++; } } break;
      /* Bounded repeat: {n} / {n,} / {n,m}. Modifies the preceding atom
       * by recording a min/max repeat count. Clamps to 0..255 since the
       * union storage is two bytes; {n,} encodes max == 0xFF (unbounded
       * sentinel). Malformed {...} (missing closing brace, non-digit
       * inside) is rejected by failing the compile, matching mino's
       * regex-engine contract of returning NULL on bad input rather
       * than silently treating the braces as literals. */
      case '{':
      {
        int min_v = 0, max_v = 0;
        int has_max = 0;
        int saved_i = i;
        i++;
        if (pattern[i] < '0' || pattern[i] > '9')
        {
          /* Not a valid {n...}; treat '{' as a literal character so
           * `{abc}` and similar non-quantifier braces still parse. */
          i = saved_i;
          re_compiled[j].type = RE_CHAR;
          re_compiled[j].u.ch = '{';
          break;
        }
        while (pattern[i] >= '0' && pattern[i] <= '9')
        {
          /* Saturate at the eventual clamp (255) instead of letting the
           * accumulator overflow: a pathological count like {9999999999}
           * would otherwise wrap a signed int (UB). The post-loop clamp
           * caps the bound at 255 regardless, so stopping early is
           * behaviour-preserving for every in-range count. */
          if (min_v <= 255) min_v = min_v * 10 + (pattern[i] - '0');
          i++;
        }
        if (pattern[i] == ',')
        {
          i++;
          if (pattern[i] == '}')
          {
            has_max = 0; /* {n,} -- unbounded */
          }
          else if (pattern[i] >= '0' && pattern[i] <= '9')
          {
            while (pattern[i] >= '0' && pattern[i] <= '9')
            {
              if (max_v <= 255) max_v = max_v * 10 + (pattern[i] - '0');
              i++;
            }
            has_max = 1;
          }
          else
          {
            free(re_compiled); return 0;
          }
        }
        else
        {
          max_v = min_v; /* {n} -- exact */
          has_max = 1;
        }
        if (pattern[i] != '}')
        {
          free(re_compiled); return 0;
        }
        if (min_v > 255) min_v = 255;
        if (has_max && max_v > 255) max_v = 255;
        re_compiled[j].type = BOUNDED;
        if (pattern[i+1] == '?') { re_compiled[j].type = LAZY_BOUNDED; i++; }
        re_compiled[j].u.bnd.min = (unsigned char)min_v;
        re_compiled[j].u.bnd.max = has_max ? (unsigned char)max_v : (unsigned char)0xFF;
      } break;
      case '|':  {    re_compiled[j].type = ALT;             } break;

      /* Escaped character-classes (\s \w ...): */
      case '\\':
      {
        if (pattern[i+1] != '\0')
        {
          /* Skip the escape-char '\\' */
          i += 1;
          /* ... and check the next */
          switch (pattern[i])
          {
            /* Meta-character: */
            case 'd': {    re_compiled[j].type = DIGIT;            } break;
            case 'D': {    re_compiled[j].type = NOT_DIGIT;        } break;
            case 'w': {    re_compiled[j].type = ALPHA;            } break;
            case 'W': {    re_compiled[j].type = NOT_ALPHA;        } break;
            case 's': {    re_compiled[j].type = WHITESPACE;       } break;
            case 'S': {    re_compiled[j].type = NOT_WHITESPACE;   } break;

            /* Zero-width word-boundary assertions. */
            case 'b': {    re_compiled[j].type = WORD_BOUNDARY;     } break;
            case 'B': {    re_compiled[j].type = NOT_WORD_BOUNDARY; } break;

            /* Control-character escapes. */
            case 'n': {    re_compiled[j].type = RE_CHAR;
                           re_compiled[j].u.ch = '\n';            } break;
            case 'r': {    re_compiled[j].type = RE_CHAR;
                           re_compiled[j].u.ch = '\r';            } break;
            case 't': {    re_compiled[j].type = RE_CHAR;
                           re_compiled[j].u.ch = '\t';            } break;
            case 'f': {    re_compiled[j].type = RE_CHAR;
                           re_compiled[j].u.ch = '\f';            } break;
            case 'a': {    re_compiled[j].type = RE_CHAR;
                           re_compiled[j].u.ch = '\a';            } break;
            case 'e': {    re_compiled[j].type = RE_CHAR;
                           re_compiled[j].u.ch = 0x1b;            } break;
            case '0': {    re_compiled[j].type = RE_CHAR;
                           re_compiled[j].u.ch = '\0';            } break;

            /* Escaped character, e.g. '.' or '$' */
            default:
            {
              re_compiled[j].type = RE_CHAR;
              re_compiled[j].u.ch = pattern[i];
            } break;
          }
        }
        /* '\\' as last char in pattern -> invalid regular expression. */
/*
        else
        {
          re_compiled[j].type = RE_CHAR;
          re_compiled[j].ch = pattern[i];
        }
*/
      } break;

      /* Character class: */
      case '[':
      {
        /* Remember where the char-buffer starts. */
        int buf_begin = ccl_bufidx;

        /* Look-ahead to determine if negated */
        if (pattern[i+1] == '^')
        {
          re_compiled[j].type = INV_CHAR_CLASS;
          i += 1; /* Increment i to avoid including '^' in the char-buffer */
          if (pattern[i+1] == 0) /* incomplete pattern, missing non-zero char after '^' */
          {
            free(re_compiled); return 0;
          }
        }
        else
        {
          re_compiled[j].type = CHAR_CLASS;
        }

        /* Copy characters inside [..] to buffer */
        while (    (pattern[++i] != ']')
                && (pattern[i]   != '\0')) /* Missing ] */
        {
          if (pattern[i] == '\\')
          {
            if (ccl_bufidx >= MAX_CHAR_CLASS_LEN - 1)
            {
              free(re_compiled); return 0;
            }
            if (pattern[i+1] == 0) /* incomplete pattern, missing non-zero char after '\\' */
            {
              free(re_compiled); return 0;
            }
            ccl_buf[ccl_bufidx++] = pattern[i++];
          }
          else if (ccl_bufidx >= MAX_CHAR_CLASS_LEN)
          {
              free(re_compiled); return 0;
          }
          ccl_buf[ccl_bufidx++] = pattern[i];
        }
        if (ccl_bufidx >= MAX_CHAR_CLASS_LEN)
        {
            /* Catches cases such as [00000000000000000000000000000000000000][ */
            free(re_compiled); return 0;
        }
        /* Null-terminate string end */
        ccl_buf[ccl_bufidx++] = 0;
        re_compiled[j].u.ccl = &ccl_buf[buf_begin];
      } break;

      /* Capture groups. Maximum nesting depth = RE_MAX_GROUPS.
       *
       * (? introduces an inline-flag op: (?<flags>) sets/clears
       * pattern-level flags from this point onwards; (?<flags>:...) is
       * the scoped variant (a non-capturing group with local flags),
       * currently rejected with a clear diagnostic because mino's
       * matchpattern has no group-scoped state to restore the prior
       * flag word on group exit. */
      case '(':
      {
        if (pattern[i+1] == '?')
        {
          /* (?:...) -- non-capturing group: a group marker with gid 0,
           * which the matcher's record path skips. */
          if (pattern[i+2] == ':')
          {
            if (group_stack_depth >= RE_MAX_GROUPS - 1)
            {
              free(re_compiled); return 0;
            }
            group_stack[group_stack_depth++] = 0;
            re_compiled[j].type  = GROUP_OPEN;
            re_compiled[j].u.gid = 0;
            i += 2; /* leave i on ':'; the outer i++ steps past it */
            break;
          }
          /* (?<name>...) -- named capture: compiled as the next
           * positional group (canonical numbering includes named
           * groups); the name itself is not retained. (?<= and (?<!
           * lookbehind fall through to the flag parser's rejection. */
          if (pattern[i+2] == '<'
              && pattern[i+3] != '=' && pattern[i+3] != '!')
          {
            int k = i + 3;
            while (pattern[k] != '\0' && pattern[k] != '>') k++;
            if (pattern[k] == '\0' || k == i + 3)
            {
              free(re_compiled); return 0;
            }
            if (group_count >= RE_MAX_GROUPS - 1
             || group_stack_depth >= RE_MAX_GROUPS - 1)
            {
              free(re_compiled); return 0;
            }
            group_count++;
            group_stack[group_stack_depth++] = (unsigned char)group_count;
            re_compiled[j].type  = GROUP_OPEN;
            re_compiled[j].u.gid = (unsigned char)group_count;
            i = k; /* leave i on '>'; the outer i++ steps past it */
            break;
          }
          unsigned int  set_mask   = 0;
          unsigned int  clear_mask = 0;
          int           direction  = 1; /* 1 = setting, 0 = clearing */
          int           saw_close  = 0;
          int           saw_colon  = 0;
          i += 2; /* skip '(' and '?' */
          while (pattern[i] != '\0')
          {
            char fc = pattern[i];
            unsigned int *target;
            unsigned int  bit;
            if (fc == ')')      { saw_close = 1; break; }
            if (fc == ':')      { saw_colon = 1; break; }
            if (fc == '-')      { direction = 0; i++; continue; }
            target = direction ? &set_mask : &clear_mask;
            switch (fc)
            {
              case 'i': bit = RE_FLAG_ICASE;     break;
              case 's': bit = RE_FLAG_DOTALL;    break;
              case 'm': bit = RE_FLAG_MULTILINE; break;
              case 'x': bit = RE_FLAG_EXTENDED;  break;
              default:
                /* Unknown flag char or malformed inline-flag op. */
                free(re_compiled); return 0;
            }
            *target |= bit;
            i++;
          }
          if (!saw_close && !saw_colon)
          {
            /* Unterminated (?... at end of pattern. */
            free(re_compiled); return 0;
          }
          if (saw_colon)
          {
            /* Scoped flag group (?<flags>:...). Not supported yet:
             * the matcher has no per-group flag-state stack to
             * restore on group close. Reject the pattern rather
             * than silently misinterpreting the body. */
            free(re_compiled); return 0;
          }
          /* (?x) is compile-time-significant for the *parser* (so
           * subsequent whitespace and #-comments get stripped); the
           * other flags are runtime-significant for the matcher. We
           * update extended_mode immediately so the outer parser's
           * whitespace/comment skipping honors (?x) toggles partway
           * through the pattern. */
          if (set_mask & RE_FLAG_EXTENDED)   extended_mode = 1;
          if (clear_mask & RE_FLAG_EXTENDED) extended_mode = 0;
          /* Emit a clear-then-set pair so a single op like (?i-s)
           * cleanly clears DOTALL and sets ICASE in one position.
           * Most patterns use only one direction so the second
           * slot is usually a no-op (mask == 0). */
          if (clear_mask != 0)
          {
            re_compiled[j].type      = SET_FLAGS;
            re_compiled[j].u.flg.mask = (unsigned char)clear_mask;
            re_compiled[j].u.flg.set  = 0;
            j++;
            if (j+1 >= MAX_REGEXP_OBJECTS) { free(re_compiled); return 0; }
          }
          re_compiled[j].type      = SET_FLAGS;
          re_compiled[j].u.flg.mask = (unsigned char)set_mask;
          re_compiled[j].u.flg.set  = 1;
          /* fallthrough into the i++/j++ at end of outer loop */
          break;
        }
        if (group_count >= RE_MAX_GROUPS - 1
         || group_stack_depth >= RE_MAX_GROUPS - 1)
        {
          free(re_compiled); return 0;
        }
        group_count++;
        group_stack[group_stack_depth++] = (unsigned char)group_count;
        re_compiled[j].type   = GROUP_OPEN;
        re_compiled[j].u.gid  = (unsigned char)group_count;
      } break;
      case ')':
      {
        if (group_stack_depth == 0)
        {
          free(re_compiled); return 0;
        }
        re_compiled[j].type  = GROUP_CLOSE;
        re_compiled[j].u.gid = group_stack[--group_stack_depth];
      } break;

      /* Other characters: */
      default:
      {
        re_compiled[j].type = RE_CHAR;
        re_compiled[j].u.ch = c;
      } break;
    }
    /* no buffer-out-of-bounds access on invalid patterns - see https://github.com/kokke/tiny-regex-c/commit/1a279e04014b70b0695fba559a7c05d55e6ee90b */
    if (pattern[i] == 0)
    {
      free(re_compiled); return 0;
    }

    i += 1;
    j += 1;
  }
  if (pattern[i] != '\0')
  {
    /* Pattern overflowed MAX_REGEXP_OBJECTS. Report as invalid rather
     * than silently truncating; otherwise re_matchp would walk a
     * partial compile and either misbehave or fail the group-balance
     * check below depending on where truncation lands. */
    free(re_compiled); return 0;
  }
  if (group_stack_depth != 0)
  {
    /* Unmatched opening paren in pattern. */
    free(re_compiled); return 0;
  }
  /* 'UNUSED' is a sentinel used to indicate end-of-pattern. The slot
   * also carries the pattern's total group count so re_n_groups can
   * read it back without scanning. */
  re_compiled[j].type  = UNUSED;
  re_compiled[j].u.gid = (unsigned char)group_count;

  return (re_t) re_compiled;
}

int re_n_groups(re_t pattern)
{
  int j = 0;
  if (pattern == NULL) return 0;
  while (pattern[j].type != UNUSED) j++;
  return (int)pattern[j].u.gid;
}

/* Private functions: */
static int matchdigit(char c)
{
  return isdigit((unsigned char)c);
}
static int matchalpha(char c)
{
  return isalpha((unsigned char)c);
}
static int matchwhitespace(char c)
{
  return isspace((unsigned char)c);
}
static int matchalphanum(char c)
{
  return ((c == '_') || matchalpha(c) || matchdigit(c));
}
static int re_fold(char c)
{
  if (re_flags & RE_FLAG_ICASE)
  {
    return tolower((unsigned char)c);
  }
  return (unsigned char)c;
}
static int matchrange(char c, const char* str)
{
  if (re_flags & RE_FLAG_ICASE)
  {
    /* Case-fold both endpoints and the input so [A-Z] matches
     * lowercase too. Ranges that span case (e.g. [A-z]) are
     * intentionally not special-cased; the fold makes them
     * equivalent to [a-z] in icase mode, matching JVM semantics. */
    int lc = tolower((unsigned char)c);
    int lo = tolower((unsigned char)str[0]);
    int hi = tolower((unsigned char)str[2]);
    return (    (c != '-')
             && (str[0] != '\0')
             && (str[0] != '-')
             && (str[1] == '-')
             && (str[2] != '\0')
             && (lc >= lo)
             && (lc <= hi));
  }
  return (    (c != '-')
           && (str[0] != '\0')
           && (str[0] != '-')
           && (str[1] == '-')
           && (str[2] != '\0')
           && (    (c >= str[0])
                && (c <= str[2])));
}
static int matchdot(char c)
{
  /* (?s) DOTALL flag overrides the default \n/\r exclusion. The
   * compile-time RE_DOT_MATCHES_NEWLINE option (if set) still wins
   * since it predates the flag mechanism. */
#if defined(RE_DOT_MATCHES_NEWLINE) && (RE_DOT_MATCHES_NEWLINE == 1)
  (void)c;
  return 1;
#else
  if (re_flags & RE_FLAG_DOTALL) return 1;
  return c != '\n' && c != '\r';
#endif
}
static int ismetachar(char c)
{
  return ((c == 's') || (c == 'S') || (c == 'w') || (c == 'W') || (c == 'd') || (c == 'D'));
}

/* Escape chars that matchmetachar resolves authoritatively -- the
 * class-membership fallback that compares the escape char literally
 * must not run for these (e.g. [\n] must not match the letter n). */
static int is_class_escape(char c)
{
  return ismetachar(c)
      || (c == 'n') || (c == 'r') || (c == 't') || (c == 'f')
      || (c == 'a') || (c == 'e') || (c == '0') || (c == 'b');
}

static int matchmetachar(char c, const char* str)
{
  switch (str[0])
  {
    case 'd': return  matchdigit(c);
    case 'D': return !matchdigit(c);
    case 'w': return  matchalphanum(c);
    case 'W': return !matchalphanum(c);
    case 's': return  matchwhitespace(c);
    case 'S': return !matchwhitespace(c);
    /* Control-character escapes inside classes, e.g. [\n\t].
     * In a class \b is the backspace character, not a boundary. */
    case 'b': return (c == '\b');
    case 'n': return (c == '\n');
    case 'r': return (c == '\r');
    case 't': return (c == '\t');
    case 'f': return (c == '\f');
    case 'a': return (c == '\a');
    case 'e': return (c == 0x1b);
    case '0': return (c == '\0');
    default:  return (c == str[0]);
  }
}

static int matchcharclass(char c, const char* str)
{
  do
  {
    if (matchrange(c, str))
    {
      return 1;
    }
    else if (str[0] == '\\')
    {
      /* Escape-char: increment str-ptr and match on next char */
      str += 1;
      if (matchmetachar(c, str))
      {
        return 1;
      }
      else if (!is_class_escape(str[0])
               && (re_fold(c) == re_fold(str[0])) && !ismetachar(c))
      {
        return 1;
      }
    }
    else if (re_fold(c) == re_fold(str[0]))
    {
      if (c == '-')
      {
        return ((str[-1] == '\0') || (str[1] == '\0'));
      }
      else
      {
        return 1;
      }
    }
  }
  while (*str++ != '\0');

  return 0;
}

static int matchone(regex_t p, char c)
{
  switch (p.type)
  {
    case DOT:            return matchdot(c);
    case CHAR_CLASS:     return  matchcharclass(c, (const char*)p.u.ccl);
    case INV_CHAR_CLASS: return !matchcharclass(c, (const char*)p.u.ccl);
    case DIGIT:          return  matchdigit(c);
    case NOT_DIGIT:      return !matchdigit(c);
    case ALPHA:          return  matchalphanum(c);
    case NOT_ALPHA:      return !matchalphanum(c);
    case WHITESPACE:     return  matchwhitespace(c);
    case NOT_WHITESPACE: return !matchwhitespace(c);
    default:             return  (re_fold(p.u.ch) == re_fold(c));
  }
}

static int matchstar(regex_t p, regex_t* pattern, const char* text, int* matchlength)
{
  int prelen = *matchlength;
  const char* prepoint = text;
  while ((text[0] != '\0') && matchone(p, *text))
  {
    int w = re_match_width(p, text);
    text += w;
    *matchlength += w;
  }
  for (;;)
  {
    if (matchpattern(pattern, text, matchlength))
      return 1;
    if (text == prepoint) break;
    {
      const char* back = re_back_one(p, prepoint, text);
      *matchlength -= (int)(text - back);
      text = back;
    }
  }

  *matchlength = prelen;
  return 0;
}

static int matchplus(regex_t p, regex_t* pattern, const char* text, int* matchlength)
{
  const char* prepoint = text;
  while ((text[0] != '\0') && matchone(p, *text))
  {
    int w = re_match_width(p, text);
    text += w;
    *matchlength += w;
  }
  while (text > prepoint)
  {
    if (matchpattern(pattern, text, matchlength))
      return 1;
    {
      const char* back = re_back_one(p, prepoint, text);
      *matchlength -= (int)(text - back);
      text = back;
    }
  }

  return 0;
}

/* Bounded-repeat matcher: match the preceding atom p between min and
 * max times (max == 0xFF means unbounded). Greedy with backtracking,
 * same shape as matchstar/matchplus but with explicit bounds. */
static int matchbounded(regex_t p, regex_t* pattern, const char* text,
                        int* matchlength, int min, int max)
{
  int prelen   = *matchlength;
  int count    = 0;
  const char *prepoint;
  const char *start = text;
  /* Greedy: consume as many as possible up to max. */
  while (count < max && text[0] != '\0' && matchone(p, *text))
  {
    int w = re_match_width(p, text);
    text += w;
    *matchlength += w;
    count++;
  }
  /* Backtrack down to min trying to match the suffix at each point. */
  prepoint = text;
  while (count >= min)
  {
    if (matchpattern(pattern, text, matchlength))
      return 1;
    if (count == 0) break;
    {
      const char* back = re_back_one(p, start, text);
      *matchlength -= (int)(text - back);
      text = back;
    }
    count--;
  }
  (void)prepoint;
  *matchlength = prelen;
  return 0;
}

/* Consume width in bytes for a matched atom. DOT swallows a whole
 * UTF-8 sequence so "any char" means one codepoint, not one byte;
 * every other atom is byte-sized. Truncated sequences clamp at the
 * NUL so the scan never walks past end-of-input. */
static int re_match_width(regex_t p, const char* text)
{
  unsigned char b;
  int w;
  int i;
  if (p.type != DOT) return 1;
  b = (unsigned char)text[0];
  if (b < 0x80) return 1;
  if      ((b & 0xE0) == 0xC0) w = 2;
  else if ((b & 0xF0) == 0xE0) w = 3;
  else if ((b & 0xF8) == 0xF0) w = 4;
  else return 1;
  for (i = 1; i < w; i++)
  {
    if (text[i] == '\0') return i;
  }
  return w;
}

/* Step one consumed atom backwards during quantifier backtracking.
 * For DOT the step crosses the whole UTF-8 sequence (skipping
 * continuation bytes) so retry positions stay on codepoint
 * boundaries; never steps below `floor`. */
static const char* re_back_one(regex_t p, const char* floor, const char* text)
{
  text--;
  if (p.type == DOT)
  {
    while (text > floor && (((unsigned char)text[0]) & 0xC0) == 0x80)
    {
      text--;
    }
  }
  return text;
}

/* Lazy quantifier variants: prefer the shortest consumption, growing
 * one atom at a time only when the suffix fails. */
static int matchquestion_lazy(regex_t p, regex_t* pattern, const char* text, int* matchlength)
{
  if (p.type == UNUSED)
    return !re_anchor_end || text[0] == '\0';
  if (matchpattern(pattern, text, matchlength))
    return 1;
  if (*text && matchone(p, *text))
  {
    int w = re_match_width(p, text);
    if (matchpattern(pattern, text + w, matchlength))
    {
      *matchlength += w;
      return 1;
    }
  }
  return 0;
}

static int matchstar_lazy(regex_t p, regex_t* pattern, const char* text, int* matchlength)
{
  int prelen = *matchlength;
  for (;;)
  {
    if (matchpattern(pattern, text, matchlength))
      return 1;
    if (text[0] == '\0' || !matchone(p, *text))
      break;
    {
      int w = re_match_width(p, text);
      text += w;
      *matchlength += w;
    }
  }
  *matchlength = prelen;
  return 0;
}

static int matchplus_lazy(regex_t p, regex_t* pattern, const char* text, int* matchlength)
{
  if (text[0] != '\0' && matchone(p, *text))
  {
    int w = re_match_width(p, text);
    if (matchstar_lazy(p, pattern, text + w, matchlength))
    {
      *matchlength += w;
      return 1;
    }
  }
  return 0;
}

static int matchbounded_lazy(regex_t p, regex_t* pattern, const char* text,
                             int* matchlength, int min, int max)
{
  int prelen = *matchlength;
  int count  = 0;
  /* Consume the mandatory minimum first. */
  while (count < min && text[0] != '\0' && matchone(p, *text))
  {
    int w = re_match_width(p, text);
    text += w;
    *matchlength += w;
    count++;
  }
  if (count < min)
  {
    *matchlength = prelen;
    return 0;
  }
  for (;;)
  {
    if (matchpattern(pattern, text, matchlength))
      return 1;
    if (count >= max || text[0] == '\0' || !matchone(p, *text))
      break;
    {
      int w = re_match_width(p, text);
      text += w;
      *matchlength += w;
      count++;
    }
  }
  *matchlength = prelen;
  return 0;
}

static int matchquestion(regex_t p, regex_t* pattern, const char* text, int* matchlength)
{
  if (p.type == UNUSED)
    return !re_anchor_end || text[0] == '\0';
  /* Greedy: prefer consuming the optional atom, fall back to skipping
   * it -- the same longest-first order matchstar keeps. */
  if (*text && matchone(p, *text))
  {
    int w = re_match_width(p, text);
    if (matchpattern(pattern, text + w, matchlength))
    {
      *matchlength += w;
      return 1;
    }
  }
  return matchpattern(pattern, text, matchlength);
}


/* Find the matching GROUP_CLOSE for the GROUP_OPEN at gopen[0].
 * Returns the index relative to gopen, or -1 on malformed input
 * (no matching close before end-of-pattern). */
static int find_group_close(regex_t* gopen)
{
  int depth = 1;
  int i = 1;
  while (gopen[i].type != UNUSED)
  {
    if (gopen[i].type == GROUP_OPEN) depth++;
    else if (gopen[i].type == GROUP_CLOSE)
    {
      depth--;
      if (depth == 0) return i;
    }
    i++;
  }
  return -1;
}

/* Find the next ALT at depth-0 within span [start..end) of `gopen`.
 * Returns the index relative to gopen, or -1 when no such ALT exists.
 * Used to enumerate the branches of a (foo|bar) group. */
static int find_alt_in_span(regex_t* gopen, int start, int end)
{
  int depth = 0;
  int i;
  for (i = start; i < end; i++)
  {
    if (gopen[i].type == GROUP_OPEN) depth++;
    else if (gopen[i].type == GROUP_CLOSE) depth--;
    else if (gopen[i].type == ALT && depth == 0) return i;
  }
  return -1;
}

/* Recursive group-loop driver. Greedy: try one more iteration of the
 * group body first, fall back to matching the suffix on failure.
 * `gopen` points at the GROUP_OPEN slot; gc_idx is the relative index
 * of the matching GROUP_CLOSE; `suffix` is the pattern slot to match
 * after the group plus any quantifier; count_so_far is the number of
 * successful iterations already consumed; min/max are the quantifier
 * bounds (no quantifier => min=1, max=1). */
static int matchgroup_loop(regex_t* gopen, int gc_idx, regex_t* suffix,
                            const char* text, int* matchlength,
                            int count_so_far, int min_reps, int max_reps,
                            int lazy)
{
  int ml_before = *matchlength;
  int gid       = (int)gopen[0].u.gid;

  /* Lazy repetition: try the suffix before growing the count. */
  if (lazy && count_so_far >= min_reps)
  {
    int suffix_ml = 0;
    if (matchpattern(suffix, text, &suffix_ml))
    {
      *matchlength = ml_before + suffix_ml;
      return 1;
    }
  }

  if (count_so_far < max_reps)
  {
    int branch_start = 1;
    for (;;)
    {
      int           alt_idx     = find_alt_in_span(gopen, branch_start, gc_idx);
      int           branch_end  = (alt_idx < 0) ? gc_idx : alt_idx;
      unsigned char saved_type;
      int           branch_ml   = 0;
      int           saved_start = -1;
      int           saved_end   = -1;
      int           branch_ok;

      if (gid >= 1 && gid <= RE_MAX_GROUPS)
      {
        saved_start = re_g_state.starts[gid - 1];
        saved_end   = re_g_state.ends[gid - 1];
        re_g_state.starts[gid - 1] = (int)(text - re_g_state.base);
      }

      saved_type = gopen[branch_end].type;
      gopen[branch_end].type = UNUSED;
      {
        /* The branch body ends at a synthetic terminator, not the real
         * pattern end -- end-of-input anchoring must not apply to it.
         * (The suffix match below keeps the caller's anchor state.) */
        int saved_anchor = re_anchor_end;
        re_anchor_end = 0;
        branch_ok = matchpattern(&gopen[branch_start], text, &branch_ml);
        re_anchor_end = saved_anchor;
      }
      gopen[branch_end].type = saved_type;

      if (branch_ok && branch_ml > 0)
      {
        const char *after = text + branch_ml;
        int         rec_ml;
        if (gid >= 1 && gid <= RE_MAX_GROUPS)
        {
          re_g_state.ends[gid - 1] = (int)(after - re_g_state.base);
        }
        rec_ml = 0;
        if (matchgroup_loop(gopen, gc_idx, suffix, after, &rec_ml,
                            count_so_far + 1, min_reps, max_reps, lazy))
        {
          *matchlength = ml_before + branch_ml + rec_ml;
          return 1;
        }
      }

      if (gid >= 1 && gid <= RE_MAX_GROUPS && saved_start >= 0)
      {
        re_g_state.starts[gid - 1] = saved_start;
        re_g_state.ends[gid - 1]   = saved_end;
      }

      if (alt_idx < 0) break;
      branch_start = alt_idx + 1;
    }
  }

  if (!lazy && count_so_far >= min_reps)
  {
    int suffix_ml = 0;
    if (matchpattern(suffix, text, &suffix_ml))
    {
      *matchlength = ml_before + suffix_ml;
      return 1;
    }
  }

  *matchlength = ml_before;
  return 0;
}

#if 0

/* Recursive matching */
static int matchpattern(regex_t* pattern, const char* text, int *matchlength)
{
  int pre = *matchlength;
  if ((pattern[0].type == UNUSED) || (pattern[1].type == QUESTIONMARK))
  {
    return matchquestion(pattern[1], &pattern[2], text, matchlength);
  }
  else if (pattern[1].type == STAR)
  {
    return matchstar(pattern[0], &pattern[2], text, matchlength);
  }
  else if (pattern[1].type == PLUS)
  {
    return matchplus(pattern[0], &pattern[2], text, matchlength);
  }
  else if ((pattern[0].type == END) && pattern[1].type == UNUSED)
  {
    return text[0] == '\0';
  }
  else if ((text[0] != '\0') && matchone(pattern[0], text[0]))
  {
    (*matchlength)++;
    return matchpattern(&pattern[1], text+1);
  }
  else
  {
    *matchlength = pre;
    return 0;
  }
}

#else

/* Iterative matching */
static int matchpattern(regex_t* pattern, const char* text, int* matchlength)
{
  int pre = *matchlength;
  do
  {
    /* Capture-group markers and SET_FLAGS slots: skip and record. A
     * GROUP_OPEN whose body has internal `|` alternation OR whose
     * matching close is followed by a quantifier (STAR/PLUS/?/{n,m})
     * is a compound atom — dispatch to matchgroup_loop. Other groups
     * are no-ops at the matcher level; matchplus / matchstar / etc.
     * inside them backtrack naturally because the suffix slots are
     * visible through the skip-loop's `pattern++`. */
    while (pattern[0].type == GROUP_OPEN
        || pattern[0].type == GROUP_CLOSE
        || pattern[0].type == SET_FLAGS)
    {
      if (pattern[0].type == SET_FLAGS)
      {
        if (pattern[0].u.flg.set) re_flags |=  pattern[0].u.flg.mask;
        else                      re_flags &= ~pattern[0].u.flg.mask;
        pattern++;
        continue;
      }
      if (pattern[0].type == GROUP_OPEN)
      {
        int gc_rel    = find_group_close(pattern);
        int has_alt   = (gc_rel >= 0)
                        && (find_alt_in_span(pattern, 1, gc_rel) >= 0);
        int has_quant = 0;
        if (gc_rel >= 0)
        {
          switch (pattern[gc_rel + 1].type)
          {
            case STAR: case PLUS: case QUESTIONMARK: case BOUNDED:
            case LAZY_STAR: case LAZY_PLUS: case LAZY_QUESTIONMARK:
            case LAZY_BOUNDED:
              has_quant = 1;
              break;
            default:
              break;
          }
        }
        if (gc_rel >= 0 && (has_alt || has_quant))
        {
          int      min_r  = 1;
          int      max_r  = 1;
          int      lazy_r = 0;
          regex_t *suffix = &pattern[gc_rel + 1];
          int      gml;
          int      r;
          switch (suffix[0].type)
          {
            case STAR:         min_r = 0; max_r = INT_MAX; suffix++; break;
            case PLUS:         min_r = 1; max_r = INT_MAX; suffix++; break;
            case QUESTIONMARK: min_r = 0; max_r = 1;       suffix++; break;
            case LAZY_STAR:         min_r = 0; max_r = INT_MAX; lazy_r = 1; suffix++; break;
            case LAZY_PLUS:         min_r = 1; max_r = INT_MAX; lazy_r = 1; suffix++; break;
            case LAZY_QUESTIONMARK: min_r = 0; max_r = 1;       lazy_r = 1; suffix++; break;
            case BOUNDED:
            case LAZY_BOUNDED:
              min_r = (int)suffix[0].u.bnd.min;
              max_r = (suffix[0].u.bnd.max == 0xFF)
                      ? INT_MAX
                      : (int)suffix[0].u.bnd.max;
              lazy_r = (suffix[0].type == LAZY_BOUNDED);
              suffix++;
              break;
            default:
              break;
          }
          gml = *matchlength;
          r = matchgroup_loop(pattern, gc_rel, suffix, text, &gml,
                              0, min_r, max_r, lazy_r);
          *matchlength = r ? gml : pre;
          return r;
        }
        /* Simple group: fall through to the skip-and-record path so
         * matchplus/matchstar inside can see the post-group atoms. */
      }
      {
        int gid = (int)pattern[0].u.gid;
        if (gid >= 1 && gid <= RE_MAX_GROUPS)
        {
          int off = (int)(text - re_g_state.base);
          if (pattern[0].type == GROUP_OPEN)
          {
            re_g_state.starts[gid - 1] = off;
          }
          else
          {
            re_g_state.ends[gid - 1] = off;
          }
        }
        pattern++;
      }
    }
    if (pattern[0].type == WORD_BOUNDARY
        || pattern[0].type == NOT_WORD_BOUNDARY)
    {
      /* Zero-width assertion: a word boundary sits between a \w and
       * a non-\w neighbor (start/end of input count as non-word).
       * The window base stands in for start-of-input; a scan that
       * begins mid-string cannot see further left. */
      int prev_w = (text > re_g_state.base) && matchalphanum(text[-1]);
      int cur_w  = (text[0] != '\0') && matchalphanum(text[0]);
      int at_b   = (prev_w != cur_w);
      if (pattern[0].type == NOT_WORD_BOUNDARY ? at_b : !at_b)
      {
        *matchlength = pre;
        return 0;
      }
      pattern++;
      continue;
    }
    if ((pattern[0].type == UNUSED) || (pattern[1].type == QUESTIONMARK))
    {
      int r = matchquestion(pattern[0], &pattern[2], text, matchlength);
      if (!r) *matchlength = pre;
      return r;
    }
    else if (pattern[1].type == STAR)
    {
      int r = matchstar(pattern[0], &pattern[2], text, matchlength);
      if (!r) *matchlength = pre;
      return r;
    }
    else if (pattern[1].type == PLUS)
    {
      int r = matchplus(pattern[0], &pattern[2], text, matchlength);
      if (!r) *matchlength = pre;
      return r;
    }
    else if (pattern[1].type == BOUNDED)
    {
      int max = (int)pattern[1].u.bnd.max;
      int r;
      /* 0xFF is the unbounded sentinel; matchbounded uses INT_MAX
       * internally so the greedy loop runs to end-of-input. */
      if (max == 0xFF) max = 0x7FFFFFFF;
      r = matchbounded(pattern[0], &pattern[2], text, matchlength,
                       (int)pattern[1].u.bnd.min, max);
      if (!r) *matchlength = pre;
      return r;
    }
    else if (pattern[1].type == LAZY_QUESTIONMARK)
    {
      int r = matchquestion_lazy(pattern[0], &pattern[2], text, matchlength);
      if (!r) *matchlength = pre;
      return r;
    }
    else if (pattern[1].type == LAZY_STAR)
    {
      int r = matchstar_lazy(pattern[0], &pattern[2], text, matchlength);
      if (!r) *matchlength = pre;
      return r;
    }
    else if (pattern[1].type == LAZY_PLUS)
    {
      int r = matchplus_lazy(pattern[0], &pattern[2], text, matchlength);
      if (!r) *matchlength = pre;
      return r;
    }
    else if (pattern[1].type == LAZY_BOUNDED)
    {
      int max = (int)pattern[1].u.bnd.max;
      int r;
      if (max == 0xFF) max = 0x7FFFFFFF;
      r = matchbounded_lazy(pattern[0], &pattern[2], text, matchlength,
                            (int)pattern[1].u.bnd.min, max);
      if (!r) *matchlength = pre;
      return r;
    }
    else if ((pattern[0].type == END) && pattern[1].type == UNUSED)
    {
      int r;
      /* (?m) makes `$` match before any `\n` as well as at EOF. */
      if (re_flags & RE_FLAG_MULTILINE)
      {
        r = (text[0] == '\0' || text[0] == '\n');
      }
      else
      {
        r = (text[0] == '\0');
      }
      if (!r) *matchlength = pre;
      return r;
    }
/*  Branching is not working properly
    else if (pattern[1].type == BRANCH)
    {
      return (matchpattern(pattern, text) || matchpattern(&pattern[2], text));
    }
*/
  /* Single atom: consume its width and continue with the next slot. */
  if ((text[0] != '\0') && matchone(pattern[0], text[0]))
  {
    int w = re_match_width(pattern[0], text);
    *matchlength += w;
    pattern += 1;
    text    += w;
    continue;
  }
  break;
  }
  while (1);

  *matchlength = pre;
  return 0;
}

#endif
