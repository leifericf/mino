(require "tests/test")

;; Signal handling and at-exit hooks: (on-signal sig handler) traps
;; :int :term :hup :usr1 :usr2 with a zero-arg fn that runs at the
;; interpreter safepoint (never in signal context), or reshapes the
;; disposition with the data keywords :default and :ignore. (at-exit
;; thunk) registers shutdown hooks that run last-registered-first on a
;; plain (exit n) and on a trapped-then-exiting signal path. Both
;; prims install under the :signal capability group.
;;
;; Pinned contracts:
;; - on-signal and at-exit both return nil; the previous disposition
;;   is not handed back.
;; - at-exit hooks run LIFO and never disturb the exit code they run
;;   under; falling off the end of a script runs them too.
;; - a trapped signal runs the mino handler, which can do arbitrary
;;   work and call (exit n) itself; an untrapped signal keeps the OS
;;   default, so the child dies and the supervisor reports 128+signo.
;;
;; Delivery is observed from outside: each test writes a small child
;; program to a scratch file and starts it under a backgrounded shell
;; supervisor that records the child's pid and, once it ends, its exit
;; code. The parent then drives delivery with kill(1) against that
;; recorded pid and reads back the exit code, output, and marker-file
;; evidence. A child cannot target itself through $PPID: the sh
;; primitive interposes a wrapper shell, so $PPID inside a child names
;; the wrapper, never the mino process.

(def ^:private windows? (some? (getenv "OS")))

(def ^:private root "/tmp/mino-signal-test")

(defn- reset-root! []
  (try (rm-rf root) (catch _ nil))
  (mkdir-p root))

(defn- await-pred
  "Poll pred every 20ms for up to 10s; true once it holds."
  [pred]
  ((fn wait [n]
     (cond (pred)    true
           (zero? n) false
           :else     (do (thread-sleep 20) (wait (dec n)))))
   500))

