(require "tests/test")

;; glob: the one walker (ADR 22). Fixtures are built with mino's own
;; mkdir-p / spit under /tmp and torn down with rm-rf (the fs_test
;; precedent; no external tools per the repo rule). The contract
;; under test: sorted byte-order vector of strings rendered
;; as-given, dotfiles hidden unless the segment starts with a dot
;; or :match-dot, symlinks not followed unless :follow-links, **
;; whole-segment zero-or-more directories, depth-bounded.

(def windows? (some? (getenv "OS")))

(def root "/tmp/mino-glob-test")

(defn build-tree []
  (rm-rf root)
  (mkdir-p (str root "/sub/deeper"))
  (mkdir-p (str root "/onedir/sub2"))
  (spit (str root "/a.clj") "a")
  (spit (str root "/b.txt") "b")
  (spit (str root "/.hidden.txt") "h")
  (spit (str root "/sub/c.clj") "c")
  (spit (str root "/sub/d.md") "d")
  (spit (str root "/sub/.h2.txt") "h2")
  (spit (str root "/sub/deeper/e.clj") "e")
  (spit (str root "/onedir/x1.txt") "x1")
  (spit (str root "/onedir/sub2/x2.txt") "x2"))

(defn teardown-tree []
  (rm-rf root))

;;; basics: star, sorting, as-given rendering

(deftest star-root-and-sortedness
  (build-tree)
  (is (= [(str root "/a.clj") (str root "/b.txt") (str root "/onedir")
          (str root "/sub")]
         (glob "*" root)))
  (is (= [(str root "/a.clj")]
         (glob "*.clj" root)))
  (is (= [(str root "/b.txt")]
         (glob "b.txt" root)))
  (teardown-tree))

(deftest byte-order-sorting
  ;; creation order is z, a, B; byte order sorts uppercase first
  (rm-rf root)
  (mkdir-p root)
  (spit (str root "/zebra") "1")
  (spit (str root "/apple") "2")
  (spit (str root "/Banana") "3")
  (is (= [(str root "/Banana") (str root "/apple") (str root "/zebra")]
         (glob "*" root)))
  (rm-rf root))

(deftest default-root-is-cwd-unprefixed
  ;; no root argument: walks "." and renders results unprefixed.
  ;; (No try/finally here: the restored cwd matters more than the
  ;; exception path, and a bare do keeps the BC fn body simple.)
  (build-tree)
  (let [cwd (getcwd)
        results (do (chdir root) (glob "*.clj"))]
    (chdir cwd)
    (is (= ["a.clj"] results)))
  (teardown-tree))

;;; ** semantics

(deftest doublestar-recursive
  (build-tree)
  (is (= [(str root "/a.clj") (str root "/sub/c.clj")
          (str root "/sub/deeper/e.clj")]
         (glob "**/*.clj" root)))
  (teardown-tree))

(deftest doublestar-zero-dir-case
  ;; a/**/b matches a/b: the zero-directory case pinned before the
  ;; walker landed (ADR 22 risk register)
  (build-tree)
  (is (= [(str root "/sub/c.clj")]
         (glob "sub/**/c.clj" root)))
  (is (= [(str root "/sub/deeper/e.clj")]
         (glob "**/deeper/e.clj" root)))
  (teardown-tree))

(deftest trailing-doublestar-emits-every-descendant
  (build-tree)
  (is (= [(str root "/sub/c.clj") (str root "/sub/d.md")
          (str root "/sub/deeper") (str root "/sub/deeper/e.clj")]
         (glob "sub/**" root)))
  (teardown-tree))

(deftest doublestar-non-recursive-opt
  (build-tree)
  ;; {:recursive false}: ** behaves as a single *
  (is (= [(str root "/sub/c.clj")]
         (glob "**/*.clj" root {:recursive false})))
  (teardown-tree))

(deftest doublestar-max-depth-opt
  (build-tree)
  (is (= [(str root "/a.clj") (str root "/sub/c.clj")]
         (glob "**/*.clj" root {:max-depth 1})))
  (teardown-tree))

