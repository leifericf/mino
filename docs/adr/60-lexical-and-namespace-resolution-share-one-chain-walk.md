# ADR 60: Lexical and namespace resolution share one chain walk

Date: 2026-09-07

## Context

An unqualified symbol resolves through a single walk of the environment
chain. `eval_symbol` (`src/eval/special.c`) tries the dynamic stack,
then calls `mino_env_get_sym_ns` (`src/names/env.c`), which walks
`env->parent` from the innermost frame outward and returns the first
binding it finds. Lexical `let` and `fn` frames and the namespace root
frame sit on the same chain: a namespace env is an ordinary `mino_env`
whose `is_ns_root` flag is set (`ns_env.c:81,93`), and its cells are
namespace bindings rather than locals. The chain therefore carries two
concerns at once. A `let`-bound local and a `def`-bound global are found
by the same probe loop; only the `is_ns_root` flag on the frame that hit
tells them apart.

That flag is load-bearing, not decorative. The walk reports it back
through `*from_ns`, and the caller applies namespace semantics only on a
ns-frame hit: a hit in a ns root that holds a `MINO_VAR` is auto-derefed
through `var_read` (`special.c:244`), because a namespace cell must
surface its value, while a lexical or dynamic binding that happens to
hold a var is left intact so `(let [v (resolve 'foo)] v)` still returns
the var. The single `is_ns_root` bit shares the `ht_cap` word so the
env struct stays 48 bytes (`runtime_types.h:161-168`). The
decomplection pass asked whether fusing lexical scope resolution and
namespace resolution into one chain walk is a deliberate braid worth
keeping or a complect to separate into two resolvers.

## Decision

The braid is accepted. Lexical scope and namespace lookup share one
chain walk, and the `is_ns_root` flag on the frame that hit is the seam
that recovers the distinction where it matters. This is faithful to the
language: a closure captured inside a namespace resolves its free
symbols through the defining ns as the outermost link of the same
lexical chain, which is exactly how `core.clj` macros and fns see the
core vars they call. Modeling the namespace as the tail of the lexical
chain is not an accident of the implementation; it is the resolution
rule. Splitting it into two resolvers with two probe loops would
reproduce the same outward walk twice and reintroduce the join at the
call site anyway, since the caller must still know which loop produced
the hit to decide whether to deref.

The braid is fenced by keeping the ns-versus-lexical decision in exactly
one place: the `is_ns_root` flag, read at the single hit site. The
variant walkers exist for the cases that need a narrower rule
(`mino_env_get_sym_lex` stops at the first ns root so a raw var never
spills into value position; `mino_env_get_sym` ignores the flag on the
already-classified ns-env paths), but they share the same frame model,
so there is one notion of a frame and one notion of a ns frame.

## Consequences

Resolution stays one code path with one hot loop, and the symbol carries
its own interned length and hash so each probed parent frame skips
`strlen` and the FNV recompute. The cost is that the `is_ns_root` flag
must be honored everywhere a walk can cross a ns boundary, and the
auto-deref rule at `special.c:244` is subtle: it is correct precisely
because `refer` binds the source var and `declare` binds an unbound var,
so the ns-frame hit must deref while the lexical hit must not. That
subtlety lives at one site and is commented there. The record names the
condition that would flip the decision: if an embedding ever needed
namespace resolution without a lexical chain (a pure symbol table with
no closure capture), or if the two concerns grew genuinely different
frame shapes, the braid would be worth cutting into a dedicated
namespace resolver with the lexical walk delegating to it at the ns
boundary. No such need exists today.

## Alternatives

Separate the two into distinct resolvers: walk lexical frames to the
first ns root, then hand off to a namespace resolver keyed on the
current ns. Its strength is that each resolver states one rule and the
`is_ns_root` flag disappears from the env struct. Rejected because the
handoff point is exactly where the lexical chain already ends (the ns
root frame), so the split adds a second walk and a second dispatch
without removing the join: the caller still must know which resolver
answered to apply or withhold the var deref. The fused walk with the
flag is the smaller, faster expression of the same rule.

Push the ns-versus-lexical distinction into the binding value instead of
the frame, tagging each cell as lexical or namespace. Rejected because
it moves a per-frame property onto every binding, growing the common
lexical binding to carry a bit it never varies within a frame, and the
frame already knows the answer once for all its cells via `is_ns_root`.
