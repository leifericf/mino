# ADR 09: Primitives are static; the registration tables are the registry

Date: 2026-06-12

## Context

All 303 primitive entry functions carried extern declarations in
`src/prim/internal.h`, though only ~120 are referenced outside their
defining translation unit (evaluator/VM/JIT fast paths, cross-file
registration tables, a handful of cross-domain calls). The
per-domain `k_prims_<domain>[]` brace tables are what `install.c`
actually consumes, and downstream consumers of the primitive
inventory (the site's language reference, the LSP's completions)
read it from a live state via runtime introspection, not from C
declarations. The amalgamated build concatenates every `.c` into one
TU, so function names are globally unique already.

## Decision

A primitive is `static` in its defining file by default; the
registration tables are the single extern-visible registry. The
exceptions — functions with real cross-TU callers — live in one
labeled section of `prim/internal.h`, grouped with the reason they
are exceptions. The add-a-primitive ritual drops to four steps
(`docs/INTERNAL_MODULE_MAP.md`), and the lint lane's
`-Wmissing-prototypes` flags any stray non-static prim. No generated
registry: both existing inventory consumers already get richer data
through introspection (`apropos`, doc strings) against a running
state; a data-file-plus-codegen path would be a second source of
truth for no second consumer. Revisit only for a consumer that
cannot run a mino binary.

## Consequences

- Promoting a prim to a fast path or another domain's table means
  dropping `static` and adding a declared exception — one extra,
  deliberate step.
- `internal.h` now states which prims are load-bearing across TUs
  and why; the compiler enforces the classification.
- Names must stay globally unique despite `static` (the amalgam
  builds one TU), so file-local naming freedom does not increase.

## Alternatives

- X-macro registration table: one definition point generating decls
  and tables together, proven in comparable C runtimes; rejected
  because it hides definitions from grep and debuggers and adds
  machinery where subtraction sufficed.
- Data-file registry with generated C: appealing dogfooding (mino
  generating mino), but runtime introspection already is the
  data-driven pipeline with zero drift; codegen would duplicate it.
- Status quo: no churn, but ~180 unneeded externs, misplaced
  declarations, and a five-step ritual that implied every prim is
  public to the whole runtime.
