(require "tests/test")
(require "tools/integrate_fixes")
(require '[clojure.string :as str])

;; POSIX-only: the fixture builds a real git repo under a temp dir and
;; tears it down with rm-rf. On Windows git marks files in .git
;; read-only, which rm-rf cannot unlink without first clearing the
;; attribute, so the fixture's teardown errors. This exercises a dev
;; workflow tool (integrate-fixes) that runs on the maintainer's POSIX
;; hosts; the tests are skipped on Windows and keep full coverage on
;; POSIX.
(def ^:private windows? (some? (getenv "OS")))

;; Integration lands worktree fix branches on the feature branch in
;; the given (dependency) order. A branch that conflicts is never
;; guessed at: the merge is aborted, the conflict recorded in
;; escalations.edn, and the remaining branches still land.

(def if-run "/tmp/mino-tooling-integrate-run")
(def if-repo "/tmp/mino-tooling-integrate-repo")

(defn- if-git! [& args]
  (let [r (apply sh "git" "-C" if-repo args)]
    (when-not (= 0 (:exit r))
      (throw (ex-info (str "fixture git failed: " (pr-str args) " -> " (:err r))
                      {:args args :result r})))
    (str/trim (str (:out r)))))

(defn- if-fresh! []
  (rm-rf if-run)
  (mkdir-p if-run)
  (rm-rf if-repo)
  (mkdir-p if-repo)
  (if-git! "init" "-q" "-b" "main")
  (if-git! "config" "user.email" "test@example.invalid")
  (if-git! "config" "user.name" "Fixture")
  (spit (str if-repo "/a.txt") "alpha\n")
  (spit (str if-repo "/b.txt") "beta\n")
  (if-git! "add" ".")
  (if-git! "commit" "-q" "-m" "Init")
  (if-git! "checkout" "-q" "-b" "feature")
  ;; fix-a: edits a.txt
  (if-git! "checkout" "-q" "-b" "fix-a" "feature")
  (spit (str if-repo "/a.txt") "alpha fixed\n")
  (if-git! "commit" "-aqm" "Fix a")
  ;; fix-b: edits b.txt (disjoint)
  (if-git! "checkout" "-q" "-b" "fix-b" "feature")
  (spit (str if-repo "/b.txt") "beta fixed\n")
  (if-git! "commit" "-aqm" "Fix b")
  ;; fix-conflict: edits a.txt incompatibly
  (if-git! "checkout" "-q" "-b" "fix-conflict" "feature")
  (spit (str if-repo "/a.txt") "alpha conflicting\n")
  (if-git! "commit" "-aqm" "Fix a differently")
  (if-git! "checkout" "-q" "feature"))

(when-not windows?

(deftest disjoint-branches-land-in-order
  (if-fresh!)
  (let [r (tools.integrate-fixes/integrate!
            if-run {:repo if-repo :target "feature" :branches ["fix-a" "fix-b"]})]
    (is (= ["fix-a" "fix-b"] (:merged r)))
    (is (= [] (:escalated r)))
    (is (= "alpha fixed" (str/trim (slurp (str if-repo "/a.txt")))))
    (is (= "beta fixed" (str/trim (slurp (str if-repo "/b.txt")))))
    (is (not (file-exists? (str if-run "/escalations.edn"))))))

(deftest conflict-escalates-and-rest-still-lands
  (if-fresh!)
  (tools.integrate-fixes/integrate!
    if-run {:repo if-repo :target "feature" :branches ["fix-a"]})
  (let [r (tools.integrate-fixes/integrate!
            if-run {:repo if-repo :target "feature"
                    :branches ["fix-conflict" "fix-b"]})]
    (is (= ["fix-b"] (:merged r)))
    (is (= ["fix-conflict"] (mapv :branch (:escalated r))))
    ;; Escalation names the conflicted file and the run is on disk.
    (let [esc (read-string (slurp (str if-run "/escalations.edn")))]
      (is (= 1 (count esc)))
      (is (= "fix-conflict" (:branch (first esc))))
      (is (= "feature" (:target (first esc))))
      (is (= ["a.txt"] (:conflict-files (first esc)))))
    ;; The repo is left clean on the target, not mid-merge.
    (is (= "feature" (if-git! "rev-parse" "--abbrev-ref" "HEAD")))
    (is (= "" (if-git! "status" "--porcelain")))
    ;; The clean branch landed despite the earlier conflict.
    (is (= "beta fixed" (str/trim (slurp (str if-repo "/b.txt")))))))

(deftest escalations-accumulate-across-calls
  (if-fresh!)
  (tools.integrate-fixes/integrate!
    if-run {:repo if-repo :target "feature" :branches ["fix-a"]})
  (tools.integrate-fixes/integrate!
    if-run {:repo if-repo :target "feature" :branches ["fix-conflict"]})
  (tools.integrate-fixes/integrate!
    if-run {:repo if-repo :target "feature" :branches ["fix-conflict"]})
  (let [esc (read-string (slurp (str if-run "/escalations.edn")))]
    (is (= 2 (count esc)))))

(deftest unknown-branch-throws
  (if-fresh!)
  (is (thrown? Exception
        (tools.integrate-fixes/integrate!
          if-run {:repo if-repo :target "feature" :branches ["no-such-branch"]}))))

(deftest unknown-target-throws
  (if-fresh!)
  (is (thrown? Exception
        (tools.integrate-fixes/integrate!
          if-run {:repo if-repo :target "no-such-target" :branches ["fix-a"]}))))

) ;; end (when-not windows?)

(run-tests-and-exit)
