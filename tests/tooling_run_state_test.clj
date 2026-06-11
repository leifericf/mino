(require "tests/test")
(require "tools/run_state")

;; Run/round state for orchestration runs: init -> advance* -> done,
;; crash-resumable because every transition is written to state.edn.

(def run-dir "/tmp/mino-tooling-run-state-test")

(defn- fresh! []
  (rm-rf run-dir)
  (tools.run-state/init! run-dir {:kind "audit" :scope "src/gc" :branch "audit/gc"}))

(deftest init-creates-state-and-layout
  (rm-rf run-dir)
  (let [st (tools.run-state/init! run-dir {:kind "audit" :scope "src/gc" :branch "audit/gc"})]
    (is (= :audit (:kind st)))
    (is (= "src/gc" (:scope st)))
    (is (= "audit/gc" (:branch st)))
    (is (= 0 (:round st)))
    (is (= :running (:status st)))
    (is (= [] (:rounds st)))
    (is (file-exists? (str run-dir "/state.edn")))
    (is (directory? (str run-dir "/findings")))
    (is (directory? (str run-dir "/proposals")))))

(deftest init-requires-kind
  (rm-rf run-dir)
  (is (thrown? Exception (tools.run-state/init! run-dir {:scope "src/gc"}))))

(deftest init-refuses-overwrite
  (fresh!)
  (is (thrown? Exception
        (tools.run-state/init! run-dir {:kind "audit"}))))

(deftest init-force-overwrites
  (fresh!)
  (let [st (tools.run-state/init! run-dir {:kind "fix" :force true})]
    (is (= :fix (:kind st)))
    (is (= 0 (:round st)))))

(deftest advance-found-new-keeps-running
  (fresh!)
  (let [st (tools.run-state/advance! run-dir {:found-new true})]
    (is (= 1 (:round st)))
    (is (= :running (:status st)))
    (is (= [{:round 0 :found-new true}] (:rounds st)))))

(deftest advance-nothing-new-finishes
  (fresh!)
  (tools.run-state/advance! run-dir {:found-new true})
  (let [st (tools.run-state/advance! run-dir {:found-new false})]
    (is (= 2 (:round st)))
    (is (= :done (:status st)))
    (is (= [{:round 0 :found-new true}
            {:round 1 :found-new false}]
           (:rounds st)))))

(deftest advance-after-done-throws
  (fresh!)
  (tools.run-state/advance! run-dir {:found-new false})
  (is (thrown? Exception (tools.run-state/advance! run-dir {:found-new true}))))

(deftest status-resumes-from-disk
  ;; Everything lives in state.edn: a fresh reader (a resumed session)
  ;; sees exactly what the crashed one wrote.
  (fresh!)
  (tools.run-state/advance! run-dir {:found-new true})
  (let [st (tools.run-state/status run-dir)]
    (is (= 1 (:round st)))
    (is (= :running (:status st)))
    (is (= :audit (:kind st)))))

(deftest status-without-init-throws
  (rm-rf run-dir)
  (is (thrown? Exception (tools.run-state/status run-dir))))

(run-tests-and-exit)
