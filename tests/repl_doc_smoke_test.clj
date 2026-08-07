(require "tests/test")

;; Regression for the standalone REPL ergonomics contract: the helpers
;; clj's clojure.main/repl refers into the user ns at the repl :init
;; must work on the first prompt with no manual require. That is the
;; clojure.repl surface (doc, apropos) AND clojure.pprint (pprint),
;; matching clj. Root cause lived in main.c run_repl (no such refer),
;; so this spawns the real standalone binary in REPL mode rather than
;; asserting against the in-process harness, which never enters
;; run_repl.
;;
;; sh returns combined stdout+stderr as :out (proc.c appends 2>&1), so
;; one :out assertion sees both the printed output (stdout) and any
;; unbound-symbol error (stderr). POSIX-only, like proc_test's
;; run-reports-failed-chdir: mino's stdin redirect is POSIX.

(deftest repl-default-helpers-available-without-require
  (when-not (some? (getenv "OS"))
    (spit "/tmp/mino_repl_doc_probe.txt"
      "(doc map)\n(apropos \"cons\")\n(pprint {:x 1})\n")
    (let [result (sh "sh" "-c"
                     "./mino < /tmp/mino_repl_doc_probe.txt")
          out    (:out result)]
      (is (= 0 (:exit result)))
      (is (re-find #"lazy sequence" out))
      ;; apropos is a referable clojure.repl var: available unqualified,
      ;; returns namespace-qualified symbols across all loaded namespaces.
      (is (re-find #"clojure\.core/cons" out))
      ;; pprint is referred from clojure.pprint, as in clj's repl :init.
      (is (re-find #":x" out))
      (is (not (re-find #"MNS001" out))))))

(run-tests-and-exit)
