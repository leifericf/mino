(require "tests/test")
(require '[mino.http.server :as srv])

;; The live server under malformed and hostile traffic: random byte
;; soups, cap-boundary truncations, pipelined garbage, peer
;; mid-request closes, conflicting framing, and oversized chunk
;; framing, every leg through a real loopback socket into serve-conn*.
;; The properties: every hostile input ends classified (a response, a
;; close, or a deadline drop), the engine never crashes, never hangs,
;; and the next connection is always served.

;; The soup generators mirror the codec suite's xorshift battery
;; (same constants, same alphabets); a test file cannot require
;; another: each ends in run-tests-and-exit, which would run and exit
;; the process mid-load on a standalone invocation.

(defn- fz-xorshift [x]
  (let [x (bit-xor x (bit-shift-left x 13))
        x (bit-xor x (unsigned-bit-shift-right x 7))
        x (bit-xor x (bit-shift-left x 17))]
    (bit-and x 0x7FFFFFFFFFFFFFFF)))

(defn- fz-range [x lo hi] (+ lo (rem x (- hi lo))))

(def ^:private fz-alphabets
  [(vec (map int "GETPOST * HTTP/1.01\r\n:;/\r\n"))
   (vec (map int "0123456789abcdefABCDEF\r\n; "))
   (vec (range 0 256))])

(defn- fz-bb [& ss]
  (byte-array (mapcat #(if (bytes? %) (vec %) (map int %)) ss)))

(defn- fz-conn [port]
  (net-connect "127.0.0.1" port {:read-timeout 3000 :write-timeout 3000}))

(defn- fz-send [c & parts]
  (try (net-write c (apply fz-bb parts)) (catch e nil)))

(defn- fz-drop-outcome
  "One bounded read classifying how a connection that was never
  served ends: :eof a clean close, :reset an aborted close, :held
  still open when the read budget lapsed. Response bytes answer
  :served and belong to the caller."
  [c]
  (try
    (let [r (net-read c 65536)]
      (if (and r (pos? (count r))) :served :eof))
    (catch e
      (if (= :net/timeout (:mino/kind e)) :held :reset))))

(defn- fz-read-all
  "Everything the peer still sends, seeded with bytes already read,
  until it closes or the budget lapses."
  ([c] (fz-read-all c []))
  ([c seed]
   (loop [acc seed]
     (let [r (try (net-read c 65536) (catch e nil))]
       (if (bytes? r) (recur (into acc (vec r))) acc)))))

(defn- fz-response-code
  "Parse one full response out of accumulated bytes against end of
  stream; nil when the bytes never formed one."
  [acc]
  (let [r (http-parse-response (byte-array acc) {:eof true})]
    (when (= :done (:status r)) (:code r))))

(def ^:private fz-default-opts
  ;; a short idle budget so a soup that randomly forms a valid
  ;; keep-alive request releases its connection quickly; the next
  ;; soup waits in the kernel backlog until it does
  {:idle-timeout 1000 :request-timeout 400})

(defn- fz-await-slot
  "Wait for a free host-thread slot before spawning the acceptor
  future. A finished worker's slot is released a beat after the future
  that held it resolves, so on a loaded low-core CI runner a
  back-to-back fixture can otherwise spawn into a momentarily-full grant
  and throw MTH001. Bounded so a genuine exhaustion still surfaces."
  []
  (loop [n 300]
    (when (and (>= (mino-thread-count) (mino-thread-limit)) (pos? n))
      (thread-sleep 10)
      (recur (dec n)))))

(defn- fz-with
  "Run (body started) against a fresh listener serving n connections
  through serve-conn*, one after another inside a single future.
  Every served connection's engine outcome lands in :results; a
  :engine-crash entry means the engine let an exception escape."
  [n handler opts body]
  (fz-await-slot)
  (let [l (net-listen "127.0.0.1" 0 {:backlog 16})
        o (merge fz-default-opts opts)
        running? (atom true)
        results (atom [])
        fut (future
              (loop [i 0]
                (when (and @running? (< i n))
                  (let [c (try (net-accept l {:accept-timeout 250
                                              :read-timeout 50
                                              :write-timeout 5000})
                               (catch e nil))]
                    (when c
                      (swap! results conj
                             (try (srv/serve-conn* c handler o)
                                  (catch e :engine-crash)))
                      (try (net-close c) (catch e nil)))
                    ;; Count served connections, not accept attempts, so a
                    ;; slow runner whose client lands after the accept
                    ;; window is still served (running? bounds the retry).
                    (recur (if c (inc i) i)))))
              (try (net-close l) (catch e nil))
              :served)
        started {:port (net-listener-port l)}]
    (try
      (body started)
      (finally
        (reset! running? false)
        (try (net-close l) (catch e nil))
        (try (deref fut 20000 :join-timeout) (catch e nil))))
    {:join (try (deref fut 20000 :join-timeout) (catch e :future-error))
     :results @results}))

(defn- fz-was-clean
  "The accept loop finished on its own and no connection crashed the
  engine."
  [r]
  (is (= :served (:join r)))
  (is (not-any? #{:engine-crash} (:results r))))

(def ^:private fz-ok (fn [req] {:status 200 :body "ok"}))

(defn- fz-canary
  "One well-formed request answered 200 after a hostile leg: the
  engine is still serving."
  [port]
  (let [c (fz-conn port)]
    (fz-send c "GET /canary HTTP/1.1\r\nHost: t.example\r\n"
             "Connection: close\r\n\r\n")
    (let [code (fz-response-code (fz-read-all c))]
      (try (net-close c) (catch e nil))
      (is (= 200 code) "a well-formed request after hostile traffic is served"))))

;;;; random byte soups through a real socket

(defn- fz-soups
  "Bounded sequence of soup byte vectors."
  [n seed]
  (loop [i 0, x seed, acc []]
    (if (= i n)
      acc
      (let [x1 (fz-xorshift x)
            len (fz-range x1 0 64)
            alphabet (nth fz-alphabets (rem x1 3))
            soup (loop [j 0, y x1, s []]
                   (if (= j len)
                     s
                     (let [y2 (fz-xorshift y)]
                       (recur (inc j) y2
                              (conj s (nth alphabet
                                           (rem y2 (count alphabet))))))))
            x2 (fz-xorshift x1)]
        (recur (inc i) x2 (conj acc soup))))))

(deftest byte-soups-through-a-live-socket-end-classified
  (let [soups (fz-soups 90 42424242424242424)
        held (atom [])
        weird (atom [])
        r (fz-with (inc (count soups)) fz-ok {}
             (fn [s]
               (doseq [pair (map-indexed vector soups)]
                 (let [i (nth pair 0)
                       c (fz-conn (:port s))]
                   (fz-send c (byte-array (nth pair 1)))
                   (let [t0 (time-ms)
                         first (try (net-read c 65536)
                                    (catch e
                                      (if (= :net/timeout (:mino/kind e))
                                        :held :reset)))]
                     (cond
                       ;; whatever came back must be one parseable
                       ;; response with a legal status
                       (bytes? first)
                       (let [code (fz-response-code
                                    (fz-read-all c (vec first)))]
                         (when-not (and (int? code) (<= 100 code 599))
                           (swap! weird conj [i code])))

                       (= :held first)
                       (swap! held conj i)

                       ;; nil is a clean close, anything else a reset:
                       ;; both are classified ends
                       :else nil)
                     (is (< (- (time-ms) t0) 6000)
                         (str "soup " i " took too long to end")))
                   (try (net-close c) (catch e nil))))
               (fz-canary (:port s))))]
    (fz-was-clean r)
    (is (= [] @held) (str "soups held open past every budget: " @held))
    (is (= [] @weird)
        (str "soups answered unparseable bytes: " @weird))))

;;;; cap boundaries: truncated and over-cap prefixes

(defn- fz-prefix
  "A valid-shaped request head whose single padding header runs long,
  cut at exactly n total section bytes with no terminator."
  [n]
  (let [head "GET /t HTTP/1.1\r\nX-Pad: "
        pad (- n (count head))]
    (assert (pos? pad))
    (fz-bb head (apply str (repeat pad "a")))))

(deftest header-section-cap-boundary-is-exact-on-the-wire
  ;; a prefix at or under the cap never completes and dies at the
  ;; deadline; a prefix over the cap is refused at once
  (let [cap 128
        r (fz-with 5 fz-ok {:max-header-bytes cap}
             (fn [s]
               (doseq [k [(- cap 16) cap]]
                 (let [c (fz-conn (:port s))]
                   (fz-send c (fz-prefix k))
                   (let [t0 (time-ms)
                         out (fz-drop-outcome c)]
                     (is (contains? #{:eof :reset} out)
                         (str "prefix at " k " of the cap is dropped at the deadline, got " out))
                     (is (< (- (time-ms) t0) 6000)))
                   (try (net-close c) (catch e nil))))
               (doseq [k [(inc cap) (+ cap 16)]]
                 (let [c (fz-conn (:port s))]
                   (fz-send c (fz-prefix k))
                   (let [code (fz-response-code (fz-read-all c))]
                     (is (= 400 code)
                         (str "prefix at " k " of the cap is refused")))
                   (try (net-close c) (catch e nil))))
               (fz-canary (:port s))))]
    (fz-was-clean r)))

(deftest body-cap-boundary-is-exact-on-the-wire
  ;; a content length at the cap is served; one over is refused at
  ;; the header; a body truncated under it dies at the deadline
  (let [r (fz-with 4 fz-ok {:max-body-bytes 64}
             (fn [s]
               (let [c (fz-conn (:port s))]
                 (fz-send c "POST /at HTTP/1.1\r\nHost: t.example\r\n"
                          "Connection: close\r\n"
                          "Content-Length: 64\r\n\r\n"
                          (apply str (repeat 64 "b")))
                 (let [code (fz-response-code (fz-read-all c))]
                   (is (= 200 code) "the at-cap body is served"))
                 (try (net-close c) (catch e nil)))
               (let [c (fz-conn (:port s))]
                 (fz-send c "POST /over HTTP/1.1\r\nHost: t.example\r\n"
                          "Content-Length: 65\r\n\r\n"
                          (apply str (repeat 65 "b")))
                 (let [code (fz-response-code (fz-read-all c))]
                   (is (= 400 code)
                       "the over-cap length is refused at the header"))
                 (try (net-close c) (catch e nil)))
               (let [c (fz-conn (:port s))]
                 (fz-send c "POST /cut HTTP/1.1\r\nHost: t.example\r\n"
                          "Content-Length: 64\r\n\r\n"
                          (apply str (repeat 63 "b")))
                 (let [t0 (time-ms)
                       out (fz-drop-outcome c)]
                   (is (contains? #{:eof :reset} out)
                       (str "the truncated body is dropped at the deadline, got " out))
                   (is (< (- (time-ms) t0) 6000)))
                 (try (net-close c) (catch e nil)))
               (fz-canary (:port s))))]
    (fz-was-clean r)))

(deftest header-count-cap-boundary-is-exact-on-the-wire
  ;; four rows fit and are served; a fifth row is refused the moment
  ;; it arrives; a fifth row cut mid-name dies at the deadline
  (let [rows (fn [n] (apply str (map #(str "X-" % ": " % "\r\n") (range n))))
        r (fz-with 4 fz-ok {:max-headers 4}
             (fn [s]
               ;; HTTP/1.0 closes by default, so the served leg ends
               ;; without holding the connection for its idle budget
               (let [c (fz-conn (:port s))]
                 (fz-send c "GET /fit HTTP/1.0\r\nHost: t.example\r\n"
                          (rows 3) "\r\n")
                 (let [code (fz-response-code (fz-read-all c))]
                   (is (= 200 code)
                       "host plus three rows fits the cap of four"))
                 (try (net-close c) (catch e nil)))
               (let [c (fz-conn (:port s))]
                 (fz-send c "GET /over HTTP/1.1\r\nHost: t.example\r\n"
                          (rows 4) "\r\n")
                 (let [code (fz-response-code (fz-read-all c))]
                   (is (= 400 code)
                       "the fifth row is refused as it arrives"))
                 (try (net-close c) (catch e nil)))
               (let [c (fz-conn (:port s))]
                 (fz-send c "GET /cut HTTP/1.1\r\nHost: t.example\r\n"
                          (rows 3) "X-four: aa")
                 (let [t0 (time-ms)
                       out (fz-drop-outcome c)]
                   (is (contains? #{:eof :reset} out)
                       (str "the cut row is dropped at the deadline, got " out))
                   (is (< (- (time-ms) t0) 6000)))
                 (try (net-close c) (catch e nil)))
               (fz-canary (:port s))))]
    (fz-was-clean r)))

;;;; framing lies and oversized chunk framing

(deftest conflicting-framing-headers-are-refused
  (let [lies [["both-framing"
               "POST /x HTTP/1.1\r\nHost: t.example\r\n"
               "Content-Length: 4\r\n"
               "Transfer-Encoding: chunked\r\n\r\n"
               "5\r\nhello\r\n0\r\n\r\n"]
              ["two-lengths"
               "POST /y HTTP/1.1\r\nHost: t.example\r\n"
               "Content-Length: 4\r\n"
               "Content-Length: 5\r\n\r\n"
               "aaaa"]]
        r (fz-with 3 fz-ok {}
             (fn [s]
               (doseq [[label & parts] lies]
                 (let [c (fz-conn (:port s))]
                   (apply fz-send c parts)
                   (let [code (fz-response-code (fz-read-all c))]
                     (is (= 400 code)
                         (str "conflicting " label " framing is refused")))
                   (try (net-close c) (catch e nil))))
               (fz-canary (:port s))))]
    (fz-was-clean r)))

(deftest oversized-and-malformed-chunk-framing-are-refused
  (let [r (fz-with 5 fz-ok {:max-body-bytes 4096}
              (fn [s]
                ;; a chunk-size line past the framing cap
                (let [c (fz-conn (:port s))]
                  (fz-send c "POST /a HTTP/1.1\r\nHost: t.example\r\n"
                           "Transfer-Encoding: chunked\r\n\r\n"
                           (apply str (repeat 1100 "f")))
                  (let [code (fz-response-code (fz-read-all c))]
                    (is (= 400 code)
                        "an oversized chunk-size line is refused"))
                  (try (net-close c) (catch e nil)))
                ;; a size value past the length ceiling
                (let [c (fz-conn (:port s))]
                  (fz-send c "POST /b HTTP/1.1\r\nHost: t.example\r\n"
                           "Transfer-Encoding: chunked\r\n\r\n"
                           "FFFFFFFFFFFFFFFF\r\n")
                  (let [code (fz-response-code (fz-read-all c))]
                    (is (= 400 code)
                        "an absurd chunk size is refused"))
                  (try (net-close c) (catch e nil)))
                ;; a size line that is not hex
                (let [c (fz-conn (:port s))]
                  (fz-send c "POST /c HTTP/1.1\r\nHost: t.example\r\n"
                           "Transfer-Encoding: chunked\r\n\r\n"
                           "zz\r\n")
                  (let [code (fz-response-code (fz-read-all c))]
                    (is (= 400 code)
                        "a non-hex chunk size is refused"))
                  (try (net-close c) (catch e nil)))
                ;; a chunked body cut mid-data dies at the deadline
                (let [c (fz-conn (:port s))]
                  (fz-send c "POST /d HTTP/1.1\r\nHost: t.example\r\n"
                           "Transfer-Encoding: chunked\r\n\r\n"
                           "5\r\nhel")
                  (let [t0 (time-ms)
                        out (fz-drop-outcome c)]
                    (is (contains? #{:eof :reset} out)
                        (str "the cut chunk body is dropped at the deadline, got " out))
                    (is (< (- (time-ms) t0) 6000)))
                  (try (net-close c) (catch e nil)))
                (fz-canary (:port s))))]
    (fz-was-clean r)))

;;;; pipelined garbage after a valid request

(deftest garbage-pipelined-after-a-valid-request-is-refused
  (let [r (fz-with 2 fz-ok {}
             (fn [s]
               (let [c (fz-conn (:port s))]
                 (fz-send c "GET /first HTTP/1.1\r\nHost: t.example\r\n\r\n"
                          "%%%%\r\n\r\n")
                  (let [wire (apply str (map char (fz-read-all c)))]
                    (is (re-find #"200 OK" wire)
                        "the valid request is served before the garbage")
                    (is (re-find #"400 Bad Request" wire)
                        "the pipelined garbage is refused"))
                  (try (net-close c) (catch e nil)))
                (fz-canary (:port s))))]
    (fz-was-clean r)))

;;;; the peer leaves mid-request

(deftest a-response-abandoned-by-a-reset-peer-never-kills-the-engine
  ;; the response is written while the peer is already gone: the
  ;; peer closes with the answer unread, the kernel resets, and the
  ;; engine's parked read must swallow the reset as an end
  (let [r (fz-with 2 fz-ok {}
             (fn [s]
               (let [c (fz-conn (:port s))]
                 (fz-send c "GET /gone HTTP/1.1\r\nHost: t.example\r\n\r\n")
                 ;; let the engine write its 200 and park on the next
                 ;; read before the reset lands
                 (thread-sleep 80)
                 (try (net-close c) (catch e nil)))
               (fz-canary (:port s))))]
    (fz-was-clean r)))

(deftest peer-close-mid-headers-and-mid-body-never-kills-the-engine
  ;; the peer vanishes without a shutdown half-close prim; a full
  ;; close delivers the same FIN the engine reads
  (let [cuts [["mid-headers"
               "GET /x HTTP/1.1\r\nHost: t.example\r\n"
               "X-Cut: aa"]
              ["mid-body"
               "POST /y HTTP/1.1\r\nHost: t.example\r\n"
               "Content-Length: 8\r\n\r\nabcd"]
              ["mid-chunk"
               "POST /z HTTP/1.1\r\nHost: t.example\r\n"
               "Transfer-Encoding: chunked\r\n\r\n"
               "5\r\nhel"]]
        r (fz-with 4 fz-ok {}
             (fn [s]
               (doseq [[_label & parts] cuts]
                 (let [c (fz-conn (:port s))]
                   (apply fz-send c parts)
                   ;; the engine may answer 400 into the closing
                   ;; socket or just drop; either way it must end
                   ;; inside the budget
                   (let [t0 (time-ms)
                         out (fz-drop-outcome c)]
                     (is (not= :held out)
                         "an abandoned connection never outlives its budgets")
                     (is (< (- (time-ms) t0) 6000)))
                   (try (net-close c) (catch e nil))))
               (fz-canary (:port s))))]
    (fz-was-clean r)))

(run-tests-and-exit)
