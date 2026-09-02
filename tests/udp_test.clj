(require "tests/test")

;; UDP datagram sockets and DNS lookup: (udp-socket opts) binds a
;; loopback datagram socket (port 0 or omitted asks the kernel);
;; udp-socket-port reads the bound port back; udp-send ships a string
;; or bytes payload; udp-recv returns the datagram as a public map
;; {:data :address :port :truncated?}. dns-lookup resolves a host to a
;; vector of {:address :family} maps. Everything installs under the
;; :udp capability group.
;;
;; Fixtures are loopback pairs built in process: a receiver socket
;; with a bounded :read-timeout so a lost datagram fails the test
;; instead of hanging the suite, and a sender whose bound port is the
;; oracle for the recv map's sender address.

;; ---- capability metadata ----

(deftest udp-prims-labelled-with-udp-capability
  (is (= :udp (mino-capability 'udp-socket)))
  (is (= :udp (mino-capability 'udp-send)))
  (is (= :udp (mino-capability 'udp-recv)))
  (is (= :udp (mino-capability 'udp-close)))
  (is (= :udp (mino-capability 'udp-socket-port)))
  (is (= :udp (mino-capability 'dns-lookup))))

;; ---- ephemeral bind ----

(deftest udp-socket-port-reads-back-ephemeral-bind
  ;; No port and an explicit :port 0 both take a kernel-chosen
  ;; ephemeral port, readable back through udp-socket-port.
  (let [a (udp-socket)
        b (udp-socket {:host "127.0.0.1" :port 0 :read-timeout 5000})]
    (try
      (doseq [s [a b]]
        (let [p (udp-socket-port s)]
          (is (integer? p))
          (is (>= p 1))
          (is (<= p 65535))))
      (is (not= (udp-socket-port a) (udp-socket-port b)))
      ;; The reported port is real: a datagram sent to it arrives.
      (is (= 2 (udp-send a "127.0.0.1" (udp-socket-port b) "ok")))
      (is (= "6f6b" (hex-encode (:data (udp-recv b)))))
      (finally
        (udp-close a)
        (udp-close b)))))

;; ---- loopback round trip and the recv map contract ----

(deftest udp-loopback-round-trip-pins-recv-map-keys
  (let [rx (udp-socket {:read-timeout 5000})
        tx (udp-socket)]
    (try
      (is (= 5 (udp-send tx "127.0.0.1" (udp-socket-port rx) "hello")))
      (let [m (udp-recv rx)]
        ;; The recv map is a public contract: every documented key,
        ;; present and typed, with the loopback sender as the oracle.
        (is (map? m))
        (is (contains? m :data))
        (is (contains? m :address))
        (is (contains? m :port))
        (is (contains? m :truncated?))
        (is (true? (bytes? (:data m))))
        (is (= "68656c6c6f" (hex-encode (:data m))))
        (is (string? (:address m)))
        (is (= "127.0.0.1" (:address m)))
        (is (integer? (:port m)))
        (is (= (udp-socket-port tx) (:port m)))
        (is (false? (:truncated? m))))
      (finally
        (udp-close rx)
        (udp-close tx)))))

(deftest udp-send-accepts-bytes-payload
  (let [rx (udp-socket {:read-timeout 5000})
        tx (udp-socket)
        payload (byte-array [0 1 2 250 255])]
    (try
      (is (= 5 (udp-send tx "127.0.0.1" (udp-socket-port rx) payload)))
      (let [m (udp-recv rx)]
        (is (= payload (:data m)))
        (is (= (udp-socket-port tx) (:port m))))
      (finally
        (udp-close rx)
        (udp-close tx)))))

(deftest udp-send-encodes-strings-as-utf8
  (let [rx (udp-socket {:read-timeout 5000})
        tx (udp-socket)]
    (try
      ;; a (1) + é (2) + ☃ (3) bytes.
      (is (= 6 (udp-send tx "127.0.0.1" (udp-socket-port rx) "aé☃")))
      (is (= "61c3a9e29883" (hex-encode (:data (udp-recv rx)))))
      (finally
        (udp-close rx)
        (udp-close tx)))))

;; ---- truncation policy ----

(deftest udp-recv-truncates-oversized-datagram-and-drops-excess
  ;; A datagram over the recv cap comes back cut to the cap with
  ;; :truncated? true; the excess is dropped with the datagram, not
  ;; queued for the next read (standard datagram truncation).
  (let [rx (udp-socket {:read-timeout 5000})
        tx (udp-socket)
        port (udp-socket-port rx)]
    (try
      (is (= 10 (udp-send tx "127.0.0.1" port "0123456789")))
      (let [m (udp-recv rx {:max-bytes 4})]
        (is (= 4 (count (:data m))))
        (is (= "30313233" (hex-encode (:data m))))
        (is (true? (:truncated? m))))
      ;; The next recv sees the next datagram, never the cut tail.
      (is (= 4 (udp-send tx "127.0.0.1" port "tail")))
      (let [m (udp-recv rx)]
        (is (= "7461696c" (hex-encode (:data m))))
        (is (false? (:truncated? m))))
      (finally
        (udp-close rx)
        (udp-close tx)))))

(deftest udp-recv-datagram-at-cap-arrives-whole
  ;; Exactly at the cap is not truncation: full data, flag false.
  (let [rx (udp-socket {:read-timeout 5000})
        tx (udp-socket)]
    (try
      (is (= 4 (udp-send tx "127.0.0.1" (udp-socket-port rx) "0123")))
      (let [m (udp-recv rx {:max-bytes 4})]
        (is (= "30313233" (hex-encode (:data m))))
        (is (false? (:truncated? m))))
      (finally
        (udp-close rx)
        (udp-close tx)))))

;; ---- recv timeout ----

(deftest udp-recv-socket-timeout-throws-error-data
  ;; Nothing is sent: the socket-level :read-timeout fires at ~300ms
  ;; as a :mino/kind data error. The elapsed bound is what gives the
  ;; throw assertion teeth.
  (let [rx (udp-socket {:read-timeout 300})]
    (try
      (let [t0 (time-ms)
            r  (try (udp-recv rx) (catch Throwable e e))
            dt (- (time-ms) t0)]
        (is (keyword? (:mino/kind r)))
        (is (< dt 2400) (str "recv timeout fired at " dt " ms")))
      (finally
        (udp-close rx)))))

(deftest udp-recv-read-timeout-opt-overrides-socket-setting
  ;; The per-call opt wins over the generous socket default; a
  ;; working override fires at ~250ms, a broken one trips the
  ;; elapsed bound instead of hanging the suite.
  (let [rx (udp-socket {:read-timeout 30000})]
    (try
      (let [t0 (time-ms)
            r  (try (udp-recv rx {:read-timeout 250}) (catch Throwable e e))
            dt (- (time-ms) t0)]
        (is (keyword? (:mino/kind r)))
        (is (< dt 2400) (str "recv opt timeout fired at " dt " ms")))
      (finally
        (udp-close rx)))))

;; ---- dns-lookup ----

(deftest dns-lookup-localhost-returns-address-maps
  (let [rs (dns-lookup "localhost")]
    (is (vector? rs))
    (is (pos? (count rs)))
    ;; The address map is a public contract: every documented key,
    ;; present and typed, on every entry.
    (doseq [r rs]
      (is (map? r))
      (is (contains? r :address))
      (is (contains? r :family))
      (is (string? (:address r)))
      (is (contains? #{:inet :inet6} (:family r))))
    (is (some #{"127.0.0.1" "::1"} (map :address rs)))))

(deftest dns-lookup-unknown-host-throws-error-data
  ;; The .invalid TLD is RFC-reserved to never resolve. The failure
  ;; classifies as :net/dns, the kind resolver failures already carry.
  (let [r (try (dns-lookup "no-such-host.invalid")
               (catch Throwable e e))]
    (is (= :net/dns (:mino/kind r)))))

(run-tests-and-exit)
