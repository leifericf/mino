(require "tests/test")
(require '[clojure.string :as str])

;; Terminal info primitives: tty?, terminal-width, terminal-height.
;;
;; Determinism strategy: every assertion that depends on fd shape or
;; on the environment runs in a subprocess with the fd redirected or
;; the env pinned (`env -u COLUMNS -u ROWS ...`). The suite's own
;; stdout is a capture pipe, but an interactive `./mino task test` run
;; has a real tty on stdin/stdout and a shell that may export COLUMNS,
;; so in-process assertions are limited to type shape. The true case
;; runs under script(1) (a real pty harness) when the host has one;
;; hosts without a harness skip that assertion rather than fail it.
;;
;; Width semantics under test: TIOCGWINSZ when a tty, else COLUMNS /
;; ROWS env when set and a plain 1..6-digit numeric value in
;; 1..10000, else the 80x24 default. Width and height fall back
;; independently.

(def ^:private windows? (some? (getenv "OS")))

(def ^:private bin
  (or (System/getenv "MINO_TEST_BIN")
      (when (file-exists? "./mino") "./mino")))

(defn- sh-e
  "Run one sh -c body against the binary under test; trimmed stdout."
  [body]
  (str/trim (:out (sh "sh" "-c" body))))

(defn- eval-e
  "Eval expr in a fresh mino with an env prefix and no fd redirects;
  stdout is the capture pipe, so :stdout/:stderr are not ttys."
  [prefix expr]
  (sh-e (str prefix " " bin " -e '" expr "'")))

(defn- clean-env
  "Env prefix that clears the size vars then applies assignments."
  [assignments]
  (str "env -u COLUMNS -u ROWS " assignments))

(deftest tty-answers-a-boolean-for-every-stream
  (is (contains? #{true false} (tty? :stdout)))
  (is (contains? #{true false} (tty? :stderr)))
  (is (contains? #{true false} (tty? :stdin))))

(deftest tty-false-for-pipes-files-and-redirects
  (when (and (not windows?) bin)
    ;; stdout is the pipe feeding cat
    (is (= "false" (eval-e "" "(tty? :stdout)")))
    ;; stdin is /dev/null
    (is (= "false" (sh-e (str bin " -e '(tty? :stdin)' < /dev/null"))))
    ;; stderr folds into the captured pipe
    (is (= "false" (sh-e (str bin " -e '(tty? :stderr)' 2>&1"))))
    ;; stdout to a file is not a tty either; read the file back
    (let [tmp "/tmp/mino_term_tty_out"]
      (is (= "false" (sh-e (str bin " -e '(tty? :stdout)' > " tmp
                                 " && cat " tmp)))))))

(deftest tty-true-under-a-real-pty
  (when (and (not windows?) bin)
    (let [uname (str/trim (:out (sh "uname" "-s")))
          body  (cond
                  (= "Darwin" uname)
                  (str "script -q /dev/null " bin
                       " -e '(if (tty? :stdout) :yes :no)'")

                  (= "Linux" uname)
                  (str "script -qec \"" bin
                       " -e (if (tty? :stdout) :yes :no)\" /dev/null")

                  :else nil)]
      (when body
        (let [r (sh "sh" "-c" (str body " 2>/dev/null"))]
          ;; skip-not-tty: no pty harness on this host is a skip, not
          ;; a failure; when the harness ran, the answer must be true.
          ;; script(1) decorates the pty transcript with control
          ;; artifacts (^D backspace pairs on BSD), so the assertion
          ;; matches the printed token rather than the whole stream.
          (when (zero? (:exit r))
            (is (str/includes? (str/replace (:out r) "\r" "") ":yes"))))))))

(deftest terminal-size-defaults-when-env-is-unset
  (when (and (not windows?) bin)
    (is (= "80" (eval-e (clean-env "") "(terminal-width)")))
    (is (= "24" (eval-e (clean-env "") "(terminal-height)")))))

(deftest terminal-size-reads-the-env-fallback
  (when (and (not windows?) bin)
    (is (= "100" (eval-e (clean-env "COLUMNS=100") "(terminal-width)")))
    (is (= "50" (eval-e (clean-env "ROWS=50") "(terminal-height)")))
    (is (= "(100 50)"
           (eval-e (clean-env "COLUMNS=100 ROWS=50")
                   "(list (terminal-width) (terminal-height))")))
    ;; each dimension falls back independently of the other
    (is (= "(100 24)"
           (eval-e (clean-env "COLUMNS=100")
                   "(list (terminal-width) (terminal-height))")))
    (is (= "(80 50)"
           (eval-e (clean-env "ROWS=50")
                   "(list (terminal-width) (terminal-height))")))))

(deftest terminal-size-rejects-nonnumeric-and-out-of-range-env
  (when (and (not windows?) bin)
    (doseq [v ["COLUMNS=abc" "COLUMNS=" "COLUMNS=0" "COLUMNS=-5"
               "COLUMNS=9999999" "COLUMNS=10001" "COLUMNS=1.5"
               "COLUMNS= 12" "COLUMNS=12 "]]
      (is (= "80" (eval-e (clean-env (str "\"" v "\"")) "(terminal-width)"))
          (str "rejects env value " v)))
    (doseq [v ["ROWS=abc" "ROWS=" "ROWS=0" "ROWS=9999999"]]
      (is (= "24" (eval-e (clean-env (str "\"" v "\"")) "(terminal-height)"))
          (str "rejects env value " v)))))

(deftest terminal-size-env-boundaries
  (when (and (not windows?) bin)
    (is (= "1" (eval-e (clean-env "COLUMNS=1") "(terminal-width)")))
    (is (= "10000" (eval-e (clean-env "COLUMNS=10000") "(terminal-width)")))
    (is (= "10000" (eval-e (clean-env "ROWS=10000") "(terminal-height)")))))

(deftest terminal-size-in-process-shape
  ;; In-process the values depend on the harness fds and env, so only
  ;; the shape is assertable: positive integers in a sane range.
  (is (and (int? (terminal-width)) (pos? (terminal-width))))
  (is (and (int? (terminal-height)) (pos? (terminal-height))))
  (is (<= (terminal-width) 10000))
  (is (<= (terminal-height) 10000)))

(deftest tty-arity-and-type-errors
  (is (= :eval/arity (try (tty?) (catch e (:mino/kind e)))))
  (is (= :eval/arity (try (tty? :stdout :stderr) (catch e (:mino/kind e)))))
  (is (= :eval/type (try (tty? 42) (catch e (:mino/kind e)))))
  (is (= :eval/type (try (tty? "stdout") (catch e (:mino/kind e)))))
  (is (= :eval/type (try (tty? :bogus) (catch e (:mino/kind e)))))
  ;; the size prims take no arguments at all; any argument is arity
  (is (= :eval/arity (try (terminal-width :stdout) (catch e (:mino/kind e)))))
  (is (= :eval/arity (try (terminal-height 1 2) (catch e (:mino/kind e))))))

(run-tests-and-exit)
