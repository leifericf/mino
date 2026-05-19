/*
 * gc/state.h -- type-alias header for the GC subsection of
 * mino_state.
 *
 * The architectural intent is that the GC subsystem owns a
 * dedicated state block, separately from the runtime container.
 * The fields themselves still live inline in struct mino_state for
 * byte-layout compatibility with the stencil ABI invariants pinned
 * in src/eval/bc/stencils/runtime_layout.h. A future cycle moves
 * them into a real gc_state_t embedded sub-struct once the offset
 * preservation work has been validated.
 *
 * For now this header serves as the documented home of the type
 * name. Consumers writing new gc-side code can take a
 * `gc_state_t *` parameter (currently equivalent to a
 * `mino_state_t *`) to express the intent without coupling to the
 * full state surface; the typedef cuts cleanly when the body moves.
 *
 * See .local/cycle-4-followups.md for the deferred body extraction
 * plan and the stencil ABI constraints that gate it.
 *
 * Internal to the runtime; embedders should only use mino.h.
 */

#ifndef GC_STATE_H
#define GC_STATE_H

#include "mino.h"

/* Alias for the GC subsection of mino_state. Currently aliased to
 * the full state until the deferred body extraction lands. */
typedef struct mino_state gc_state_t;

#endif /* GC_STATE_H */
