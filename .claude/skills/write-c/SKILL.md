---
name: write-c
description: Recipe for writing new ANSI C99 runtime code in mino — module placement, GC discipline, error classes, the add-a-primitive and add-a-special-form rituals. Invoked by writer agents.
user-invocable: false
---

# write-c

Write new C for the mino runtime. The standards are
`.claude/skills/check-style/references/c-style.md` (read it first) and
the contracts in `docs/ARCHITECTURE_CONTRACT.md`; placement comes from
`docs/INTERNAL_MODULE_MAP.md`.

Procedure:

1. **Place it.** Find the owning module in the module map; read that
   TU end to end before adding to it. New TU only when the
   responsibility is genuinely new — name it after the responsibility
   and add it to the map.
2. **Follow the rituals.** New primitive: the 5-step "How to Add a
   Primitive" in the module map (domain file → signature → declare in
   `prim/internal.h` → `k_prims_<domain>[]` row → tests). New special
   form: the 5-step ritual below it. Don't improvise registration.
3. **GC discipline up front.** Decide ownership of every allocation
   before writing: GC-owned via `gc_alloc_typed` (pin temporaries
   across allocation points) or host-owned malloc (every error path
   frees). When a helper allocates twice, write the `gc_depth`/pin
   guard immediately — not after the bug.
4. **Error classes.** Every failure path picks its class consciously:
   `MINO_ERR_RECOVERABLE` via `prim_throw_classified` for anything
   user-catchable; `MINO_ERR_HOST` for limits/IO; `abort()` only with
   a why-unrecoverable comment. User input must never reach an abort.
5. **Verify like the matrix.** `./mino task build` (the -Werror flag
   set is the floor), then the module's tests, then
   `./mino task qa-arch` if you grew a TU. Sanitizer lanes run at the
   landing wave — but if you wrote pointer-heavy code, run
   `./mino task build-asan` + the relevant test yourself.

Tests for new behavior go in `tests/*_test.clj` (language surface) or
the C embed tests (ABI surface) — write them or coordinate with the
write-tests dispatch, but never land implementation without them.

Public-facing text rule: never describe code as "hand-written" or
"hand-rolled" in docstrings, docs, or changelog lines.
