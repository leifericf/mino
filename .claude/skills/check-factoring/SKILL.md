---
name: check-factoring
description: Review recipe for the factoring dimension — module boundaries, duplication, function/TU decomposition, dependency direction. Invoked by reviewer agents.
user-invocable: false
---

# check-factoring

Review the assigned shard's structure. The factoring authority is
`docs/INTERNAL_MODULE_MAP.md` (one responsibility per module, allowed
dependency directions) and `docs/ARCHITECTURE_CONTRACT.md`.

Look for:

1. **Boundary violations.** Includes against the dependency
   directions; reaching into another module's `internal.h` types;
   logic that belongs in another module's domain (the map's
   "Responsibility" column is the test).
2. **Duplication.** The same nontrivial logic in two TUs — especially
   helper patterns (iteration, growth, validation) that should live
   in the owning module's helpers. Cite both sites.
3. **Oversized units.** TUs over ~1000 LOC or functions over ~200 LOC
   (the carve-out precedent: `numeric_coerce.c`, `sequences_seq.c` —
   split by domain, name the new TU after its responsibility).
4. **Mixed concerns.** A function doing allocate + compute + report;
   special-form handlers carrying logic that belongs in a helper;
   primitives reimplementing collection internals.
5. **Dead structure.** Exported (non-static) symbols with one caller;
   parameters every caller passes identically.

Do NOT propose moves that change the public surface (`src/mino.h`,
`src/public/`) — that is a maintainer decision; file as `:medium`
with the suggestion clearly marked cross-module.

Findings are `:level :factoring`; severity `:medium` for boundary
violations and duplication, `:low` for size pressure. Factoring fixes
land in their own wave after correctness, before style.
