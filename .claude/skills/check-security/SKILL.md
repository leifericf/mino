---
name: check-security
description: Review recipe for the security dimension — untrusted input to memory unsafety, traversal, overflow — over one C module shard. Invoked by reviewer agents.
user-invocable: false
---

# check-security

Review the assigned C module for security defects. mino is an
embeddable runtime: every primitive is reachable from untrusted mino
code, and the host's trust boundary is `src/mino.h`. The contract rule
is absolute: **no user-triggerable input may reach a
`MINO_ERR_CORRUPT` (abort) path** — a user-caused abort is a bug.

Read first: `references/known-bugs.md` in this skill — every pattern
there shipped. Then sweep:

1. **Buffer arithmetic.** `snprintf` return values used as lengths;
   size-doubling growth without overflow guards; any
   user-controlled count flowing into `malloc`/`memcpy`/indexing.
2. **Filesystem.** `stat` vs `lstat` in walks; path joins with
   user-supplied components; TOCTOU between check and use; symlink
   handling in anything recursive (CWE-59 shipped here once).
3. **Parser surfaces.** read.c, regex, format strings: repetition
   counts, literal overflow (must saturate or promote, never wrap),
   recursion depth on attacker-shaped input.
4. **Abort reachability.** For each `abort()` in the module, can mino
   code or hostile input steer execution there?
5. **Capability boundary.** Primitives behind `MINO_CAP_*` gates must
   not be reachable when the capability is off; host callbacks must
   not be invokable with confused arguments.

Ignore here: leaks without an unsafety consequence (memory dimension),
style, factoring. Severity: reachable memory unsafety from mino code
is `:high`. Level is `:correctness`. Put a candidate
`./mino -e '...'` repro in `:suggestion` whenever you can.
