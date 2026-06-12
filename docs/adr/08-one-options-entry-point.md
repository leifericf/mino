# ADR 08: One options entry point for per-state config knobs

Date: 2026-06-12

## Context

The embedding API grew four idioms for per-state configuration:
`mino_set_limit` with a two-member kind enum (steps, heap),
individual JIT setters (`mino_state_set_jit_mode`,
`mino_state_set_jit_hot_threshold`, plus getters), individual thread
setters (`mino_set_thread_limit`, `mino_get_thread_limit`,
`mino_set_thread_stack_size`), and `mino_gc_set_param` for GC tuning.
Each new knob forced a new public symbol pair and its own error
convention. mino is alpha: the surface can be cut cleanly, with no
compatibility shims.

## Decision

Per-state config knobs go through one pair:
`mino_set_option(S, opt, value)` / `mino_get_option(S, opt)`, with a
single `mino_option` enum (step limit, heap limit, thread limit,
thread stack bytes, JIT mode, JIT hot threshold). The setter follows
`mino_gc_set_param`'s contract — 0 ok, -1 on NULL state, unknown
option, or out-of-range value, rejected writes leave the old value.
The nine per-knob symbols are removed outright. `mino_gc_set_param`
stays as the GC-domain analog: GC tuning is a parameter space with
its own units and validation, and the options contract was modeled
on it rather than absorbing it.

## Consequences

- New knobs are an enum member and a switch case, not new symbols.
- Unstable knobs (thread stack bytes) ride the same entry point with
  a doc-tier annotation instead of a separate UNSTABLE function.
- Invalid JIT modes are now rejected with -1; the old setter silently
  ignored them. Embedders relying on the silence see a behavior change.
- `size_t` is the one value type; enum-valued options (JIT mode) are
  widened, which trades a little type precision for one signature.

## Alternatives

- Per-knob setters: typed signatures and IDE-discoverable names, but
  the surface grows linearly with knobs and had already split into
  four conventions.
- A config struct at `mino_state_new`: one-shot and self-documenting,
  but knobs must be adjustable on a live state (limits per tenant
  eval, JIT mode per workload), which a constructor-time struct
  cannot do.
- Folding GC params into the same enum: one entry point fewer, but
  GC tuning has its own units, ranges, and audience; merging buys
  uniformity at the cost of a 20-member mixed-domain enum.
