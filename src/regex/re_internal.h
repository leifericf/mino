/*
 * re_internal.h -- private types and definitions shared between
 * re_compile.c (the pattern compiler) and re_match.c (the matcher).
 *
 * Nothing in this header is part of the public API; only the two
 * translation units within src/regex/ include it.
 */

#ifndef RE_INTERNAL_H
#define RE_INTERNAL_H

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
enum {
    UNUSED,
    DOT,
    BEGIN,
    END,
    QUESTIONMARK,
    STAR,
    PLUS,
    RE_CHAR,
    CHAR_CLASS,
    INV_CHAR_CLASS,
    DIGIT,
    NOT_DIGIT,
    ALPHA,
    NOT_ALPHA,
    WHITESPACE,
    NOT_WHITESPACE,
    GROUP_OPEN,
    GROUP_CLOSE,
    BOUNDED,
    SET_FLAGS,
    ALT,
    WORD_BOUNDARY,
    NOT_WORD_BOUNDARY,
    LAZY_QUESTIONMARK,
    LAZY_STAR,
    LAZY_PLUS,
    LAZY_BOUNDED,
    BACKREF
};

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

/* Thread-local qualifier. These three match-state variables are written
 * at the start of every re_matchp / re_matchp_groups call and read
 * throughout the match. Making them thread-local ensures concurrent
 * mino_state instances each see only their own match context.
 * MSVC uses __declspec(thread); GCC/Clang use __thread. */
#if defined(_WIN32) && defined(_MSC_VER)
#  define RE_TLS __declspec(thread)
#else
#  define RE_TLS __thread
#endif

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
 * leaves spans pointing at the final write for each group.
 * Thread-local so concurrent mino_state instances do not share state. */
typedef struct re_g_state_s {
  const char *base;                 /* start of input text */
  int         n;
  int         starts[RE_MAX_GROUPS];
  int         ends[RE_MAX_GROUPS];
} re_g_state_t;

/* Declared in re_match.c; visible here for re_compile.c (not needed
 * currently, but kept for future cross-file helpers). */

#endif /* RE_INTERNAL_H */
