(require "tests/test")
(require '[clojure.string :as str])

;; The argv contract, end to end against the real binary. A single
;; `--` separator is honored in every invocation mode; the tail after
;; it is exactly *command-line-args*, never a file or task dispatch:
;;
;;   mino FILE -- ARGS        FILE runs, args are ARGS
;;   mino -e EXPR -- ARGS     EXPR runs, args are ARGS, mino exits
;;   mino task NAME -- ARGS   NAME runs, args are ARGS
;;
;; Without a separator everything after the dispatch slot (the script
;; path or the task name) is the args. Tracked as the fragile argv
;; contract issue (ki-18). POSIX-only: the sh prim redirects through
;; the POSIX shell.

(def ^:private probe-script "/tmp/mino_cli_args_probe.clj")
(def ^:private task-dir "/tmp/mino_cli_taskproj")

(defn- run-cli
  "Run the standalone binary with args, returning the sh result map."
  [& args]
  (apply sh (cons "./mino" args)))

(defn- setup-task-project
  "Create a throwaway task project whose probe task prints the args
  it was invoked with."
  []
  (sh "mkdir" "-p" (str task-dir "/lib"))
  (spit (str task-dir "/lib/cliprobe.clj")
        "(ns cliprobe)\n(defn probe [] (prn *command-line-args*))\n")
  (spit (str task-dir "/mino.edn")
        (str "{:paths [\"lib\"]\n"
             " :tasks {probe {:doc \"probe\" :task cliprobe/probe}}}\n")))

(defn- run-task-cli
  "Run `mino task probe <args>` inside the throwaway project."
  [& args]
  (let [bin (str/trim (:out (sh "pwd")))]
    (sh "sh" "-c"
        (str "cd " task-dir " && '" bin "/mino' task probe "
             (str/join " " args)))))

(when-not (some? (getenv "OS"))
  (spit probe-script "(prn *command-line-args*)\n")
  (setup-task-project))

(deftest eval-with-separator-binds-exact-tail-and-exits
  (when-not (some? (getenv "OS"))
    (let [r (run-cli "-e" "(prn *command-line-args*)" "--" "foo" "bar")]
      (is (= 0 (:exit r)))
      (is (= "(\"foo\" \"bar\")\n" (:out r))))))

(deftest eval-with-separator-and-empty-tail-binds-nil
  (when-not (some? (getenv "OS"))
    (let [r (run-cli "-e" "(prn *command-line-args*)" "--")]
      (is (= 0 (:exit r)))
      (is (= "nil\n" (:out r))))))

(deftest eval-without-args-binds-nil
  (when-not (some? (getenv "OS"))
    (let [r (run-cli "-e" "(prn *command-line-args*)")]
      (is (= 0 (:exit r)))
      (is (= "nil\n" (:out r))))))

(deftest script-with-separator-strips-it-from-args
  (when-not (some? (getenv "OS"))
    (let [r (run-cli probe-script "--" "foo" "bar")]
      (is (= 0 (:exit r)))
      (is (= "(\"foo\" \"bar\")\n" (:out r))))))

(deftest script-without-separator-passes-all-args
  (when-not (some? (getenv "OS"))
    (let [r (run-cli probe-script "foo" "bar")]
      (is (= 0 (:exit r)))
      (is (= "(\"foo\" \"bar\")\n" (:out r))))))

(deftest separator-first-still-dispatches-the-file
  (when-not (some? (getenv "OS"))
    (let [r (run-cli "--" probe-script "foo" "bar")]
      (is (= 0 (:exit r)))
      (is (= "(\"foo\" \"bar\")\n" (:out r))))))

(deftest task-with-separator-binds-tail-without-the-name
  (when-not (some? (getenv "OS"))
    (let [r (run-task-cli "--" "foo" "bar")
          out (:out r)]
      (is (= 0 (:exit r)))
      (is (re-find #"\(\"foo\" \"bar\"\)" out))
      (is (not (re-find #"\(\"probe\"" out))))))

(deftest task-without-separator-skips-the-task-name
  (when-not (some? (getenv "OS"))
    (let [r (run-task-cli "foo" "bar")
          out (:out r)]
      (is (= 0 (:exit r)))
      (is (re-find #"\(\"foo\" \"bar\"\)" out))
      (is (not (re-find #"\(\"probe\"" out))))))

(deftest thread-limit-env-overrides-the-host-grant
  (when-not (some? (getenv "OS"))
    ;; MINO_THREAD_LIMIT caps the auto-detected grant so an operator can
    ;; constrain mino's host-thread usage; the suite relies on it to
    ;; reproduce a small-core host for the pool concurrency tests.
    (let [r (sh "sh" "-c"
                "MINO_THREAD_LIMIT=3 ./mino -e '(mino-thread-limit)'")]
      (is (= 0 (:exit r)))
      (is (= "3" (str/trim (:out r)))))
    ;; A non-positive or unparseable value is ignored; the grant falls
    ;; back to the auto-detected count, which is always a positive int.
    (let [r (sh "sh" "-c"
                "MINO_THREAD_LIMIT=bogus ./mino -e '(pos? (mino-thread-limit))'")]
      (is (= 0 (:exit r)))
      (is (= "true" (str/trim (:out r)))))))

(run-tests-and-exit)
