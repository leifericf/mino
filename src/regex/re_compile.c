/*
 *
 * Mini regex-module inspired by Rob Pike's regex code described in:
 *
 * http://www.cs.princeton.edu/courses/archive/spr09/cos333/beautiful.html
 *
 *
 *
 * re_compile.c -- pattern compiler (re_compile, re_n_groups, re_free).
 *
 * Style note. This translation unit and its sibling re_match.c
 * preserve the upstream tinyregex-c style (Allman braces, two-space
 * indent, dense locals, fixed-size pattern arena). Per Rule 15 of the
 * project's C implementation guide, the local conventions inside
 * src/regex/ take precedence over the codebase-wide style.
 *
 */

#include "re_internal.h"

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
      case '*': {    if (j > 0 && re_compiled[j-1].type == BACKREF) { free(re_compiled); return 0; }
                     re_compiled[j].type = STAR;
                     if (pattern[i+1] == '?') { re_compiled[j].type = LAZY_STAR; i++; } } break;
      case '+': {    if (j > 0 && re_compiled[j-1].type == BACKREF) { free(re_compiled); return 0; }
                     re_compiled[j].type = PLUS;
                     if (pattern[i+1] == '?') { re_compiled[j].type = LAZY_PLUS; i++; } } break;
      case '?': {    if (j > 0 && re_compiled[j-1].type == BACKREF) { free(re_compiled); return 0; }
                     re_compiled[j].type = QUESTIONMARK;
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
        if (j > 0 && re_compiled[j-1].type == BACKREF) { free(re_compiled); return 0; }
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

            /* Backreferences \1..\9. */
            case '1': case '2': case '3': case '4': case '5':
            case '6': case '7': case '8': case '9':
            {
              re_compiled[j].type  = BACKREF;
              re_compiled[j].u.gid = (unsigned char)(pattern[i] - '0');
            } break;

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
