(require "tests/test")
(require '[clojure.string :as pstr])

;; Process execution primitives: sh, sh!.
;;
;; echo's exact stdout is shell-specific: cmd.exe on Windows emits a
;; trailing space before the newline (and double-quotes an argument
;; containing an apostrophe) where POSIX sh does not. These tests
;; exercise mino's capture / exit / throw mechanism, not the shell's
;; byte-exact formatting, so they strip the trailing-space difference
;; before comparing and assert substring content for the quoted case.

(defn- norm-eol
  "Strip trailing spaces before each newline so cmd.exe's trailing-space
   echo compares equal to POSIX echo. A no-op on POSIX output."
  [s]
  (pstr/replace s #" +\n" "\n"))

(deftest sh-echo
  (let [result (sh "echo" "hello")]
    (is (= 0 (:exit result)))
    (is (= "hello\n" (norm-eol (:out result))))))

(deftest sh-multiple-args
  (let [result (sh "echo" "a" "b" "c")]
    (is (= 0 (:exit result)))
    (is (= "a b c\n" (norm-eol (:out result))))))

(deftest sh-nonzero-exit
  (let [result (sh "false")]
    (is (not= 0 (:exit result)))))

(deftest sh!-returns-stdout
  (is (= "hello\n" (norm-eol (sh! "echo" "hello")))))

(deftest sh!-throws-on-failure
  (is (thrown? (sh! "false"))))

(deftest sh-special-chars
  ;; The apostrophe must survive argument passing + capture. cmd.exe
  ;; wraps the arg in double quotes where POSIX sh does not, so assert
  ;; the content is present rather than byte-exact.
  (let [result (sh "echo" "it's a test")]
    (is (= 0 (:exit result)))
    (is (pstr/includes? (norm-eol (:out result)) "it's a test"))))

(deftest sh-type-errors
  (is (thrown? (sh 42)))
  (is (thrown? (sh "echo" 42))))

(deftest run-reports-failed-chdir
  ;; run with :dir pointing at a nonexistent directory must fail the
  ;; child (exit 127) and put the chdir error on captured stderr, not
  ;; silently run the command in the inherited cwd and report success.
  ;; Qualified clojure.core/run: bare `run` collides with core.logic's
  ;; run macro when the full suite loads core.logic before proc_test.
  ;; `run` is POSIX-only (fork/execvp); on Windows it throws, so skip
  ;; there rather than report a spurious error.
  (when-not (some? (getenv "OS"))
    (let [result (clojure.core/run {:dir "/no/such/mino-dir-xyz"} "true")]
      (is (= 127 (:exit result)))
      (is (pstr/includes? (:err result) "chdir")))))

(deftest sh-in-future-does-not-starve-main-thread
  ;; A worker running a blocking (sh ...) must not hold the state lock
  ;; across its child IO. If it did, the main thread could not run the
  ;; very IO the child waits on, and the two would deadlock. Here a
  ;; future's sh spins until a release file appears; the main thread
  ;; creates that file with a pure IO prim (spit). Pre-fix the future's
  ;; deref times out (deadlock); post-fix it resolves promptly.
  ;; Needs a worker thread; skip cleanly where threads are not granted.
  (when (> (mino-thread-limit) 1)
    (let [stamp   (str (System/currentTimeMillis) "-" (rand-int 1000000))
          startf  (str "/tmp/mino-p8-start-" stamp)
          relf    (str "/tmp/mino-p8-rel-" stamp)]
      (sh "rm" "-f" startf relf)
      (let [f (future
                (sh "sh" "-c"
                    (str "touch " startf
                         "; while [ ! -e " relf
                         " ]; do sleep 0.02; done; echo go"))
                :done)]
        ;; Bounded wait (~5s) for the worker's sh to be running.
        (loop [n 0]
          (when (and (< n 250) (not (file-exists? startf)))
            (thread-sleep 20)
            (recur (inc n))))
        (spit relf "x")
        (is (= :done (deref f 8000 :TIMEOUT)))
        (sh "rm" "-f" startf relf)))))

(run-tests-and-exit)
