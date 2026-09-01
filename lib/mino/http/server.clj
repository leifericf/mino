(ns mino.http.server
  "HTTP server: Ring maps over the net prims (ADR 36).

  (require '[mino.http.server :as srv])
  (srv/run-server handler {:port 8080}) => {:port 8080 :stop fn}

  A handler takes a request map and returns a response map; both are
  plain data in the mino.http vocabulary. The request map keys:
  :request-method :uri :query-string :headers :body :scheme
  :http-version :conn. :remote-addr is omitted until net-accept
  widens to expose the peer address. The response map keys: :status
  (100..599), :headers (a map or a vector of pairs, values strings),
  :body (string, bytes, or nil); Content-Length, Transfer-Encoding,
  Connection, and Date belong to the server, and handler supply of
  them is answered with a 500 close. The connection loop is one
  tail-recursive function per connection with its state in loop
  locals, parked in a blocking read between iterations; a throwing
  handler yields a 500 text/plain response and a close, never
  propagation."
  (:require [clojure.string :as str]
            [clojure.core.async :as a]))

;;;; pure decisions

(def ^:private default-idle-timeout-ms 30000)
(def ^:private default-request-timeout-ms 60000)
(def ^:private read-chunk-bytes 65536)

(def ^:private error-body "internal server error")
(def ^:private bad-request-body "bad request")

(defn- bad
  [msg]
  (throw (ex-info (str "mino.http.server: " msg) {})))

(defn- connection-tokens
  "Connection header value(s) to a lowercased token set. The parser
  hands repeated headers back as vectors; values may be comma lists."
  [v]
  (->> (if (vector? v) v (if (nil? v) [] [v]))
       (mapcat #(str/split % #","))
       (map str/trim)
       (map str/lower-case)
       set))

(defn- keep-alive?
  "Does this parsed request ask the connection to stay open?
  HTTP/1.0 persists only on an explicit keep-alive; HTTP/1.1 persists
  unless close is asked."
  [parsed]
  (let [tokens (connection-tokens
                 (get (:headers parsed) "connection"))]
    (if (= "HTTP/1.0" (:http-version parsed))
      (contains? tokens "keep-alive")
      (not (contains? tokens "close")))))

(defn- response-flags
  "Encoder engine keys for the protocol and the keep-alive decision.
  HTTP/1.1 persistence is silence (no header); an HTTP/1.0 that
  persists and any close are announced on the wire."
  [http-version alive?]
  (cond
    (and alive? (= "HTTP/1.0" http-version)) {:http10? true :keep-alive? true}
    (= "HTTP/1.0" http-version) {:http10? true :close? true}
    alive? {}
    :else {:close? true}))

(defn- split-target
  "Origin-form target into Ring's uri and query-string halves."
  [t]
  (let [i (str/index-of t "?")]
    (if (nil? i)
      {:uri t :query-string nil}
      {:uri (subs t 0 i)
       :query-string (if (= i (dec (count t)))
                       ""
                       (subs t (inc i)))})))

(defn- request-map
  "One parsed request into the handler-facing map. The socket rides
  as :conn so a future transport stays possible without hiding it."
  [c parsed]
  (assoc (split-target (:target parsed))
         :request-method (keyword (str/lower-case (:method parsed)))
         :headers (:headers parsed)
         :body (:body parsed)
         :scheme :http
         :http-version (:http-version parsed)
         :conn c))

(defn- response-spec
  "Validate a handler response map and project it to encoder input;
  extra handler keys are dropped, engine keys never leak in from the
  handler. Throws on a bad shape; the connection boundary answers
  that with a 500."
  [resp]
  (when-not (map? resp)
    (bad "the handler response must be a map"))
  (let [status (:status resp)]
    (when-not (int? status)
      (bad ":status must be an integer"))
    (when-not (<= 100 status 599)
      (bad ":status must be within 100..599")))
  (let [body (:body resp)]
    (when-not (or (nil? body) (string? body) (bytes? body))
      (bad ":body must be a string, bytes, or nil")))
  (let [base {:status (:status resp)}
        base (if-let [h (:headers resp)] (assoc base :headers h) base)]
    (if-let [b (:body resp)] (assoc base :body b) base)))

(def ^:private allowed-conn-keys
  #{:idle-timeout :request-timeout :seed
    :max-header-bytes :max-body-bytes :max-headers})

(defn- normalize-opts
  "Public opts into engine opts; every deadline carries its unit and
  is type-checked here, the single normalization pass. Unknown keys
  are an error naming them. :seed is already-read socket bytes (any
  byte seq) handed to the engine by an embedder."
  [opts]
  (when-not (map? opts)
    (bad "the connection opts must be a map"))
  (let [unknown (filter (fn [k] (not (contains? allowed-conn-keys k)))
                        (keys opts))]
    (when (seq unknown)
      (bad (str "unknown connection key(s): "
                (str/join ", " (map pr-str unknown))))))
  (let [m {:idle-timeout-ms (opt-long (:idle-timeout opts)
                                      :idle-timeout default-idle-timeout-ms)
           :request-timeout-ms (opt-long (:request-timeout opts)
                                         :request-timeout
                                         default-request-timeout-ms)
           :max-header-bytes (:max-header-bytes opts)
           :max-body-bytes (:max-body-bytes opts)
           :max-headers (:max-headers opts)}]
    (if-let [s (:seed opts)] (assoc m :seed (vec s)) m)))

(defn- parse-caps
  "Engine opts into http-parse-request caps; absent caps stay absent
  so the prim applies its own defaults."
  [opts]
  (reduce (fn [o k]
            (if-let [v (get opts k)] (assoc o k v) o))
          {}
          [:max-header-bytes :max-body-bytes :max-headers]))

(defn- expired?
  "Pure: has now-ms passed since-ms by budget-ms?"
  [now-ms since-ms budget-ms]
  (>= (- now-ms since-ms) budget-ms))

;;;; the run-server lifecycle

(def ^:private allowed-server-keys
  #{:port :host :idle-timeout :request-timeout
    :max-header-bytes :max-body-bytes :max-headers
    :acceptors :max-conns})

