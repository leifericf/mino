# ADR 34: C-prim arglists

Date: 2026-08-27
Status: proposed (implementation starting)

## Context

After ADR 33 interned install-time primitives as vars, 270 of mino's
889 clojure.core publics carry `:arglists` (the core.clj defns) while
619 do not. Of those 619, 312 also lack arglists in the JVM 1.12.4
oracle (dynamic vars, mino-only extensions) and are correctly absent.
The remaining 307 vars are introspectable dead spots: `(doc first)`
prints no signature, editors and tooling see nothing, and the census
surface comparison records every one as a standing arglists mismatch
(absence is a real mismatch per comparison.clj, not a vacuous pass),
which can mask genuine regressions.

Measured composition of the 307 (live binary, main ac37805e):

- 294 C prims (292 clojure.core + clojure.string join and split)
- 8 special-form macros interned by eval_special_register_vars:
  binding declare defmacro fn lazy-seq let loop ns
- 4 core.clj def aliases: array-map get-in interleave partition
- 1 proc prim excluded from dynamic probing for safety: spit

An arity-conformance sweep (each target probed at arities 0..9 with
sentinel keywords; MAR001 vs any other outcome) against oracle
arglists found:

- 253 of 294 probed prims fully conformant (86 percent)
- 16 narrower (mino rejects an oracle arity)
- 25 wider (mino accepts arities the oracle rejects)

The narrower set decomposes into two classes. Seven prims reject
dangling key-value or option counts with the arity code MAR001 where
JVM rejects the same call with a value error (hash-map, sorted-map,
sorted-map-by, atom, agent, assoc, restart-agent): the arity is
accepted at the signature level and only the value shape is wrong, so
the arity check is miscoded, not the signature. The rest are real
missing arities (slurp opts, resolve env-arity, ref options,
with-bindings* variadic, require and use zero-arity, disj! one-arity,
aset and aget variadic dimensions).

The wider set is laxity: comparators accepting zero args, bit-fns
accepting one, and roughly twenty prims that silently ignore extra
arguments. Richer-than-oracle does not flag in the census.

## Decision

1. Attach `:arglists` to all 307 vars. The metadata must be true:
   never claim an arity that throws MAR001, and never silently claim
   fewer arities than the var accepts without recording the gap.

2. Source of truth is the census oracle (clojure/1.12.4-surface.edn)
   for the 253 verbatim-conformant prims (251 core prims swept plus
   join and split, both conformant; spit is verified from source during
   implementation and joins whichever class its arities support).

3. Class fixes before attach where cheap:
   - The seven dangling-count prims change their parity rejection
     from MAR001 to a value-type error, after which the oracle
     arglists are honest for them.
   - require and use gain zero-arity no-ops; disj! gains its
     one-arity. Their oracle arglists then apply verbatim.
   - Real gaps (slurp, resolve, ref, with-bindings*, aset, aget, and
     any residue) attach mino-true arglists and get census divergence
     entries; the missing arities themselves are out of scope.
   - Lax prims attach oracle arglists unchanged; the lax class is
     recorded as one census divergence entry listing the vars.

4. Mechanism: a generator (tools/gen_arglists.clj, a bb script
   following the tools/bump_satellites.clj precedent) reads the oracle
   from the census checkout
   and emits src/prim/arglists_data.h, a sorted table of
   {ns, name, arglists-edn} plus the hand-curated mino-true
   overrides. The emitted artifact is committed; regeneration is a
   documented task, never a Makefile step. At install,
   prim_install_table_with_capability bsearches the table after
   var_set_root, reads the EDN string with mino_read (the reader is
   available; core.clj itself is read later in the same flow), and
   attaches {:arglists v} through the meta helper exported from
   eval/defs.c (currently a static duplicate pair; one shared copy
   gains a header declaration). Special-form macros grow an arglists
   field in k_public_form_docs and a third meta key in
   eval_special_register_vars. The def aliases convert to defn or
   gain explicit arglists meta matching their real shapes.

5. doc renders signatures: prim_doc resolves the symbol and prepends
   the name and arglists lines (JVM shape) when var meta carries
   :arglists. Prim docstrings stay in the meta_find store; :doc is
   not ridden into var meta (not compared by census; optional
   follow-up).

6. Gates, so the metadata cannot rot:
   - A permanent mino-tests suite iterates every var carrying
     :arglists in clojure.core, clojure.string, clojure.repl: each
     declared minimum arity called with sentinel args must not throw
     MAR001; each undeclared arity 0..9 must throw MAR001 except a
     documented lax allowlist. A seeded shuffle fuzzes the same
     invariant across vars and arities.
   - The jvm-core corpus drops the "deferred: C-prim arglists"
     allowlist entry and gains arglists probes (prim, string, macro,
     mino-true, dangling-count error class).
   - Census re-run: the 307 standing mismatches collapse to the
     documented mino-true and lax entries.
   - Startup and RSS measured before and after; baseline 10 ms real
     on this host (contended; re-measured at implementation). 294
     tiny reads and 307 map allocations are expected to be noise.

## Consequences

- (meta #'first) carries :arglists; doc shows signatures; census
  arglists noise disappears, leaving only named divergences.
- Attaching metadata that claims arities a prim rejects is now a
  test-visible lie: the gate suite fails on any such drift in either
  direction (except the allowlisted lax set).
- The seven parity fixes change user-visible error codes for
  malformed calls (MAR001 to a value-type code); JVM callers see the
  same throw-versus-return behavior, only mino's class changes.
- The generator depends on an external census checkout only at
  regeneration time; the repo carries the emitted table.
- Var meta survives image save and load (verified by round-trip), and
  prim vars are re-interned by install before any image load, so the
  image seam needs no work.

## Deferred

- Implementing the real missing arities (slurp opts, ref options,
  resolve env-arity, variadic aset/aget, with-bindings* args).
- Riding :doc into prim var meta.
- Tightening the lax prims to reject extra arguments.
