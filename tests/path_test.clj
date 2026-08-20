(require "tests/test")
(require '[clojure.test.check :as tc])
(require '[clojure.test.check.properties :as prop])
(require '[clojure.test.check.generators :as gen])

;; Path string algebra prims (ADR 22). Golden vectors come from the
;; path-lib research PoCs (node, python3, elixir, bb verified live);
;; where ecosystems disagree the ADR records the majority rule and
;; the tests pin mino's chosen edge. Properties: normalize is
;; idempotent, join/split round-trip modulo normalization, split-ext
;; rebuilds its input, and every prim is total over garbage strings.

(def windows? (some? (getenv "OS")))

(defn pq [p seed n]
  (:result (tc/quick-check n p :seed seed)))

(def seg-alphabet ["a" "b" "c.d" "e.txt" "." ".." "/" "\\" "//" "" "x y" "z\\w"])

;; Garbage alphabet excludes the backslash: prims fold \ to / (the
;; Windows v1 acceptance rule), so string-level laws like the
;; split-ext rebuild hold on canonical paths. The fold itself is
;; pinned by golden vectors.
(def garbage-gen
  (gen/fmap (fn [cs] (apply str cs))
            (gen/vector (gen/elements
                          (mapv char (filter #(not= % 92)
                                             (range 33 127))))
                        0 24)))

;;; join

(deftest join-goldens
  (are [expected parts] (= expected (apply path-join parts))
    "a/b/c"        ["a" "b" "c"]
    "/foo/bar"     ["/foo" "/bar"]       ; absolute segment does not reset
    "/foo/bar/baz" ["/foo" "bar" "/baz"]
    "a/b"          ["a/" "/b"]           ; separators fold
    "a/b"          ["a" "" "b"]          ; empty skipped
    "a/b"          [nil "a" nil "b" nil] ; nil skipped
    "a"            ["a"]
    "/"            ["/"]
    "."            []
    "."            ["" ""]
    "."            [nil nil]
    "a/c"          ["a/b/../c"]          ; joined result normalizes
    "/foo"         ["/foo" nil]
    "a/b/c"        ["a\\" "b" "c"]   ; backslashes fold to /
    "a"            ["a\\"]))

(deftest join-rejects-non-strings
  (is (thrown? (path-join "a" 42)))
  (is (thrown? (path-join :foo)))
  (is (thrown-with-msg? #"path-join" (path-join "a" []))))

;;; split (raw segments: no dot resolution, empty segments dropped)

(deftest split-goldens
  (are [expected s] (= expected (path-split s))
    ["/" "a" "b"]  "/a/b"
    ["a" "b"]      "a/b"
    []             ""
    ["/"]          "/"
    ["a" "b"]      "a//b/"
    ["a" "b"]      "a\\b"
    ["a"]          "a"
    ["."]          "."
    ["a" ".." "b"] "a/../b"
    ["a" "." "b"]  "a/./b"
    ["a" "b"]      "a\\/b//"))

;;; basename (raw last segment: no .. resolution, node parity)

(deftest basename-goldens
  (are [expected s] (= expected (path-basename s))
    "c.txt"  "/a/b/c.txt"
    ""       "/"
    "."      ""
    "c.txt"  "c.txt"
    "b"      "a/b/"
    "c"      "a\\b\\c"
    "."      "."
    ".."     ".."
    ".."     "/.."
    "b"      "a//b"
    ""       "///"
    "b.c"    "a.b/b.c"))

;;; dirname (cleaned prefix)

(deftest dirname-goldens
  (are [expected s] (= expected (path-dirname s))
    "/a/b"   "/a/b/c"
    "."      "c"
    "/"      "/"
    "."      ""
    "/"      "/a"
    "a"      "a/b"
    "a/b"    "a/b/c.txt"
    "a"      "a/b/"
    "a"      "a//b"
    "/a"     "/a/b/"
    "a/b"    "a\\b\\c.txt"
    "/"      "/a.txt"))

;;; extension (with dot, last dot only, dotfile => "")

(deftest extension-goldens
  (are [expected s] (= expected (path-extension s))
    ".txt"    "a/b.txt"
    ""        ".bashrc"
    ".gz"     "a.tar.gz"
    "."       "index."
    ""        "a.b/c"     ; a dot in the directory part does not count
    ""        "a/.bashrc"
    ""        ""
    ""        "/"
    ""        "a/b/c"
    ".txt"    "a\\b.txt"
    ""        ".."
    "."       "a.."
    ".tar"    "a.tar.gz.tar" ; last dot only, twice over
    ""        ".hidden.d/.")) ; basename "." of dir ".hidden.d"

;;; split-ext (os.path shape: full-path stem, ext or nil)

(deftest split-ext-goldens
  (are [expected s] (= expected (path-split-ext s))
    ["a.tar" ".gz"]   "a.tar.gz"
    [".bashrc" nil]   ".bashrc"
    ["README" nil]    "README"
    ["index" "."]     "index."
    ["/a/b/c" ".txt"] "/a/b/c.txt"
    ["/a/b/c" nil]    "/a/b/c"
    ["a.b/c" nil]     "a.b/c"
    ["" nil]          ""
    ["a" ".b"]        "a.b"))

;;; stem (pathlib shape: basename minus last ext)

(deftest stem-goldens
  (are [expected s] (= expected (path-stem s))
    "c.tar"  "/a/b/c.tar.gz"
    ".bashrc" ".bashrc"
    "c"       "a/b/c"
    "index"   "index."
    "c"       "a/b/c.txt"
    "f"       "d.e/f"))

;;; normalize (lexical clean, trailing slash strips, backslash folds)

(deftest normalize-goldens
  (are [expected s] (= expected (path-normalize s))
    "."      ""
    "."      "."
    "a/c"    "a/./b/../c//"
    "/foo/bar" "/foo//bar"
    "/bar"   "/foo/../bar"
    "/a"     "/../a"       ; .. after root drops
    "../a"   "../a"        ; leading .. preserved
    "."      "a/.."
    ".."     "a/../.."
    "/"      "/.."
    "/"      "/"
    "a/b/c"  "a\\b\\c"
    "foo"    "foo/"
    "/"      "///"
    "../../b" "../../b"
    "/a"     "\\a"
    "a"      "a\\."))

(deftest normalize-many-segments
  ;; review round: 33 one-char segments (65 bytes) once overflowed
  ;; path_clean's stack scratch; pinned so it can never regress
  (let [deep (apply str (interpose "/" (repeat 33 "a")))]
    (is (= deep (path-normalize deep))))
  (let [deep (apply str (interpose "/" (repeat 100 "ab")))]
    (is (= deep (path-normalize deep)))))

(deftest normalize-is-idempotent-prop
  (is (pq (prop/for-all [s garbage-gen]
                         (= (path-normalize (path-normalize s))
                            (path-normalize s)))
          20260820 400)))

;;; absolute?

(deftest absolute?-goldens
  (are [expected s] (= expected (path-absolute? s))
    true  "/a"
    true  "/"
    false "a/b"
    false ""
    false "~/x"
    true  "\\a"
    false "a\\b"))

;;; expand-home

(deftest expand-home-no-op-and-passthrough
  (are [s] (= s (path-expand-home s))
    "foo/bar"
    "/abs/path"
    "a~b/c"
    "~foo/bar"          ; ~otheruser passes through (documented)
    ""
    "~~"))

(deftest expand-home-expands-tilde
  (when-not windows?
    (let [home (getenv "HOME")]
      (is (= home (path-expand-home "~")))
      (is (= (str home "/x") (path-expand-home "~/x")))
      (is (= (str home "/x/y") (path-expand-home "~/x//y"))))))

;;; errors

(deftest type-and-arity-errors
  (is (thrown? (path-basename 42)))
  (is (thrown? (path-dirname nil)))
  (is (thrown? (path-extension :kw)))
  (is (thrown? (path-split-ext [])))
  (is (thrown? (path-stem 1.5)))
  (is (thrown? (path-normalize :x)))
  (is (thrown? (path-absolute? 0)))
  (is (thrown? (path-split 'a)))
  (is (thrown-with-msg? #"path-basename" (path-basename 42))))

;;; properties

;; For paths built only from simple names, join of split rebuilds
;; the path and split of the joined path is the names back.
(deftest join-split-recompose-prop
  (is (pq (prop/for-all [segs (gen/vector (gen/elements ["a" "b" "c.d"]) 1 6)]
                         (let [p (apply path-join segs)]
                           (and (= segs (path-split p))
                                (= p (apply path-join (path-split p))))))
          20260821 400)))

(deftest split-ext-rebuilds-prop
  (is (pq (prop/for-all [s garbage-gen]
                         (let [[st ext] (path-split-ext s)]
                           (= s (if ext (str st ext) st))))
          20260822 500)))

(deftest extension-stem-agreement-prop
  (is (pq (prop/for-all [s garbage-gen]
                         (let [[_ ext] (path-split-ext s)]
                           (= (path-extension s) (or ext ""))))
          20260823 500))
  (is (pq (prop/for-all [s garbage-gen]
                         (= (path-stem s)
                            (first (path-split-ext (path-basename s)))))
          20260824 500)))

(deftest basename-dirname-chain-prop
  (is (pq (prop/for-all [s garbage-gen]
                         (let [d (path-dirname s)]
                           (or (= d ".") (= d "/")
                               (not= "" (path-basename d)))))
          20260825 500)))

(deftest pure-prims-total-over-garbage-prop
  ;; ADR 22: the algebra is total over any byte string. Garbage in,
  ;; string or boolean or vector out, never a throw.
  (is (pq (prop/for-all [s garbage-gen]
                         (and (string? (path-normalize s))
                              (string? (path-basename s))
                              (string? (path-dirname s))
                              (string? (path-extension s))
                              (string? (path-stem s))
                              (string? (path-expand-home s))
                              (vector? (path-split s))
                              (vector? (path-split-ext s))
                              (or (true? (path-absolute? s))
                                  (false? (path-absolute? s)))))
          20260826 600)))

(run-tests-and-exit)
