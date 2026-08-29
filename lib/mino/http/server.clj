(ns mino.http.server
  "HTTP server: Ring-shaped handler maps over the net prims (ADR 36).

  (require '[mino.http.server :as srv])
  (srv/run-server handler {:port 8080}) => {:port 8080 :stop fn}

  A handler takes a request map and returns a response map; both are
  plain data in the mino.http vocabulary. The connection loop is one
  tail-recursive function per connection with its state in loop
  locals, parked in a blocking read between iterations; a throwing
  handler yields a 500 text/plain response and a close, never
  propagation."
  (:require [clojure.string :as str]))

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

(defn- normalize-opts
  "Public opts into engine opts; every deadline carries its unit."
  [opts]
  {:idle-timeout-ms (or (:idle-timeout opts) default-idle-timeout-ms)
   :request-timeout-ms (or (:request-timeout opts) default-request-timeout-ms)
   :max-header-bytes (:max-header-bytes opts)
   :max-body-bytes (:max-body-bytes opts)
   :max-headers (:max-headers opts)})

(defn- parse-caps
  "Engine opts into http-parse-request caps; absent caps stay absent
  so the prim applies its own defaults."
  [opts]
  (reduce (fn [o k]
            (if-let [v (get opts k)] (assoc o k v) o))
          {}
          [:max-header-bytes :max-body-bytes :max-headers]))

;;;; the run-server lifecycle

(def ^:private allowed-server-keys
  #{:port :host :idle-timeout :request-timeout
    :max-header-bytes :max-body-bytes :max-headers})

(def ^:private accept-poll-ms 250)
(def ^:private write-timeout-ms 5000)
(def ^:private stop-grace-ms 5000)

(defn- opt-long
  [v k default]
  (cond (nil? v) default
        (int? v) v
        :else (bad (str "key :" (name k) " must be an integer"))))

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

(defn run-server
  "Start handler on a loopback listener; returns {:port :stop}. opts:
  :port (default 0, kernel-chosen and read back), :host (default
  127.0.0.1), :idle-timeout, :request-timeout, :max-header-bytes,
  :max-body-bytes, :max-headers. Unknown keys are an error naming
  them. One acceptor future serves connections sequentially; stop
  ends the accept loop, closes the listener, and joins the future
  with a bounded grace. Accepted sockets are closed by their own
  serve cycle, never underneath a parked read; a connection that
  outlives the grace is left to its own deadline."
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
        _ (doseq [k [:max-header-bytes :max-body-bytes :max-headers]]
            (check-cap opts k))
        poll-ms (socket-poll-ms idle-timeout)
        l (net-listen host port {:backlog 16})
        running? (atom true)
        conns (atom [])
        fut (future
              (loop []
                (when @running?
                  (let [c (try (net-accept
                                 l {:accept-timeout accept-poll-ms
                                    :read-timeout poll-ms
                                    :write-timeout write-timeout-ms})
                               (catch e nil))]
                    (when c
                      (swap! conns conj c)
                      ;; serve-conn* owns the single normalization pass;
                      ;; opts arrives in its public shape
                      (try (serve-conn* c handler opts)
                           (catch e nil))
                      (try (net-close c) (catch e nil)))
                    (recur))))
              :served)]
    {:port (net-listener-port l)
     :stop (fn []
             (reset! running? false)
             (try (net-close l) (catch e nil))
             (let [joined (try (deref fut stop-grace-ms :grace-expired)
                               (catch e :future-error))]
               ;; Sweep only after the loop joined: a live loop owns
               ;; its parked sockets and closes them itself once their
               ;; reads end.
               (when (not= :grace-expired joined)
                 (doseq [c @conns]
                   (try (net-close c) (catch e nil))))
               nil))}))

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
               (>= (- (time-ms) first-byte-ms)
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
                       (>= (- (time-ms) idle-since-ms)
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
                          {:date (format-time (now) :rfc1123)})))
    (catch e
      (send-error-response c 500 error-body))))

(defn serve-conn*
  "Serve one accepted connection as keep-alive Ring traffic until the
  peer asks to close, goes quiet past :idle-timeout, drips a request
  past :request-timeout, or closes. Connection state lives in loop
  locals; every blocking read parks the future. Public as the test
  and embedding seam over the private loop, not part of the request
  vocabulary. opts: :idle-timeout :request-timeout :max-header-bytes
  :max-body-bytes :max-headers."
  [c handler opts]
  (let [opts (normalize-opts opts)]
    (loop [seed []
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
