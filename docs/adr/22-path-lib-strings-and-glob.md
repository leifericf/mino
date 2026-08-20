# ADR 22: Path library, strings only, one glob walker

Date: 2026-08-20

## Context

mino scripts manipulate file path strings today with `str`, `subs`,
and `clojure.string` splits; every script hand-rolls `basename`,
extension handling, and joining, and there is no glob anywhere (the
surface check for this ADR found fs.c's `realpath` / `which` /
`file-mtime` and io.c's `getcwd` / `chdir` / `file-seq`, but zero
path algebra and zero pattern matching). The tracker rates this
ki-14 (path) and ki-15 (glob/search).

The ecosystem evidence was gathered from primary sources and
verified by running the real implementations locally (babashka,
python3, elixir, janet, lua, node; the run record and PoC outputs
live in the path-lib run directory):

- Babashka's `babashka.fs` is the de facto JVM Clojure standard.
  Its most-cited friction is java.nio `Path` objects leaking into
  user code: every glob result needs a `(map str ...)` ritual
  (verified: `fs/glob` returns `sun.nio.fs.UnixPath`), and its own
  Node port went back to plain strings.
- Janet ships no path module in core (spork/path is a separate
  dependency) and no glob; every Janet tool grows private path
  helpers. Lua's lfs covers filesystem facts only; Penlight layers
  Python-shaped string ops on top. Leaving paths out of core is the
  single most-cited gap in both ecosystems.
- Elixir's `Path` / `File` split is the cleanest discipline found:
  `Path` is pure string algebra, `File` owns every effect, and one
  walker (`Path.wildcard`, over Erlang filelib) owns discovery with
  the politest contract in the field: results sorted, rendered
  exactly as the input was given (relative in, relative out), and
  dotfiles hidden by default with an explicit opt-in.
- Cross-language edge rules, verified live: extension of a dotfile
  is `""` everywhere except Go; `a.tar.gz` has extension `.gz`
  everywhere; `join` does not reset on an absolute later segment
  in node, Elixir, Go, and java.io.File (only java.nio resets);
  pathlib is the lone glob that shows dotfiles to `*`.

## Decision

Paths are strings. Pure string algebra prims land in
`src/prim/path.c` as a new ungated core domain `path`; one glob
walker lands over them; `mino.path` wraps both with idiomatic
names.

- **No path type, ever.** Every prim takes and returns strings.
  This is the data-orientation house rule and the single loudest
  lesson from babashka.fs. Where ecosystems disagree, the majority
  rule of node / Python / Elixir / Go wins and the divergence is
  documented, not averaged away.
- **Vocabulary: unix shell names.** `basename`, `dirname`,
  `extension`, `split-ext`, `join`, `split`, `normalize`,
  `absolute?`, `expand-home`, plus pathlib's one envied
  property-word as a plain function: `stem`. Babashka's
  `file-name` / `parent` / `components` map to
  `basename` / `dirname` / `split` through a table in the
  `mino.path` docstring; no alias functions ship (one canonical
  name per operation).
- **Extension carries the dot:** `".gz"`. Dotfiles answer `""`.
  Last dot only. `split-ext` answers `[stem ext]` with `nil` for a
  missing extension; `(str stem ext)` always rebuilds the input.
- **`join` concatenates and normalizes; an absolute segment does
  not reset the accumulation** (node / Elixir / Go majority).
  `(if (absolute? x) x (join a x))` expresses the reset behavior
  when a script wants it. Empty and nil elements are skipped;
  all-empty answers `"."`.
- **`normalize` is lexical Go-Clean-style:** backslashes fold to
  `/`, duplicate separators collapse, `.` segments drop, inner
  `..` cancels against a non-`..` parent, `..` after the root
  drops, leading `..` on a relative path stays, the trailing
  separator strips (root stays `/`), and `""` normalizes to
  `"."`. No filesystem contact.
