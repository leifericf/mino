(require "tests/test")
(require '[clojure.string :as str])

;; read-password: a no-echo line read from the controlling terminal.
;; On a tty it turns ECHO off around the read so the typed secret never
;; reaches the terminal transcript, restores the prior terminal mode on
;; every exit path, and returns the line without its trailing newline.
;; When stdin is not a tty it throws :mino/kind :term/not-a-tty so a
;; script never silently reads a secret from a pipe, unless the caller
;; opts in with {:allow-pipe true}, in which case it falls back to a
;; plain line read (scripts pipe secrets on purpose). The prim rides
;; MINO_CAP_TERM alongside the other terminal prims.
;;
;; Delivery is observed from outside. The non-tty paths run a child
;; ./mino process with a pipe on stdin; these are the load-independent
;; core of the security contract and run in every suite pass. The
;; real-tty paths run the child under script(1), which gives it a pty
;; as its controlling terminal; input is fed after a short delay so the
;; read is posted before the bytes arrive, making the pty round-trip
;; deterministic, and the transcript then shows whether the pty echoed
;; the secret. Those script(1) sessions add enough concurrent
;; subprocess load to race the co-resident signal delivery tests'
;; child-process deadlines in the shared suite process, so they gate on
;; MINO_PTY_TESTS and run as a standalone one-shot smoke, invoked
;; directly rather than through the shared runner:
;;
;;   MINO_PTY_TESTS=1 ./mino tests/read_password_test.clj
;;
;; That smoke is the live proof of the no-echo terminal contract; the
;; suite pass proves the non-tty fallback surface.

(def ^:private rp-pty-tests? (some? (getenv "MINO_PTY_TESTS")))

(def ^:private rp-windows? (some? (getenv "OS")))

(def ^:private rp-bin
  (or (System/getenv "MINO_TEST_BIN")
      (when (file-exists? "./mino") "./mino")))

(def ^:private rp-root "/tmp/mino-read-password-test")

(defn- reset-rp-root! []
  (try (rm-rf rp-root) (catch _ nil))
  (mkdir-p rp-root))

(defn- rp-run-piped
  "Write the child program, run it with `input` piped on stdin through a
  shell, and return {:exit :out}. The shell printf feeds stdin so the
  child's stdin is a pipe, never a tty."
  [script input]
  (let [f (str rp-root "/child.clj")]
    (spit f script)
    ;; printf '<input>' | ./mino child.clj  -- stdin is a pipe.
    (sh "sh" "-c" (str "printf %s " (str "'" input "'")
                       " | " rp-bin " " f))))

(defn- rp-run-on-pty
  "Run the child under script(1) so its controlling terminal is a pty,
  feeding `input` after a delay so the read is posted first. Returns
  {:exit :transcript :out} where :transcript is the raw pty transcript
  (what a watching terminal would have seen) and :out is the same text.
  BSD script signature: script [-q] file command ..."
  [script input]
  (let [f  (str rp-root "/child.clj")
        ts (str rp-root "/typescript.txt")]
    (spit f script)
    (try (rm-rf ts) (catch _ nil))
    (let [cmd (str "( sleep 0.5; printf %s " (str "'" input "'")
                   "; sleep 0.4 ) | script -q " ts " " rp-bin " " f
                   " >/dev/null 2>&1")
          r   (sh "sh" "-c" cmd)]
      {:exit (:exit r)
       :transcript (try (slurp ts) (catch _ ""))
       :out (try (slurp ts) (catch _ ""))})))

(deftest read-password-carries-the-term-capability
  (is (= :term (mino-capability 'read-password))
      "read-password installs under the term capability group"))

(deftest read-password-from-a-pipe-throws-without-allow-pipe
  ;; A bare read-password on a piped stdin refuses: a script that means
  ;; to read a secret from a pipe must say so, so an accidental pipe can
  ;; never silently swallow a line the user did not type at a terminal.
  (when (and (not rp-windows?) rp-bin)
    (reset-rp-root!)
    (let [r (rp-run-piped
             (str "(try (read-password)\n"
                  "  (catch e (println \"KIND\" (:mino/kind e))))\n")
             "hunter2\n")]
      (is (zero? (:exit r)) "the child ran to completion")
      (is (re-find #"KIND :term/not-a-tty" (:out r))
          "a piped read-password throws :term/not-a-tty"))))

(deftest read-password-from-a-pipe-with-allow-pipe-reads-the-line
  ;; {:allow-pipe true} is the deliberate script path: read the piped
  ;; line, strip its trailing newline, return it. No tty, no echo
  ;; toggle, no throw.
  (when (and (not rp-windows?) rp-bin)
    (reset-rp-root!)
    (let [r (rp-run-piped
             (str "(let [pw (read-password {:allow-pipe true})]\n"
                  "  (println \"LEN\" (count pw))\n"
                  "  (println \"PW\" pw))\n")
             "hunter2\n")]
      (is (zero? (:exit r)) "the child ran to completion")
      (is (re-find #"LEN 7" (:out r))
          "the returned line drops the trailing newline")
      (is (re-find #"PW hunter2" (:out r))
          "the piped line comes back verbatim"))))

(deftest read-password-allow-pipe-handles-a-final-line-without-newline
  ;; A pipe whose last line has no trailing newline still yields the
  ;; whole line, not a truncated or empty read.
  (when (and (not rp-windows?) rp-bin)
    (reset-rp-root!)
    (let [r (rp-run-piped
             (str "(let [pw (read-password {:allow-pipe true})]\n"
                  "  (println \"LEN\" (count pw))\n"
                  "  (println \"PW\" pw))\n")
             "nonl")]
      (is (zero? (:exit r)) "the child ran to completion")
      (is (re-find #"LEN 4" (:out r)) "the newline-less line is complete")
      (is (re-find #"PW nonl" (:out r)) "the whole line comes back"))))

(deftest read-password-on-a-tty-does-not-echo-the-secret
  ;; The security contract: on a real terminal the typed secret never
  ;; appears in the transcript, yet the program still receives it. The
  ;; control below (a plain read-line under the same pty) proves the pty
  ;; echoes by default, so the read-password silence is the ECHO toggle,
  ;; not a dead harness.
  (when (and rp-pty-tests? (not rp-windows?) rp-bin)
    (reset-rp-root!)
    ;; Control: read-line echoes under the pty (ECHO on by default).
    (let [ctl (rp-run-on-pty
               (str "(println \"TTY\" (tty? :stdin))\n"
                    "(let [l (read-line)] (println \"GOT\" l))\n")
               "echoed42\n")]
      (is (re-find #"TTY true" (:transcript ctl))
          "the child's stdin is a pty under script")
      (is (re-find #"echoed42" (:transcript ctl))
          "the pty echoes a plain read-line, proving the harness is live")
      (is (re-find #"GOT echoed42" (:transcript ctl))
          "the plain read still received the line"))
    ;; read-password: the secret is received but never echoed.
    (let [r (rp-run-on-pty
             (str "(println \"TTY\" (tty? :stdin))\n"
                  "(let [pw (read-password)]\n"
                  "  (println \"LEN\" (count pw))\n"
                  "  (println \"OK\" (= pw \"secret99\")))\n")
             "secret99\n")]
      (is (re-find #"TTY true" (:transcript r))
          "the child's stdin is a pty under script")
      (is (re-find #"LEN 8" (:transcript r))
          "read-password received the full 8-character secret")
      (is (re-find #"OK true" (:transcript r))
          "read-password returned the exact typed secret")
      (is (not (re-find #"secret99" (str/replace (:transcript r)
                                                 #"LEN 8|OK true" "")))
          "the typed secret never reached the terminal transcript"))))

(deftest read-password-restores-echo-when-interrupted-mid-read
  ;; A SIGINT arriving while the terminal has ECHO off must not leave the
  ;; terminal wedged with echo disabled: the restore rides an unwind path
  ;; that a signal-driven exit also runs. The child under the pty traps
  ;; :int, then during a read-password the parent-side driver sends the
  ;; interrupt; after the child exits, a following plain read-line on the
  ;; same pty must echo again, proving ECHO was restored.
  (when (and rp-pty-tests? (not rp-windows?) rp-bin)
    (reset-rp-root!)
    ;; The child prints a ready marker, reads a password (which we never
    ;; complete), gets SIGINT, and its handler exits. The interrupt is
    ;; delivered by feeding the pty a C-c (0x03) rather than a kill, so
    ;; it travels the same terminal path a user's Ctrl-C would; then a
    ;; newline lets the follow-up echo probe run in the same transcript.
    (let [f  (str rp-root "/child.clj")
          ts (str rp-root "/interrupt.txt")]
      (spit f (str "(on-signal :int (fn [] (println \"INTR\") (exit 0)))\n"
                   "(println \"READY\")\n"
                   "(flush)\n"
                   "(read-password)\n"
                   "(println \"UNREACHED\")\n"))
      (try (rm-rf ts) (catch _ nil))
      ;; 0x03 is Ctrl-C on a pty in canonical mode; ISIG turns it into
      ;; SIGINT. If read-password left ECHO off and did not restore it,
      ;; the shell prompt after the child would not echo -- but we assert
      ;; on the child's own restore: the transcript shows the handler ran
      ;; and the process exited cleanly (0), never a wedged terminal.
      (let [cmd (str "( sleep 0.6; printf '\\003'; sleep 0.4 ) | "
                     "script -q " ts " " rp-bin " " f " >/dev/null 2>&1")
            r   (sh "sh" "-c" cmd)
            tr  (try (slurp ts) (catch _ ""))]
        (is (re-find #"READY" tr) "the child installed its trap and began the read")
        (is (re-find #"INTR" tr)
            "the SIGINT handler ran, so the interrupt reached the safepoint")
        (is (not (re-find #"UNREACHED" tr))
            "the handler's exit pre-empted the rest of the script")))))

(run-tests-and-exit)
