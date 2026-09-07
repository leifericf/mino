# ADR 59: The EAVT store stays in-tree behind its capability

Date: 2026-09-07

## Context

The compile-out axis now lets a build subtract a large optional
capability with a MINO_NO_<CAP> flag: TLS with its vendored client and CA
roots, the timezone database, the document parsers. Each was a
self-contained blob or parser reachable through a narrow seam, so the cut
was clean. The EAVT fact store (`src/prim/store/store.c`, its
`mino.store` facade, and its `MINO_CAP_STORE` gate) is the next-largest
optional capability, and the question the axis raises is whether it
should leave the tree entirely for a separate lib repo now that compiling
it out is cheap.

Unlike the compiled-out blobs, the store is woven into the runtime. It
defines its own value-model type (`MINO_STORE`, with `mino_is_store`,
`mino_store_val`, and a GC finalizer), and store.c reaches for
internal-only APIs: `gc_write_barrier` and `gc_oom_throw` from the
collector, `mino_eval_string_ex` and `mino_print_to` from eval, and
`ns_env_ensure` from the name layer. None of these are public embed API.

## Decision

The store stays in-tree, gated by `MINO_CAP_STORE` as it is today; this
phase does not extract it and does not add a `MINO_NO_STORE` compile-out
flag. Extraction is deferred, not rejected: the record marks the seam a
future cycle would cut and names the precondition. The store is already
subtractable at install time (an embedder omits `MINO_CAP_STORE`), so the
capability axis it needs already exists; what it lacks is a public C
surface a separately-compiled repo could build against.

## Consequences

The store keeps riding the same build as the runtime, so the binary
carries its ~800 lines and its value-model type whether or not the
embedder installs it. That cost is small next to the blobs the other
flags drop, and the install-time gate already keeps it out of a state
that does not ask for it. A future extraction stays possible and is now
scoped: it must first promote the store's dependencies (the GC write
barrier and OOM throw, the eval-string and print entry points, the
name-env accessor, and a way to register a host value type) into public
embed API, then the store compiles against `mino.h` alone and moves to
its own repo. Until that surface exists, extraction would either fork the
internal headers or freeze them as a private ABI, both worse than the
current braid.

## Alternatives

Extract now, exposing whatever internals the store needs as public API in
the same change. Its real strength is that the compile-out axis makes the
timing natural and a lib-repo store would prove the embed surface is rich
enough for a non-trivial third-party capability. Rejected for this phase
because the store's runtime coupling is deeper than any capability the
axis has cut so far: it owns a value-model type and touches the collector
and evaluator directly, so extraction is an embed-API design project in
its own right, not a mechanical move, and doing it under the compile-out
phase would widen the phase past its intent.

Add a `MINO_NO_STORE` flag without extracting, for symmetry with the
other compile-outs. Rejected because the store is not a blob or a
vendored tree; it is live runtime code with a value type, and a flag that
guards it out would have to also guard its `MINO_STORE` type out of the
value model, the printer, and equality, a far more invasive seam than the
blob guards for no user asking for a smaller binary that the install-time
gate does not already serve.
