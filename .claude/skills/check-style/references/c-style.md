# mino C style — the checkable standard

The build is the first gate; this file is what reviewers check beyond
"it compiles". Sources of truth: the Makefile CFLAGS, `./mino task
qa-arch`, `./mino task lint-zig`, and `docs/ARCHITECTURE_CONTRACT.md`.
When this file and a gate disagree, the gate wins — fix this file.

## Language level

- ANSI C99, no compiler extensions: every TU compiles under
  `-std=c99 -Wall -Wpedantic -Wextra -Werror` (gcc, Apple clang,
  mingw, MSVC canary, and pinned zig clang all gate in CI).
- No `.zig`, no C11/GNU features (no anonymous unions, no statement
  expressions, no `__attribute__` outside existing guarded uses).
- Platform splits live behind `_WIN32` guards, isolated to the files
  that already carry them; POSIX calls that follow symlinks (`stat`)
  vs not (`lstat`) are chosen deliberately and the choice commented.

## Size discipline (qa-arch gates, hard numbers)

- Translation unit ≤ 1100 LOC. Approaching the limit means the TU is
  carrying two responsibilities — split by domain (`numeric_math.c`,
  `sequences_seq.c` are the precedent), don't shave lines.
- Function body ≤ 250 LOC. Same rule: split by phase or case family.

## Linkage and prototypes (lint-zig gates)

- `static` by default. Anything non-static needs a prototype in the
  owning `internal.h` (`-Wmissing-prototypes`,
  `-Wmissing-variable-declarations` are -Werror).
- Also gated: `-Wshadow -Wstrict-prototypes -Wpointer-arith
  -Wwrite-strings -Wundef -Wvla -Wimplicit-fallthrough -Wcomma
  -Wunreachable-code -Wnested-externs -Wredundant-decls -Wformat=2`.
- Deliberately NOT gated (don't "fix" these): partial-brace zero-init
  `{0}`, `default:` arms in VM switches, const-dropping casts in GC
  marking, function-return casts. They are audited idiom.

## Naming and ownership (ARCHITECTURE_CONTRACT §4)

- `*_new` / `*_alloc` — caller owns the result.
- `*_free` / `*_destroy` — caller releases ownership.
- `*_get` / `*_peek` — borrowed pointer, do not free.
- `*_take` — ownership transfers to caller.
- State access is explicit `S->field`; no alias macros. The only
  macros that implicitly need a local `S` are `gc_pin`/`gc_unpin`.
- GC-owned (via `gc_alloc_typed`) vs host-owned (`malloc`) is a hard
  boundary: never `free` a GC-owned value, never let a host-owned
  struct hold the only reference to a GC value across an allocation.

## Includes

- Path-qualified: `#include "runtime/internal.h"`, never bare
  `#include "internal.h"` (every subsystem owns one).
- Respect the dependency directions in `docs/INTERNAL_MODULE_MAP.md`;
  no new cross-module includes without a cross-module escalation.

## Error handling

- Every `abort()` carries a comment explaining why recovery is
  impossible (qa-arch inventories abort sites; new ones need explicit
  justification).
- User-triggerable input must never reach a `MINO_ERR_CORRUPT` path.
- Classify via the three-class model in `src/diag/diag_contract.h`;
  per-subsystem `internal.h` files carry "Error classes emitted"
  blocks — keep them current when you add paths.

## Comments

- Comment density matches the surrounding file. Comments state
  constraints the code can't (ownership, GC-safety windows, why an
  abort is unrecoverable) — not what the next line does.
- TU-top block comment states the file's single responsibility.