(def ^:private default-acceptors 2)
(def ^:private default-max-conns 16)
(def ^:private accept-poll-ms 250)
(def ^:private write-timeout-ms 5000)
(def ^:private stop-grace-ms 5000)
(def ^:private spawn-retries 12)
(def ^:private spawn-retry-ms 25)

(defn- opt-long
  [v k default]
  (cond (nil? v) default
        (int? v) v
        :else (bad (str "key :" (name k) " must be an integer"))))

(defn- opt-pos-int
  [v k default]
  (cond (nil? v) default
        (and (int? v) (pos? v)) v
        :else (bad (str "key :" (name k) " must be a positive integer"))))

(defn- check-cap
  [opts k]
  (when-let [v (get opts k)]
    (when-not (int? v)
      (bad (str "key :" (name k) " must be an integer")))))

(defn- socket-poll-ms
  "Socket read-timeout derived from the idle budget: a poll interval
  well under the budget, so a quiet connection wakes and re-parks
  inside it."
  [idle-timeout-ms]
  (max 20 (min 250 (quot idle-timeout-ms 4))))

(defn- spawn-future
  "future-call with a short retry against a momentarily-full thread
  grant; nil when the grant stays exhausted. A grant too small for
  the pool's shape degrades it (fewer acceptors, the spawner serving
  connections inline) instead of failing the server."
  [thunk]
  (loop [n spawn-retries]
    (or (try (future-call thunk) (catch e nil))
        (when (pos? n)
          (thread-sleep spawn-retry-ms)
          (recur (dec n))))))

(defn- run-conn!
  "One connection's whole life on whatever thread runs this: serve,
  close, and hand the permit back. The catch is the pool boundary; a
  connection that somehow escapes serve-conn* still closes and still
  returns its permit."
  [c handler opts permits]
  (try (serve-conn* c handler opts) (catch e nil))
  (try (net-close c) (catch e nil))
  (try (a/>!! permits :p) (catch e nil)))

(defn- sweep-mailbox!
  "Close every connection still waiting in the mailbox and return its
  permit. A swept connection has no worker, so nothing is parked on
  it and closing is safe; channel takes are exactly-once, so a racing
  spawner can never double-own one."
  [mailbox permits]
  (loop []
    (when-let [c (a/poll! mailbox)]
      (try (net-close c) (catch e nil))
      (try (a/>!! permits :p) (catch e nil))
      (recur))))

