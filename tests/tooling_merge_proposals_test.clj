(require "tests/test")
(require "tools/merge_proposals")
(require '[clojure.string :as str])

;; Changelog-via-proposal: editor agents never touch CHANGELOG.md;
;; their mp-changelog lines travel as proposal EDN and are appended here,
;; deterministically and exactly once, under "## Unreleased".

(def mp-dir   "/tmp/mino-tooling-merge-proposals-test")
(def mp-changelog (str mp-dir "/CHANGELOG.md"))

(def base-mp-changelog
  "# Changelog\n\n## v0.1.0 — Old Release\n\n- old entry\n")

(defn- mp-fresh! []
  (rm-rf mp-dir)
  (mkdir-p (str mp-dir "/proposals"))
  (spit mp-changelog base-mp-changelog))

(defn- mp-propose! [fname m]
  (spit (str mp-dir "/proposals/" fname) (pr-str m)))

(defn- mp-merge! []
  (tools.merge-proposals/merge! mp-dir {:changelog mp-changelog}))

(deftest creates-unreleased-section-and-appends
  (mp-fresh!)
  (mp-propose! "a.edn" {:branch "fix/gc-1" :changelog ["GC: Fix sweep ordering"]})
  (let [r (mp-merge!)
        text (slurp mp-changelog)]
    (is (= ["GC: Fix sweep ordering"] (:appended r)))
    (is (= [] (:skipped r)))
    (is (str/includes? text "## Unreleased"))
    (is (str/includes? text "- GC: Fix sweep ordering"))
    ;; Unreleased sits above the old release, old content intact.
    (is (< (str/index-of text "## Unreleased")
           (str/index-of text "## v0.1.0")))
    (is (str/includes? text "- old entry"))))

(deftest merges-in-filename-order-and-dedupes-batch
  (mp-fresh!)
  (mp-propose! "b.edn" {:branch "fix/2" :changelog ["Second line" "Shared line"]})
  (mp-propose! "a.edn" {:branch "fix/1" :changelog ["First line" "Shared line"]})
  (let [r (mp-merge!)]
    (is (= ["First line" "Shared line" "Second line"] (:appended r)))
    (is (= ["Shared line"] (:skipped r)))))

(deftest remerge-is-idempotent
  (mp-fresh!)
  (mp-propose! "a.edn" {:branch "fix/1" :changelog ["Only once"]})
  (mp-merge!)
  (let [before (slurp mp-changelog)
        r      (mp-merge!)]
    (is (= [] (:appended r)))
    (is (= ["Only once"] (:skipped r)))
    (is (= before (slurp mp-changelog)))))

(deftest skips-lines-already-in-mp-changelog
  (mp-fresh!)
  (mp-propose! "a.edn" {:branch "fix/1" :changelog ["old entry"]})
  (let [r (mp-merge!)]
    (is (= [] (:appended r)))
    (is (= ["old entry"] (:skipped r)))))

(deftest appends-into-existing-unreleased-section
  (mp-fresh!)
  (mp-propose! "a.edn" {:branch "fix/1" :changelog ["Line one"]})
  (mp-merge!)
  (mp-propose! "b.edn" {:branch "fix/2" :changelog ["Line two"]})
  (let [_ (mp-merge!)
        text (slurp mp-changelog)]
    (is (= 2 (count (str/split text #"## Unreleased")))
        "exactly one Unreleased heading")
    (is (str/includes? text "- Line one"))
    (is (str/includes? text "- Line two"))))

(deftest dry-run-reports-without-writing
  (mp-fresh!)
  (mp-propose! "a.edn" {:branch "fix/1" :changelog ["Would append"]})
  (let [r (tools.merge-proposals/merge! mp-dir {:changelog mp-changelog :dry-run true})]
    (is (= ["Would append"] (:appended r)))
    (is (= base-mp-changelog (slurp mp-changelog)))))

(deftest invalid-proposal-throws
  (mp-fresh!)
  (mp-propose! "bad.edn" {:changelog ["no branch key"]})
  (is (thrown? Exception (mp-merge!)))
  (mp-fresh!)
  (mp-propose! "bad2.edn" {:branch "fix/1" :changelog "not a vector"})
  (is (thrown? Exception (mp-merge!))))

(deftest no-proposals-is-a-clean-noop
  (mp-fresh!)
  (let [r (mp-merge!)]
    (is (= [] (:appended r)))
    (is (= base-mp-changelog (slurp mp-changelog)))))

(run-tests-and-exit)
