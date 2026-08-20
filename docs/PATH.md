# mino.path

Paths as plain strings, and the one glob walker. Design contract:
`docs/adr/22-path-lib-strings-and-glob.md`. The full reference is
the docstring:

```
(require '[mino.path :as p] '[clojure.repl :refer [doc]])
(doc p/glob)
```

No path type, ever: every fn takes and returns strings. The
separator is `/` in every output; `\` is accepted in path inputs
and folds to `/`. Patterns (glob, match) are the carve-out: `\`
in a pattern is an escape, never a separator. That is the whole
Windows v1 story: no drive letters, no UNC, and that layer can be
added later without breaking one signature.

## The vocabulary

```
(p/join "src" "demo" "core.clj")  ; "src/demo/core.clj"
(p/split "src/demo/core.clj")     ; ["src" "demo" "core.clj"]
(p/basename ".../core.clj")       ; "core.clj"
(p/dirname "src/demo/core.clj")   ; "src/demo"
(p/extension "a.tar.gz")          ; ".gz"     (dotfiles answer "")
(p/split-ext "a.tar.gz")          ; ["a.tar" ".gz"]  (os.path shape)
(p/stem "/a/b/c.tar.gz")          ; "c.tar"   (the pathlib stem)
(p/normalize "a/./b/../c/")       ; "a/c"     (lexical, no fs)
(p/absolute? "/etc/passwd")       ; true
(p/expand-home "~/x")             ; "/home/you/x"
```

## Edge rules

The cross-language majority, verified against node, Python,
Elixir, bb, and Go during the design run:

| Rule | mino |
|------|------|
| extension of `.bashrc` | `""` (Go is the outlier) |
| extension of `a.tar.gz` | `".gz"` (last dot only; bb's bare `gz` is the outlier) |
| `join` with an absolute later part | concatenates: `(p/join "/foo" "/bar")` is `"/foo/bar"` (only java.nio resets; `(if (p/absolute? x) x (p/join a x))` expresses the reset) |
| `normalize` | lexical Go-Clean: strips the trailing separator, drops `..` after the root, keeps leading `..` on relative paths; never touches the filesystem |
| `basename` | raw last segment, no `..` resolution (node parity); `"/"` answers `""`, `""` answers `"."` |
| `dirname` | the cleaned prefix; a one-segment relative path answers `"."` |

## glob: the one walker

```
(p/glob "**/*.clj" "src")            ; sorted vector of strings
(p/glob "*" "." {:match-dot true})   ; reveal dotfiles
```

Syntax: `*` (within a segment), `?`, `[class]` with ranges and
`!` negation, `{a,b}` alternation (nested, top-level commas
split), backslash escape, and `**` as a whole segment matching
zero or more directories (`a/**/b` matches `a/b`; a trailing `**`
matches everything left).

The contract:

- Results are a **sorted** (byte order, not locale collation)
  **vector of strings rendered as-given**: a relative pattern
  with no root answers relative paths, an explicit root prefixes
  them with that root exactly as you spelled it, and an absolute
  pattern walks from `/` and ignores root.
- **Dotfiles are hidden** unless the pattern segment itself
  starts with a dot (`.h*` reveals `.hidden`) or
  `{:match-dot true}` is given. The matcher itself is pure:
  `(p/match "*" ".foo")` is true; visibility is walker policy.
- **Symlinks**: directories found by wildcards are not followed
  unless `{:follow-links true}`. Segments the pattern names
  literally are your explicit path and resolve through symlinks
  (so `/tmp/x` works where `/tmp` is one). The walk is depth
  bounded so a followed symlink loop terminates.
- `{:recursive false}` caps `**` to one directory level;
  `{:max-depth n}` bounds the walk (`1..4096`, default 128; the
  walk recurses one C frame per level).
- Missing or unreadable directories answer `[]`.
- Patterns are capped at 256 bytes and the matcher carries a
  work budget; star-heavy patterns whose non-match proof would
  take exponential backtracking throw `:eval/bounds` instead of
  hanging.

## Babashka porting

The namespace docstring carries the full `babashka.fs` mapping
table with every divergence noted (argument order of `fs/glob`,
`{:hidden true}` vs `{:match-dot true}`, whole-path vs basename
`strip-ext`, nil vs `"."` for `parent`, and more). One canonical
name per operation: no alias functions ship.

## Capabilities

The string algebra and `match` are ungated floor prims (string
math reads nothing). `glob` reads directory contents and installs
with the **fs** capability, the file-seq precedent: a directory
walker rides a capability gate.
