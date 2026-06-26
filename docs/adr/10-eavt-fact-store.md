# ADR 10: EAVT fact store, per-state isolated, no cross-runtime shared state

Date: 2026-06-26

## Context

mino needs embedded per-runtime storage for use cases where each
`mino_state` carries its own "memory": game NPCs with individual state,
AI agents with episodic memory, plugin scripts with persistent config.
Erlang's ETS/DETS is the inspiration; Clojure's durable-atom and
Datomic's db-as-value are the idiomatic shapes.

Hirsla (`~/Code/microBrain`) is a full Datomic Local implementation in
Zig with a C ABI, datom model, Datalog query, and proven crash safety.
Embedding it was considered as a way to get Datomic parity inside mino.

## Decision

Ship an EAVT fact store native to mino, not an embedded foreign engine.

The store model:

- **Fact-based, not document-based.** The atomic unit is a fact
  `[e a v tx instant op]`. Facts accumulate; the materialized view is
  derived. Time travel (`as-of`, `since`, `history`) falls out.
- **`MINO_STORE` first-class value tag.** Consistent with
  `MINO_ATOM`/`MINO_AGENT`/`MINO_TX_REF`: an identity type with a
  GC-owned value part (the current db) and a host-owned part (file
  handle, WAL, lock, schema, clock). Finalizer flushes-if-durable on
  GC sweep or `mino_state_free`.
- **Per-state isolation.** A store belongs to one `mino_state`.
  Cross-state access throws `MST007`, matching the agent/ref pattern.
  Cross-runtime value transfer uses `mino_clone` (values cross;
  host-owned handles stay). The host brokers all cross-state movement.
- **No cross-runtime shared state.** Direct pointer reads between
  states are unsound given mino's per-state GC and single-threading.
  Shared mutable state (collaborative editing, multi-agent corpora) is
  a host responsibility, done at the host layer via `host/*` interop
  or an external database. mino stores are single-state citizens.
- **In-memory by default; durability opt-in per store.** Most stores
  never touch disk. Durable stores use snapshot file + append-only EDN
  WAL with host-driven checkpoint cadence (never auto-fsync).
- **Capability-gated install.** The `caps_installed` field is widened
  from 32-bit to 64-bit (`uint64_t`) to accommodate `MINO_CAP_STORE`
  at bit 32. The store installs via the standard `mino_install(S, env,
  MINO_CAP_STORE)` path through `k_cap_dispatch[]`, consistent with
  IO/FS/STM/AGENT. Not in `MINO_CAP_DEFAULT` (it is a heavier feature
  with optional file I/O); included in `MINO_CAP_ALL`.

## Consequences

- No cross-state GC coordination, no MVCC, no snapshot isolation, no
  per-store read locks. Each store's lifecycle is fully contained in
  its owning state.
- The fact model subsumes document-store usage (store the whole
  document as one fact's `:v`) but its native shape is finer-grained,
  giving per-attribute time travel.
- Bounded history (`compact`) is the host's or script's responsibility;
  unbounded fact logs are unacceptable at high cardinality (thousands
  of stores per host process).
- `:tx` is a local monotonic counter for per-store `as-of`. Cross-store
  ordering uses `:instant` (host-provided clock). No tx collision
  problem because tx was never global.
- ~200 lines of C for the value type + ~6 C primitives for lifecycle +
  ~300 lines of Clojure for the `mino.store` namespace. Hirsla's datom
  model informs the store's shape; its Zig implementation does not
  enter the runtime.

## Alternatives

- **Document store (mutable map per entity).** Simpler (~150 lines of
  Clojure, zero C) but loses the temporal/episodic properties that make
  the store valuable for agent/player memory. Rejected because the use
  cases specifically want accumulating, time-ordered memory.
- **Embed Hirsla as a capability.** Full Datomic parity: proven WAL,
  real AVET/VAET indexes, Datalog, pull. Rejected for two independent
  reasons: (1) Hirsla is Zig source, violating ADR 02's "Zig is a
  toolchain, never a source language" and breaking the "embedder never
  sees Zig" promise; (2) the per-instance use case (thousands of
  stores per host process) is incompatible with Hirsla's per-instance
  overhead (file handles, GC, index tries). Cardinality kills it
  independently of ADR 02.
- **Defrecord-wrapping-atom (no new value tag).** Zero C, ships
  faster, but the C API for host-driven lifecycle and cross-state clone
  needs direct struct access to the connection's current-value field.
  A tag makes `mino_is_store` a fast check and the clone path
  efficient. Rejected because the C surface is part of the contract.
- **No capability bit.** The 32-bit `MINO_CAP_*` field is full (ADR
  09). Rejected: consistency with how other features are granted
  matters more than avoiding a field widening. The field is widened
  to 64-bit (`uint64_t`), which is source-compatible for embedders
  using named `MINO_CAP_*` constants, and the ABI is not frozen
  pre-v1.0.
