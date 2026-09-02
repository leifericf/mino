(require "tests/test")
(require '[mino.ws :as ws])

;; The mino.ws websocket client over the frame codec and the net
;; prims: handshake (nonce out, accept echo verified), send/recv
;; round-trips, automatic pong below the API, fragment reassembly,
;; and both directions of the close handshake. Every fixture is an
;; in-process loopback upgrade stub built from net-listen plus the
;; server-side codec calls; decoding the client's frames at :role
;; :server is what proves the client masked every one of them.

(defn- wc-bb
  [& xs]
  (byte-array (mapcat #(cond (bytes? %) (vec %)
                             (string? %) (map int %)
                             (number? %) [%]
                             :else (vec %))
                      xs)))

(defn- wc-kind
  [thunk]
  (try (do (thunk) :ok) (catch e (:mino/kind e))))

(defn- wc-read-request
  "Accumulate conn bytes until the client's upgrade request parses."
  [c]
  (loop [buf (byte-array [])]
    (let [r (http-parse-request buf)]
      (if (= :done (:status r))
        r
        (let [b (net-read c 65536)]
          (when (nil? b)
            (throw "eof during the upgrade request"))
          (recur (wc-bb buf b)))))))

(defn- wc-101
  "Hand-written 101 wire bytes (http-encode-response owns Connection,
  so the stub writes the upgrade head itself, like the server tests)."
  [accept]
  (str "HTTP/1.1 101 Switching Protocols\r\n"
       "Upgrade: websocket\r\n"
       "Connection: Upgrade\r\n"
       "Sec-WebSocket-Accept: " accept "\r\n\r\n"))

(defn- wc-upgrade!
  "Server side of the upgrade on an accepted conn: parse the request,
  answer 101 with accept-fn of the client's key (ws-accept-key is the
  honest recipe). Returns {:request parsed :key .. :rest leftover}."
  ([c] (wc-upgrade! c ws-accept-key))
  ([c accept-fn]
   (let [r (wc-read-request c)
         key (get (:headers r) "sec-websocket-key")]
     (net-write c (wc-101 (accept-fn key)))
     {:request r :key key :rest (:leftover r)})))

(defn- wc-server-recv
  "Read conn bytes through the server-role decoder (which enforces
  that the client masked) until at least one frame lands. Returns the
  decoder map; nil on EOF with an empty buffer."
  [c rest-bytes]
  (loop [buf rest-bytes]
    (let [r (ws-decode-frames buf {:role :server})]
      (if (seq (:frames r))
        r
        (let [b (net-read c 65536)]
          (if (nil? b)
            (when (pos? (alength buf))
              (throw "eof mid-frame on the server"))
            (recur (wc-bb buf b))))))))

(defn- with-ws-server
  "Stand up a one-connection loopback stub. serve-fn scripts the whole
  server side on the accepted conn and its return value is delivered;
  body-fn gets [port done-promise]."
  [serve-fn body-fn]
  (let [port-p (promise)
        done-p (promise)]
    (future
      (let [l (net-listen "127.0.0.1" 0 {})]
        (deliver port-p (net-listener-port l))
        (try
          (let [c (net-accept l {:accept-timeout 10000
                                 :read-timeout 10000})]
            (try
              (deliver done-p (serve-fn c))
              (finally (try (net-close c) (catch e nil)))))
          (catch e (deliver done-p {:server-error e}))
          (finally (try (net-close l) (catch e nil))))))
    (let [port (deref port-p 10000 ::timeout)]
      (body-fn port done-p))))

(defn- wc-url [port] (str "ws://127.0.0.1:" port "/chat"))

;;; handshake and the handle contract

(deftest ws-client-handshake-sends-the-rfc-upgrade-and-verifies-accept
  (with-ws-server
    (fn [c]
      (let [up (wc-upgrade! c)]
        (select-keys up [:request :key])))
    (fn [port done]
      (let [h (ws/ws-connect (wc-url port))
            sv (deref done 10000 ::timeout)
            hdrs (:headers (:request sv))]
        (is (= "GET" (:method (:request sv))))
        (is (= "/chat" (:target (:request sv))))
        (is (= "websocket" (get hdrs "upgrade")))
        (is (= "Upgrade" (get hdrs "connection")))
        (is (= "13" (get hdrs "sec-websocket-version")))
        (is (= (str "127.0.0.1:" port) (get hdrs "host")))
        (is (= 24 (count (:key sv)))
            "a 16-byte nonce base64-encodes to 24 characters")
        ;; The handle is plain data with frozen keys; the socket handle
        ;; is opaque and only ever passed back to the prims.
        (is (map? h))
        (is (= #{:socket :secure? :role :url :max-payload :state}
               (set (keys h))))
        (is (= :client (:role h)))
        (is (false? (:secure? h)))
        (is (= (wc-url port) (:url h)))
        (ws/ws-close h)))))

