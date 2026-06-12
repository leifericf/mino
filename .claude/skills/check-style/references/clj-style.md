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

## Simplicity (design filter)

- Prefer simple over easy: don't intertwine unrelated concerns —
  state with time, data with behavior, logic with effects. Choose the
  design with fewer braided concerns even when it is less familiar.
- Values are immutable facts; model change as a succession of new
  values, not in-place mutation. Mutable references (atoms) live at
  the edges; pure functions take values and return values; `swap!`
  takes a pure function.
- Data first: plain maps with keyword keys for entities, options, and
  configuration. `defrecord` only for protocol dispatch or a measured
  hotspot — never to imitate classes.

## Idiom checklist

- Sequence library first: `map`/`filter`/`reduce`/`into`/`group-by`/
  `keep` over manual `loop`/`recur`; `mapv`/`filterv`/`reduce-kv`
  when a vector is wanted; `vec`, not `(into [] ...)`.
- Nil punning: `(when (seq coll) ...)`, not
  `(when-not (empty? coll) ...)`; sets as predicates where natural.
- `when` for one-armed `if`; `if-let`/`when-let` over `let` + `if`;
  `if-not`/`not=` over wrapped `not`; `cond` with `:else`; `case`
  for compile-time constants; threading macros over deep nesting.
- Multi-arity for defaulting (small arities call the largest,
  ordered fewest-to-most); an options map instead of more than 3-4
  positional parameters.
- Errors: `ex-info` with rich data maps (ids, the relevant input
  slice) at boundaries; explicit error values inside pure cores when
  callers branch on outcome. Pick one per area; don't mix
  arbitrarily.

## Naming and formatting

- Predicates end in `?`; effectful or mutating functions end in `!`;
  dynamic vars wear earmuffs (`*out*`); conversions use `->`
  (`row->user`); unused bindings are `_` or `_`-prefixed.
- 2-space indent, no tabs; align `let` bindings and map values; no
  commas in sequential literals; gather trailing parens; one blank
  line between top-level forms.

## What to avoid

- Imperative index-walking where sequence functions suffice.
- `def` inside functions; vars as hidden mutable state.
- Macros where functions suffice: write the function first; a macro
  is for genuine syntactic abstraction, never for a single call site
  or to save characters.
- Tacit overuse — `comp`/`partial`/`#()` chains that obscure intent.

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

## Core and shell

Spine scripts and tasks separate deciding from doing — the existing
scripts are the template:

- Pure transformation functions first (`insert-lines` in
  merge_proposals: text → text; the triage fold: findings → ordered
  punch list), then a thin effectful layer (`merge!`, `-main`) that
  reads files, calls the pure core, writes results.
- Decisions return data (EDN-shaped maps like
  `{:merged [...] :escalated [...]}`) so the shell's behavior is a
  switch on values, not logic of its own.
- Tests require the script as a library and exercise the functions —
  never by shelling out to the CLI and never by mocking; effects are
  exercised against real scratch dirs and scratch git repos (see
  tests/tooling_integrate_fixes_test.clj).
- Reading state directly in a decision function is fine (slurp the
  EDN, query the table); introducing an indirection layer purely for
  testability is not.

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
