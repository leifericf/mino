(require '[clojure.string :as str])
(require '[clojure.data.json :as json])
(require '[mino.http.server :as srv])

;; Loopback HTTP fixture server written in mino itself: the Ring
;; routes ride the mino.http.server engine over the net listener
;; prims, so the client suites drive the real stack against the real
;; server library with no subprocess and no platform gating.
;;
;; Route table (GET unless noted):
;;   /hello         200 "hello world"
;;   /echo          200 echoes the request body
;;   /echo-path     200 echoes the raw request path (with query)
;;   /echo-headers  200 JSON: path, parsed query params, and the
;;                  user-agent / accept / x-probe / content-type
;;                  request headers
;;   /echo-json     POST 200 JSON {"echo": <parsed body>, "seen": the
;;                  request Content-Type}; 400 when the Content-Type
;;                  is not application/json or the body does not parse
;;   /items         200 JSON page N ("?page=N", default 1): two pages
;;                  of items, page 1 carries "next_page": 2, page 2
;;                  carries "next_page": null
;;   /r1 /r2 /final 301 chain /r1 -> /r2 -> /final (200
;;                  "final-landing")
;;   /r307          307 to /echo (method and body preserved; the
;;                  offered body is consumed before the redirect)
;;   /boom          throws; the engine answers 500 and the server
;;                  survives (the isolation boundary, client-visible)
;;   /gzip          200 Content-Encoding: gzip; the payload constant
;;                  below carries its generation provenance
;;   /conncount     200 the number of connections accepted so far
;;   /big           200 100000 bytes of "x"
;;
;; Raw-wire legs keep a slim custom serve path (exact wire shapes the
;; Content-Length framed engine cannot emit):
;;   /chunked       200 raw chunked frames dechunking to "hello world"
;;   /http10        raw HTTP/1.0 close-delimited response, then close
;;   /head-cl       HEAD: Content-Length 11, never a body
;;   /head-no-cl    HEAD: no framing header, never a body
;;   /close         200 "bye-close" with Connection: close, then close
;;   /hold          consumes the request, then silence until teardown
;;                  (the read-timeout fixture; no delayed-response
;;                  route exists)
;;   anything else  404 "not here"
;;
;; The first request on a connection decides its path: a raw target
;; stays on this loop, anything else hands the connection (seeded
;; with the bytes already read) to the engine, which then owns its
;; whole keep-alive life. Connections are accepted one at a time
;; inside the accept future; the suites drive one conversation at a
;; time, so no worker is pinned per idle connection and the fixture
;; stays inside the host thread grant on small runners.

(def ^:private fx-idle-ms
  "Keep-alive idle budget per connection: how long a served
  connection keeps watching for the next request after a response."
  2000)

(def ^:private fx-poll-ms
  "Read timeout preset on accepted sockets: bounds one parked read
  (a poll interval well under the idle budget, a stalled peer, and
  the /hold silence cycle at teardown)."
  250)

(def ^:private fx-max-request
  "Hard cap on one request's header-plus-body bytes; a peer that
  exceeds it gets the connection dropped."
  1048576)

(def ^:private fx-gzip-b64
  "Static /gzip payload: \"gz-integration-\" repeated 40 times,
  compressed once with gzip(1) and base64-encoded via
  printf 'gz-integration-%.0s' {1..40} | gzip -n | base64
  so the wire bytes are stable across runs."
  "H4sIAAAAAAAAA0uv0s3MK0lNL0osyczP000f5Y5yqcEFAEyeTMhYAgAA")

(def ^:private fx-big
  "Static /big payload: 100000 bytes of \"x\", built once at load.
  Built per request it cost a 100k-element lazy reduce per /big hit
  (measured 1.2 s under ASan on CI runners), which pushed the
  body-cap test's client past its 3 s read-timeout and flipped the
  expected :codec/limit into :net/timeout."
  (apply str (repeat 100000 "x")))

;;;; request peeking

(defn- fx-bytes-text
  "Widen bytes through char; fixture requests are ASCII."
  [b]
  (apply str (map char (seq b))))

(defn- fx-split-path
  "Split a raw request path into [path query]."
  [path]
  (let [i (str/index-of path "?")]
    (if (nil? i)
      [path ""]
      [(subs path 0 i)
       (if (= i (dec (count path))) "" (subs path (inc i)))])))

(defn- fx-parse-query
  "Query string to {k [v]}: values collected like a form parser,
  percent-escapes decoded, a bare key yielding [\"\"]."
  [q]
  (if (= "" q)
    {}
    (into {}
          (map (fn [pair]
                 (let [i (str/index-of pair "=")]
                   [(percent-decode (if i (subs pair 0 i) pair))
                    [(percent-decode
                       (if i (subs pair (inc i)) ""))]]))
               (str/split q #"&")))))

(defn- fx-read-chunk
  "One net-read as a status: the bytes when any arrived, :idle when
  the read window elapsed with nothing, :eof when the peer closed or
  the connection failed."
  [c]
  (try (let [b (net-read c 65536)]
         (if b b :eof))
       (catch e
         (if (= :net/timeout (:mino/kind e)) :idle :eof))))

(defn- fx-read-step
  "Fold one read outcome into the request loop: more bytes, the idle
  marker with the partial bytes, or nil when the peer went away."
  [c acc]
  (let [ch (fx-read-chunk c)]
    (cond
      (bytes? ch) [:more (into acc (seq ch))]
      (= :idle ch) {:idle acc}
      :else nil)))

(defn- fx-read-request
  "Read one request off c seeded with any leftover bytes. Returns a
  parsed request map (carrying :seed, every byte read so far, so the
  engine can reparse the connection without losing them), {:idle acc}
  when no full request arrived within the read window (acc keeps the
  partial bytes), :fx-too-large past the byte cap, or nil when the
  peer went away."
  [c seed]
  (loop [acc (vec seed)]
    (let [s (apply str (map char acc))
          hdr-end (str/index-of s "\r\n\r\n")]
      (cond
        (some? hdr-end)
        (let [lines (str/split (subs s 0 hdr-end) #"\r\n")
              [method path] (str/split (first lines) #" ")
              headers (into {}
                            (map (fn [line]
                                   (let [i (str/index-of line ":")]
                                     [(str/lower-case (subs line 0 i))
                                      (str/trim (subs line (inc i)))]))
                                 (rest lines)))
              clen (try (parse-long (get headers "content-length"))
                        (catch e nil))
              n (or clen 0)
              total (+ hdr-end 4 n)]
          (cond
            (> total fx-max-request) :fx-too-large
            (< (count acc) total) (let [r (fx-read-step c acc)]
                                    (if (vector? r)
                                      (recur (second r))
                                      r))
            :else {:method method
                   :path path
                   :headers headers
                   :body (byte-array (take n (drop (+ hdr-end 4) acc)))
                   :leftover (vec (drop total acc))
                   :seed acc}))

        (> (count acc) fx-max-request) :fx-too-large
        :else (let [r (fx-read-step c acc)]
                (if (vector? r)
                  (recur (second r))
                  r))))))

;;;; Ring routes (served by the mino.http.server engine)

(defn- fx-headers-echo [path q headers]
  {"path" path
   "query" (fx-parse-query q)
   "user-agent" (or (get headers "user-agent") "")
   "accept" (or (get headers "accept") "")
   "x-probe" (or (get headers "x-probe") "")
   "content-type" (or (get headers "content-type") "")})

(defn- fx-json-echo [headers body]
  (let [ctype (or (get headers "content-type") "")]
    (if (not (str/starts-with? ctype "application/json"))
      {:status 400
       :headers [["Content-Type" "application/json"]]
       :body (json/write-str
               {:error (str "expected application/json, got " ctype)})}
      (try
        {:status 200
         :headers [["Content-Type" "application/json"]]
         :body (json/write-str {:echo (json/read-str (fx-bytes-text body))
                                :seen ctype})}
        (catch e
          {:status 400
           :headers [["Content-Type" "application/json"]]
           :body (json/write-str {:error "body is not valid JSON"})})))))

(defn- fx-items-page [q]
  (let [raw (get q "page")
        n (when raw (try (parse-long (first raw)) (catch e nil)))]
    (if (= n 2)
      {:page 2 :items ["gamma"] :next_page nil}
      {:page 1 :items ["alpha" "beta"] :next_page 2})))

(defn- fx-text
  "A text/plain response body."
  [status body]
  {:status status
   :headers [["Content-Type" "text/plain"]]
   :body body})

(defn- fx-ring-handler
  "The fixture's Ring route table, served keep-alive by the engine."
  [accepts]
  (fn [req]
    (let [uri (:uri req)
          q (or (:query-string req) "")
          path (if (= "" q) uri (str uri "?" q))
          headers (:headers req)]
      (cond
        (= uri "/hello") (fx-text 200 "hello world")
        (= uri "/echo") (fx-text 200 (or (:body req) (byte-array 0)))
        (str/starts-with? uri "/echo-path") (fx-text 200 path)
        (= uri "/echo-headers")
        {:status 200
         :headers [["Content-Type" "application/json"]]
         :body (json/write-str (fx-headers-echo path q headers))}
        (= uri "/echo-json") (fx-json-echo headers (:body req))
        (= uri "/items")
        {:status 200
         :headers [["Content-Type" "application/json"]]
         :body (json/write-str (fx-items-page (fx-parse-query q)))}
        (= uri "/r1") {:status 301 :headers [["Content-Type" "text/plain"]
                                             ["Location" "/r2"]]
                       :body "moved"}
        (= uri "/r2") {:status 301 :headers [["Content-Type" "text/plain"]
                                             ["Location" "/final"]]
                       :body "moved"}
        (= uri "/final") (fx-text 200 "final-landing")
        (= uri "/r307") {:status 307 :headers [["Content-Type" "text/plain"]
                                               ["Location" "/echo"]]
                         :body "keep it"}
        (= uri "/boom") (throw (ex-info "fixture boom route" {}))
        (= uri "/gzip") {:status 200
                         :headers [["Content-Type" "text/plain"]
                                   ["Content-Encoding" "gzip"]]
                         :body (base64-decode fx-gzip-b64)}
        (= uri "/conncount") (fx-text 200 (str @accepts))
        (= uri "/big") (fx-text 200 fx-big)
        :else (fx-text 404 "not here")))))

;;;; raw-wire routes (served by this file's slim loop)

(defn- fx-reason [code]
  (get {200 "OK"
        301 "Moved Permanently"
        307 "Temporary Redirect"
        400 "Bad Request"
        404 "Not Found"}
       code "OK"))

(defn- fx-raw-route
  "One peeked request to a [response-spec close-after?] pair on the
  raw-wire legs, or nil when the request belongs to the engine. A
  :raw spec is written verbatim; a :hold spec answers nothing and
  goes silent."
  [method p]
  (cond
    (= p "/chunked")
    [{:raw (str "HTTP/1.1 200 OK\r\n"
                "Transfer-Encoding: chunked\r\n\r\n"
                "6\r\nhello \r\n"
                "5\r\nworld\r\n"
                "0\r\n\r\n")}
     false]
    (= p "/http10")
    [{:raw (str "HTTP/1.0 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "\r\nclose-delimited-body")}
     true]
    (= p "/close")
    [{:code 200 :body "bye-close"
      :headers [["Connection" "close"]]} true]
    (and (= method "HEAD") (= p "/head-cl"))
    [{:raw (str "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 11\r\n\r\n")}
     false]
    (and (= method "HEAD") (= p "/head-no-cl"))
    [{:raw (str "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n\r\n")}
     false]
    (= p "/hold") [{:hold true} false]
    :else nil))

;;;; serving

(defn- fx-send
  "Write one raw-path response spec. Framed bodies carry
  Content-Length; :raw specs are written exactly as given. Every
  framed response carries a Date header (RFC 1123 via format-time,
  dogfooding the time layer) so tests can exercise Date-header
  parsing end to end."
  [c spec]
  (if-let [raw (:raw spec)]
    (net-write c raw)
    (let [body (:body spec)
          blen (if (nil? body) 0 (count body))]
      (net-write
        c
        (str "HTTP/1.1 " (:code spec) " " (fx-reason (:code spec)) "\r\n"
             "Date: " (format-time (now) :rfc1123) "\r\n"
             "Content-Type: " (or (:ctype spec) "text/plain") "\r\n"
             (apply str (map (fn [h] (str (first h) ": " (second h) "\r\n"))
                             (or (:headers spec) [])))
             "Content-Length: " blen "\r\n\r\n"))
      (when (pos? blen)
        (net-write c body)))))

(defn- fx-hold-open
  "The /hold silence: consume whatever arrives, answer nothing, and
  wake at most one poll interval past teardown."
  [c running?]
  (loop []
    (when @running?
      (let [b (try (net-read c 1) (catch e ::tick))]
        ;; nil is the peer closing; a read-window expiry (or more
        ;; inbound bytes, consumed and ignored) keeps the silence.
        (when (some? b)
          (recur))))))

(defn- fx-serve-conn
  "Serve one connection: the first request decides its path. A
  raw-wire target stays on this slim loop; anything else hands the
  connection, seeded with every byte already read, to the server
  engine, which owns its whole keep-alive life from there. The
  socket's read timeout is only a poll interval: a quiet connection
  is re-parked while it is still inside the budget and its reads can
  still end on their own."
  [c srv]
  (loop [seed [] idle-since (time-ms)]
    (let [r (fx-read-request c seed)]
      (cond
        (nil? r) nil
        (:idle r) (when (and @(:running? srv)
                              (< (- (time-ms) idle-since) fx-idle-ms))
                    (recur (:acc r) idle-since))
        (map? r)
        (let [[p _q] (fx-split-path (:path r))]
          (if-let [[spec close-after] (fx-raw-route (:method r) p)]
            (if (:hold spec)
              (fx-hold-open c (:running? srv))
              (do
                (fx-send c spec)
                (when-not close-after
                  (recur (:leftover r) (time-ms)))))
            (srv/serve-conn* c (:handler srv)
                             {:idle-timeout fx-idle-ms
                              :seed (:seed r)})))))))

(defn- fx-accept-loop [l srv]
  (loop []
    (when @(:running? srv)
      (try
        (let [c (net-accept l {:accept-timeout 250
                               :read-timeout fx-poll-ms
                               :write-timeout 5000})]
          (swap! (:accepts srv) inc)
          (swap! (:conns srv) conj c)
          (try
            (fx-serve-conn c srv)
            (catch e nil))
          (try (net-close c) (catch e nil)))
        (catch e nil))
      (recur))))

;;;; fixture API

(defn fx-start!
  "Start the fixture server on a kernel-chosen loopback port. Returns
  {:port n :accepts atom :stop fn}; stop ends the accept loop, closes
  the listener, and joins every serve cycle with a bounded grace
  period. Accepted sockets are closed by the serve loop itself once
  its reads have ended, so a descriptor is never closed underneath a
  blocked read."
  []
  (let [l (net-listen "127.0.0.1" 0 {:backlog 16})
        accepts (atom 0)
        srv {:port (net-listener-port l)
             :listener l
             :running? (atom true)
             :conns (atom [])
             :accepts accepts
             :handler (fx-ring-handler accepts)}
        fut (future (fx-accept-loop l srv))]
    (assoc srv
           :stop (fn []
                   (reset! (:running? srv) false)
                   (try (net-close l) (catch e nil))
                   (let [r (deref fut 5000 :fx-grace-expired)]
                     ;; After the loop joined, any straggler it failed
                     ;; to close (an unexpected throw) is safe to sweep.
                      (doseq [c @(:conns srv)]
                        (try (net-close c) (catch e nil)))
                      r)))))

(defn fx-with-server
  "Run (body srv) with a fresh fixture server; teardown runs whether
  the body passed, threw, or errored. srv exposes :port, the :accepts
  tally, and :stop."
  [body]
  (let [srv (fx-start!)]
    (try
      (body srv)
      (finally
        ((:stop srv))))))
