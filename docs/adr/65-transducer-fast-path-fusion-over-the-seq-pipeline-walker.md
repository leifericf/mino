# ADR 65: Transducer fast-path fusion over the seq pipeline walker

Date: 2026-09-10

## Context

A transducer in mino is a plain `(rf -> rf)` closure, exactly as in the
canon. `transduce`, `into`-with-xform, and `sequence`-with-xform thread
each element through the composed closure chain one interpreted call per
stage per element. Measured on this machine, `(transduce (comp (map inc)
(filter odd?)) + 0 (range 1000))` runs ~314 us, about 19x slower than the
lazy-seq `(reduce + 0 (filter odd? (map inc (range 1000))))` it is meant
to beat. Clojure users reach for transducers for speed; being slower than
the lazy path is a violated expectation.

The seq path already has a shared fast path: the v0.145 pipeline walker
(`pipeline_walk` in `src/prim/core/sequences.c`) drains a source through
an unwound `pipeline_stage_t[]` with inline range / vector / chunked
drains and canonical-op inlining, and fuses map / filter / take / remove /
keep / map-indexed (the last extended by phases 1-3 of this cycle). The
walker matches lazy cells by exact C thunk-pointer identity: anything it
does not recognise stops the unwind and the slow path runs unchanged.

The transducer values, being opaque closures, carry no shape the walker
can inspect. To route a `comp` of standard stage transducers through the
walker we need those transducers to become C-recognisable while staying
callable as `(rf -> rf)`.

## Decision

Fuse transducers through the existing pipeline walker when, and only when,
the whole xform is a standard stage transducer or a `comp` of standard
stage transducers. Fusion is a fast path indistinguishable from the slow
closure path; anything unrecognised takes the slow path unchanged.

We take Option A from the plan (inspectable stage tag), implemented
entirely in the Clojure layer plus one new C reduce entry point, so the
`(rf -> rf)` contract and `comp`'s composition semantics are untouched:

- The standard 1-arity transducers (`map`, `filter`, `remove`, `keep`,
  `map-indexed`) return their existing `(rf -> rf)` closure carrying an
  inspectable stage tag in metadata: `^{:mino.fusion/stage {:kind ...
  :f ...}}`. The closure is unchanged as a callable; only metadata is
  added. Transducers that are stateful or that the walker cannot model as
  a single element-at-a-time stage (`take`, `drop`, `take-while`,
  `partition-*`, `dedupe`, `cat`, `keep-indexed`, and every user xform)
  carry NO tag and so are never fused.

- `comp` composes into the same opaque closure it does today. When every
  operand carries a `:mino.fusion/stage` tag it additionally attaches a
  `:mino.fusion/stages` vector (the ordered stage descriptors) as metadata
  on that closure. If any operand is untagged, no stages vector is
  attached; `comp`'s callable behaviour is identical in both cases. `comp`
  stays a pure composition: the stages vector is advisory metadata a
  consumer may ignore.

- `transduce` inspects the xform's `:mino.fusion/stage` /
  `:mino.fusion/stages` metadata. When the reducing function is a fusible
  scalar accumulator over a fusible source, it hands the stage descriptors
  and the source to a new C entry point that builds `pipeline_stage_t[]`
  and calls `pipeline_walk`. Otherwise it runs the current closure path.
  `into`-with-xform and `sequence`-with-xform route through `transduce`
  and inherit the fast path with no new logic.

Take, take-while, halt-when, and every other reduced-producing or stateful
stage stay on the slow path, so `reduced` short-circuit, the completion
arity `(xrf result)`, and stage statefulness are preserved by
construction: the fast path only ever runs stateless element-at-a-time
stages that the seq walker already fuses and already tests fused-equals-
unfused.

## Consequences

- `(transduce (comp (map ...) (filter ...)) + 0 coll)` folds to the same
  reduce-class scalar accumulation the fused lazy `reduce` uses, closing
  the ~19x gap for the standard-stage case.
- Transducer values gain a metadata slot; `MINO_FN` already supports
  metadata, so no new representation and no new binary or startup cost.
- `comp` gains a tag-merge branch guarded on all-operands-tagged; the
  common (untagged) path is unchanged, and the returned closure is
  identical as a callable. Composition semantics are unaffected.
- The fusible stage set is exactly the seq walker's stateless stages;
  widening it later means tagging one more transducer, no new fast path.
- Unrecognised, stateful, or user xforms are indistinguishable from
  today: they never carry a stage tag and always take the closure path.

## Alternatives

- **Option B (prim-identity only):** fuse only when the xform is literally
  a recognised stage or a `comp` of stages detected by primitive identity,
  with no metadata tag. Narrower blast radius but it cannot see through a
  `comp` result (an opaque closure) without either re-deriving the chain
  or the same tag it avoids; it also cannot fuse a stage bound through a
  var or passed as a value. Rejected as strictly less general for no
  safety gain over the guarded tag, which is inspected only on the fusion
  fast path.
- **Tag by wrapping transducers in a record/new type:** a distinct type
  would need every transducer call site and `fn?`/`ifn?` check to keep
  treating it as callable, a far larger blast radius than a metadata slot
  on the existing `MINO_FN`. Rejected.
- **Make `comp` build the stage vector in C unconditionally:** pushes tag
  awareness into the hot `comp` path used by all composition, not just
  transducers. Rejected in favour of the all-operands-tagged guard, which
  keeps the common path untouched.
