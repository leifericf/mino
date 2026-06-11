# mino Clojure style — the checkable standard

Applies to everything written in the mino dialect: `src/core.clj`,
`lib/clojure/*`, `lib/mino/*`, `tools/*.clj`, `tests/*_test.clj`.

## The spec is JVM Clojure

- Canonical Clojure (the JVM implementation) defines correct behavior.
  Where mino implements a form, it must match JVM Clojure observable
  semantics; divergences are documented at the implementation site
  (see the deviation comments in `src/prim/stm.c` for the idiom).
- Follow the community Clojure style guide where the dialect supports
  the construct; don't invent house style where a community norm
  exists.
- Conformance taxonomy for any behavioral gap found while reviewing:
  it is either a **real mino gap** (file a finding; fix in C or
  core.clj — never paper over in the caller), an **upstream platform
  difference** (document at site), or **infrastructure** (test harness
  issue, fix the harness).

## Tooling lives in the repo, in mino

- Orchestration and tooling scripts are written in mino first
  (`tools/*.clj`, `lib/mino/tasks/builtin.clj`), Python only when mino
  genuinely can't, shell last. Tooling exercises the language — every
  spine script is also a dogfood test.
- Scripts live in the repo (`tools/`, `tests/`), never `/tmp`.
- Self-dispatching single files: a script defines its functions under
  an `(ns tools.<name>)` declaration and ends with

  ```clojure
  (when (str/ends-with? (str *file*) "tools/<name>.clj")
    (apply -main *command-line-args*))
  ```

  so tests can `(require "tools/<name>")` the same file the CLI runs.
  (`*file*` stays the entry script across `require` — that is what
  makes the guard work.)

## Tests

- Test files are flat: `(require "tests/test")`, `deftest`/`is`
  forms, `(run-tests-and-exit)` at the bottom, registered in
  `tests/run.clj`.
- The whole suite shares ONE namespace at runtime: top-level `def`s
  in a test file must carry a file-unique prefix (`rs-dir`, `tf-dir`),
  or they will silently clobber another file's fixtures.
- Test surface law: language behavior → `tests/*_test.clj`; the C
  embedding ABI → C embed tests (`tests/*.c`, `examples/embed_*.c`);
  never `.zig` tests.

## Task-runner idiom

- Tasks are plain `defn`s in `lib/mino/tasks/builtin.clj`, registered
  in `mino.edn` with `:doc` and optional `:deps`. Shell out with
  `sh` (returns `{:exit :out :err}`) or `sh!` (throws on non-zero).
- Print progress with one-line `(println "  task: status")` messages;
  throw with a message (or `ex-info`) on failure — the runner reports
  it.

## Public-facing text

- Never describe code as "hand-written" or "hand-rolled" in
  user-facing docs, docstrings, or changelog lines.
