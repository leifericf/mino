# ADR 55: The runtime directory splits into names and state, shared headers stay

Date: 2026-09-06

## Context

`src/runtime/` had grown to hold two unrelated domains under one roof:
the name-resolution machinery (environments, vars, namespace tables,
module loading) and the state lifecycle (the `mino_state` container,
host threads, capabilities, heap images, error reporting). The physical
layout no longer mapped to the domains, and a reader could not tell from
a path which concern a file served.

The complication is `runtime/internal.h`: it defines the `mino_state`
struct by pulling in every per-subsystem header, and 42 translation
units include it. It is neither a names header nor a state header; it is
the shared aggregator that every layer above the value model depends on.
Two parked declaration sets sat in the wrong homes for the same reason:
the arbitrary-precision numeric-tower value interface
(`mino_bigint_*`, `mino_ratio_*`, `mino_bigdec_*`) lived in
`collections/internal.h`, and the prim fast-lane fallback declarations
(`prim_add`, `prim_conj`, and their siblings) lived in
`eval/bc/internal.h`.

## Decision

The `.c` files move to two new directories: `src/names/` (env, var,
ns_env, module) and `src/state/` (state, host_threads, capabilities,
image, image_load, error). The shared runtime headers stay in
`src/runtime/`, which becomes the shared-state header layer both new
domains include downward. `runtime/internal.h` does not split; it spans
both domains by definition and inverting its 42 includers to chase a
cleaner name would buy nothing this phase.

The numeric-tower value interface moves to a dedicated `values/bignum.h`
included by `values/internal.h`, because its consumers are value-layer
paths: the GC finalizer, the printer, and the equality and hash paths.
The prim fast-lane fallback declarations stay in `eval/bc/internal.h`:
the bytecode layer declares them as the interface its miss paths depend
on, and the prim layer fulfills it. Both placements keep the include
graph pointing one way.

## Consequences

`src/runtime/` now holds only headers, which reads as a layer rather
than a grab bag. The bignum value interface has a named home instead of
being buried in the collections aggregator, and `prim/internal.h`'s
pointer to it is corrected. The deeper un-inversion, where
`values/gc_handlers.c` stops linking upward into prim for bignum
teardown, is left for the semantic-decomplection phase; this move is
byte-inert and does not attempt it. The move touches both build files
(the Makefile wildcard and the explicit source list in the task runner)
and the size-gate allow-list, which key on paths.

## Alternatives

**Split `runtime/internal.h` into a names header and a state header.**
Most faithful to the domain split. Rejected for this phase: it forces
42 include-site rewrites and a real decomposition of the `mino_state`
struct definition, which is semantic work, not physical motion, and
belongs with the decomplection phase if it happens at all.

**Move the bignum interface to a prim-owned header.** The definitions
live in `src/prim/`, so a prim header is the intuitive home. Rejected:
the consumers are the value layer, which sits below prim, so a prim
header would force `values/val.c` and `collections/map_hash.c` to
include upward into prim, the exact inversion this split removes.

**Move the prim fallback declarations to `prim/internal.h`.** They are
`prim_*` symbols, so the prim header looks like their home. Rejected for
the mirror reason: the bytecode layer that calls them sits below prim,
so relocating the declarations upward would make `compile.c` and `vm.c`
include into prim and create a cycle.
