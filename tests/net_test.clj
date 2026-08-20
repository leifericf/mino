(require "tests/test")
(require '[clojure.string :as str])

;; TCP net capability and socket prims.
;;
;; The CLI installs every capability, so the net bit is present here;
;; the sandbox preset (MINO_CAP_DEFAULT) keeps net out, pinned at the
;; C level in tests/embed_api_test.c where a state can be built
;; without it.
;;
;; Behaviour tests drive loopback servers built from mino's own
;; listener prims: the accept loop runs in a future on the worker
;; threads and the test thread plays the client, so every fixture is
;; in process, portable to each platform the CLI builds on, and torn
;; down deterministically in a finally block.

(defn- nt-serve-echo
  "Echo everything until the peer goes quiet or closes."
  [c]
  (loop []
    (let [b (try (net-read c 65536) (catch e nil))]
      (when b
        (net-write c b)
        (recur)))))

(defn- nt-hold-open
  "Consume one read, then silence until teardown wakes; the
  read-timeout fixture body."
  [c running?]
  (try (net-read c 1) (catch e nil))
  (loop []
    (when @running?
      (let [b (try (net-read c 1) (catch e ::tick))]
        (when (some? b)
          (recur))))))

(defn- nt-serve-one
  "Serve a single accepted connection in the fixture mode."
  [mode c running?]
  (case mode
    :echo (nt-serve-echo c)
    :payload (net-write c "0123456789")
    :shut nil
    :slow (nt-hold-open c running?)))

(defn- nt-start-server
  "Loopback fixture server in mode (:echo :payload :shut :slow).
  Connections are served serially inside one accept-loop future;
  returns the port and a stop fn with guaranteed teardown."
  [mode]
  (let [l (net-listen "127.0.0.1" 0 {})
        running? (atom true)
        conns (atom [])
        port (net-listener-port l)
        fut (future
              (loop []
                (when @running?
                  (try
                    (let [c (net-accept l {:accept-timeout 250
                                           :read-timeout 1500})]
                      (swap! conns conj c)
                      (try (nt-serve-one mode c running?) (catch e nil))
                      (try (net-close c) (catch e nil)))
                    (catch e nil))
                  (recur))))
        stop (fn []
               (reset! running? false)
               (try (net-close l) (catch e nil))
               ;; The loop closes its own sockets once their reads
               ;; end; the sweep after the join only covers a serve
               ;; cycle that exited through an unexpected throw.
               (let [r (deref fut 4000 ::stopped)]
                 (doseq [c @conns]
                   (try (net-close c) (catch e nil)))
                 r))]
    {:port port :stop stop}))

(defn- with-server
  "Run body with a fresh fixture server in mode; the server is
  stopped after the body whether it passed, threw, or errored."
  [mode body]
  (let [srv (nt-start-server mode)]
    (try
      (body srv)
      (finally
        ((:stop srv))))))

(defn- free-dead-port
  "A loopback port that was bound and then released, so connecting to
  it is refused rather than timing out."
  []
  (let [l (net-listen "127.0.0.1" 0 {})
        p (net-listener-port l)]
    (net-close l)
    p))

;; ---- capability metadata ----

(deftest net-capability-present-under-cli
  (is (true? (mino-installed? :net))))

(deftest net-prims-labelled-with-net-capability
  (is (= :net (mino-capability 'net-connect)))
  (is (= :net (mino-capability 'net-read)))
  (is (= :net (mino-capability 'net-read-all)))
  (is (= :net (mino-capability 'net-write)))
  (is (= :net (mino-capability 'net-close))))

;; ---- argument shapes (no server needed; runs on every platform) ----

(deftest net-connect-validates-arguments
  (is (thrown? (net-connect)))
  (is (thrown? (net-connect "127.0.0.1")))
  (is (= :eval/type
         (try (net-connect 42 80) (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (net-connect "127.0.0.1" 0) (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (net-connect "127.0.0.1" 70000) (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (net-connect "127.0.0.1" 80 {:read-timeout -1})
              (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (net-connect "127.0.0.1" 80 {:read-timeout "soon"})
              (catch e (:mino/kind e)))))
  (is (= :eval/type
         (try (net-connect "127.0.0.1" 80 :not-a-map)
              (catch e (:mino/kind e))))))

(deftest net-io-prims-validate-socket-arguments
  (is (= :eval/type
         (try (net-read "not-a-socket" 1) (catch e (:mino/kind e)))))
  (is (= :eval/type
         (try (net-read-all 7) (catch e (:mino/kind e)))))
  (is (= :eval/type
         (try (net-write 7 "x") (catch e (:mino/kind e)))))
  (is (= :eval/type
         (try (net-close :keyword) (catch e (:mino/kind e)))))
  (is (thrown? (net-read (byte-array 1)))))

;; ---- DNS failure (no server; the .invalid TLD is RFC-reserved to
;; ---- never resolve) ----

(deftest net-connect-dns-failure-classifies-as-net-dns
  (let [r (try (net-connect "host-that-cannot-resolve.invalid" 80
                            {:connect-timeout 2000})
               (catch e e))]
    (is (= :net/dns (:mino/kind r)))
    (is (str/includes? (:mino/message r) "cannot resolve"))))

;; ---- loopback behaviour ----

(deftest net-connect-echo-round-trip-string
  (with-server :echo
    (fn [srv]
      (let [s (net-connect "127.0.0.1" (:port srv))]
        (is (= :handle (type s)))
        (is (= 11 (net-write s "hello world")))
        ;; A single net-read returns what has arrived, up to n.
        (is (= "68656c6c6f" (hex-encode (net-read s 5))))
        (is (= "20776f726c64" (hex-encode (net-read s 100))))
        (is (nil? (net-close s)))))))

(deftest net-connect-echo-round-trip-bytes
  (with-server :echo
    (fn [srv]
      (let [s (net-connect "127.0.0.1" (:port srv))
            payload (byte-array [0 1 2 250 255])]
        (is (= 5 (net-write s payload)))
        (is (= payload (net-read s 5)))
        (is (nil? (net-close s)))))))

(deftest net-write-encodes-strings-as-utf8
  (with-server :echo
    (fn [srv]
      (let [s (net-connect "127.0.0.1" (:port srv))]
        ;; a (1) + é (2) + ☃ (3) bytes.
        (is (= 6 (net-write s "aé☃")))
        (is (= "61c3a9e29883" (hex-encode (net-read s 6))))
        (is (nil? (net-close s)))))))

(deftest net-read-all-reads-until-eof
  (with-server :payload
    (fn [srv]
      (let [s (net-connect "127.0.0.1" (:port srv))]
        (is (= "30313233343536373839" (hex-encode (net-read-all s))))
        ;; EOF seen again: the drained stream yields empty bytes.
        (is (= 0 (count (net-read-all s))))
        (is (nil? (net-close s)))))))

(deftest net-read-all-cap-throws-overflow
  (with-server :payload
    (fn [srv]
      (let [s (net-connect "127.0.0.1" (:port srv))
            r (try (net-read-all s {:max-bytes 4}) (catch e e))]
        (is (= :net/overflow (:mino/kind r)))
        (is (str/includes? (:mino/message r) "max-bytes"))
        (net-close s)))))

(deftest net-read-all-cap-exact-length-stream-succeeds
  ;; The cap check is strict-after-read: a stream exactly at the
  ;; cap drains to EOF and returns; only data past the cap throws.
  (with-server :payload
    (fn [srv]
      (let [s (net-connect "127.0.0.1" (:port srv))]
        (is (= "30313233343536373839"
               (hex-encode (net-read-all s {:max-bytes 10}))))
        (net-close s)))))

(deftest net-read-clean-eof-returns-nil
  (with-server :shut
    (fn [srv]
      (let [s (net-connect "127.0.0.1" (:port srv))]
        (is (nil? (net-read s 10)))
        (is (nil? (net-close s)))))))

(deftest net-read-timeout-classifies-and-fires-on-schedule
  (with-server :slow
    (fn [srv]
      (let [s (net-connect "127.0.0.1" (:port srv)
                           {:read-timeout 400})
            t0 (time-ms)
            r  (try (do (net-write s "x")
                        (net-read s 10))
                    (catch e e))
            dt (- (time-ms) t0)]
        ;; The server stays silent after consuming the request; a
        ;; working timeout surfaces at ~400ms. The elapsed bound is
        ;; what gives the kind assertion teeth.
        (is (= :net/timeout (:mino/kind r)))
        (is (< dt 2400) (str "read timeout fired at " dt " ms"))
        (net-close s)))))

(deftest net-connect-refused-classifies-as-net-connect
  (let [port (free-dead-port)
        r (try (net-connect "127.0.0.1" port {:connect-timeout 2000})
               (catch e e))]
    (is (= :net/connect (:mino/kind r)))
    ;; Refusal says "cannot connect"; a firewall that DROPs to the
    ;; unbound port surfaces the connect deadline instead. The kind
    ;; plus the addressed port are the invariants either way.
    (is (str/includes? (:mino/message r) "127.0.0.1"))))

(deftest net-connect-timeout-classifies-as-net-connect
  ;; A non-routable address: the SYN gets no answer, so the connect
  ;; deadline fires on the poll-wait path (the fd there was once
  ;; leaked; the sanitizer lanes are the leak oracle, this test pins
  ;; the classification). Some CI networks answer anything; a
  ;; surprise connection means the fixture cannot work there, so it
  ;; skips rather than fails.
  (let [r (try (net-connect "10.255.255.1" 81 {:connect-timeout 250})
               (catch e e))]
    (if (= :handle (type r))
      (do (net-close r)
          (println "  skip: 10.255.255.1 is reachable on this network"))
      (do
        (is (= :net/connect (:mino/kind r)))
        (is (str/includes? (:mino/message r) "timed out"))))))

(deftest net-close-is-idempotent-and-marks-the-socket
  (with-server :echo
    (fn [srv]
      (let [s (net-connect "127.0.0.1" (:port srv))]
        (net-write s "x")
        (is (nil? (net-close s)))
        (is (nil? (net-close s)))
        (is (= :net
               (try (net-read s 1) (catch e (:mino/kind e)))))
        (is (= :net
               (try (net-write s "y") (catch e (:mino/kind e)))))
        (is (= :net
               (try (net-read-all s) (catch e (:mino/kind e)))))))))

(deftest net-socket-finalizer-closes-dropped-sockets
  ;; Drop an open socket and force a full collection; the handle
  ;; finalizer must close the descriptor without disturbing the
  ;; server or the runtime. A conservative stack pin may delay the
  ;; collection; the server keeps serving regardless (a dropped
  ;; connection ages out of its idle window and the next one is
  ;; accepted). The sanitizer lanes are the leak oracle.
  (with-server :echo
    (fn [srv]
      ((fn []
         (let [doomed (net-connect "127.0.0.1" (:port srv))]
           (net-write doomed "x")
           :dropped)))
      (gc!)
      (let [s (net-connect "127.0.0.1" (:port srv)
                           {:read-timeout 8000})]
        (is (= 2 (net-write s "ok")))
        (is (= "6f6b" (hex-encode (net-read s 2))))
        (is (nil? (net-close s)))))))

(defn- read-n
  "Read exactly n bytes from a socket, folding short reads together.
  Throws if the peer closes before n bytes arrive."
  [sock n]
  (loop [acc []]
    (if (= n (count acc))
      (byte-array acc)
      (let [chunk (net-read sock (- n (count acc)))]
        (if (nil? chunk)
          (throw (str "unexpected EOF after " (count acc) " of " n " bytes"))
          (recur (into acc (seq chunk))))))))

;; ---- server prims: capability metadata ----

(deftest net-listener-prims-labelled-with-net-capability
  (is (= :net (mino-capability 'net-listen)))
  (is (= :net (mino-capability 'net-accept)))
  (is (= :net (mino-capability 'net-listener-port))))

;; ---- server prims: argument shapes (no connection needed) ----

(deftest net-listen-validates-arguments
  (is (thrown? (net-listen)))
  (is (thrown? (net-listen "127.0.0.1")))
  (is (= :eval/type
         (try (net-listen 42 80) (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (net-listen "127.0.0.1" -1) (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (net-listen "127.0.0.1" 70000) (catch e (:mino/kind e)))))
  (is (= :eval/type
         (try (net-listen "127.0.0.1" 80 :not-a-map) (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (net-listen "127.0.0.1" 80 {:backlog 0}) (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (net-listen "127.0.0.1" 80 {:backlog -4}) (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (net-listen "127.0.0.1" 80 {:backlog "many"})
              (catch e (:mino/kind e)))))
  ;; port 0 asks the kernel to choose; the empty-string wildcard
  ;; binds the IPv4 wildcard address.
  (let [l (net-listen "" 0 {})]
    (is (>= (net-listener-port l) 1))
    (is (nil? (net-close l)))))

(deftest net-listener-prims-validate-arguments
  (let [l (net-listen "127.0.0.1" 0 {})]
    (is (= :eval/type
           (try (net-accept "not-a-listener") (catch e (:mino/kind e)))))
    (is (= :eval/type
           (try (net-accept 7) (catch e (:mino/kind e)))))
    (is (= :eval/type
           (try (net-accept l :not-a-map) (catch e (:mino/kind e)))))
    (is (= :eval/contract
           (try (net-accept l {:accept-timeout -1}) (catch e (:mino/kind e)))))
    (is (= :eval/type
           (try (net-listener-port :keyword) (catch e (:mino/kind e)))))
    (is (thrown? (net-listener-port)))
    ;; A listener is not a socket: the io prims reject it.
    (is (= :eval/type
           (try (net-read l 1) (catch e (:mino/kind e)))))
    (net-close l)))

;; ---- server prims: in-process loopback behaviour ----

(deftest net-listen-accept-echo-round-trip
  (let [port-p (promise)
        done-p (promise)]
    (future
      (let [l (net-listen "127.0.0.1" 0 {})]
        (deliver port-p (net-listener-port l))
        (let [c (net-accept l {})]
          (net-write c (read-n c 11))
          (net-close c))
        (net-close l)
        (deliver done-p :done)))
    (let [port (deref port-p 10000 ::timeout)
          s (net-connect "127.0.0.1" port)]
      (is (not= ::timeout port))
      ;; net-listener-port reported the kernel-chosen port: the
      ;; connection succeeding to it is the oracle.
      (is (and (>= port 1) (<= port 65535)))
      (is (= 11 (net-write s "hello world")))
      (is (= "68656c6c6f20776f726c64" (hex-encode (read-n s 11))))
      (net-close s)
      (is (= :done (deref done-p 10000 ::timeout))))))

(deftest net-listen-accept-serves-two-sequential-connections
  (let [port-p (promise)
        done-p (promise)]
    (future
      (let [l (net-listen "127.0.0.1" 0 {})]
        (deliver port-p (net-listener-port l))
        (let [c1 (net-accept l {})]
          (net-write c1 (read-n c1 3))
          (net-close c1))
        (let [c2 (net-accept l {})]
          (net-write c2 (read-n c2 3))
          (net-close c2))
        (net-close l)
        (deliver done-p :done)))
    (let [port (deref port-p 10000 ::timeout)]
      (is (not= ::timeout port))
      (let [s1 (net-connect "127.0.0.1" port)]
        (is (= 3 (net-write s1 "one")))
        (is (= "6f6e65" (hex-encode (read-n s1 3))))
        (net-close s1))
      (let [s2 (net-connect "127.0.0.1" port)]
        (is (= 3 (net-write s2 "two")))
        (is (= "74776f" (hex-encode (read-n s2 3))))
        (net-close s2))
      (is (= :done (deref done-p 10000 ::timeout))))))

(deftest net-listen-wildcard-host-accepts-loopback
  ;; "*" and "" bind INADDR_ANY; a loopback connection reaches it.
  (let [port-p (promise)
        done-p (promise)]
    (future
      (let [l (net-listen "*" 0 {})]
        (deliver port-p (net-listener-port l))
        (let [c (net-accept l {})]
          (net-write c (read-n c 4))
          (net-close c))
        (net-close l)
        (deliver done-p :done)))
    (let [port (deref port-p 10000 ::timeout)
          s (net-connect "127.0.0.1" port)]
      (is (not= ::timeout port))
      (is (= 4 (net-write s "ping")))
      (is (= "70696e67" (hex-encode (read-n s 4))))
      (net-close s)
      (is (= :done (deref done-p 10000 ::timeout))))))

(deftest net-accept-timeout-classifies-and-fires-on-schedule
  (let [l (net-listen "127.0.0.1" 0 {})
        t0 (time-ms)
        r  (try (net-accept l {:accept-timeout 300}) (catch e e))
        dt (- (time-ms) t0)]
    ;; Nothing connects: the accept deadline fires at ~300ms. The
    ;; elapsed bound is what gives the kind assertion teeth.
    (is (= :net/timeout (:mino/kind r)))
    (is (str/includes? (:mino/message r) "accept"))
    (is (< dt 2400) (str "accept timeout fired at " dt " ms"))
    (net-close l)))

(deftest net-close-closes-listeners-idempotently
  (let [l (net-listen "127.0.0.1" 0 {})]
    (is (= :handle (type l)))
    (is (nil? (net-close l)))
    (is (nil? (net-close l)))
    (is (= :net
           (try (net-listener-port l) (catch e (:mino/kind e)))))
    (is (= :net
           (try (net-accept l) (catch e (:mino/kind e)))))))

(deftest net-listener-finalizer-closes-dropped-listeners
  ;; Drop an open listener and force a full collection; the handle
  ;; finalizer must close the descriptor without disturbing the
  ;; runtime. A conservative stack pin may delay the collection. The
  ;; sanitizer lanes are the leak oracle; a fresh listener still
  ;; serving an echo is the behaviour oracle.
  ((fn []
     (let [doomed (net-listen "127.0.0.1" 0 {})]
       :dropped)))
  (gc!)
  (let [port-p (promise)
        done-p (promise)]
    (future
      (let [l (net-listen "127.0.0.1" 0 {})]
        (deliver port-p (net-listener-port l))
        (let [c (net-accept l {})]
          (net-write c (read-n c 2))
          (net-close c))
        (net-close l)
        (deliver done-p :done)))
    (let [port (deref port-p 10000 ::timeout)
          s (net-connect "127.0.0.1" port)]
      (is (not= ::timeout port))
      (is (= 2 (net-write s "ok")))
      (is (= "6f6b" (hex-encode (read-n s 2))))
      (net-close s)
      (is (= :done (deref done-p 10000 ::timeout))))))

(deftest net-accept-presets-read-timeout-on-accepted-socket
  ;; The client connects and then sends nothing; the accepted
  ;; socket's read timeout, preset from net-accept's :read-timeout,
  ;; must fire server-side.
  (let [port-p (promise)
        kind-p (promise)]
    (future
      (let [l (net-listen "127.0.0.1" 0 {})]
        (deliver port-p (net-listener-port l))
        (let [c (net-accept l {:read-timeout 400})]
          (deliver kind-p (try (net-read c 10) (catch e (:mino/kind e))))
          (net-close c)
          (net-close l))))
    (let [port (deref port-p 10000 ::timeout)
          s  (net-connect "127.0.0.1" port)
          t0 (time-ms)
          kind (deref kind-p 10000 ::timeout)
          dt (- (time-ms) t0)]
      (net-close s)
      (is (not= ::timeout port))
      (is (= :net/timeout kind))
      (is (< dt 2400) (str "accepted-socket read timeout fired at " dt " ms")))))

(run-tests-and-exit)