(deftest doublestar-dedupe
  ;; ambiguous ** assignments must not duplicate a path
  (build-tree)
  (let [results (glob "**/**" root)]
    (is (= results (distinct results)))
    (is (= (count results) (count (distinct results)))))
  (teardown-tree))

;;; dotfile policy

(deftest dotfiles-hidden-by-default
  (build-tree)
  (is (= [(str root "/a.clj") (str root "/b.txt") (str root "/onedir")
          (str root "/sub")]
         (glob "*" root)))
  (teardown-tree))

(deftest dotfiles-visible-via-literal-dot-segment
  (build-tree)
  (is (= [(str root "/.hidden.txt")]
         (glob ".hidden.txt" root)))
  (is (= [(str root "/sub/.h2.txt")]
         (glob ".h*" (str root "/sub"))))
  (teardown-tree))

(deftest dotfiles-visible-via-match-dot
  (build-tree)
  (is (= [(str root "/.hidden.txt") (str root "/a.clj")
          (str root "/b.txt") (str root "/onedir") (str root "/sub")]
         (glob "*" root {:match-dot true})))
  ;; match-dot combined with a class pattern
  (is (= [(str root "/.hidden.txt")]
         (glob "[.]*" root {:match-dot true})))
  (teardown-tree))

;;; absolute patterns

(deftest absolute-pattern-answers-absolute-results
  (build-tree)
  (let [results (glob (str root "/sub/*.clj") "/some/ignored/root")]
    (is (= [(str root "/sub/c.clj")] results)))
  (teardown-tree))

;;; symlink policy (POSIX-only: ln -s has no portable equivalent)

(deftest symlinks-not-followed-by-default
  (when-not windows?
    (build-tree)
    (sh! "ln" "-s" (str root "/sub") (str root "/link"))
    (is (= [(str root "/a.clj") (str root "/sub/c.clj")
            (str root "/sub/deeper/e.clj")]
           (glob "**/*.clj" root))
        "the symlinked directory must not be descended")
    (teardown-tree)))

(deftest symlinks-followed-with-opt
  (when-not windows?
    (build-tree)
    (sh! "ln" "-s" (str root "/sub") (str root "/link"))
  (is (= [(str root "/a.clj") (str root "/link/c.clj")
          (str root "/link/deeper/e.clj") (str root "/sub/c.clj")
          (str root "/sub/deeper/e.clj")]
         (glob "**/*.clj" root {:follow-links true})))
    (teardown-tree)))

(deftest symlink-loop-terminates-via-depth-cap
  (when-not windows?
    (rm-rf root)
    (mkdir-p root)
    (spit (str root "/x.clj") "x")
    (sh! "ln" "-s" root (str root "/loop"))
    ;; a self-referential loop under follow-links must terminate
    (is (vector? (glob "**/*.clj" root {:follow-links true
                                        :max-depth 8})))
    (rm-rf root)))

;;; missing roots and unreadable directories

(deftest missing-root-answers-empty
  (is (= [] (glob "*" "/nonexistent-xyz-mino-123")))
  (is (= [] (glob "**/*.clj" "/nonexistent-xyz-mino-123")))
  ;; only-separator pattern: nothing to match, pinned deliberately
  ;; (Python and Elixir answer ["/"] here; mino answers [])
  (is (= [] (glob "/"))))

;;; errors

(deftest glob-errors
  (is (thrown? (glob 42)))
  (is (thrown? (glob "")))
  (is (thrown? (glob)))
  (is (thrown? (glob "*" :kw)))
  (is (thrown? (glob "*" "." :kw)))
  (is (thrown? (glob "*" "." {:match-dot "yes"})))
  (is (thrown? (glob "*" "." {:max-depth 0})))
  (is (thrown? (glob "*" "." {:max-depth 4097}))
      "the walk recurses one C frame per level: capped at 4096")
  (is (= :eval/bounds
         (try (glob (apply str (repeat 257 "a")))
              (catch e (:mino/kind e))))
      "patterns over 256 bytes throw :eval/bounds")
  (is (thrown-with-msg? #"glob" (glob "*" "." {:match-dot 1}))))

(run-tests-and-exit)
