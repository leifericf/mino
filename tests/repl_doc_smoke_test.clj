(require "tests/test")

;; Regression for the standalone REPL ergonomics contract: (doc map)
;; must work on the first prompt with no manual require, matching clj,
;; whose clojure.main/repl refers clojure.repl into the user ns at the
;; repl :init. Root cause lived in main.c run_repl (no such refer), so
;; this spawns the real standalone binary in REPL mode rather than
;; asserting against the in-process harness, which never enters
;; run_repl.
;;
;; sh returns combined stdout+stderr as :out (proc.c appends 2>&1), so
;; one :out assertion sees both the docstring (stdout) and any
;; unbound-symbol error (stderr). POSIX-only, like proc_test's
;; run-reports-failed-chdir: mino's stdin redirect is POSIX.

(deftest repl-doc-available-without-require
  (when-not (some? (getenv "OS"))
    (spit "/tmp/mino_repl_doc_probe.txt" "(doc map)\n")
    (let [result (sh "sh" "-c"
                     "./mino < /tmp/mino_repl_doc_probe.txt")
          out    (:out result)]
      (is (= 0 (:exit result)))
      (is (re-find #"lazy sequence" out))
      (is (not (re-find #"MNS001" out))))))

(run-tests-and-exit)