(defn run-server
  "Start handler on a loopback listener; returns {:port :stop}. opts:
  :port (default 0, kernel-chosen and read back), :host (default
  127.0.0.1), :acceptors (default 2, futures racing on the shared
  listener), :max-conns (default 16, the permit count bounding
  simultaneous connections), :idle-timeout, :request-timeout,
  :max-header-bytes, :max-body-bytes, :max-headers. Unknown keys are
  an error naming them. An acceptor takes a permit before it accepts,
  so a full permit set parks the acceptors and the kernel backlog
  absorbs the waiting connections; each accepted connection goes to
  its own worker future, which serves it to completion and returns
  the permit at close. stop wakes every acceptor, closes the
  listener, drains unserved connections from the mailbox, and joins
  the pool within a bounded grace; a connection that outlives the
  grace is left to its own deadline, never closed underneath a
  parked read.

  For a clean shutdown on SIGTERM, trap the signal and stop from the
  handler; stop drains live connections inside the grace, so an
  in-flight request finishes before exit runs:

    (let [s (run-server handler {:port 8080})]
      (on-signal :term (fn [] ((:stop s)) (exit 0))))"
  [handler opts]
  (when-not (map? opts)
    (bad "the server opts must be a map"))
  (let [unknown (filter (fn [k] (not (contains? allowed-server-keys k)))
                        (keys opts))]
    (when (seq unknown)
      (bad (str "unknown server key(s): "
                (str/join ", " (map pr-str unknown))))))
  (let [host (or (:host opts) "127.0.0.1")
        port (opt-long (:port opts) :port 0)
        idle-timeout (opt-long (:idle-timeout opts)
                                :idle-timeout default-idle-timeout-ms)
        request-timeout (opt-long (:request-timeout opts)
                                   :request-timeout
                                   default-request-timeout-ms)
         acceptors-n (opt-pos-int (:acceptors opts)
                                  :acceptors default-acceptors)
         max-conns (opt-pos-int (:max-conns opts)
                                :max-conns default-max-conns)
         _ (doseq [k [:max-header-bytes :max-body-bytes :max-headers]]
             (check-cap opts k))
         conn-opts (select-keys opts [:idle-timeout :request-timeout
                                      :max-header-bytes :max-body-bytes
                                      :max-headers])
         poll-ms (socket-poll-ms idle-timeout)
         l (net-listen host port {:backlog 16})
        running? (atom true)
        permits (a/chan max-conns)
        mailbox (a/chan max-conns)
        stop-sig (a/chan)
        active (atom 0)
        _ (dotimes [i max-conns] (a/>!! permits i))
        sweep! (fn [] (sweep-mailbox! mailbox permits))
        accept! (fn []
                  (try (net-accept
                         l {:accept-timeout accept-poll-ms
                            :read-timeout poll-ms
                            :write-timeout write-timeout-ms})
                       (catch e nil)))
         spawn-conn! (fn [c]
                       (swap! active inc)
                       (if-let [fut (try (future-call
                                           (fn []
                                             (run-conn! c handler conn-opts permits)
                                             (swap! active dec)))
                                         (catch e nil))]
                         nil
                         ;; no thread to spare: the spawner becomes this
                         ;; connection's worker. Keep it counted in
                         ;; active across the inline serve so the stop
                         ;; drain waits for it like any pooled worker;
                         ;; permits still bound what queues behind it.
                         (try (run-conn! c handler conn-opts permits)
                              (finally (swap! active dec)))))
        spawner-thunk (fn []
                        (loop []
                          (let [[c src] (a/alts!! [mailbox stop-sig])]
                            (when (= src mailbox)
                              (spawn-conn! c)
                              (recur))))
                        (sweep!)
                        :spawner-done)
        spawner (spawn-future spawner-thunk)
        acceptor-thunk (fn []
                         (loop []
                           (when @running?
                             (let [[p src] (a/alts!! [permits stop-sig])]
                               (when (= src permits)
                                 (let [c (accept!)]
                                   (cond
                                     (nil? c)
                                     (do (try (a/>!! permits :p) (catch e nil))
                                         (recur))

                                     ;; the stop raced the accept: this
                                     ;; connection is unserved and this
                                     ;; acceptor still holds its permit
                                     (not @running?)
                                     (do (try (net-close c) (catch e nil))
                                         (try (a/>!! permits :p)
                                              (catch e nil)))

                                     :else
                                     (do (try (a/>!! mailbox c)
                                              (catch e nil))
                                         (recur))))))))
                         :acceptor-done)]
    (when (nil? spawner)
      (reset! running? false)
      (a/close! stop-sig)
      (try (net-close l) (catch e nil))
      (bad "the connection spawner could not start; the host thread grant is too small"))
    (let [acc-futs (atom [])
          _ (dotimes [i acceptors-n]
              (when-let [f (spawn-future acceptor-thunk)]
                (swap! acc-futs conj f)))
          _ (when (empty? @acc-futs)
              (reset! running? false)
              (a/close! stop-sig)
              (try (net-close l) (catch e nil))
              (try (deref spawner stop-grace-ms :grace-expired)
                   (catch e nil))
              (bad "no acceptor could start; the host thread grant is too small"))]
      {:port (net-listener-port l)
       :stop (fn []
               (reset! running? false)
               (a/close! stop-sig)
               (try (net-close l) (catch e nil))
               (let [t0 (time-ms)
                     left (fn []
                            (max 0 (- (+ t0 stop-grace-ms) (time-ms))))]
                 (doseq [f @acc-futs]
                   (try (deref f (left) :grace-expired) (catch e nil)))
                 (try (deref spawner (left) :grace-expired)
                      (catch e nil))
                 (sweep!)
                 (loop []
                   (when (and (pos? @active) (pos? (left)))
                     (thread-sleep 25)
                     (recur))))
               nil)})))

