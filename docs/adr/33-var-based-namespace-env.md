# ADR 33: Namespace environments bind vars uniformly

Date: 2026-08-27

## Context

mino namespaces hold their bindings in a flat env: `def` binds the
VALUE while `refer`, `declare`, and var sync bind Var objects.
`eval_symbol` compensates by auto-dereferencing any Var it finds in an
ns env. The mixed representation leaks: `(def ret (def x 33))` stores
the Var in ret's root, but reading ret returns 33, because the read
path derefs whatever the binding holds rather than dereferencing the
name's own Var exactly once. The bytecode compiler additionally
compiles `def` to produce the value, so the tiers disagree on the
def form's result. JVM Clojure is uniform: ns mappings are always
Vars, reads deref the mapped Var once, and `(def ret (def x 33))`
leaves ret holding #'x.

## Decision

Migrate to Var-based ns environments in three landed steps. First,
install-time primitives intern as Vars: prim tables create (or reuse)
the Var, set the raw primitive as root, and bind the Var into the env,
so every core read flows through the existing auto-deref. Second,
`def`, `defmacro`, `intern`, `refer`, `declare`, and module load bind
the Var like-for-like, which makes the read-time deref correct by
construction: one deref per name, exactly the JVM's single level.
Third, the bytecode `def` emits the Var after the global set, so both
tiers return #'name from a def form.

Two structural pieces keep the migration honest. Ns-root envs carry
an `is_ns_root` flag and two walk variants: lexical walks stop at ns
frames (a Var never spills into value position through a captured
parent env), while symbol reads traverse ns frames and deref hits, so
macros defined in clojure.core keep defining-ns resolution.
`var_set_root` bumps the inline-cache generation on every path, so
redefinition invalidates cached global reads uniformly. Raw bindings
from embedders keep working: the lazy prim-to-var promotion now
prefers an already-interned Var instead of re-rooting one.

## Consequences

`(do (def a 1) (def b (def a 1)) (var? b))` is true on every tier.
Reads of globals pay one type check plus root fetch past the env
lookup; the inline cache absorbs the steady-state call path, measured
at +2-8% on the sub-microsecond benches with zero allocation change.
Var identity is stable across `(var inc)`, `(resolve 'inc)`, and
qualified spellings. Image format is unchanged. Code that reached
into ns envs expecting raw values must go through the var helpers.

## Alternatives

Deref-free raw bindings with a parallel var registry: preserves the
old speed but keeps two representations and the off-by-one read
semantics that caused the divergence. Copying def's value-meta to the
var without uniform binding: solves arglists only, not the def-return
read semantics.
