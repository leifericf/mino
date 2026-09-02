(ns mino.ws
  "Websocket client: ws-connect opens ws:// or wss:// and returns a
  plain handle map; ws-send, ws-recv, and ws-close drive it.

  (require '[mino.ws :as ws])
  (def c (ws/ws-connect \"ws://127.0.0.1:8080/chat\"))
  (ws/ws-send c \"hi\")
  (ws/ws-recv c)   => {:opcode :text :payload \"echo: hi\"}
  (ws/ws-close c)

  A string sends a :text message and bytes send :binary. ws-recv
  returns the next application message {:opcode :text|:binary
  :payload ..}, answering pings below the API, and returns a close
  (the peer's, or the synthetic 1006 of a connection dropped without
  one) as {:opcode :close :code .. :reason ..}. Every frame the
  client sends is masked, and the handshake is verified against
  ws-accept-key before the handle exists. Errors are :mino/kind data
  maps: :ws/invalid (a contract fault), :ws/handshake (the upgrade
  was refused or forged), :ws/closed (a finished handle),
  :ws/timeout (a ws-recv deadline).

  The server half of the same vocabulary: mino.http.server hands a
  handler's :ws entry a ws-accept handle, and the same ws-send,
  ws-recv, and ws-close drive it under the mirrored role rules
  (frames go out unmasked, inbound frames must arrive masked).
  ws-shutdown asks the loop that owns a connection to close it
  gracefully from another thread.

  Capabilities: the frame codec and this namespace ride
  MINO_CAP_WEBSOCKET. The connect/read/write path needs MINO_CAP_NET
  (it is not in the default capability set, so an embedder must
  install it).")

;;;; Errors

(defn- bad
  [msg]
  (throw {:mino/kind :ws/invalid
          :mino/code "MWSV001"
          :mino/message (str "mino.ws: " msg)
          :mino/data {}}))

(defn- handshake-fail
  [msg data]
  (throw {:mino/kind :ws/handshake
          :mino/code "MWSH001"
          :mino/message (str "mino.ws: " msg)
          :mino/data data}))

(defn- closed-fail
  []
  (throw {:mino/kind :ws/closed
          :mino/code "MWSC001"
          :mino/message "mino.ws: the connection is closed"
          :mino/data {}}))

(defn- timeout-fail
  [ms]
  (throw {:mino/kind :ws/timeout
          :mino/code "MWST001"
          :mino/message (str "mino.ws: no message within " ms " ms")
          :mino/data {:timeout ms}}))

;;;; Bytes

(defn- bconcat
  [a b]
  (byte-array (concat (seq a) (seq b))))

(defn- header-end
  "Index just past the first CRLFCRLF in bs, or nil while the response
  head is still incomplete."
  [bs]
  (let [n (alength bs)]
    (loop [i 0]
      (when (<= (+ i 4) n)
        (if (and (= 13 (aget bs i)) (= 10 (aget bs (+ i 1)))
                 (= 13 (aget bs (+ i 2))) (= 10 (aget bs (+ i 3))))
          (+ i 4)
          (recur (inc i)))))))

;;;; Transport: one scheme flag picks the net or tls quad

(defn- sock-read!
  [handle n]
  (if (:secure? handle)
    (tls-read (:socket handle) n)
    (net-read (:socket handle) n)))

(defn- sock-write!
  [handle b]
  (if (:secure? handle)
    (tls-write (:socket handle) b)
    (net-write (:socket handle) b)))

(defn- close-socket!
  [handle]
  (try (if (:secure? handle)
         (tls-close (:socket handle))
         (net-close (:socket handle)))
       (catch e nil)))

(defn- write-frame!
  "Encodes and sends one frame at the handle's role. A client frame is
  masked with four fresh secure-random bytes; never rand: a
  predictable mask defeats the cache-poisoning defense masking exists
  for (ADR 41). A server frame goes out unmasked, as RFC 6455
  requires."
  [handle frame]
  (sock-write! handle
               (ws-encode-frame
                (if (= :server (:role handle))
                  frame
                  (assoc frame :mask (secure-rand-bytes 4))))))

;;;; Connect

(def ^:private connect-keys
  #{:connect-timeout :read-timeout :write-timeout :insecure? :max-payload})

(defn- opt-long
  [v k default]
  (cond (nil? v) default
        (int? v) v
        :else (bad (str "key :" (name k) " must be an integer"))))

(defn- parse-ws-url
  "Splits url on the ws/wss scheme and parses the remainder through
  parse-url as http/https, whose default ports (80/443) are exactly
  RFC 6455's. Returns the parse-url map with :scheme rewritten."
  [url]
  (when-not (string? url)
    (bad "the url must be a string"))
  (let [[scheme http-scheme tail]
        (cond
          (= "ws://" (subs url 0 (min 5 (count url))))
          ["ws" "http" (subs url 5)]

          (= "wss://" (subs url 0 (min 6 (count url))))
          ["wss" "https" (subs url 6)]

          :else (bad (str "url must start with ws:// or wss://, got "
                          (pr-str url))))]
    (assoc (parse-url (str http-scheme "://" tail)) :scheme scheme)))

(defn- open-socket!
  [parsed opts]
  (let [conn {:connect-timeout (opt-long (:connect-timeout opts)
                                         :connect-timeout 10000)
              :read-timeout (opt-long (:read-timeout opts)
                                      :read-timeout 30000)
              :write-timeout (opt-long (:write-timeout opts)
                                       :write-timeout 30000)}]
    (if (= "wss" (:scheme parsed))
      (tls-connect (:host parsed) (:port parsed)
                   (if (:insecure? opts) (assoc conn :insecure? true) conn))
      (net-connect (:host parsed) (:port parsed) conn))))

(defn- upgrade-request
  "The RFC 6455 client handshake bytes for a parsed url and nonce."
  [parsed nonce]
  (let [default-port (if (= "wss" (:scheme parsed)) 443 80)
        host (if (= (:port parsed) default-port)
               (:host parsed)
               (str (:host parsed) ":" (:port parsed)))
        target (if (:query parsed)
                 (str (:path parsed) "?" (:query parsed))
                 (:path parsed))]
    (http-encode-request {:method "GET" :target target :host host
                          :headers [["Upgrade" "websocket"]
                                    ["Connection" "Upgrade"]
                                    ["Sec-WebSocket-Key" nonce]
                                    ["Sec-WebSocket-Version" "13"]]})))

(defn- read-response-head!
  "Accumulates socket bytes up to the first CRLFCRLF and returns
  [head-bytes leftover-bytes]. The leftover is kept: a server may glue
  its first frames to the 101."
  [handle]
  (loop [buf (byte-array [])]
    (if-let [end (header-end buf)]
      [(byte-array (take end (seq buf)))
       (byte-array (drop end (seq buf)))]
      (if (> (alength buf) 65536)
        (do (close-socket! handle)
            (handshake-fail "response head exceeds 64 KiB" {}))
        (let [b (try (sock-read! handle 65536)
                     (catch e (do (close-socket! handle) (throw e))))]
          (if (nil? b)
            (do (close-socket! handle)
                (handshake-fail "connection closed during the handshake" {}))
            (recur (bconcat buf b))))))))

(defn- verify-handshake!
  "Parses the response head and verifies the 101 plus the
  Sec-WebSocket-Accept echo of the nonce; a miss closes the socket and
  throws :ws/handshake."
  [handle head nonce]
  (let [r (http-parse-response head {:informational true})]
    (when-not (and (= :done (:status r)) (= 101 (:code r)))
      (close-socket! handle)
      (handshake-fail (str "expected 101, got "
                           (if (= :done (:status r)) (:code r) (:status r)))
                      {:code (:code r)}))
    (when-not (= (ws-accept-key nonce)
                 (get (:headers r) "sec-websocket-accept"))
      (close-socket! handle)
      (handshake-fail "Sec-WebSocket-Accept does not match the sent nonce"
                      {:code 101}))))

(defn ws-connect
  "Opens a websocket connection to a ws:// or wss:// url and returns a
  plain handle map for ws-send / ws-recv / ws-close. wss:// rides the
  TLS quad with the vendored roots; :insecure? true skips verification
  for local fixtures. Other opts: :connect-timeout / :read-timeout /
  :write-timeout ms (defaults 10000 / 30000 / 30000; :read-timeout is
  the per-read bound every ws-recv inherits) and :max-payload (caps
  each received message, default 16 MiB). Throws :ws/handshake when
  the server refuses the upgrade or echoes a wrong accept key."
  ([url] (ws-connect url {}))
  ([url opts]
   (when-not (map? opts)
     (bad "opts must be a map"))
   (let [unknown (filter #(not (contains? connect-keys %)) (keys opts))]
     (when (seq unknown)
       (bad (str "unknown ws-connect key(s): " (pr-str (vec unknown))))))
   (let [parsed (parse-ws-url url)
         handle {:socket (open-socket! parsed opts)
                 :secure? (= "wss" (:scheme parsed))
                 :role :client
                 :url url
                 :max-payload (:max-payload opts)
                 :state (atom {:rest (byte-array []) :pending []
                               :status :open})}
         ;; 16 secure-random bytes, base64; never rand -- a predictable
         ;; nonce defeats the masking defense (ADR 41).
         nonce (base64-encode (secure-rand-bytes 16))]
     (sock-write! handle (upgrade-request parsed nonce))
     (let [[head leftover] (read-response-head! handle)]
       (verify-handshake! handle head nonce)
       (swap! (:state handle) assoc :rest leftover)
       handle))))

;;;; Send

(defn ws-send
  "Sends one message: a string as a :text frame, bytes as :binary.
  Always masked with a fresh secure-random key. Throws :ws/closed once
  the connection has finished."
  [handle msg]
  (when-not (= :open (:status @(:state handle)))
    (closed-fail))
  (let [opcode (cond (string? msg) :text
                     (bytes? msg) :binary
                     :else (bad "the message must be a string or bytes"))]
    (write-frame! handle {:opcode opcode :payload msg})
    nil))

;;;; Receive

(defn- decode-opts
  [handle]
  (if-let [mp (:max-payload handle)]
    {:role (:role handle) :max-payload mp}
    {:role (:role handle)}))

(defn- fail-close!
  "RFC 6455's fail-the-connection: best-effort close frame carrying
  code, then drop the socket, on either role."
  [handle code]
  (let [st (:state handle)]
    (when (= :open (:status @st))
      (try (write-frame! handle {:opcode :close :code code})
           (catch e nil))
      (close-socket! handle)
      (swap! st assoc :status :closed))))

(defn- pop-pending!
  [handle]
  (let [st (:state handle)
        f (first (:pending @st))]
    (when f
      (swap! st update :pending #(vec (rest %)))
      f)))

(defn- decode-buffer!
  "Runs the accumulated buffer through the decoder and queues any
  complete messages. Returns true when at least one frame landed. A
  corrupt or over-cap peer fails the connection (a 1002 or 1009 close
  before the drop) and the decode error still reaches the caller."
  [handle more]
  (let [st (:state handle)
        r (try (ws-decode-frames (bconcat (:rest @st) more)
                                 (decode-opts handle))
               (catch e
                 (do (case (:mino/kind e)
                       :codec/corrupt (fail-close! handle 1002)
                       :codec/limit (fail-close! handle 1009)
                       nil)
                     (throw e))))]
    (swap! st assoc :rest (:rest r))
    (swap! st update :pending #(into % (:frames r)))
    (boolean (seq (:frames r)))))

(defn- read-frames!
  "Decodes whatever the buffer already holds (a server may glue frames
  to the 101, or several to one read) and only then blocks on the
  socket. A clean EOF without a close frame is RFC 6455's abnormal
  closure: it queues a synthetic 1006 close (data, never sent).
  Returns nil; the caller re-checks :pending."
  [handle deadline ms]
  (when (and deadline (>= (time-ms) deadline))
    (timeout-fail ms))
  (when-not (decode-buffer! handle (byte-array []))
    ;; a read-window expiry is a tick, never an error, under a caller
    ;; deadline and always on a server handle: an accepted socket's
    ;; read window is the engine's poll interval, not a peer signal.
    (let [b (try (sock-read! handle 65536)
                 (catch e
                   (if (and (= :net/timeout (:mino/kind e))
                            (or deadline (= :server (:role handle))))
                     (byte-array [])
                     (throw e))))]
      (if (nil? b)
        (let [st (:state handle)]
          (close-socket! handle)
          (swap! st assoc :status :closed)
          (swap! st update :pending conj
                 {:opcode :close :fin? true :code 1006 :reason ""}))
        (decode-buffer! handle b))))
  nil)

(defn- send-shutdown-close!
  "The owning loop's half of ws-shutdown: send the flagged close frame
  once, then keep reading toward the peer's echo or the EOF."
  [handle]
  (let [st (:state handle)
        m @st
        sd (:shutdown m)]
    (when (and sd (= :open (:status m)) (not (:close-sent? m)))
      (try (write-frame! handle
                         (if (:reason sd)
                           {:opcode :close :code (:code sd)
                            :reason (:reason sd)}
                           {:opcode :close :code (:code sd)}))
           (catch e nil))
      (swap! st assoc :close-sent? true))))

(defn- finish-close!
  "The receive side of the close handshake: echo one close frame back
  (best effort; the peer may already be gone) and drop the socket. A
  synthetic 1006 arrives with the socket already closed and is never
  echoed; neither is a close this side already sent via ws-shutdown."
  [handle f]
  (let [st (:state handle)]
    (when (= :open (:status @st))
      (when-not (:close-sent? @st)
        (try (write-frame! handle (if (:code f)
                                    {:opcode :close :code (:code f)}
                                    {:opcode :close}))
             (catch e nil)))
      (close-socket! handle)
      (swap! st assoc :status :closed))))

(defn ws-recv
  "Returns the next application message {:opcode :text|:binary
  :payload ..}, reading through control traffic: a ping is answered
  with a masked pong and never surfaced, a pong is swallowed. A close
  (the peer's, or the synthetic 1006 of a dropped connection) runs the
  close handshake and returns as {:opcode :close :code .. :reason ..};
  after it, ws-recv throws :ws/closed. Opts: :timeout ms, a deadline
  over the whole wait, checked between socket reads (each read blocks
  at most the connection's :read-timeout); expiry throws :ws/timeout.
  Without :timeout a socket read timeout surfaces as :net/timeout on
  a client handle; on a server handle it is a tick and the wait
  continues (see ws-accept). A pending ws-shutdown flag is honored
  between reads."
  ([handle] (ws-recv handle {}))
  ([handle opts]
   (when-not (map? opts)
     (bad "opts must be a map"))
   (let [ms (:timeout opts)
         deadline (when ms (+ (time-ms) ms))]
     (loop []
       (if-let [f (pop-pending! handle)]
         (case (:opcode f)
           :ping (do (write-frame! handle {:opcode :pong
                                           :payload (:payload f)})
                     (recur))
           :pong (recur)
           :close (do (finish-close! handle f)
                      {:opcode :close :code (:code f) :reason (:reason f)})
           {:opcode (:opcode f) :payload (:payload f)})
         (if (= :closed (:status @(:state handle)))
           (closed-fail)
           (do (send-shutdown-close! handle)
               (read-frames! handle deadline ms)
               (recur))))))))

;;;; Close

(defn ws-close
  "This side of the close handshake: sends a close frame (:code
  default 1000, optional :reason; masked on a client handle) and
  closes the socket. Idempotent; if the peer's close already finished
  the connection (via ws-recv) this is a no-op. Returns nil."
  ([handle] (ws-close handle {}))
  ([handle opts]
   (when-not (map? opts)
     (bad "opts must be a map"))
   (let [st (:state handle)]
     (when (= :open (:status @st))
       (try (write-frame! handle
                          (let [f {:opcode :close
                                   :code (opt-long (:code opts) :code 1000)}]
                            (if (:reason opts)
                              (assoc f :reason (:reason opts))
                              f)))
            (catch e nil))
       (close-socket! handle)
       (swap! st assoc :status :closed))
     nil)))

;;;; Server side

(def ^:private accept-keys #{:leftover :max-payload :url})

(defn ws-accept
  "Adopts a server-side connection whose 101 upgrade head is already
  written and returns the plain handle map for ws-send / ws-recv /
  ws-close (mino.http.server builds one for a handler's :ws entry).
  opts: :leftover (bytes read past the upgrade request head),
  :max-payload (caps each received message, default 16 MiB), :url
  (the request target, informational). The role mirror of
  ws-connect: frames go out unmasked, inbound frames must arrive
  masked, and a socket read timeout is a tick rather than an error
  (an accepted socket's read window is the engine's poll interval),
  so a bare ws-recv parks until traffic, a close, or a ws-shutdown
  flag."
  ([conn] (ws-accept conn {}))
  ([conn opts]
   (when-not (map? opts)
     (bad "opts must be a map"))
   (let [unknown (filter #(not (contains? accept-keys %)) (keys opts))]
     (when (seq unknown)
       (bad (str "unknown ws-accept key(s): " (pr-str (vec unknown))))))
   (let [lo (:leftover opts)]
     {:socket conn
      :secure? false
      :role :server
      :url (:url opts)
      :max-payload (:max-payload opts)
      :state (atom {:rest (if (bytes? lo) lo (byte-array (vec (or lo []))))
                    :pending []
                    :status :open})})))

(defn ws-shutdown
  "Asks a connection to close from another thread: flags the handle so
  the ws-recv loop that owns it sends a close frame (:code default
  1001, optional :reason) at its next wake-up and finishes through
  the normal close handshake. Never writes or closes anything itself,
  so it cannot race the owning loop's frames. Idempotent; returns
  nil."
  ([handle] (ws-shutdown handle {}))
  ([handle opts]
   (when-not (map? opts)
     (bad "opts must be a map"))
   (swap! (:state handle) assoc :shutdown
          {:code (opt-long (:code opts) :code 1001)
           :reason (:reason opts)})
   nil))
