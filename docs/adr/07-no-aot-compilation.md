# ADR 07: No AOT compilation — the tiers stay interpreter, bytecode VM, and runtime JIT

Date: 2026-06-12

## Context

mino executes through three tiers sharing one semantics: tree-walker,
bytecode VM, and a copy-and-patch JIT assembled at runtime from
build-time stencils. Embeddable Lisps that ship an AOT-to-native
pipeline (transpile to C, invoke the platform C compiler, load the
result) gain a higher performance ceiling and a ship-as-library story,
but couple every deployment to a host C compiler, produce
platform-specific artifacts, and complicate static linking. The classic
AOT motivation — startup — is already ~1 ms for the floor install.

## Decision

mino does not add AOT compilation, in either form. No AOT-to-native:
the JIT is the only native tier and requires nothing at deploy time.
No serialized bytecode images for now: the bytecode tier is
deliberately a hot-path subset, not a full-language representation,
and an artifact covering a subset would misrepresent the surface.

Revisit triggers: (1) an embedder on a no-JIT platform (iOS, WASM)
needs more than interpreter speed — the fix is broadening bytecode
coverage, after which luac-style image serialization becomes cheap and
can be reconsidered; (2) demand for source concealment in shipped
scripts, which rides on the same serialization.

## Consequences

- Peak throughput stays below transpile-to-C systems for hot numeric
  code; the JIT is the answer within that ceiling.
- Deployment remains one self-contained binary or library: no
  compiler, temp dirs, or dlopen at runtime.
- Startup work targets install tiers and state cloning, not
  compilation caching.

## Alternatives

- AOT-to-native via C: highest ceiling, proven in older embeddable
  Lisps; rejected for the deploy-time compiler dependency and
  platform-specific artifact management.
- Bytecode images (luac-style): cheap and proven in Lua; rejected
  until bytecode covers the full language.
- Heap snapshots: fast startup, but startup is already ~1 ms and
  snapshots complect built state with the build itself.
