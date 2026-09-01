(ns mino.wait
  "Wait helpers: poll for a TCP port or a filesystem path to become
  available, returning an elapsed-ms map or throwing :mino/kind
  :wait/timeout.

  (require '[mino.wait :as wait])
  (wait/for-port \"localhost\" 8080 {:timeout-ms 10000})
  ;; => {:elapsed-ms 342}

  (wait/for-path \"/tmp/ready\" {:timeout-ms 5000 :interval-ms 250})
  ;; => {:elapsed-ms 1200}

  Both functions block in a polling loop with a sleep between probes.
  Options (keyword map):
    :timeout-ms   total wait budget in ms (default 10000)
    :interval-ms  sleep between probes in ms (default 250)

  On success returns {:elapsed-ms n} where n is the number of
  milliseconds elapsed from the call to the first successful probe.
  On timeout throws :mino/kind :wait/timeout with :mino/data carrying
  :host / :port or :path plus :timeout-ms and :elapsed-ms.

  Requires the :net capability for for-port (uses net-connect to probe
  the TCP port). for-path requires only the :fs capability.")

(defn- wait-fail
  [kind code msg data]
  (throw {:mino/kind kind :mino/code code :mino/message msg
          :mino/data data}))

(defn for-port
  "Polls host:port with a short-lived TCP connect until it accepts a
  connection or the timeout expires. Returns {:elapsed-ms n} on success.
  Throws :mino/kind :wait/timeout on timeout."
  ([host port] (for-port host port {}))
  ([host port opts]
   (let [timeout-ms  (get opts :timeout-ms 10000)
         interval-ms (get opts :interval-ms 250)
         start-ms    (time-ms)]
     (loop []
       (let [elapsed (- (time-ms) start-ms)
             ok?     (try
                       (let [s (net-connect host port {:connect-timeout interval-ms})]
                         (try (net-close s) (catch _ nil))
                         true)
                       (catch _ false))]
         (cond
           ok?
           {:elapsed-ms elapsed}

           (>= elapsed timeout-ms)
           (wait-fail :wait/timeout "MWT001"
                      (str "wait/for-port: timed out connecting to "
                           host ":" port)
                      {:host host :port port
                       :timeout-ms timeout-ms
                       :elapsed-ms elapsed})

           :else
           (do (thread-sleep interval-ms)
               (recur))))))))

(defn for-path
  "Polls a filesystem path until file-exists? returns true or the timeout
  expires. Returns {:elapsed-ms n} on success.
  Throws :mino/kind :wait/timeout on timeout."
  ([path] (for-path path {}))
  ([path opts]
   (let [timeout-ms  (get opts :timeout-ms 10000)
         interval-ms (get opts :interval-ms 250)
         start-ms    (time-ms)]
     (loop []
       (let [elapsed (- (time-ms) start-ms)]
         (cond
           (file-exists? path)
           {:elapsed-ms elapsed}

           (>= elapsed timeout-ms)
           (wait-fail :wait/timeout "MWT002"
                      (str "wait/for-path: timed out waiting for " path)
                      {:path path
                       :timeout-ms timeout-ms
                       :elapsed-ms elapsed})

           :else
           (do (thread-sleep interval-ms)
               (recur))))))))
