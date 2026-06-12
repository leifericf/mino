/*
 * read_internal.h -- private helpers shared between read.c and read_numeric.c.
 *
 * Not part of the public API. Only include from read.c and read_numeric.c.
 */

#ifndef EVAL_READ_INTERNAL_H
#define EVAL_READ_INTERNAL_H

#include "runtime/internal.h"
#include <errno.h>

/* Reader error codes. */
#define MRE001 "MRE001" /* unterminated string literal */
#define MRE002 "MRE002" /* unexpected closing delimiter */
#define MRE003 "MRE003" /* unterminated list/vector/map/set */
#define MRE004 "MRE004" /* out of memory during read */
#define MRE005 "MRE005" /* unterminated reader conditional */
#define MRE006 "MRE006" /* invalid reader conditional form */
#define MRE007 "MRE007" /* invalid reader macro usage */
#define MRE008 "MRE008" /* malformed literal */
#define MRE009 "MRE009" /* expected form after reader macro */
#define MRE010 "MRE010" /* invalid metadata */
#define MRE011 "MRE011" /* nesting too deep */

/* Emit a reader diagnostic; defined in read.c. */
void set_reader_diag(mino_state *S, const char *code,
                     const char *msg, int line, int col);

/* Numeric literal parser; defined in read_numeric.c.
 * Returns the parsed value on success, NULL on error (with err set if the
 * token looks numeric but is malformed, 0 if it is simply not numeric). */
mino_val *try_parse_numeric(mino_state *S, const char *start,
                             size_t len, int *err);

#endif /* EVAL_READ_INTERNAL_H */
