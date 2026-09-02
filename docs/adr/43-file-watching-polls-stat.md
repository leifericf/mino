# ADR 43: File watching is a polling fn over stat, no native watchers

Date: 2026-09-03

## Context

Scripts want to react to file changes: rerun a task when a source file
saves, reload config, tail a growing directory. Other scripting
runtimes ship this built in, usually over the kernel notification
facilities, which differ per platform: one mechanism on Linux, another
on the BSDs and macOS, a third on Windows, each with its own handle
lifetime, event coalescing, and recursion semantics. mino already has
the observation surface: `stat` returns `{:type :size :mode :mtime
:symlink?}` with millisecond mtimes, `file-mtime` is the one-key fast
path, and the glob walker (ADR 22) enumerates trees. The dominant
scripting uses are watch-and-rerun loops where sub-second latency is
worthless because the reaction (a rebuild, a reload) costs far more
than the poll interval.

## Decision

File watching ships as a pure-mino polling fn over the existing stat
surface: snapshot the watched paths' mtimes and sizes, sleep an
interval, diff, and report created, modified, and deleted paths as
plain data, with the interval a keyword opt. No kernel watch facility
is vendored or wrapped; no new C, no new capability bit, no per-
platform event semantics to reconcile. The fn is future work scoped by
this record; its API details are settled when it lands.

## Consequences

- Change detection latency is the poll interval, and each tick costs
  one stat per watched path; watching very large trees at short
  intervals is proportionally expensive. This covers the rerun-loop
  and config-reload uses, which tolerate second-scale latency.
- The three platforms behave identically because they share the stat
  path; there is no per-platform watcher matrix to test or document.
- Events between two polls coalesce into one diff; a create-then-
  delete inside one interval is invisible. Acceptable for the target
  uses, stated here so it is a documented property, not a bug report.
- If a real workload needs kernel-latency events, that is a new
  record with the workload as evidence, not a reopening of this one.

## Alternatives

- **Vendor the kernel watch facilities.** Sub-second latency, no
  polling cost at rest, what the dedicated watcher libraries do.
  Rejected: three platform mechanisms with divergent semantics is a
  large native and test surface for a latency win the target uses do
  not need.
- **One platform's facility, polling elsewhere.** Smaller than the
  full matrix. Rejected: split semantics are worse than uniformly
  modest ones; the same script would behave differently per host.
- **No watching surface at all.** Scripts hand-roll the poll loop.
  Rejected: the loop has real edges (deletion races, first-run
  semantics, interval drift) worth solving once.