;;;; the connection loop

(defn- read-chunk
  "One net-read as a status: the bytes when any arrived, :tick on a
  read-window expiry (the poll interval), :eof when the peer closed
  or the connection failed."
  [c]
  (try
    (let [b (net-read c read-chunk-bytes)]
      (if b b :eof))
    (catch e
      (if (= :net/timeout (:mino/kind e)) :tick :eof))))

(defn- read-request
  "Read one request off c seeded with leftover bytes, parked between
  reads. Answers {:kind :request :parsed map}, {:kind :bad-request},
  {:kind :eof} (the peer left before any byte of this request), or a
  budget drop ({:kind :idle-timeout} while still waiting for the
  first byte, {:kind :request-timeout} once a partial request has
  the wall clock against it). A peer that closes mid-request gets its
  partial bytes reparsed under :eof: a request that completed just
  before the close is still served."
  [c seed opts idle-since-ms]
  (let [caps (parse-caps opts)]
    (loop [buf seed
           first-byte-ms (when (pos? (count seed)) (time-ms))]
      (let [parsed (http-parse-request (byte-array buf) caps)]
        (cond
          (= :done (:status parsed)) {:kind :request :parsed parsed}
          (= :error (:status parsed)) {:kind :bad-request}

          (and first-byte-ms
               (expired? (time-ms) first-byte-ms
                         (:request-timeout-ms opts)))
          {:kind :request-timeout}

          :else
          (let [chunk (read-chunk c)]
            (cond
              (bytes? chunk)
              (recur (into buf (vec chunk))
                     (or first-byte-ms (time-ms)))

              (= :tick chunk)
              (if (and (nil? first-byte-ms)
                       (expired? (time-ms) idle-since-ms
                                 (:idle-timeout-ms opts)))
                {:kind :idle-timeout}
                (recur buf first-byte-ms))

              :else
              (if (empty? buf)
                {:kind :eof}
                (let [fin (http-parse-request (byte-array buf)
                                              (assoc caps :eof true))]
                  (if (= :done (:status fin))
                    {:kind :request :parsed fin}
                    {:kind :bad-request}))))))))))

(defn- send-error-response
  "Write one fixed error response; a failing write only ends the
  connection sooner."
  [c status body]
  (try
    (net-write c (http-encode-response
                   {:status status
                    :headers [["Content-Type" "text/plain"]]
                    :body body
                    :date (format-time (now) :rfc1123)
                    :close? true}))
    (catch e nil)))

(defn- serve-request
  "Run the handler and write its response. The try boundary is the
  isolation: a throwing handler or an unencodable response map yields
  a 500 text/plain close instead of propagation."
  [c handler req alive?]
  (try
    (net-write c (http-encode-response
                   (merge (response-spec (handler req))
                          (response-flags (:http-version req) alive?)
                          {:date (format-time (now) :rfc1123)}
                          ;; RFC 9110: a HEAD response keeps the same
                          ;; header fields (including Content-Length)
                          ;; as GET but carries no body octets.
                          (when (= :head (:request-method req))
                            {:head? true}))))
    (catch e
      (send-error-response c 500 error-body))))

(defn serve-conn*
  "Serve one accepted connection as keep-alive Ring traffic until the
  peer asks to close, goes quiet past :idle-timeout, drips a request
  past :request-timeout, or closes. Connection state lives in loop
  locals; every blocking read parks the future. Public as the test
  and embedding seam over the private loop, not part of the request
  vocabulary. opts: :idle-timeout :request-timeout :max-header-bytes
  :max-body-bytes :max-headers, plus :seed (already-read socket
  bytes, embedding use); unknown keys are an error naming them."
  [c handler opts]
  (let [opts (normalize-opts opts)]
    (loop [seed (or (:seed opts) [])
           idle-since-ms (time-ms)]
      (let [r (read-request c seed opts idle-since-ms)]
        (case (:kind r)
          :request (let [parsed (:parsed r)
                         alive? (keep-alive? parsed)]
                     (serve-request c handler (request-map c parsed) alive?)
                     (when alive?
                       (recur (vec (:leftover parsed)) (time-ms))))
          :bad-request (send-error-response c 400 bad-request-body)
          nil)))))
