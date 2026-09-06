# ADR 57: Lazy forcing stays owned by eval, declared at the value layer

Date: 2026-09-06

## Context

`collections/iter.c` includes `eval/internal.h` for one function:
`lazy_force`, which the unified iterator calls to walk lazy seqs
transparently. The same call appears in the equality walker
(`values/val.c`), the printer (`print/print.c`), and a dozen prim
files. Forcing a lazy seq evaluates its thunk body, so the
implementation necessarily lives in the eval layer; the include from
collections upward into eval is the visible cost. The
semantic-decomplection pass asked whether the dependency should be
inverted with a registered forcing hook (a function pointer on
`mino_state`, installed by eval at state init, the same shape the GC
uses for component tracers and root walkers), or accepted as a braid.

## Decision

The braid is accepted: forcing is evaluation, and the eval layer owns
it. Realizing a lazy value runs arbitrary user code through the
evaluator's safepoint, cancellation, and namespace machinery; no hook
can move that semantic dependency, only hide it. What does move is the
declaration: `lazy_force` is declared in `values/internal.h` beside
the other cross-layer forcing declarations the value model already
carries (`mino_bc_trace_fn_bc`, `mino_future_deref`), so a lower
layer that forces lazies depends on the value-layer interface, not on
`eval/internal.h` wholesale. `collections/iter.c` drops its eval
include.

## Consequences

The link-time dependency on eval remains, as it must in a single
binary whose values can carry code. Readers of `values/internal.h`
see forcing declared where laziness is defined, with the
implementation note pointing at `eval/eval.c`. The hot path keeps its
direct call; no indirection is added to equality, print, or
iteration. A future embedding that compiles collections without eval
would need the hook after all; this record is the place a follow-on
ADR would supersede.

## Alternatives

A registered forcing hook mirrors the GC tracer registry precedent
and would let `collections/` compile against a NULL forcer, failing
closed on lazy input. Its real strengths are symmetry with
`gc_register_root_walker` and a compile-out story for eval-free
builds. Rejected because no such build exists or is planned, the hook
adds a pointer indirection to three hot walkers, and it converts a
visible compile-time dependency into an init-order obligation that
only surfaces at runtime.
