(require "tests/test")
(require '[clojure.string :as str])

;; TLS client layer (tls-connect / tls-read / tls-read-all / tls-write /
;; tls-close) over the vendored BearSSL engine. Verification defaults
;; ON: chain against the vendored Mozilla roots plus SAN/CN host
;; match, with SNI always sent.
;;
;; The vendored engine is client-only, so every case needing a live
;; TLS server (verified handshakes, wrong-host and expired refusals,
;; insecure-pass, byte fidelity, read timeouts against a live peer)
;; lives in the mino-tests satellite repo against its own cert
;; fixtures; the port manifest is
;; ~/.agentic-sdk/mino/runs/http-client/moved-tls-tests.md. The cert
;; files under tests/fixtures/tls stay for that repo.
;;
;; What remains here runs against in-process loopback listeners built
;; from mino's own net prims: adversarial handshakes (seeded garbage,
;; plaintext on a TLS port, immediate EOF) and the argument-shape and
;; classification surface. Every platform the CLI builds on runs the
;; full file.

(defn- tls-serve-one
  "Serve one accepted connection for the adversarial mode: :garbage
  writes len pseudo-random bytes derived from the connection's seed,
  :plain writes a canned plaintext HTTP response, :eof closes without
  writing. The ClientHello is drained first: closing a socket with
  unread inbound data sends a reset, which would discard the outbound
  payload before the client reads it. The connection always closes
  after the payload so no worker is pinned per connection."
  [mode c idx]
  (try
    (do
      (try (net-read c 65536) (catch e nil))
      (case mode
        :garbage (net-write c (tls-garbage-bytes (* (+ idx 1) 1000003) 64))
        :plain (net-write c (str "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                                 "Connection: close\r\n\r\nhi"))
        :eof nil))
    (catch e nil))
  (try (net-close c) (catch e nil)))

(defn- tls-start-server
  "Adversarial-handshake fixture (:garbage :plain :eof): a serial
  accept loop in a future. Returns the port and a stop fn with
  guaranteed teardown."
  [mode]
  (let [l (net-listen "127.0.0.1" 0 {})
        running? (atom true)
        idx (atom 0)
        port (net-listener-port l)
        fut (future
              (loop []
                (when @running?
                  (try
                    (let [c (net-accept l {:accept-timeout 250
                                           :read-timeout 1500})]
                      (tls-serve-one mode c (swap! idx inc)))
                    (catch e nil))
                  (recur))))
        stop (fn []
               (reset! running? false)
               (try (net-close l) (catch e nil))
               (deref fut 4000 ::stopped))]
    {:port port :stop stop}))

(defn- tls-with-server
  [mode body]
  (let [srv (tls-start-server mode)]
    (try
      (body srv)
      (finally
        ((:stop srv))))))

(defn- tls-free-dead-port
  "A loopback port that was bound and then released, so connecting to
  it is refused rather than timing out."
  []
  (let [l (net-listen "127.0.0.1" 0 {})
        p (net-listener-port l)]
    (net-close l)
    p))

(defn- tls-garbage-bytes
  "Deterministic pseudo-random byte payload: a plain LCG over a
  31-bit state, one byte per step, so every run feeds the handshake
  the same adversarial bytes for a given seed."
  [seed n]
  (byte-array
    (loop [i 0, x (mod seed 2147483648), acc []]
      (if (= i n)
        acc
        (let [x2 (mod (+ (* x 1103515245) 12345) 2147483648)]
          (recur (inc i) x2 (conj acc (mod x2 256))))))))

;; Fast opts: the behaviour suite should not sit in default timeouts
;; anywhere.
(def ^:private t-opts {:insecure? true :connect-timeout 3000
                       :read-timeout 3000 :write-timeout 3000})

;; ---- capability metadata ----

(deftest tls-prims-labelled-with-net-capability
  (is (= :net (mino-capability 'tls-connect)))
  (is (= :net (mino-capability 'tls-read)))
  (is (= :net (mino-capability 'tls-read-all)))
  (is (= :net (mino-capability 'tls-write)))
  (is (= :net (mino-capability 'tls-close))))

;; ---- argument shapes (no server; every platform) ----

(deftest tls-connect-validates-arguments
  (is (thrown? (tls-connect)))
  (is (= :eval/type
         (try (tls-connect 42 "localhost") (catch e (:mino/kind e)))))
  (is (= :eval/type
         (try (tls-connect :keyword "localhost")
              (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (tls-connect "127.0.0.1" "not-a-port")
              (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (tls-connect "127.0.0.1" :keyword)
              (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (tls-connect "127.0.0.1" 70000)
              (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (tls-connect "127.0.0.1" 80 {:read-timeout -1})
              (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (tls-connect "127.0.0.1" 80 {:insecure? "yes"})
              (catch e (:mino/kind e)))))
  (is (= :eval/type
         (try (tls-connect "127.0.0.1" 80 0)
              (catch e (:mino/kind e))))))

(deftest tls-io-prims-validate-socket-arguments
  (is (= :eval/type
         (try (tls-read "not-a-socket" 1) (catch e (:mino/kind e)))))
  (is (= :eval/type
         (try (tls-read-all 7) (catch e (:mino/kind e)))))
  (is (= :eval/type
         (try (tls-write 7 "x") (catch e (:mino/kind e)))))
  (is (= :eval/type
         (try (tls-close :keyword) (catch e (:mino/kind e))))))

;; ---- adversarial and classified handshakes (in process) ----

(deftest tls-seeded-garbage-handshakes-never-crash
  ;; Untrusted-input discipline: pseudo-random bytes as the server's
  ;; first write must classify as a :tls error, never a crash.
  (tls-with-server :garbage
    (fn [srv]
      (let [lens [1 5 16 64 200 1000 5000]]
        (dotimes [i 2000]
          (let [len (nth lens (mod i (count lens)))
                r   (try (tls-connect
                           (net-connect "127.0.0.1" (:port srv)
                                        {:connect-timeout 3000
                                         :read-timeout 500
                                         :write-timeout 500})
                           "localhost"
                           {:insecure? true :read-timeout 500
                            :write-timeout 500})
                          (catch Throwable e e))]
            (is (= :tls (:mino/kind r))
                (str "iteration " i " len " len " got "
                     (pr-str r)))))))))

(deftest tls-plain-http-on-tls-port-refused
  ;; A plaintext HTTP response where the handshake expects TLS records
  ;; must classify as :tls, not surface as HTTP or a crash.
  (tls-with-server :plain
    (fn [srv]
      (let [r (try (tls-connect (net-connect "127.0.0.1"
                                             (:port srv) t-opts)
                                "localhost"
                                (dissoc t-opts :insecure?))
                   (catch Throwable e e))]
        (is (= :tls (:mino/kind r)))))))

(deftest tls-eof-during-handshake-classifies-as-tls
  ;; The peer accepts then closes without writing: the handshake must
  ;; surface the truncation as :tls, never hang or crash.
  (tls-with-server :eof
    (fn [srv]
      (let [r (try (tls-connect (net-connect "127.0.0.1"
                                             (:port srv) t-opts)
                                "localhost"
                                {:insecure? true :connect-timeout 3000
                                 :read-timeout 3000 :write-timeout 3000})
                   (catch Throwable e e))]
        (is (= :tls (:mino/kind r)))
        (is (str/includes? (:mino/message r) "closed by peer"))))))

(deftest tls-connect-refused-passthrough-classifies-as-net-connect
  ;; Nothing is listening: the underlying connect refusal passes
  ;; through the TLS layer unrewritten. Refusal says "cannot
  ;; connect"; a firewall DROP surfaces the connect deadline; the
  ;; kind and the addressed host are the invariants either way.
  (let [port (tls-free-dead-port)
        r (try (tls-connect "127.0.0.1" port t-opts)
               (catch Throwable e e))]
    (is (= :net/connect (:mino/kind r)))
    (is (str/includes? (:mino/message r) "127.0.0.1"))))

(run-tests-and-exit)
