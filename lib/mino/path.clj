(ns mino.path
  "Paths as plain strings: joining, splitting, and the one glob
  walker over the path prims (ADR 22).

  (require '[mino.path :as p])
  (p/join \"src\" \"demo\" \"core.clj\")   ; \"src/demo/core.clj\"
  (p/split \"src/demo/core.clj\")         ; [\"src\" \"demo\" \"core.clj\"]
  (p/basename \"src/demo/core.clj\")      ; \"core.clj\"
  (p/dirname \"src/demo/core.clj\")       ; \"src/demo\"
  (p/extension \"core.clj\")              ; \".clj\" (dotfile answers \"\")
  (p/split-ext \"a.tar.gz\")              ; [\"a.tar\" \".gz\"] (os.path shape)
  (p/stem \"a.tar.gz\")                   ; \"a.tar\" (the pathlib stem)
  (p/normalize \"a/./b/../c/\")           ; \"a/c\" (lexical, no fs)
  (p/absolute? \"/etc/passwd\")           ; true
  (p/expand-home \"~/x\")                 ; \"/home/you/x\"
  (p/glob \"**/*.clj\" \"src\")           ; sorted vector of strings
  (p/match \"*.clj\" \"core.clj\")        ; the pure matcher

  No path type, ever: everything takes and returns strings. The
  separator is / in every output; \\ is accepted in path inputs
  and folds to / (the Windows v1 stance: no drive letters, no
  UNC, documented as an additive-later layer). Patterns are the
  carve-out: \\ in a glob or match pattern is an escape, never a
  separator.

  Edge rules (the cross-language majority, verified against
  node, Python, Elixir, bb, and Go): extension carries the dot
  and a dotfile answers \"\"; join does not reset on an absolute
  later segment, so (p/join \"/foo\" \"/bar\") is \"/foo/bar\";
  normalize is lexical and strips the trailing separator; the
  glob walker hides dotfiles unless the pattern segment itself
  starts with a dot or {:match-dot true} is given, does not
  follow wildcard-found symlinked directories unless
  {:follow-links true} (literal pattern segments are your
  explicit path and resolve through symlinks), and answers a
  byte-order sorted vector rendered as-given.

  Babashka fs mapping (documented, not aliased; one canonical
  name per operation; every divergence noted so a port is never
  silent):
    fs/path           (p/join & parts)       ; also fs/file; ours
                                          ; does not reset on an
                                          ; absolute later segment
    fs/file-name      (p/basename s)
    fs/parent         (p/dirname s)          ; ours answers "."
                                          ; where bb answers nil
    fs/components     (p/split s)            ; ours keeps a
                                          ; leading "/" element
    fs/extension      (p/extension s)        ; ours keeps the dot
    fs/strip-ext      (p/stem s)             ; ours is the
                                          ; basename stem; bb's
                                          ; whole-path strip-ext
                                          ; is (first (p/split-ext s))
    fs/split-ext      (p/split-ext s)        ; ours keeps the dot
    fs/normalize      (p/normalize s)
    fs/absolute?      (p/absolute? s)
    fs/expand-home    (p/expand-home s)
    fs/glob           (p/glob pattern root? opts?)
                       ; ours takes the pattern first; bb's
                       ; {:hidden true} is {:match-dot true}
    fs/match          (p/glob ...)           ; bb's match is a
                       ; directory walker like glob; ours (p/match
                       ; pattern s) is the pure boolean matcher

  glob requires the fs capability (it reads directories); every
  other fn here is ungated string algebra over the floor prims
  (expand-home reads the environment: HOME, or USERPROFILE and
  HOMEDRIVE+HOMEPATH on Windows).")

;;;; Joining and splitting

(defn join
  "Joins parts with / and normalizes: nil and empty parts skip, an
  absolute part does not reset the accumulation, all-empty answers
  \".\"."
  [& parts]
  (apply path-join parts))

(defn split
  "Splits into raw segments: empty segments drop, a leading /
  answers a \"/\" element, . and .. pass through unresolved."
  [s]
  (path-split s))

;;;; Name parts

(defn basename
  "The last path segment, raw (no .. resolution): \"/\" answers
  \"\", \"\" answers \".\"."
  [s]
  (path-basename s))

(defn dirname
  "The directory part: the cleaned path cut at the last separator.
  A one-segment relative path answers \".\"."
  [s]
  (path-dirname s))

(defn extension
  "The extension with its dot and the last dot only: \"a.tar.gz\"
  answers \".gz\"; dotfiles and plain names answer \"\"."
  [s]
  (path-extension s))

(defn split-ext
  "Splits into [stem extension] over the whole path (os.path
  shape); the extension is nil when absent. (str stem extension)
  rebuilds the input."
  [s]
  (path-split-ext s))

(defn stem
  "The basename minus its last extension (the pathlib stem):
  \"/a/b/c.tar.gz\" answers \"c.tar\"; \".bashrc\" answers
  \".bashrc\"."
  [s]
  (path-stem s))

;;;; Normalization

(defn normalize
  "Lexical clean: folds \\ to /, collapses //, drops . segments,
  cancels inner .. against a non-.. parent, drops .. after the
  root, keeps leading .. on relative paths, strips the trailing
  separator. No filesystem contact."
  [s]
  (path-normalize s))

(defn absolute?
  "True when the path starts with / after backslash folding."
  [s]
  (path-absolute? s))

(defn expand-home
  "Expands a lone ~ or ~/ prefix through HOME (POSIX) or
  USERPROFILE, then HOMEDRIVE+HOMEPATH (Windows). ~otheruser
  passes through; anything without a ~ prefix is returned
  as-is."
  [s]
  (path-expand-home s))

;;;; Errors

(defn- path-fail
  "Throws a classified mino.path diagnostic (ADR 37): :mino/kind names
  the error class so classed catch dispatches on it, :mino/message the
  human string ex-message returns, :mino/data the detail ex-data reads."
  [kind code msg data]
  (throw {:mino/kind kind :mino/code code :mino/message msg
          :mino/data data}))

;;;; Relative paths

(defn relativize
  "The relative path FROM base TO target, so that resolving the answer
  against base yields target (java.nio Path.relativize, lexical). Both
  sides are normalized first (\\ folds to /, . and redundant .. cancel);
  the shared leading segments are dropped, each remaining base segment
  becomes a \"..\", and the remaining target segments follow. Identical
  paths answer \"\". Base and target must agree on absoluteness: an
  absolute path against a relative one (or the reverse) cannot be
  relativized and throws a diagnostic with :mino/kind :path/relativize
  carrying the offending :base and :target. No filesystem contact."
  [base target]
  (when (not= (path-absolute? base) (path-absolute? target))
    (path-fail :path/relativize "MPR001"
               "mino.path/relativize: base and target must both be absolute or both relative"
               {:base base :target target}))
  (let [strip (fn [s]
                (let [segs (path-split (path-normalize s))]
                  (if (and (seq segs) (= "/" (first segs)))
                    (rest segs)
                    segs)))
        b (strip base)
        t (strip target)
        common (loop [n 0 bs b ts t]
                 (if (and (seq bs) (seq ts)
                          (= (first bs) (first ts)))
                   (recur (inc n) (rest bs) (rest ts))
                   n))
        up (repeat (- (count b) common) "..")
        down (drop common t)
        parts (concat up down)]
    (if (seq parts)
      (apply path-join parts)
      "")))

;;;; Discovery

(defn glob
  "Walks directories matching a glob pattern; answers a sorted
  (byte order) vector of strings rendered as-given. Opts:
  {:match-dot true} reveals dotfiles, {:follow-links true}
  follows wildcard-found symlinked directories, {:recursive
  false} caps ** to one level, {:max-depth n} bounds the walk
  (1..4096, default 128). An absolute pattern walks from / and
  ignores root. Requires the fs capability."
  ([pattern] (clojure.core/glob pattern))
  ([pattern root] (clojure.core/glob pattern root))
  ([pattern root opts] (clojure.core/glob pattern root opts)))

(defn match
  "Pure glob matcher: does one path match one pattern? Same syntax
  as glob; dotfile visibility is the walker's policy, not the
  matcher's. Patterns over 256 bytes throw :eval/bounds."
  [pattern s]
  (path-glob-match pattern s))
