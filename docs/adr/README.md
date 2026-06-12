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
