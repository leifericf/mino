# Decision records

Lightweight records of architecture decisions: a text file with
context, decision, consequences, and the alternatives weighed — written
when the decision is made, while the context is still cheap to state.
No status machinery; a decision stands until a later record supersedes
it by saying so. Recording ritual: the `record-decision` skill
(`.claude/skills/record-decision/`).

| # | Decision |
|---|----------|
| [01](01-c99-only-no-extensions.md) | Any ANSI C compiler + make builds mino — C99, no extensions |
| [02](02-zig-is-a-toolchain-not-a-source-language.md) | Zig is a toolchain, never a source language |
| [03](03-defer-pgo.md) | Defer PGO until zig ships the profile runtime and tooling |
| [04](04-curated-lint-set.md) | The lint lane gates a curated warning set, not -Weverything |
| [05](05-changelog-via-proposal.md) | Changelog lines travel as proposal EDN, serialized at land time |
| [06](06-drop-docker-ci-matrix.md) | Drop the local Docker CI mirror; zig cross-build + qemu covers it |
| [07](07-no-aot-compilation.md) | No AOT compilation — the tiers stay interpreter, bytecode VM, and runtime JIT |
| [08](08-one-options-entry-point.md) | One options entry point for per-state config knobs |
| [09](09-prims-are-static-tables-are-the-registry.md) | Primitives are static; the registration tables are the registry |
| [10](10-eavt-fact-store.md) | EAVT fact store, per-state isolated, no cross-runtime shared state |
| [11](11-store-on-disk-format.md) | Store on-disk format — EDN text with version header, line-delimited WAL |
| [12](12-save-lisp-and-die.md) | Save-lisp-and-die — value-serialization image with identity table |
| [13](13-references-and-reverse-indexes.md) | References and reverse indexes — `:ref` type, lazy reverse index, nil-on-retraction |
| [14](14-schema-migration.md) | Schema migration via `store/migrate`, validation-gated publish with `:coerce` and `:force` |
| [15](15-vector-embedding-scope.md) | Vector embedding and similarity search are out of scope for mino.store |
| [16](16-binary-snapshot-format.md) | EDN text is the stable on-disk format for mino.store v1 |
| [17](17-slad-forward-compatibility.md) | SLAD forward compatibility via skip-and-warn, plus an offline migration tool |
