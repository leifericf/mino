/*
 * internal_bridge.h -- narrow re-export of runtime + prim internals
 * used by the public layer (src/public).
 *
 * The public API in src/mino.h is intentionally minimal: every symbol
 * here is a runtime internal that some public function genuinely
 * needs to implement (longjmp through try-frames, raise a classified
 * diagnostic, drive the GC). Centralising the dependency in a single
 * header makes the surface visible at a glance: any new internal a
 * public function reaches for shows up here, signalling that an
 * internal refactor needs to consider the public-side impact.
 *
 * The public layer reads owned-layer data through named accessors, not
 * raw struct fields: var roots via var_root_get / var_set_dynamic_flag,
 * the load-path list via runtime_module_load_path_*, the reader file via
 * reader_current_file, and GC counters via gc_stats_fill. The only
 * remaining raw reach is the try-stack longjmp in mino_throw, which is
 * the control-flow seam this bridge exists for.
 *
 * Public-layer source files include mino.h for the ABI plus
 * public/internal_bridge.h for the implementation borrows -- never
 * runtime/internal.h or prim/internal.h directly. The bridge
 * transitively includes the heavyweight internal headers; we keep
 * them here rather than re-declaring fields so a structural runtime
 * change still surfaces as a public-side compile error if the
 * bridge falls out of sync.
 */
#ifndef MINO_PUBLIC_INTERNAL_BRIDGE_H
#define MINO_PUBLIC_INTERNAL_BRIDGE_H

#include "runtime/internal.h"
#include "prim/internal.h"

/* This header is intentionally just a re-export. The point is that
 * every public-layer source file declares its internal dependency
 * through ONE include line, so a reader can see at the top of any
 * src public file that it touches internals. */

#endif /* MINO_PUBLIC_INTERNAL_BRIDGE_H */
