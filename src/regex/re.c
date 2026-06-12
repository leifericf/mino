/*
 * re.c -- formerly the monolithic regex translation unit.
 *
 * Split into re_compile.c (pattern compiler) and re_match.c (matcher)
 * to bring the module under the 1100-LOC per-TU limit. Internal types
 * shared by both TUs live in re_internal.h. The public API is unchanged
 * in re.h.
 *
 * This file exists only to preserve the file name in git history.
 * ISO C99 requires at least one external declaration per translation
 * unit; the typedef below satisfies that requirement.
 */

typedef int re_placeholder_t;
