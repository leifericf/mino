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
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Definitions: */

#define MAX_REGEXP_OBJECTS      30    /* Max number of regex symbols in expression. */
#define MAX_CHAR_CLASS_LEN      40    /* Max length of character-class buffer in.   */


enum { UNUSED, DOT, BEGIN, END, QUESTIONMARK, STAR, PLUS, CHAR, CHAR_CLASS, INV_CHAR_CLASS, DIGIT, NOT_DIGIT, ALPHA, NOT_ALPHA, WHITESPACE, NOT_WHITESPACE, GROUP_OPEN, GROUP_CLOSE /* , BRANCH */ };

typedef struct regex_t
{
  unsigned char  type;   /* CHAR, STAR, etc.                      */
  union
  {
    unsigned char  ch;   /*      the character itself             */
    unsigned char* ccl;  /*  OR  a pointer to characters in class */
    unsigned char  gid;  /*  OR  the capture group id (1..15)     */
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

int re_matchp(re_t pattern, const char* text, int* matchlength)
{
  *matchlength = 0;
  /* Reset group spans for groupless callers too, so stale state from
   * a prior re_matchp_groups call doesn't bleed in. The base pointer
   * is set so GROUP_OPEN/CLOSE markers (if any) can record offsets. */
  if (re_g_state.base != text)
  {
    re_g_state_reset(text, 0);
  }
  if (pattern != 0)
  {
    if (pattern[0].type == BEGIN)
    {
      return ((matchpattern(&pattern[1], text, matchlength)) ? 0 : -1);
    }
    else
    {
      int idx = -1;

      do
      {
        idx += 1;

        if (matchpattern(pattern, text, matchlength))
        {
          if (text[0] == '\0')
            return -1;

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

  re_compiled = (regex_t *)malloc(total);
  if (re_compiled == NULL) return 0;
  memset(re_compiled, 0, total);
  ccl_buf = (unsigned char *)re_compiled + obj_bytes;

  while (pattern[i] != '\0' && (j+1 < MAX_REGEXP_OBJECTS))
  {
    c = pattern[i];

    switch (c)
    {
      /* Meta-characters: */
      case '^': {    re_compiled[j].type = BEGIN;           } break;
      case '$': {    re_compiled[j].type = END;             } break;
      case '.': {    re_compiled[j].type = DOT;             } break;
      case '*': {    re_compiled[j].type = STAR;            } break;
      case '+': {    re_compiled[j].type = PLUS;            } break;
      case '?': {    re_compiled[j].type = QUESTIONMARK;    } break;
/*    case '|': {    re_compiled[j].type = BRANCH;          } break; <-- not working properly */

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

            /* Escaped character, e.g. '.' or '$' */
            default:
            {
              re_compiled[j].type = CHAR;
              re_compiled[j].u.ch = pattern[i];
            } break;
          }
        }
        /* '\\' as last char in pattern -> invalid regular expression. */
/*
        else
        {
          re_compiled[j].type = CHAR;
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

      /* Capture groups. Maximum nesting depth = RE_MAX_GROUPS. */
      case '(':
      {
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
        re_compiled[j].type = CHAR;
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
static int matchrange(char c, const char* str)
{
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
#if defined(RE_DOT_MATCHES_NEWLINE) && (RE_DOT_MATCHES_NEWLINE == 1)
  (void)c;
  return 1;
#else
  return c != '\n' && c != '\r';
#endif
}
static int ismetachar(char c)
{
  return ((c == 's') || (c == 'S') || (c == 'w') || (c == 'W') || (c == 'd') || (c == 'D'));
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
      else if ((c == str[0]) && !ismetachar(c))
      {
        return 1;
      }
    }
    else if (c == str[0])
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
    default:             return  (p.u.ch == c);
  }
}

static int matchstar(regex_t p, regex_t* pattern, const char* text, int* matchlength)
{
  int prelen = *matchlength;
  const char* prepoint = text;
  while ((text[0] != '\0') && matchone(p, *text))
  {
    text++;
    (*matchlength)++;
  }
  while (text >= prepoint)
  {
    if (matchpattern(pattern, text--, matchlength))
      return 1;
    (*matchlength)--;
  }

  *matchlength = prelen;
  return 0;
}

static int matchplus(regex_t p, regex_t* pattern, const char* text, int* matchlength)
{
  const char* prepoint = text;
  while ((text[0] != '\0') && matchone(p, *text))
  {
    text++;
    (*matchlength)++;
  }
  while (text > prepoint)
  {
    if (matchpattern(pattern, text--, matchlength))
      return 1;
    (*matchlength)--;
  }

  return 0;
}

static int matchquestion(regex_t p, regex_t* pattern, const char* text, int* matchlength)
{
  if (p.type == UNUSED)
    return 1;
  if (matchpattern(pattern, text, matchlength))
      return 1;
  if (*text && matchone(p, *text++))
  {
    if (matchpattern(pattern, text, matchlength))
    {
      (*matchlength)++;
      return 1;
    }
  }
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
    /* Capture-group markers are no-op for the matcher; they just
     * record the current text offset for the corresponding group.
     * Multiple writes during backtracking inside matchstar / matchplus
     * settle to the final successful path's offsets. */
    while (pattern[0].type == GROUP_OPEN || pattern[0].type == GROUP_CLOSE)
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
    if ((pattern[0].type == UNUSED) || (pattern[1].type == QUESTIONMARK))
    {
      return matchquestion(pattern[0], &pattern[2], text, matchlength);
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
      return (text[0] == '\0');
    }
/*  Branching is not working properly
    else if (pattern[1].type == BRANCH)
    {
      return (matchpattern(pattern, text) || matchpattern(&pattern[2], text));
    }
*/
  (*matchlength)++;
  }
  while ((text[0] != '\0') && matchone(*pattern++, *text++));

  *matchlength = pre;
  return 0;
}

#endif
