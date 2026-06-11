---
name: check-memory
description: Review recipe for the memory dimension — GC safety, ownership, lifetimes, leaks — over one C module shard. Invoked by reviewer agents; findings format and return contract come from the agent body.
user-invocable: false
---

# check-memory

Review the assigned C module for memory defects. mino's collector is
generational + incremental with a conservative stack scan as a safety
net — the bug class that ships is the one the scanner happens to hide.

Read first: `.claude/skills/check-security/references/known-bugs.md`
(the GC-window, realloc, and leak patterns are real shipped bugs) and
the ownership rules in `docs/ARCHITECTURE_CONTRACT.md` §4 and §7.

Look for, in priority order:

1. **GC windows.** Any GC-visible pointer (or raw buffer feeding one)
   held across `gc_alloc_typed` / `alloc_val` / any allocating helper,
   without `gc_pin`/`gc_unpin` or a `gc_depth` guard. Check pin/unpin
   pairing on every path including error paths (the gc_save stack
   asserts underflow only in debug builds).
2. **Ownership violations.** `free` on a GC-owned value; a GC value
   stored in host-owned memory the tracer can't see; `*_peek`/`*_get`
   results freed or retained past the next collection; `*_take`
   results leaked.
3. **realloc misuse.** `p = realloc(p, n)` without a temp.
4. **Error-path leaks.** For each early return/longjmp out of a
   function that allocated host memory: what frees it?
5. **Write-barrier gaps.** Direct stores into OLD-generation objects
   that bypass the barrier (mutation outside the provided mutators).

Ignore here: style, naming, factoring — other dimensions own those.
A finding needs file+line and the specific pointer/path; suggest a
`MINO_GC_STRESS=1 ./mino -e '...'` or ASan repro in `:suggestion`
when you can construct one. Severity: anything user-triggerable that
corrupts or reads OOB is `:high`; leaks are `:medium` unless unbounded.
Level is `:correctness`.
