(require "tests/test")
(require '[mino.http.server :as hsrv])

;; Graceful HTTP server shutdown on SIGTERM: a mino process serving
;; Ring traffic through mino.http.server/run-server traps :term with
;; a handler that calls the server's :stop and then (exit 0). stop
;; closes the listener and joins the pool but never closes a
;; connection underneath a live worker, so a request in flight when
;; the signal lands is drained: its response arrives whole before the
;; process exits.
;;
;; Pinned contracts:
;; - stop returns only after the active worker finished its request
;;   (or the join grace ran out); the in-flight request completes
;;   with its full body, its socket never cut.
;; - the trapped path exits with the handler's 0, never 128+15.
;; - after stop the listener is gone: a fresh connect is refused.
;;
;; Delivery is observed from outside, with the supervisor idiom from
;; signal_test: a backgrounded shell supervisor records the child's
;; pid (its shell execs the mino binary, so the pid names the mino
;; process) and, once it ends, its exit code. A child cannot target
;; itself through $PPID: the sh primitive interposes a wrapper shell.

(def ^:private windows? (some? (getenv "OS")))

(def ^:private root "/tmp/mino-signal-shutdown-test")

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

(defn- get-request
  "One origin-form GET as wire bytes, asking the connection to close
  so the served worker ends with the response."
  [target]
  (byte-array (map int (str "GET " target " HTTP/1.1\r\n"
                            "Host: t.example\r\n"
                            "Connection: close\r\n\r\n"))))

(defn- read-response
  "Read one complete response off c; the parsed map, or nil when the
  peer closed before a full response arrived."
  [c]
  (loop [acc []]
    (let [r (http-parse-response (byte-array acc))]
      (if (= :done (:status r))
        r
        (let [b (try (net-read c 65536) (catch e nil))]
          (if b
            (recur (into acc (vec b)))
            (let [fin (http-parse-response (byte-array acc) {:eof true})]
              (when (= :done (:status fin)) fin))))))))

(defn- body-text [r]
  (apply str (map char (seq (:body r)))))

(defn- fetch
  "Connect, send one closing GET, read the response back, close."
  [port target]
  (let [c (net-connect "127.0.0.1" port
                       {:read-timeout 8000 :write-timeout 8000})]
    (try
      (net-write c (get-request target))
      (read-response c)
      (finally (try (net-close c) (catch e nil))))))

(deftest stop-returns-only-after-the-in-flight-request-drained
  ;; The drain pinned in isolation, without a signal or an exit in
  ;; play: stop's active-worker wait must cover the whole in-flight
  ;; request, so the handler-finished timestamp exists by the time
  ;; stop hands control back. (exit joins live futures on its own, so
  ;; only this in-process arm can tell stop's drain apart from
  ;; exit's.)
  (let [started (atom false)
        handler-done-ms (atom nil)
        h (fn [req]
            (reset! started true)
            (thread-sleep 500)
            (reset! handler-done-ms (time-ms))
            {:status 200 :body "drained-ok"})
        s (hsrv/run-server h {})]
    (try
      (let [slow (future (fetch (:port s) "/slow"))]
        (is (await-pred #(deref started)) "the request is in flight")
        ((:stop s))
        (is (some? @handler-done-ms)
            "stop waited for the active worker instead of returning early")
        (let [r (deref slow 10000 :timeout)]
          (is (map? r) "the in-flight response arrived, whole")
          (when (map? r)
            (is (= 200 (:code r)))
            (is (= "drained-ok" (body-text r))))))
      (finally
        ((:stop s))
        (is (await-pred #(zero? (mino-thread-count)))
            "the worker grant drained after stop")))))

(deftest trapped-term-stops-the-server-and-drains-the-in-flight-request
  ;; POSIX-only: :term names SIGTERM, which mingw does not deliver.
  ;; The child's /slow handler writes in-flight evidence, sleeps well
  ;; inside the stop grace, and answers a distinctive body; the parent
  ;; raises SIGTERM exactly while that handler is mid-sleep.
  (when-not windows?
    (reset-root!)
    (let [portf    (str root "/port.txt")
          ready    (str root "/ready")
          inflight (str root "/inflight")
          pid
          (launch-child!
           (str "(require '[mino.http.server :as srv])\n"
                "(def s (srv/run-server\n"
                "        (fn [req]\n"
                "          (if (= \"/slow\" (:uri req))\n"
                "            (do (spit \"" inflight "\" \"in\")\n"
                "                (thread-sleep 500)\n"
                "                {:status 200 :body \"drained-ok\"})\n"
                "            {:status 200 :body \"pong\"}))\n"
                "        {:port 0}))\n"
                "(spit \"" portf "\" (str (:port s)))\n"
                "(on-signal :term (fn [] ((:stop s)) (exit 0)))\n"
                "(spit \"" ready "\" \"up\")\n"
                "((fn wait [n] (when (pos? n)"
                " (thread-sleep 50) (wait (dec n)))) 200)\n"
                "(exit 99)\n"))]
      (is (await-file ready) "child signalled its trap is installed")
      (let [port (read-string (re-find #"\d+" (slurp portf)))]
        (let [r (fetch port "/ping")]
          (is (= 200 (:code r)) "the server answers before the signal")
          (is (= "pong" (body-text r))))
        (let [slow (future (fetch port "/slow"))]
          (is (await-file inflight) "the slow handler is mid-request")
          (sh "kill" "-TERM" pid)
          (let [r (deref slow 10000 :timeout)]
            (is (map? r) "the in-flight response arrived, whole")
            (when (map? r)
              (is (= 200 (:code r)))
              (is (= "drained-ok" (body-text r))
                  "stop drained the in-flight request before exit")))
          (is (= 0 (child-exit pid))
              "the trap+stop path exits with the handler's 0, never 128+15")
          (is (thrown? (net-connect "127.0.0.1" port
                                    {:connect-timeout 2000}))
              "after shutdown a fresh connect is refused"))))
    ;; drain the parent's worker grant so the next suite file starts
    ;; against a settled pool
    (is (await-pred #(zero? (mino-thread-count)))
        "the parent's future released its worker slot")))

(run-tests-and-exit)
