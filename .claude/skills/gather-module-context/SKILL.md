---
name: gather-module-context
description: Recipe for producing a compact module brief — responsibility, files, boundary, owning tests, recent change history — used to make agent dispatch prompts cheap and grounded. Invoked by orchestrating skills and the round runner.
user-invocable: false
---

# gather-module-context

Produce a module brief: the few hundred tokens a dispatched agent
needs instead of rediscovering the module itself. Build it from disk,
not from memory.

Sources, in order:

1. `docs/INTERNAL_MODULE_MAP.md` — the module's row(s): files and
   responsibilities, its `internal.h`, the allowed dependency
   directions touching it.
2. `ls <module-dir>` — actual files (the map can lag; note any file
   missing from it as a candidate doc fix).
3. Owning tests — grep `tests/run.clj` and `tests/` for the module's
   domain (e.g. `src/gc` → `gc_test.clj`, stress/regression files);
   embed/examples coverage if it touches the public surface.
4. `git log --oneline -15 -- <module-dir>` — what changed recently
   and why; flags active work and fresh bug history.
5. `grep -c '' <files>` — sizes, to warn editors near the qa-arch
   limits (1100 TU / 250 fn).

Brief format (keep under ~40 lines):

```
MODULE <dir>
RESPONSIBILITY: <one line per file, from the map>
INTERNAL.H: <path> (types owned: ...)
BOUNDARY: may include <...>; must NOT touch <other modules' headers, src/public/>
TESTS: <test files> (run: ./mino tests/<f>.clj)
RECENT: <3-5 one-line commits>
SIZES: <any TU/function near limits>
NOTES: <gotchas: GC-sensitive areas, platform splits, generated files>
```

The brief is pasted into dispatch prompts; it is not written to the
run dir unless the orchestrator says so (then:
`<run-dir>/context/<module-slug>.md`).