- **Windows v1:** `/` is canonical in every output, `\` is
  accepted in every input (folded at each prim's edge). No drive
  letters, no UNC, no per-platform dialect; that layer can be
  added later without breaking one signature, the same additive
  stance as ADR 21's offsets-before-zones.
- **One glob, Elixir's contract.** `(glob pattern root? opts?)`:
  syntax `*`, `?`, `[class]` with ranges and `!` negation,
  `{a,b}` alternation, backslash escape, and `**` legal only as a
  whole segment (zero or more directories, never mid-segment).
  Dotfiles are hidden from wildcard matching unless the pattern
  segment itself starts with a dot or `{:match-dot true}` is
  given. Results are a sorted (byte order, not locale collation)
  vector of strings rendered as-given: a relative pattern rooted
  at a relative root answers relative paths. Symlinked directories
  are not followed by default (`{:follow-links true}` opts in),
  and the walk is depth-bounded (128 default, `{:max-depth n}`
  opt) so a followed symlink loop terminates. `{:recursive false}`
  caps `**` to a single directory level. Unmatched or unreadable
  directories answer `[]`, mirroring filelib and Python glob.
- **The matcher is pure and separate.** `path-glob-match` answers
  whether one string matches one pattern with the same syntax
  minus walker policy: `*` does not cross `/`, and dotfile
  visibility is not its business (the walker filters; the matcher
  matches). Patterns are user-written, so the pattern side never
  gets backslash acceptance; `\` in a pattern is always an escape.
  Pattern length is capped at 256 with a classified
  `:eval/bounds` throw.
- **`expand-home` is the one impure algebra resident:** a lone `~`
  or `~/` prefix expands through `HOME` (POSIX) or `USERPROFILE`,
  then `HOMEDRIVE` + `HOMEPATH` (Windows). `~otheruser` is not
  expandable without pwd.h and passes through unchanged, as in
  Elixir. Inputs without a `~` prefix are returned unchanged.
- **Capabilities: the algebra is ungated, the walker is gated.**
  String math reads nothing and mutates nothing, so
  `k_prims_path` installs in the floor like `time`. The glob
  walker reads directory contents, so `glob` installs with the fs
  capability alongside fs.c's table (file-seq's precedent: a
  directory walker rides a capability gate; io carries file-seq,
  fs carries glob). `path-glob-match` is pure and ungated.
- **Scope guard, v1:** no walk-file-tree visitor, no zip/unzip,
  no `which`/XDG discovery (fs.c already has `which`), no
  copy/move tree, no permission surface, no `relativize`, no
  dynamic `*path-cwd*` override (roots are plain arguments, so
  tests need no global state), no win32 dialect.

The `mino.path` namespace wraps the prims with short names
(`join`, `split`, `basename`, `dirname`, `extension`, `split-ext`,
`stem`, `normalize`, `absolute?`, `expand-home`, `glob`, `match`),
carries the babashka mapping table and the Windows stance in its
docstring, and adds no logic of its own.

## Consequences

- The pure prims are total functions over bytes: no filesystem
  contact, no locale, no encoding assumptions (filenames are
  bytes), testable as golden vectors and round-trip properties
  without fixtures.
- The matcher is untrusted-input surface: seeded garbage and
  mutation fuzz (match-or-return, never crash) plus the escape
  law: every string matches its own escaped pattern.
- Glob tests build their fixture trees with mino's own `mkdir-p` /
  `spit` under a temp root and tear them down with `rm-rf`; no
  external tools, per the repo rule. The external-oracle battery
  (cross-checks against `find`, `ls`, and Python's glob) lives in
  the mino-tests satellite repo.
- Symlink-behavior tests are POSIX-tier (fs_test precedent):
  `ln -s` has no portable Windows equivalent, and the Windows
  glob walker uses `stat` because lstat is unavailable there, the
  same split io.c and fs.c already make.
- Sorting is byte order. Scripts wanting locale collation sort
  again with their own comparator; the walker stays deterministic
  across machines.
- A future win32 dialect, a `relativize`, or a visitor walker all
  extend without breaking: they are new names over the same
  strings.
