(require "tests/test")
(require '[mino.ws :as ws])
(require '[mino.http.server :as srv])

;; The websocket upgrade on mino.http.server: a handler answers an
;; upgrade request with {:ws f} and the server validates the RFC 6455
;; handshake, writes the 101 head, and hands f a server-role mino.ws
;; handle for ws-send / ws-recv / ws-close (one vocabulary, ADR 41).
;; Server frames go out unmasked and inbound frames must arrive
;; masked, the mirror of the client rules; decoding the server's
;; bytes at :role :client is what proves every frame it sent is
;; unmasked. A malformed upgrade gets a plain 400; a masking
;; violation after the upgrade is answered with a 1002 close; :stop
;; closes live websocket connections with a going-away 1001 close
;; through the same graceful drain run-server already owes HTTP
;; traffic.

(defn- wsrv-bb
  [& xs]
  (byte-array (mapcat #(cond (bytes? %) (vec %)
                             (string? %) (map int %)
                             (number? %) [%]
                             :else (vec %))
                      xs)))

(defn- wsrv-kind
  [thunk]
  (try (do (thunk) :ok) (catch e (:mino/kind e))))

(defn- wsrv-await
  "Poll pred every 20ms for up to 10s; true once it holds."
  [pred]
  ((fn wait [n]
     (cond (pred)    true
           (zero? n) false
           :else     (do (thread-sleep 20) (wait (dec n)))))
   500))

(defn- wsrv-with-server
  "run-server around body-fn with the stop and worker-grant drain in
  the teardown, so every test leaves a settled pool."
  [handler body-fn]
  (let [s (srv/run-server handler {})]
    (try (body-fn s)
         (finally
           ((:stop s))
           (is (wsrv-await #(zero? (mino-thread-count)))
               "the worker grant drained after stop")))))

(defn- wsrv-connect
  [port]
  (net-connect "127.0.0.1" port {:connect-timeout 8000
                                 :read-timeout 8000
                                 :write-timeout 8000}))

;; The RFC 6455 sample nonce and its published accept derivation pin
;; the handshake against the spec, not against our own prim alone.
(def ^:private wsrv-rfc-key "dGhlIHNhbXBsZSBub25jZQ==")
(def ^:private wsrv-rfc-accept "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")

(defn- wsrv-upgrade-req
  "Upgrade request wire bytes; overrides replace well-formed parts to
  build each malformed variant (:key nil drops the header line)."
  [overrides]
  (let [method (get overrides :method "GET")
        target (get overrides :target "/ws")
        version (get overrides :version "13")
        key (get overrides :key wsrv-rfc-key)
        connection (get overrides :connection "Upgrade")
        upgrade (get overrides :upgrade "websocket")]
    (str method " " target " HTTP/1.1\r\n"
         "Host: t.example\r\n"
         "Upgrade: " upgrade "\r\n"
         "Connection: " connection "\r\n"
         (when key (str "Sec-WebSocket-Key: " key "\r\n"))
         "Sec-WebSocket-Version: " version "\r\n\r\n")))

(defn- wsrv-header-end
  "Index just past the first CRLFCRLF in bs, or nil while incomplete."
  [bs]
  (let [n (alength bs)]
    (loop [i 0]
      (when (<= (+ i 4) n)
        (if (and (= 13 (aget bs i)) (= 10 (aget bs (+ i 1)))
                 (= 13 (aget bs (+ i 2))) (= 10 (aget bs (+ i 3))))
          (+ i 4)
          (recur (inc i)))))))

(defn- wsrv-read-head
  "Accumulate conn bytes to the first CRLFCRLF; [head leftover]."
  [c]
  (loop [buf (byte-array [])]
    (if-let [end (wsrv-header-end buf)]
      [(byte-array (take end (seq buf)))
       (byte-array (drop end (seq buf)))]
      (let [b (net-read c 65536)]
        (when (nil? b)
          (throw "eof during the upgrade response head"))
        (recur (wsrv-bb buf b))))))

(defn- wsrv-read-response
  "One complete non-101 response off c; the parsed map, or nil when
  the peer closed before a full response arrived."
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

(defn- wsrv-read-msgs
  "Accumulate conn bytes through the client-role decoder until n
  messages arrived; :role :client enforcing no mask is what proves
  the server never masked a frame. Returns {:frames .. :rest ..}."
  [c rest0 n]
  (let [r0 (ws-decode-frames rest0 {:role :client})]
    (loop [rest (:rest r0) acc (vec (:frames r0))]
      (if (>= (count acc) n)
        {:frames acc :rest rest}
        (let [b (net-read c 65536)]
          (when (nil? b)
            (throw "eof awaiting server frames"))
          (let [r (ws-decode-frames (wsrv-bb rest b) {:role :client})]
            (recur (:rest r) (into acc (:frames r)))))))))

(defn- wsrv-masked
  "One masked client frame, fresh secure-random mask."
  [frame]
  (ws-encode-frame (assoc frame :mask (secure-rand-bytes 4))))

(defn- wsrv-echo-handler
  "Routes /ws to a websocket echo loop recording its close (or its
  error kind) and serves /plain as ordinary Ring traffic. The loop
  uses ws-recv with no :timeout: on a server handle the accepted
  socket's short read window is the engine's poll interval, never an
  error, so a bare ws-recv parks until traffic or a close."
  [done-p]
  (fn [req]
    (if (= "/ws" (:uri req))
      {:ws (fn [h]
             (try
               (loop []
                 (let [m (ws/ws-recv h)]
                   (if (= :close (:opcode m))
                     (deliver done-p m)
                     (do (ws/ws-send h (:payload m))
                         (recur)))))
               (catch e (deliver done-p (:mino/kind e)))))}
      {:status 200 :body "plain"})))

;;; the handshake

(deftest ws-upgrade-answers-101-with-the-rfc-accept-key
  (let [handed (promise)
        done (promise)
        h (fn [req]
            {:ws (fn [wsc]
                   (deliver handed wsc)
                   (deliver done (ws/ws-recv wsc)))})]
    (wsrv-with-server h
      (fn [s]
        (let [c (wsrv-connect (:port s))]
          (try
            (net-write c (wsrv-upgrade-req {}))
            (let [[head leftover] (wsrv-read-head c)
                  r (http-parse-response head {:informational true})
                  hdrs (:headers r)]
              (is (= :done (:status r)))
              (is (= 101 (:code r)))
              (is (= "websocket" (get hdrs "upgrade")))
              (is (= "Upgrade" (get hdrs "connection")))
              (is (= wsrv-rfc-accept (get hdrs "sec-websocket-accept"))
                  "the accept key is the RFC 6455 sha1 derivation")
              ;; the handler was handed a plain server-role handle in
              ;; the client handle's frozen key vocabulary
              (let [wsc (deref handed 10000 ::timeout)]
                (is (map? wsc))
                (is (= #{:socket :secure? :role :url :max-payload :state}
                       (set (keys wsc))))
                (is (= :server (:role wsc)))
                (is (false? (:secure? wsc)))
                (is (= "/ws" (:url wsc))))
              ;; close the session: the server echoes the close code
              (net-write c (wsrv-masked {:opcode :close :code 1000}))
              (let [r2 (wsrv-read-msgs c leftover 1)
                    f (first (:frames r2))]
                (is (= :close (:opcode f)))
                (is (= 1000 (:code f)) "the close echo carries the code"))
              (is (= {:opcode :close :code 1000 :reason ""}
                     (deref done 10000 ::timeout))
                  "the handler's ws-recv returned the close as data")
              (is (nil? (try (net-read c 65536) (catch e nil)))
                  "the server dropped the connection after the close"))
            (finally (try (net-close c) (catch e nil)))))))))

;;; echo round-trips through our own client

(deftest ws-server-echoes-text-and-binary-to-the-own-client
  (let [done (promise)]
    (wsrv-with-server (wsrv-echo-handler done)
      (fn [s]
        ;; the plain route still serves ordinary Ring traffic
        (let [c (wsrv-connect (:port s))]
          (try
            (net-write c (str "GET /plain HTTP/1.1\r\n"
                              "Host: t.example\r\n"
                              "Connection: close\r\n\r\n"))
            (let [r (wsrv-read-response c)]
              (is (= 200 (:code r)))
              (is (= (vec (wsrv-bb "plain")) (vec (:body r)))))
            (finally (try (net-close c) (catch e nil)))))
        (let [h (ws/ws-connect (str "ws://127.0.0.1:" (:port s) "/ws"))]
          (is (nil? (ws/ws-send h "hi")))
          (is (= {:opcode :text :payload "hi"} (ws/ws-recv h {:timeout 10000}))
              "the text echo comes back whole")
          (let [payload (byte-array (range 32))]
            (ws/ws-send h payload)
            (let [m (ws/ws-recv h {:timeout 10000})]
              (is (= :binary (:opcode m)))
              (is (= (vec payload) (vec (:payload m)))
                  "the binary echo is byte-identical")))
          (ws/ws-close h)
          (is (= {:opcode :close :code 1000 :reason ""}
                 (deref done 10000 ::timeout))
              "the server saw the client's close handshake"))))))

;;; fragmentation, ping/pong, and bytes glued to the request

(deftest ws-server-reassembles-fragments-and-answers-ping-below-the-api
  (let [done (promise)]
    (wsrv-with-server (wsrv-echo-handler done)
      (fn [s]
        (let [c (wsrv-connect (:port s))]
          (try
            (net-write c (wsrv-upgrade-req {}))
            (let [[head leftover] (wsrv-read-head c)]
              (is (= 101 (:code (http-parse-response
                                  head {:informational true}))))
              ;; a masked ping interleaved between the fragments: the
              ;; pong answers first, below the handler's API, and the
              ;; reassembled message echoes whole
              (net-write c (wsrv-bb
                            (wsrv-masked {:opcode :text :payload "fr"
                                          :fin? false})
                            (wsrv-masked {:opcode :ping :payload "p1"})
                            (wsrv-masked {:opcode :continuation
                                          :payload "ag"})))
              (let [r (wsrv-read-msgs c leftover 2)
                    [pong echo] (:frames r)]
                (is (= :pong (:opcode pong)))
                (is (= (vec (wsrv-bb "p1")) (vec (:payload pong)))
                    "the pong echoes the ping payload")
                (is (= :text (:opcode echo)))
                (is (= "frag" (:payload echo))
                    "the masked fragments reassembled server-side"))
              (net-write c (wsrv-masked {:opcode :close :code 1000}))
              (deref done 10000 ::timeout))
            (finally (try (net-close c) (catch e nil)))))))))

(deftest ws-server-keeps-bytes-the-client-glued-to-the-request
  ;; The upgrade request and the first masked frame arrive in one
  ;; write; the server must treat everything past the request head as
  ;; frame bytes.
  (let [done (promise)]
    (wsrv-with-server (wsrv-echo-handler done)
      (fn [s]
        (let [c (wsrv-connect (:port s))]
          (try
            (net-write c (wsrv-bb (wsrv-upgrade-req {})
                                  (wsrv-masked {:opcode :text
                                                :payload "early"})))
            (let [[head leftover] (wsrv-read-head c)]
              (is (= 101 (:code (http-parse-response
                                  head {:informational true}))))
              (let [r (wsrv-read-msgs c leftover 1)
                    f (first (:frames r))]
                (is (= :text (:opcode f)))
                (is (= "early" (:payload f)) "no glued byte was lost"))
              (net-write c (wsrv-masked {:opcode :close :code 1000}))
              (deref done 10000 ::timeout))
            (finally (try (net-close c) (catch e nil)))))))))

;;; malformed upgrades get a plain 400 and the ws entry never runs

(deftest bad-handshake-requests-get-a-plain-400
  (let [called (atom 0)
        h (fn [req] {:ws (fn [wsc] (swap! called inc))})]
    (wsrv-with-server h
      (fn [s]
        (doseq [[label req]
                [["missing key" (wsrv-upgrade-req {:key nil})]
                 ["short key" (wsrv-upgrade-req {:key "c2hvcnQ="})]
                 ["undecodable key" (wsrv-upgrade-req {:key "!!!not-base64!!!"})]
                 ["wrong version" (wsrv-upgrade-req {:version "12"})]
                 ["not a GET" (wsrv-upgrade-req {:method "POST"})]
                 ["upgrade token absent" (wsrv-upgrade-req
                                          {:upgrade "notsocket"})]
                 ["connection token absent" (wsrv-upgrade-req
                                             {:connection "keep-alive"})]
                 ["no upgrade headers at all"
                  (str "GET /ws HTTP/1.1\r\nHost: t.example\r\n\r\n")]]]
          (let [c (wsrv-connect (:port s))]
            (try
              (net-write c req)
              (let [r (wsrv-read-response c)]
                (is (some? r) label)
                (is (= 400 (:code r)) label))
              (is (nil? (try (net-read c 65536) (catch e nil)))
                  (str label ": the connection closed after the 400"))
              (finally (try (net-close c) (catch e nil))))))
        (is (zero? @called)
            "no malformed upgrade ever reached the ws entry point")))))

;;; a masking violation is a protocol error, answered before the drop

(deftest unmasked-client-frame-is-answered-with-a-1002-close
  (let [done (promise)]
    (wsrv-with-server (wsrv-echo-handler done)
      (fn [s]
        (let [c (wsrv-connect (:port s))]
          (try
            (net-write c (wsrv-upgrade-req {}))
            (let [[head leftover] (wsrv-read-head c)]
              (is (= 101 (:code (http-parse-response
                                  head {:informational true}))))
              ;; unmasked, the server-role decoder's corrupt case
              (net-write c (ws-encode-frame {:opcode :text
                                             :payload "bare"}))
              (let [r (wsrv-read-msgs c leftover 1)
                    f (first (:frames r))]
                (is (= :close (:opcode f)))
                (is (= 1002 (:code f))
                    "a protocol-error close precedes the drop"))
              (is (nil? (try (net-read c 65536) (catch e nil)))
                  "the server dropped the connection")
              (is (= :codec/corrupt (deref done 10000 ::timeout))
                  "the handler's ws-recv surfaced the decode error"))
            (finally (try (net-close c) (catch e nil)))))))))

;;; :stop closes live websocket connections with a going-away close

(deftest stop-closes-live-ws-connections-with-a-going-away-close
  (let [done (promise)
        s (srv/run-server (wsrv-echo-handler done) {})]
    (try
      (let [h (ws/ws-connect (str "ws://127.0.0.1:" (:port s) "/ws"))]
        (is (nil? (ws/ws-send h "warm")))
        (is (= {:opcode :text :payload "warm"}
               (ws/ws-recv h {:timeout 10000}))
            "the connection is live before the stop")
        (let [parked (future (ws/ws-recv h {:timeout 10000}))]
          ((:stop s))
          (is (= {:opcode :close :code 1001 :reason ""}
                 (deref parked 10000 ::timeout))
              "stop reached the parked client as a going-away close")
          (is (= :ws/closed (wsrv-kind #(ws/ws-recv h)))
              "the client connection finished with the handshake")
          (is (= {:opcode :close :code 1001 :reason ""}
                 (deref done 10000 ::timeout))
              "the server-side loop saw the same going-away close")))
      (finally
        ((:stop s))
        (is (wsrv-await #(zero? (mino-thread-count)))
            "the worker grant drained after stop")))))

;;; the server-side handle vocabulary validates like the client's

(deftest ws-accept-and-ws-shutdown-validate-their-arguments
  (is (= :ws/invalid (wsrv-kind #(ws/ws-accept :conn {:bogus 1})))
      "an unknown ws-accept key is named an error")
  (is (= :ws/invalid (wsrv-kind #(ws/ws-accept :conn :not-a-map))))
  (let [h (ws/ws-accept :conn {})]
    (is (= #{:socket :secure? :role :url :max-payload :state}
           (set (keys h)))
        "the server handle keeps the frozen handle keys")
    (is (= :server (:role h)))
    (is (= :ws/invalid (wsrv-kind #(ws/ws-shutdown h :not-a-map))))
    (is (nil? (ws/ws-shutdown h))
        "ws-shutdown only flags; it never touches the socket")))

(run-tests-and-exit)
