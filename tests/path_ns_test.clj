(require "tests/test")

;; mino.path: the namespace seam over the path prims (ADR 22).
;; The docstring is a contract: its examples run as tests here, the
;; babashka mapping table is verified against the real fns, and the
;; glob sugar passes opts through with identical error kinds.

(def windows? (some? (getenv "OS")))

(require '[mino.path :as path])

(deftest ns-surface-examples
  ;; the docstring examples, verbatim
  (is (= "src/demo/core.clj" (path/join "src" "demo" "core.clj")))
  (is (= ["src" "demo" "core.clj"] (path/split "src/demo/core.clj")))
  (is (= "core.clj" (path/basename "src/demo/core.clj")))
  (is (= "src/demo" (path/dirname "src/demo/core.clj")))
  (is (= ".clj" (path/extension "src/demo/core.clj")))
  (is (= ["src/demo/core" ".clj"] (path/split-ext "src/demo/core.clj")))
  (is (= "core" (path/stem "src/demo/core.clj")))
  (is (= "a/c" (path/normalize "a/./b/../c/")))
  (is (true? (path/absolute? "/etc/passwd")))
  (is (false? (path/absolute? "etc/passwd")))
  (is (true? (path/match "*.clj" "core.clj")))
  (is (false? (path/match "*.clj" "core.txt"))))

(deftest ns-babashka-mapping-table
  ;; the docstring table, verified: one canonical name per operation
  (let [s "a/b/c.tar.gz"]
    (is (= (path/basename s) (path-basename s)))      ; fs/file-name
    (is (= (path/dirname s) (path-dirname s)))        ; fs/parent
    (is (= (path/split s) (path-split s)))            ; fs/components
    (is (= (path/stem s)
           (first (path-split-ext (path-basename s)))))
    (is (= (path/extension s) (path-extension s)))
    (is (= (path/split-ext s) (path-split-ext s)))
    (is (= (path/normalize s) (path-normalize s)))
    (is (= (path/absolute? s) (path-absolute? s)))))

(deftest ns-join-and-home
  (is (= "a/b" (path/join "a" nil "b")))
  (is (= "." (path/join)))
  (when-not windows?
    (is (= (str (getenv "HOME") "/x") (path/expand-home "~/x"))))
  (is (= "foo/bar" (path/expand-home "foo/bar"))))

(deftest ns-glob-sugar
  (rm-rf "/tmp/mino-path-ns-test")
  (mkdir-p "/tmp/mino-path-ns-test/sub")
  (spit "/tmp/mino-path-ns-test/a.clj" "a")
  (spit "/tmp/mino-path-ns-test/.hidden" "h")
  (spit "/tmp/mino-path-ns-test/sub/b.clj" "b")
  (is (= ["/tmp/mino-path-ns-test/a.clj"
          "/tmp/mino-path-ns-test/sub/b.clj"]
         (path/glob "**/*.clj" "/tmp/mino-path-ns-test")))
  (is (= ["/tmp/mino-path-ns-test/.hidden"
          "/tmp/mino-path-ns-test/a.clj"
          "/tmp/mino-path-ns-test/sub"]
         (path/glob "*" "/tmp/mino-path-ns-test" {:match-dot true})))
  (rm-rf "/tmp/mino-path-ns-test"))

(deftest ns-errors-surface-identically
  ;; the ns fns throw exactly what the prims throw
  (is (= :eval/type
         (try (path/basename 42) (catch e (:mino/kind e)))))
  (is (= :eval/arity
         (try (path/dirname) (catch e (:mino/kind e)))))
  (is (= :eval/type
         (try (path/join "a" :kw) (catch e (:mino/kind e)))))
  (is (= :eval/bounds
         (try (path/match (apply str (repeat 257 "a")) "x")
              (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (path/glob "*" "." {:match-dot 1})
              (catch e (:mino/kind e))))))

(deftest ns-relativize
  ;; the docstring examples, verbatim
  (is (= "c/d" (path/relativize "/a/b" "/a/b/c/d")))
  (is (= ".." (path/relativize "/a/b/c" "/a/b")))
  (is (= "../c" (path/relativize "/a/b" "/a/c")))
  (is (= "" (path/relativize "/a/b" "/a/b")))
  ;; identical relative paths answer "" too
  (is (= "" (path/relativize "a/b" "a/b")))
  ;; nested one level
  (is (= "c" (path/relativize "/a/b" "/a/b/c")))
  ;; target two levels above base
  (is (= "../.." (path/relativize "/a/b/c" "/a")))
  ;; sibling divergence deeper on both sides
  (is (= "../../x/y" (path/relativize "/a/b/c" "/a/x/y")))
  ;; relative base and target
  (is (= "d/e" (path/relativize "a/b/c" "a/b/c/d/e")))
  (is (= "../../p" (path/relativize "a/b/c" "a/p")))
  ;; the root as base
  (is (= "a/b" (path/relativize "/" "/a/b")))
  ;; normalizes both sides before diffing
  (is (= "c/d" (path/relativize "/a/./b" "/a/b/x/../c/d"))))

(deftest ns-relativize-errors
  ;; mixing absolute and relative cannot be relativized: classified throw
  (is (= :path/relativize
         (try (path/relativize "/a/b" "a/b") (catch e (:mino/kind e)))))
  (is (= :path/relativize
         (try (path/relativize "a/b" "/a/b") (catch e (:mino/kind e)))))
  ;; the diagnostic carries the offending paths
  (is (= {:base "/a" :target "x"}
         (try (path/relativize "/a" "x") (catch e (:mino/data e))))))

(run-tests-and-exit)