(deftest ws-client-rejects-a-forged-accept-key
  (with-ws-server
    (fn [c]
      (wc-upgrade! c (fn [_] "bogusbogusbogusbogusbogusbog"))
      ;; The client hangs up on the forged accept.
      (net-read c 1))
    (fn [port done]
      (is (= :ws/handshake
             (wc-kind #(ws/ws-connect (wc-url port)))))
      (is (nil? (deref done 10000 ::timeout))
          "the client closed the socket after the forged handshake"))))

(deftest ws-client-rejects-a-non-101-status
  (with-ws-server
    (fn [c]
      (wc-read-request c)
      (net-write c "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nnope")
      (net-read c 1))
    (fn [port done]
      (is (= :ws/handshake
             (wc-kind #(ws/ws-connect (wc-url port)))))
      (deref done 10000 ::timeout))))

(deftest ws-connect-validates-its-arguments-without-a-socket
  (is (= :ws/invalid (wc-kind #(ws/ws-connect "http://x/"))))
  (is (= :ws/invalid (wc-kind #(ws/ws-connect 42))))
  (is (= :ws/invalid (wc-kind #(ws/ws-connect "ws://x/" {:bogus 1}))))
  (is (= :ws/invalid (wc-kind #(ws/ws-connect "ws://x/" {:read-timeout "soon"})))))

;;; echo round-trips

(deftest ws-client-text-echo-round-trip
  (with-ws-server
    (fn [c]
      (let [up (wc-upgrade! c)
            r (wc-server-recv c (:rest up))
            f (first (:frames r))]
        (net-write c (ws-encode-frame
                      {:opcode :text
                       :payload (str "echo: " (:payload f))}))
        f))
    (fn [port done]
      (let [h (ws/ws-connect (wc-url port))]
        (is (nil? (ws/ws-send h "hi")))
        (is (= {:opcode :text :payload "echo: hi"} (ws/ws-recv h))
            "the reply carries exactly opcode and payload")
        (let [f (deref done 10000 ::timeout)]
          (is (= :text (:opcode f)))
          (is (= "hi" (:payload f))
              "the server-role decoder unmasked the client frame"))
        (ws/ws-close h)))))

(deftest ws-client-binary-echo-round-trip
  (let [payload (byte-array [0 1 2 250 255 7])]
    (with-ws-server
      (fn [c]
        (let [up (wc-upgrade! c)
              r (wc-server-recv c (:rest up))
              f (first (:frames r))]
          (net-write c (ws-encode-frame {:opcode :binary
                                         :payload (:payload f)}))
          f))
      (fn [port done]
        (let [h (ws/ws-connect (wc-url port))]
          (ws/ws-send h payload)
          (let [m (ws/ws-recv h)]
            (is (= :binary (:opcode m)))
            (is (= (vec payload) (vec (:payload m)))))
          (is (= (vec payload)
                 (vec (:payload (deref done 10000 ::timeout)))))
          (ws/ws-close h))))))

(deftest ws-send-rejects-non-message-payloads
  (with-ws-server
    (fn [c]
      (wc-upgrade! c)
      (net-read c 1))
    (fn [port done]
      (let [h (ws/ws-connect (wc-url port))]
        (is (= :ws/invalid (wc-kind #(ws/ws-send h 42))))
        (is (= :ws/invalid (wc-kind #(ws/ws-send h [1 2 3]))))
        (ws/ws-close h)
        (deref done 10000 ::timeout)))))

;;; frames glued to the 101 and fragmentation

(deftest ws-client-keeps-bytes-the-server-glued-to-the-101
  ;; The 101 head and the first frame arrive in one write; the client
  ;; must keep everything past the CRLFCRLF as frame bytes.
  (with-ws-server
    (fn [c]
      (let [r (wc-read-request c)
            key (get (:headers r) "sec-websocket-key")]
        (net-write c (wc-bb (wc-101 (ws-accept-key key))
                            (ws-encode-frame {:opcode :text
                                              :payload "early"})))
        (net-read c 1)))
    (fn [port done]
      (let [h (ws/ws-connect (wc-url port))]
        (is (= {:opcode :text :payload "early"} (ws/ws-recv h)))
        (ws/ws-close h)
        (deref done 10000 ::timeout)))))

(deftest ws-client-reassembles-a-fragmented-server-message
  (with-ws-server
    (fn [c]
      (wc-upgrade! c)
      (net-write c (ws-encode-frame {:opcode :text :payload "Hel"
                                     :fin? false}))
      (net-write c (ws-encode-frame {:opcode :continuation
                                     :payload "lo"}))
      (net-read c 1))
    (fn [port done]
      (let [h (ws/ws-connect (wc-url port))]
        (is (= {:opcode :text :payload "Hello"} (ws/ws-recv h)))
        (ws/ws-close h)
        (deref done 10000 ::timeout)))))

;;; automatic pong

(deftest ws-client-answers-a-ping-below-the-api
  (with-ws-server
    (fn [c]
      (let [up (wc-upgrade! c)]
        (net-write c (ws-encode-frame {:opcode :ping :payload "sup"}))
        (net-write c (ws-encode-frame {:opcode :text :payload "after"}))
        ;; The pong the client owes arrives masked (the server-role
        ;; decoder enforces the mask) and echoes the ping payload.
        (first (:frames (wc-server-recv c (:rest up))))))
    (fn [port done]
      (let [h (ws/ws-connect (wc-url port))]
        (is (= {:opcode :text :payload "after"} (ws/ws-recv h))
            "the ping is answered, never surfaced")
        (let [pong (deref done 10000 ::timeout)]
          (is (= :pong (:opcode pong)))
          (is (= (vec (wc-bb "sup")) (vec (:payload pong)))
              "the pong echoes the ping payload"))
        (ws/ws-close h)))))

;;; the close handshake, both directions

(deftest ws-close-sends-a-masked-close-and-drops-the-socket
  (with-ws-server
    (fn [c]
      (let [up (wc-upgrade! c)
            r (wc-server-recv c (:rest up))
            f (first (:frames r))]
        {:frame f :eof (net-read c 1)}))
    (fn [port done]
      (let [h (ws/ws-connect (wc-url port))]
        (is (nil? (ws/ws-close h {:code 1000 :reason "bye"})))
        (is (nil? (ws/ws-close h)) "ws-close is idempotent")
        (is (= :ws/closed (wc-kind #(ws/ws-send h "late"))))
        (is (= :ws/closed (wc-kind #(ws/ws-recv h))))
        (let [sv (deref done 10000 ::timeout)]
          (is (= :close (:opcode (:frame sv))))
          (is (= 1000 (:code (:frame sv))))
          (is (= "bye" (:reason (:frame sv))))
          (is (nil? (:eof sv)) "the client closed the connection"))))))

(deftest ws-client-answers-a-server-close-and-returns-it-as-data
  (with-ws-server
    (fn [c]
      (let [up (wc-upgrade! c)]
        (net-write c (ws-encode-frame {:opcode :close :code 1001
                                       :reason "going away"}))
        ;; The reply close must come back masked, then EOF.
        (let [r (wc-server-recv c (:rest up))]
          {:frame (first (:frames r)) :eof (net-read c 1)})))
    (fn [port done]
      (let [h (ws/ws-connect (wc-url port))]
        (is (= {:opcode :close :code 1001 :reason "going away"}
               (ws/ws-recv h))
            "the peer's close surfaces as data")
        (is (= :ws/closed (wc-kind #(ws/ws-recv h))))
        (is (nil? (ws/ws-close h)) "close after the handshake is a no-op")
        (let [sv (deref done 10000 ::timeout)]
          (is (= :close (:opcode (:frame sv))))
          (is (= 1001 (:code (:frame sv))) "the reply echoes the code")
          (is (nil? (:eof sv))))))))

(deftest ws-recv-turns-a-bare-eof-into-a-synthetic-1006-close
  ;; Pinned choice: a connection dropped without a close frame is RFC
  ;; 6455's abnormal closure, returned as data with code 1006; only a
  ;; recv after that throws :ws/closed.
  (with-ws-server
    (fn [c]
      (wc-upgrade! c)
      :closed-without-close-frame)
    (fn [port done]
      (let [h (ws/ws-connect (wc-url port))]
        (deref done 10000 ::timeout)
        (is (= {:opcode :close :code 1006 :reason ""} (ws/ws-recv h)))
        (is (= :ws/closed (wc-kind #(ws/ws-recv h))))))))

;;; timeouts

(deftest ws-recv-timeout-opt-bounds-the-whole-wait
  (with-ws-server
    (fn [c]
      (wc-upgrade! c)
      ;; Stay silent; the read returns when the client hangs up.
      (net-read c 65536))
    (fn [port done]
      (let [h (ws/ws-connect (wc-url port) {:read-timeout 200})
            t0 (time-ms)
            kind (wc-kind #(ws/ws-recv h {:timeout 500}))
            dt (- (time-ms) t0)]
        (is (= :ws/timeout kind))
        (is (>= dt 450) (str "deadline fired early, at " dt " ms"))
        (is (< dt 4000) (str "deadline fired late, at " dt " ms"))
        ;; Without :timeout the socket's own read timeout surfaces.
        (is (= :net/timeout (wc-kind #(ws/ws-recv h))))
        (ws/ws-close h)
        (deref done 10000 ::timeout)))))

(run-tests-and-exit)