(defn- await-file [path]
  (await-pred #(file-exists? path)))

(defn- run-child
  "Write the child program to a scratch file and run it in a fresh
  mino process, returning the sh result map."
  [script]
  (let [f (str root "/child.clj")]
    (spit f script)
    (sh "./mino" f)))

(def ^:private pidf  (str root "/child.pid"))
(def ^:private outf  (str root "/child.out"))
(def ^:private exitf (str root "/child.exit"))

(defn- launch-child!
  "Start the child program in a background mino process and return its
  pid as a string. A launcher script backgrounds a supervisor subshell
  that starts the child, waits for it, and records its exit code
  (128+signo when a signal killed it). The child's shell writes its
  own pid and then execs the mino binary, so the recorded pid names
  the mino process no matter how the shell arranges its forks. The
  supervisor redirects to /dev/null: anything holding the launching
  sh call's capture pipe open would block that call until the child
  exited."
  [script]
  (let [child    (str root "/child.clj")
        launcher (str root "/launch.sh")]
    (spit child script)
    (spit launcher (str "{\n"
                        "  sh -c 'echo $$ > " pidf
                        "; exec ./mino " child " > " outf " 2>&1' &\n"
                        "  wait $!\n"
                        "  echo $? > " exitf "\n"
                        "} > /dev/null 2>&1 &\n"))
    (sh "sh" launcher)
    (is (await-file pidf) "supervisor published the child pid")
    (re-find #"\d+" (slurp pidf))))

(defn- child-exit
  "Await and return the supervised child's exit code as an int."
  [pid]
  (is (await-file exitf) (str "child " pid " exited within the deadline"))
  (read-string (re-find #"\d+" (slurp exitf))))

(deftest signal-prims-carry-signal-capability
  (is (= :signal (mino-capability 'on-signal)))
  (is (= :signal (mino-capability 'at-exit))))

(deftest on-signal-and-at-exit-return-nil
  ;; POSIX-only: :term names SIGTERM, which mingw does not deliver.
  (when-not windows?
    (reset-root!)
    (let [r (run-child (str "(prn (on-signal :term (fn [] nil)))\n"
                            "(prn (at-exit (fn [] nil)))\n"))]
      (is (= 0 (:exit r)))
      (is (= "nil\nnil\n" (:out r))
          "registration hands back nil, not the previous handler"))))

(deftest at-exit-hooks-run-lifo-and-keep-the-exit-code
  (reset-root!)
  (let [ev (str root "/order.txt")
        r  (run-child
            (str "(at-exit (fn [] (spit \"" ev
                 "\" \"first-registered\\n\" :append true)))\n"
                 "(at-exit (fn [] (spit \"" ev
                 "\" \"second-registered\\n\" :append true)))\n"
                 "(at-exit (fn [] (spit \"" ev
                 "\" \"third-registered\\n\" :append true)))\n"
                 "(exit 7)\n"))]
    (is (= 7 (:exit r)) "the code passed to (exit n) survives the hooks")
    (is (= "third-registered\nsecond-registered\nfirst-registered\n"
           (slurp ev))
        "hooks run in reverse registration order")))

(deftest at-exit-hooks-run-when-the-script-just-ends
  (reset-root!)
  (let [ev (str root "/plain.txt")
        r  (run-child
            (str "(at-exit (fn [] (spit \"" ev "\" \"ran\\n\")))\n"
                 "(println \"BODY\")\n"))]
    (is (= 0 (:exit r)))
    (is (re-find #"BODY" (:out r)) "the script body ran to its end")
    (is (= "ran\n" (slurp ev))
        "falling off the end of a script still runs hooks")))

(deftest trapped-term-runs-handler-then-exit-hooks
  ;; POSIX-only. The handler writes evidence and picks its own exit
  ;; code; the at-exit hook must still run after it, and the exit code
  ;; must be the handler's 5, never 128+15. The fallback (exit 99)
  ;; only fires when the handler never ran.
  (when-not windows?
    (reset-root!)
    (let [ev    (str root "/term.txt")
          ready (str root "/term-ready")
          pid   (launch-child!
                 (str "(at-exit (fn [] (spit \"" ev
                      "\" \"atexit\\n\" :append true)))\n"
                      "(on-signal :term (fn []\n"
                      "  (spit \"" ev "\" \"handler\\n\" :append true)\n"
                      "  (exit 5)))\n"
                      "(spit \"" ready "\" \"up\")\n"
                      "((fn wait [n] (when (pos? n)"
                      " (thread-sleep 50) (wait (dec n)))) 200)\n"
                      "(exit 99)\n"))]
      (is (await-file ready) "child signalled its trap is installed")
      (sh "kill" "-TERM" pid)
      (is (= 5 (child-exit pid))
          "the trapped path exits with the handler's chosen code")
      (is (= "handler\natexit\n" (slurp ev))
          "handler evidence first, then the at-exit hook"))))

(deftest each-keyword-maps-to-its-posix-signal
  ;; POSIX-only. One child traps :int :hup :usr1 :usr2; the parent
  ;; raises each in turn, waiting for the previous handler's line
  ;; before the next raise so the evidence file's order is exact. The
  ;; child exits 0 on its own once all four handlers have run.
  (when-not windows?
    (reset-root!)
    (let [ev    (str root "/map.txt")
          ready (str root "/map-ready")
          lines (fn [n]
                  #(and (file-exists? ev)
                        (>= (count (re-seq #"\n" (slurp ev))) n)))
          pid   (launch-child!
                 (str "(def ev \"" ev "\")\n"
                      "(defn note [tok]\n"
                      "  (fn [] (spit ev (str tok \"\\n\") :append true)))\n"
                      "(on-signal :int  (note \"int\"))\n"
                      "(on-signal :hup  (note \"hup\"))\n"
                      "(on-signal :usr1 (note \"usr1\"))\n"
                      "(on-signal :usr2 (note \"usr2\"))\n"
                      "(spit \"" ready "\" \"up\")\n"
                      "(defn lines []\n"
                      "  (try (count (re-seq #\"\\n\" (slurp ev)))"
                      " (catch _ 0)))\n"
                      "((fn wait [n]\n"
                      "   (when (and (pos? n) (< (lines) 4))\n"
                      "     (thread-sleep 20) (wait (dec n)))) 500)\n"
                      "(exit (if (= 4 (lines)) 0 98))\n"))]
      (is (await-file ready) "child signalled its traps are installed")
      (sh "kill" "-INT" pid)
      (is (await-pred (lines 1)) "the :int handler ran")
      (sh "kill" "-HUP" pid)
      (is (await-pred (lines 2)) "the :hup handler ran")
      (sh "kill" "-USR1" pid)
      (is (await-pred (lines 3)) "the :usr1 handler ran")
      (sh "kill" "-USR2" pid)
      (is (await-pred (lines 4)) "the :usr2 handler ran")
      (is (= 0 (child-exit pid)) "trapped signals never kill the child")
      (is (= "int\nhup\nusr1\nusr2\n" (slurp ev))
          "each keyword reached its own handler, in raise order"))))

(deftest ignore-replaces-a-fn-handler-and-drops-the-signal
  ;; POSIX-only. :ignore is handler-as-data: it displaces the earlier
  ;; fn handler entirely, so the raise neither kills the child nor
  ;; runs the old fn.
  (when-not windows?
    (reset-root!)
    (let [ev      (str root "/ignored.txt")
          ready   (str root "/ignore-ready")
          release (str root "/ignore-release")
          pid     (launch-child!
                   (str "(on-signal :int (fn [] (spit \"" ev
                        "\" \"handler\\n\" :append true)))\n"
                        "(on-signal :int :ignore)\n"
                        "(spit \"" ready "\" \"up\")\n"
                        "((fn wait [n]\n"
                        "   (when (and (pos? n)\n"
                        "              (not (file-exists? \"" release
                        "\")))\n"
                        "     (thread-sleep 20) (wait (dec n)))) 500)\n"
                        "(println \"SURVIVED\")\n"))]
      (is (await-file ready) "child signalled :ignore is in place")
      (sh "kill" "-INT" pid)
      ;; Give a mistaken delivery time to land before releasing.
      (thread-sleep 300)
      (spit release "go")
      (is (= 0 (child-exit pid)) "an ignored SIGINT does not kill the child")
      (is (re-find #"SURVIVED" (slurp outf))
          "the child lived on to print its later marker")
      (is (not (file-exists? ev))
          "the displaced fn handler never ran"))))

(deftest default-restores-the-os-disposition
  ;; POSIX-only. :default undoes an earlier trap: the raise kills the
  ;; child at the OS level, so the supervisor reports 128+signo (130
  ;; for SIGINT), nothing after the raise runs, and the displaced fn
  ;; never fires. The long sleep is never served; death interrupts it.
  (when-not windows?
    (reset-root!)
    (let [ev    (str root "/defaulted.txt")
          ready (str root "/default-ready")
          pid   (launch-child!
                 (str "(on-signal :int (fn [] (spit \"" ev
                      "\" \"handler\\n\" :append true)))\n"
                      "(on-signal :int :default)\n"
                      "(spit \"" ready "\" \"up\")\n"
                      "(thread-sleep 10000)\n"
                      "(println \"SURVIVED\")\n"))]
      (is (await-file ready) "child signalled :default is in place")
      (sh "kill" "-INT" pid)
      (is (= 130 (child-exit pid))
          "an untrapped SIGINT kills with 128+signo")
      (is (not (re-find #"SURVIVED" (slurp outf)))
          "the child never got past the raise")
      (is (not (file-exists? ev))
          "the displaced fn handler never ran"))))

(run-tests-and-exit)
