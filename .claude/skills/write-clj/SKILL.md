---
name: write-clj
description: Recipe for writing Clojure/mino code — core.clj and bundled libs, task-runner tasks, and tools/ spine scripts. Invoked by writer agents.
user-invocable: false
---

# write-clj

Write Clojure-dialect code for mino. The standard is
`.claude/skills/check-style/references/clj-style.md` (read it first).

Placement decides the rules:

1. **`src/core.clj` / `lib/clojure/*`** — language surface. JVM
   Clojure behavior is the spec; check the real Clojure's behavior for
   every edge case (nil, empty, laziness, arity) before writing.
   Bundled libs are installed via `src/prim/install_stdlib.c` hooks
   and escaped into headers by `gen-stdlib-headers` — after editing,
   `./mino task build` regenerates; never edit the generated
   `src/lib_*.h`.
2. **`lib/mino/tasks/builtin.clj`** — task runner. Plain `defn`,
   registered in `mino.edn` with `:doc` (and `:deps` when it needs
   the binary). One-line `(println "  task: ...")` progress; `sh!`
   for must-succeed commands; throw on failure.
3. **`tools/*.clj`** — spine scripts. Self-dispatching single file:
   `(ns tools.<name>)`, library functions, `-main`, and the
   `*file*`-guard at the bottom so tests can require the same file.
   State is EDN on disk; scripts validate their inputs and throw on
   anything malformed — the spine never guesses.

Always:
- Tests in `tests/<area>_test.clj`, registered in `tests/run.clj`,
  with file-unique prefixes on top-level defs (the suite shares one
  namespace).
- TDD for tooling: failing test first, then the implementation —
  tooling is also a dogfood test of the language.
- Prefer mino over Python over shell for anything orchestration-like;
  if mino is missing a primitive you need, that is a real gap —
  surface it, don't silently switch language.
