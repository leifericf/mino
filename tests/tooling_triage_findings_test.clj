(require "tests/test")
(require "tools/triage_findings")

;; Triage folds reviewer findings EDN into one deterministic punch
;; list: ordered by fix level (correctness -> factoring -> style),
;; then severity (high -> medium -> low), then file/line; exact
;; duplicates from overlapping reviewers are dropped.

(def run-dir "/tmp/mino-tooling-triage-test")

(defn- finding [m]
  (merge {:id        "style-gc-001"
          :dimension :style
          :module    "src/gc"
          :file      "src/gc/driver.c"
          :line      10
          :severity  :low
          :level     :style
          :title     "example finding"
          :detail    "longer explanation"}
         m))

(defn- write-findings! [fname findings]
  (mkdir-p (str run-dir "/findings"))
  (spit (str run-dir "/findings/" fname) (pr-str findings)))

(defn- fresh! []
  (rm-rf run-dir)
  (mkdir-p (str run-dir "/findings")))

(deftest empty-run-triages-to-empty-punch-list
  (fresh!)
  (let [r (tools.triage-findings/triage! run-dir)]
    (is (= [] (:items r)))
    (is (= 0 (get-in r [:counts :total])))
    (is (file-exists? (str run-dir "/punch-list.edn")))))

(deftest orders-by-level-then-severity-then-location
  (fresh!)
  (write-findings! "style.edn"
    [(finding {:id "s1" :level :style :severity :high :line 5})])
  (write-findings! "security.edn"
    [(finding {:id "c2" :dimension :security :level :correctness :severity :medium :line 99 :title "med bug"})
     (finding {:id "c1" :dimension :security :level :correctness :severity :high :line 50 :title "bad bug"})])
  (write-findings! "factoring.edn"
    [(finding {:id "f1" :dimension :factoring :level :factoring :severity :low :line 1 :title "split fn"})])
  (let [ids (mapv :id (:items (tools.triage-findings/triage! run-dir)))]
    (is (= ["c1" "c2" "f1" "s1"] ids))))

(deftest drops-exact-duplicates-across-files
  (fresh!)
  (write-findings! "a.edn" [(finding {:id "a1" :title "dup" :line 7})])
  (write-findings! "b.edn" [(finding {:id "b1" :title "dup" :line 7})
                            (finding {:id "b2" :title "unique" :line 8})])
  (let [r (tools.triage-findings/triage! run-dir)]
    (is (= 2 (count (:items r))))
    (is (= 1 (:duplicates-dropped r)))))

(deftest counts-by-level-and-severity
  (fresh!)
  (write-findings! "x.edn"
    [(finding {:id "1" :level :correctness :severity :high :line 1 :title "t1"})
     (finding {:id "2" :level :correctness :severity :low :line 2 :title "t2"})
     (finding {:id "3" :level :style :severity :low :line 3 :title "t3"})])
  (let [c (:counts (tools.triage-findings/triage! run-dir))]
    (is (= 3 (:total c)))
    (is (= 2 (get-in c [:by-level :correctness])))
    (is (= 1 (get-in c [:by-level :style])))
    (is (= 1 (get-in c [:by-severity :high])))
    (is (= 2 (get-in c [:by-severity :low])))))

(deftest punch-list-is-readable-from-disk
  (fresh!)
  (write-findings! "x.edn" [(finding {})])
  (tools.triage-findings/triage! run-dir)
  (let [r (read-string (slurp (str run-dir "/punch-list.edn")))]
    (is (= 1 (count (:items r))))
    (is (= "style-gc-001" (:id (first (:items r)))))))

(deftest invalid-finding-throws-with-source-file
  (fresh!)
  (write-findings! "bad.edn" [(dissoc (finding {}) :severity)])
  (is (thrown? Exception (tools.triage-findings/triage! run-dir))))

(deftest invalid-enum-value-throws
  (fresh!)
  (write-findings! "bad.edn" [(finding {:severity :catastrophic})])
  (is (thrown? Exception (tools.triage-findings/triage! run-dir))))

(deftest validate-finding-accepts-canonical-shape
  ;; The canonical schema is pinned here; agent bodies state the same
  ;; shape and the Phase-4 consistency test cross-checks them.
  (is (nil? (tools.triage-findings/validate-finding (finding {}) "test")))
  (is (nil? (tools.triage-findings/validate-finding
              (finding {:suggestion "optional extra"}) "test"))))

(run-tests-and-exit)
