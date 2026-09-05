(require "tests/test")
(require '[clojure.string :as str])
(require '[mino.http :as http])
(require '[mino.http.server :as srv])

;; The connection engine: one accepted socket served as keep-alive
;; Ring traffic through serve-conn*, over real loopback sockets. The
;; client side is hand-written wire traffic (http-encode-request or
;; raw bytes) read back through the client response parser, so every
;; leg is live bytes on both halves of the codec.

(defn- srv-bb [& ss]
  (byte-array (mapcat #(if (bytes? %) (vec %) (map int %)) ss)))

(defn- srv-req
  ([method target] (http-encode-request
                     {:method method :target target :host "t.example"}))
  ([method target extra]
   (http-encode-request (merge {:method method :target target
                                :host "t.example"}
                               extra))))

(def ^:private srv-slow-host?
  ;; Sanitizer-instrumented CI legs export MINO_SLOW_HOST and run the
  ;; engine several times slower; the fixture windows scale so a
  ;; starved worker still answers inside them (the net_test churn
  ;; discipline). The budgets stay assertions: a wedged server fails
  ;; them, just later.
  (some? (getenv "MINO_SLOW_HOST")))

(defn- srv-connect [port]
  (let [ms (if srv-slow-host? 24000 8000)]
    (net-connect "127.0.0.1" port {:read-timeout ms :write-timeout ms})))

(defn- srv-close-req
  "A request that asks the connection to close, written raw: the
  request encoder never emits Connection (that framing belongs to the
  request executor)."
  [method target]
  (srv-bb method " " target " HTTP/1.1\r\nHost: t.example\r\n"
          "Connection: close\r\n\r\n"))

(defn- srv-send [c & parts]
  (net-write c (apply srv-bb parts)))

(defn- srv-response-end
  "Byte offset at which one content-length framed (or bodiless)
  response ends inside acc."
  [acc r]
  (let [s (apply str (map char acc))
        hdr-end (+ (str/index-of s "\r\n\r\n") 4)
        cl (try (parse-long (get (:headers r) "content-length"))
                (catch e nil))]
    (if cl (+ hdr-end cl) hdr-end)))

(defn- srv-read-one
  "Read one complete response off c given any pending bytes from an
  earlier read; returns {:resp r :pending rest-bytes}, or nil when the
  peer closed before a full response arrived."
  [c pending]
  (loop [acc pending]
    (let [r (http-parse-response (byte-array acc))]
      (if (= :done (:status r))
        {:resp r :pending (vec (drop (srv-response-end acc r) acc))}
        (let [b (try (net-read c 65536) (catch e nil))]
          (if b
            (recur (into acc (vec b)))
            (let [fin (http-parse-response (byte-array acc) {:eof true})]
              (when (= :done (:status fin))
                {:resp fin :pending []}))))))))

(def ^:private srv-default-opts
  (let [budget (if srv-slow-host? 12000 4000)]
    {:idle-timeout budget :request-timeout budget :poll-ms 50}))

(defn- srv-with
  "Run (body started) against a fresh listener serving n connections
  through serve-conn*, one after another inside a single future.
  Teardown ends the accept loop, closes the listener, and joins the
  future whether the body passed, threw, or errored. Returns
  {:join the accept future's value :results per-connection outcomes};
  a :engine-crash entry means the engine let an exception escape."
  [n handler opts body]
  (let [l (net-listen "127.0.0.1" 0 {:backlog 8})
        o (merge srv-default-opts opts)
        poll-ms (:poll-ms o)
        running? (atom true)
        results (atom [])
        fut (future
              (loop [i 0]
                (when (and @running? (< i n))
                  (let [c (try (net-accept l {:accept-timeout 250
                                              :read-timeout poll-ms
                                              :write-timeout 8000})
                               (catch e nil))]
                     (when c
                       (swap! results conj
                              (try (srv/serve-conn* c handler (dissoc o :poll-ms))
                                   (catch e :engine-crash)))
                      (try (net-close c) (catch e nil)))
                    ;; Count served connections, not accept attempts: an
                    ;; accept that times out with no client (c nil) must
                    ;; not advance i, or a slow runner whose client lands
                    ;; after the 250ms window starves the server early
                    ;; (nil responses). Teardown's running? flag bounds
                    ;; the retry.
                    (recur (if c (inc i) i)))))
              (try (net-close l) (catch e nil))
              :served)
        started {:port (net-listener-port l)}]
    (try
      (body started)
      (finally
        (reset! running? false)
        (try (net-close l) (catch e nil))
        (try (deref fut 20000 nil) (catch e nil))))
    {:join (try (deref fut 20000 :join-timeout) (catch e :future-error))
     :results @results}))

(defn- srv-was-clean
  "The accept loop finished on its own and every served connection
  ran the engine to completion without an escape."
  [r]
  (is (= :served (:join r)))
  (is (not-any? #{:engine-crash} (:results r))))

;;;; one request, then close

(deftest serves-one-request-and-honors-connection-close
  (let [h (fn [req] {:status 200 :body (str "got:" (:uri req))})
        r (srv-with 1 h {}
             (fn [s]
               (let [c (srv-connect (:port s))]
                 (srv-send c (srv-close-req "GET" "/hello"))
                 (let [x (srv-read-one c [])]
                   (is (some? x))
                   (is (= 200 (:code (:resp x))))
                   (is (= (srv-bb "got:/hello") (:body (:resp x))))
                   (is (= "close" (get (:headers (:resp x)) "connection"))))
                 ;; the server closes its side; the client's read ends the
                 ;; connection -- a clean EOF (nil) or a connection reset
                 ;; the OS may deliver instead (thrown), never live data
                 (is (contains? #{nil :err} (try (net-read c 65536) (catch e :err))))
                 (try (net-close c) (catch e nil)))))]
    (srv-was-clean r)))

;;;; keep-alive

(deftest keep-alive-serves-sequential-requests-on-one-socket
  (let [h (fn [req] {:status 200
                     :body (str (name (:request-method req)) " "
                                (:uri req))})
        r (srv-with 1 h {}
             (fn [s]
               (let [c (srv-connect (:port s))]
                 (srv-send c (srv-req "GET" "/one"))
                 (let [x1 (srv-read-one c [])]
                   (is (= 200 (:code (:resp x1))))
                   (is (= (srv-bb "get /one") (:body (:resp x1))))
                   (srv-send c (srv-req "POST" "/two"))
                   (let [x2 (srv-read-one c (:pending x1))]
                     (is (= 200 (:code (:resp x2))))
                     (is (= (srv-bb "post /two") (:body (:resp x2))))
                     ;; HTTP/1.1 persistence is silence: no connection
                     ;; header at all
                     (is (nil? (get (:headers (:resp x2)) "connection")))))
                 (try (net-close c) (catch e nil)))))]
    (srv-was-clean r)))

(deftest head-response-carries-length-but-no-body-and-does-not-desync
  ;; A HEAD request must get the same headers a GET would (including
  ;; Content-Length) but zero body octets. The client parses with
  ;; :bodiless true because it knows it sent HEAD; the follow-up GET on
  ;; the same keep-alive socket must then frame cleanly, proving the
  ;; server did not leave body bytes on the wire.
  (let [h (fn [req] {:status 200 :body (str "hello:" (:uri req))})
        read-bodiless
        (fn [c pending]
          (loop [acc pending]
            (let [r (http-parse-response (byte-array acc) {:bodiless true})]
              (if (= :done (:status r))
                (let [s (apply str (map char acc))
                      end (+ (str/index-of s "\r\n\r\n") 4)]
                  {:resp r :pending (vec (drop end acc))})
                (let [b (try (net-read c 65536) (catch e nil))]
                  (when b (recur (into acc (vec b)))))))))
        r (srv-with 1 h {}
             (fn [s]
               (let [c (srv-connect (:port s))]
                 (srv-send c (srv-req "HEAD" "/page") (srv-req "GET" "/page"))
                 (let [x1 (read-bodiless c [])]
                   (is (some? x1))
                   (is (= 200 (:code (:resp x1))))
                   ;; Content-Length reports the entity length a GET
                   ;; would return ("hello:/page" is 11 bytes).
                   (is (= "11" (get (:headers (:resp x1)) "content-length")))
                   ;; but no body octets arrived
                   (is (= (srv-bb "") (:body (:resp x1))))
                   ;; the pipelined GET frames cleanly: no desync
                   (let [x2 (srv-read-one c (:pending x1))]
                     (is (some? x2))
                     (is (= (srv-bb "hello:/page") (:body (:resp x2))))))
                 (try (net-close c) (catch e nil)))))]
    (srv-was-clean r)))

(deftest pipelined-requests-in-one-write-split-exactly
  (let [h (fn [req] {:status 200 :body (name (:request-method req))})
        r (srv-with 1 h {}
             (fn [s]
               (let [c (srv-connect (:port s))]
                 (srv-send c (srv-req "GET" "/a")
                           (srv-req "POST" "/b")
                           (srv-req "GET" "/c"))
                 (let [x1 (srv-read-one c [])
                       x2 (srv-read-one c (:pending x1))
                       x3 (srv-read-one c (:pending x2))]
                   (is (some? x3))
                   (is (= (srv-bb "get") (:body (:resp x1))))
                   (is (= (srv-bb "post") (:body (:resp x2))))
                   (is (= (srv-bb "get") (:body (:resp x3)))))
                 (try (net-close c) (catch e nil)))))]
    (srv-was-clean r)))

(deftest http10-defaults-to-close
  (let [h (fn [req] {:status 200 :body "ten"})
        r (srv-with 1 h {}
             (fn [s]
               (let [c (srv-connect (:port s))]
                 (srv-send c "GET / HTTP/1.0\r\nHost: t.example\r\n\r\n")
                 (let [x (srv-read-one c [])]
                   (is (some? x))
                   (is (= "HTTP/1.0" (:http-version (:resp x))))
                   (is (= "close" (get (:headers (:resp x)) "connection"))))
                 (is (contains? #{nil :err} (try (net-read c 65536) (catch e :err))))
                 (try (net-close c) (catch e nil)))))]
    (srv-was-clean r)))

(deftest http10-keep-alive-asked-for-is-announced-and-served
  (let [h (fn [req] {:status 200 :body "10ka"})
        r (srv-with 1 h {}
             (fn [s]
               (let [c (srv-connect (:port s))]
                 (srv-send c "GET /a HTTP/1.0\r\nHost: t.example\r\n"
                           "Connection: keep-alive\r\n\r\n")
                 (let [x1 (srv-read-one c [])]
                   (is (= "keep-alive"
                          (get (:headers (:resp x1)) "connection")))
                   (srv-send c "GET /b HTTP/1.0\r\nHost: t.example\r\n"
                             "Connection: keep-alive\r\n\r\n")
                   (let [x2 (srv-read-one c (:pending x1))]
                     (is (= 200 (:code (:resp x2))))))
                 (try (net-close c) (catch e nil)))))]
    (srv-was-clean r)))

(deftest connection-close-is-case-insensitive
  (let [h (fn [req] {:status 200 :body "x"})
        r (srv-with 1 h {}
             (fn [s]
               (let [c (srv-connect (:port s))]
                 (srv-send c "GET / HTTP/1.1\r\nHost: t.example\r\n"
                           "Connection: ClOsE\r\n\r\n")
                 (let [x (srv-read-one c [])]
                   (is (some? x))
                   (is (= "close" (get (:headers (:resp x)) "connection"))))
                 (is (contains? #{nil :err} (try (net-read c 65536) (catch e :err))))
                 (try (net-close c) (catch e nil)))))]
    (srv-was-clean r)))

;;;; the request map the handler sees

(deftest handler-sees-the-ring-request-map
  (let [seen (atom nil)
        h (fn [req] (reset! seen req) {:status 204})
        r (srv-with 1 h {}
             (fn [s]
               (let [c (srv-connect (:port s))]
                 (srv-send c (srv-req "GET" "/x/y?a=1&b=two"))
                 (let [x (srv-read-one c [])]
                   (is (= 204 (:code (:resp x)))))
                 (try (net-close c) (catch e nil)))))]
    (srv-was-clean r)
    (let [req @seen]
      (is (some? req))
      (is (= :get (:request-method req)))
      (is (= "/x/y" (:uri req)))
      (is (= "a=1&b=two" (:query-string req)))
      (is (= "t.example" (get (:headers req) "host")))
      (is (= (srv-bb "") (:body req)))
      (is (= :http (:scheme req)))
      (is (= "HTTP/1.1" (:http-version req)))
      (is (some? (:conn req))))))

(deftest post-and-chunked-bodies-reach-the-handler-decoded
  (let [seen (atom [])
        h (fn [req] (swap! seen conj (:body req)) {:status 200 :body "ok"})
        r (srv-with 2 h {}
             (fn [s]
               (let [c (srv-connect (:port s))]
                 (srv-send c (srv-req "POST" "/echo" {:body "name=x"}))
                 (let [x (srv-read-one c [])]
                   (is (= 200 (:code (:resp x)))))
                 (try (net-close c) (catch e nil)))
               (let [c (srv-connect (:port s))]
                 (srv-send c "POST /up HTTP/1.1\r\nHost: t.example\r\n"
                           "Transfer-Encoding: chunked\r\n\r\n"
                           "5\r\nhello\r\n3\r\n wo\r\n0\r\n\r\n")
                 (let [x (srv-read-one c [])]
                   (is (= 200 (:code (:resp x)))))
                 (try (net-close c) (catch e nil)))))]
    (srv-was-clean r)
    (is (= [(srv-bb "name=x") (srv-bb "hello wo")] @seen))))

;;;; crash isolation

(deftest throwing-handler-answers-500-and-the-server-survives
  (let [h (fn [req]
            (if (= "/boom" (:uri req))
              (throw (ex-info "handler exploded" {:uri (:uri req)}))
              {:status 200 :body "fine"}))
        r (srv-with 2 h {}
             (fn [s]
               (let [c (srv-connect (:port s))]
                 (srv-send c (srv-req "GET" "/boom"))
                 (let [x (srv-read-one c [])]
                   (is (some? x))
                   (is (= 500 (:code (:resp x))))
                   (is (= "text/plain"
                          (get (:headers (:resp x)) "content-type")))
                   (is (= (srv-bb "internal server error")
                          (:body (:resp x))))
                   (is (= "close"
                          (get (:headers (:resp x)) "connection"))))
                 (is (contains? #{nil :err} (try (net-read c 65536) (catch e :err))))
                 (try (net-close c) (catch e nil)))
               ;; a fresh connection is still served: the crash never
               ;; escaped the connection
               (let [c (srv-connect (:port s))]
                 (srv-send c (srv-close-req "GET" "/ok"))
                 (let [x (srv-read-one c [])]
                   (is (= 200 (:code (:resp x))))
                   (is (= (srv-bb "fine") (:body (:resp x)))))
                 (try (net-close c) (catch e nil)))))]
    (srv-was-clean r)))

(deftest invalid-handler-response-answers-500
  (doseq [resp [{:status "200" :body "x"}
                {:body "no status"}
                {:status 200 :headers {"Content-Length" "5"}}]]
    (let [h (fn [req] resp)
          r (srv-with 1 h {}
               (fn [s]
                 (let [c (srv-connect (:port s))]
                   (srv-send c (srv-close-req "GET" "/"))
                   (let [x (srv-read-one c [])]
                     (is (some? x) (pr-str resp))
                     (is (= 500 (:code (:resp x))) (pr-str resp))
                     (is (= "close"
                            (get (:headers (:resp x)) "connection"))
                         (pr-str resp)))
                   (try (net-close c) (catch e nil)))))]
      (srv-was-clean r))))

;;;; malformed traffic and peer lifecycle

(deftest malformed-request-answers-400-and-closes
  (let [h (fn [req] {:status 200 :body "never"})
        r (srv-with 1 h {}
             (fn [s]
               (let [c (srv-connect (:port s))]
                 (srv-send c "garbage\r\n\r\n")
                 (let [x (srv-read-one c [])]
                   (is (some? x))
                   (is (= 400 (:code (:resp x))))
                   (is (= (srv-bb "bad request") (:body (:resp x))))
                   (is (= "close"
                          (get (:headers (:resp x)) "connection"))))
                 (is (contains? #{nil :err} (try (net-read c 65536) (catch e :err))))
                 (try (net-close c) (catch e nil)))))]
    (srv-was-clean r)))

(deftest peer-close-mid-request-never-kills-the-server
  (let [h (fn [req] {:status 200 :body "later"})
        r (srv-with 2 h {}
             (fn [s]
               ;; conn one dies mid-request: partial headers, then close
               (let [c (srv-connect (:port s))]
                 (srv-send c "GET / HTTP/1.1\r\nHost: t.example\r\n")
                 (try (net-close c) (catch e nil)))
               ;; the next connection is served normally
               (let [c (srv-connect (:port s))]
                 (srv-send c (srv-close-req "GET" "/ok"))
                 (let [x (srv-read-one c [])]
                   (is (= 200 (:code (:resp x))))
                   (is (= (srv-bb "later") (:body (:resp x)))))
                 (try (net-close c) (catch e nil)))))]
    (srv-was-clean r)))

(deftest oversized-body-against-its-cap-answers-400
  (let [h (fn [req] {:status 200 :body "never"})
        r (srv-with 1 h {:max-body-bytes 8}
             (fn [s]
               (let [c (srv-connect (:port s))]
                 (srv-send c "POST /big HTTP/1.1\r\nHost: t.example\r\n"
                           "Content-Length: 16\r\n\r\n"
                           "aaaaaaaaaaaaaaaa")
                 (let [x (srv-read-one c [])]
                   (is (some? x))
                   (is (= 400 (:code (:resp x))))
                   (is (= "close"
                          (get (:headers (:resp x)) "connection"))))
                 (try (net-close c) (catch e nil)))))]
    (srv-was-clean r)))

;;;; budgets

(deftest idle-keep-alive-connection-is-reaped-within-its-budget
  (let [h (fn [req] {:status 200 :body "x"})
        r (srv-with 1 h {:idle-timeout 300}
             (fn [s]
               (let [c (srv-connect (:port s))]
                 (srv-send c (srv-req "GET" "/"))
                 (let [x (srv-read-one c [])]
                   (is (= 200 (:code (:resp x)))))
                 ;; silence: no further request; the budget closes the
                 ;; socket in bounded time, not instantly
                 (let [t0 (time-ms)
                       b (try (net-read c 65536) (catch e :err))]
                   (is (nil? b))
                   (is (< (- (time-ms) t0) 8000)
                       "reap must be bounded, not instant"))
                 (try (net-close c) (catch e nil)))))]
    (srv-was-clean r)))

(deftest slow-request-past-its-deadline-is-dropped-unserved
  (let [h (fn [req] {:status 200 :body "never"})
        r (srv-with 1 h {:request-timeout 300}
             (fn [s]
               (let [c (srv-connect (:port s))]
                 (srv-send c "GET / HTTP/1.1\r\nHost: t.example\r\n")
                 (let [t0 (time-ms)
                       b (try (net-read c 65536) (catch e :err))]
                   (is (nil? b) "the connection must close unserved")
                   (is (< (- (time-ms) t0) 8000)
                       "the deadline must be bounded"))
                 (try (net-close c) (catch e nil)))))]
    (srv-was-clean r)))

;;;; the run-server lifecycle

(defn- rs-with
  "Run (body server) against a fresh run-server instance; the server
  is stopped after the body whether it passed, threw, or errored. The
  teardown waits for the thread grant to drain: a finished future
  releases its worker slot asynchronously, and the next test's pool
  must not start against a still-teardown grant."
  [handler opts body]
  (let [s (srv/run-server handler opts)]
    (try
      (body s)
      (finally
        ((:stop s))
        (srv-wait-for #(zero? (mino-thread-count)) 2000)))))

(defn- srv-capacity-target
  "Free worker slots srv-await-capacity waits for, given a requested n
  and the host grant. Never more than the grant holds: asking for more
  free slots than exist can never be satisfied, so the wait collapses
  to a no-op that lets a leftover thread from the prior test collide
  with this one -- exactly the starvation a 3-vCPU CI runner hits. On
  such a host the target clamps to the grant, so the wait means 'let
  the grant fully drain'; on a host with slots to spare it stays n."
  [n limit]
  (min n (max 1 limit)))

(defn- srv-await-capacity
  "Wait until the grant has enough free worker slots (see
  srv-capacity-target) that a pool test needing simultaneous threads is
  not defeated by leftovers from the tests before it."
  [n]
  (let [want (srv-capacity-target n (mino-thread-limit))]
    (srv-wait-for #(<= want (- (mino-thread-limit) (mino-thread-count)))
                  5000)))

(deftest run-server-serves-traffic-and-stops
  (let [h (fn [req] {:status 200 :body (str "saw:" (:uri req))})]
    (rs-with h {}
      (fn [s]
        (is (int? (:port s)))
        (is (> (:port s) 0))
        (let [c (srv-connect (:port s))]
          (srv-send c (srv-req "GET" "/first"))
          (let [x1 (srv-read-one c [])]
            (is (= 200 (:code (:resp x1))))
            (is (= (srv-bb "saw:/first") (:body (:resp x1))))
            (srv-send c (srv-close-req "GET" "/last"))
            (let [x2 (srv-read-one c (:pending x1))]
              (is (= 200 (:code (:resp x2))))
              (is (= (srv-bb "saw:/last") (:body (:resp x2))))
              (is (= "close"
                     (get (:headers (:resp x2)) "connection")))))
          (try (net-close c) (catch e nil)))))))

(deftest run-server-port-zero-reads-back-the-kernel-choice
  (let [h (fn [req] {:status 200 :body "p"})]
    (rs-with h {:port 0}
      (fn [s]
        (is (<= 1 (:port s) 65535))
        (let [c (srv-connect (:port s))]
          (srv-send c (srv-close-req "GET" "/"))
          (let [x (srv-read-one c [])]
            (is (= 200 (:code (:resp x)))))
          (try (net-close c) (catch e nil)))))))

(deftest run-server-rejects-unknown-and-malformed-opts
  (let [h (fn [req] {:status 200})]
    (is (thrown? (srv/run-server h {:bogus 1 :worse 2})))
    (is (re-find #"bogus"
                 (try (srv/run-server h {:bogus 1})
                      (catch e (ex-message e)))))
    (is (thrown? (srv/run-server h {:port "eighty"})))
    (is (thrown? (srv/run-server h {:idle-timeout "soon"})))
    (is (thrown? (srv/run-server h {:max-body-bytes "big"})))
    (is (thrown? (srv/run-server h :not-a-map)))
    ;; the connection seam validates its own opts at the boundary
    (is (thrown? (srv/serve-conn* nil h {:request-timeout "later"})))))

(deftest run-server-stop-is-idempotent
  (let [h (fn [req] {:status 200})
        s (srv/run-server h {})]
    (is (nil? ((:stop s))))
    (is (nil? ((:stop s))))))

(deftest run-server-stop-is-bounded-under-a-parked-connection
  (let [h (fn [req] {:status 200 :body "never"})
        s (srv/run-server h {:request-timeout 5500})]
    (let [c (srv-connect (:port s))]
      (srv-send c "GET / HTTP/1.1\r\nHost: t.example\r\n")
      ;; give the engine a moment to park on the partial request
      (let [t0 (time-ms)
            r ((:stop s))
            elapsed (- (time-ms) t0)]
        (is (nil? r))
        ;; the parked request outlives the join grace; stop still
        ;; returns in bounded time instead of waiting for it
        (is (< elapsed 7000) (str "stop took " elapsed "ms"))
        ;; the listener is gone: fresh connects are refused
        (is (thrown? (net-connect "127.0.0.1" (:port s) {}))))
      ;; the straggler connection is closed by its own deadline, not
      ;; leaked and never closed underneath its parked read
      (is (contains? #{nil :err} (try (net-read c 65536) (catch e :err))))
      (try (net-close c) (catch e nil)))))

(deftest run-server-survives-repeated-start-stop-cycles
  (let [h (fn [req] {:status 200 :body "cycle"})]
    (dotimes [i 5]
      (rs-with h {}
        (fn [s]
          (let [c (srv-connect (:port s))]
            (srv-send c (srv-close-req "GET" "/"))
            (let [x (srv-read-one c [])]
              (is (= 200 (:code (:resp x))) (str "cycle " i))
              (is (= (srv-bb "cycle") (:body (:resp x))) (str "cycle " i)))
            (try (net-close c) (catch e nil))))))))

;;;; the acceptor pool

(defn- srv-wait-for
  "Poll pred every 20ms until it answers truthy or ms elapses; nil when
  the deadline passes first."
  [pred ms]
  (let [deadline (+ (time-ms) ms)]
    (loop []
      (or (pred)
          (when (< (time-ms) deadline)
            (thread-sleep 20)
            (recur))))))

(defn- srv-pool-k
  "Concurrent handlers a barrier test may demand: one thread for the
  accept loop machinery, then one per simultaneous handler."
  []
  (max 1 (min 3 (- (mino-thread-limit) 1))))

(defn- grant-affords?
  "True when the host thread grant is large enough to form the
  concurrency shape a pool test asserts. A pool test that parks one
  handler while another serves needs a spawner, its acceptors, and at
  least two worker threads -- four in total. On a smaller grant (a
  3-vCPU CI runner) run-server correctly degrades to inline serving, so
  that shape cannot form; the test prints why and is skipped rather than
  asserting a behaviour the host physically cannot produce. Every host
  with >= n threads (Linux CI, dev) runs it at full strength."
  [n]
  (>= (mino-thread-limit) n))

(defn- skip-small-grant [name n]
  (println (str "  " name ": needs thread-limit >= " n
                ", have " (mino-thread-limit) " -- skipped")))

(deftest capacity-target-never-exceeds-the-grant
  ;; With slots to spare the request is honored verbatim.
  (is (= 5 (srv-capacity-target 5 8)))
  (is (= 4 (srv-capacity-target 4 8)))
  ;; A request larger than the grant clamps to the grant: the wait then
  ;; means "let the whole grant drain", never an unsatisfiable no-op.
  (is (= 3 (srv-capacity-target 5 3)))
  (is (= 4 (srv-capacity-target 5 4)))
  ;; A grant of exactly the request is a full drain, not an overshoot.
  (is (= 3 (srv-capacity-target 3 3)))
  ;; A degenerate grant still asks for at least one free slot.
  (is (= 1 (srv-capacity-target 5 1))))

(deftest pool-serves-concurrent-connections
  (let [k (srv-pool-k)
        _ (srv-await-capacity (+ k 2))
        entered (atom 0)
        releases (vec (repeatedly k promise))
        h (fn [req]
            (let [i (swap! entered inc)]
              (deref (nth releases (dec i)) 9000 :t)
              {:status 200 :body "c"}))]
    (rs-with h {:acceptors 1 :max-conns 4
                :idle-timeout 9000 :request-timeout 9000}
      (fn [s]
        (let [conns (mapv (fn [_] (srv-connect (:port s))) (range k))]
          (doseq [c conns] (srv-send c (srv-req "GET" "/k")))
          ;; every handler must be parked inside the server at once;
          ;; a sequential server parks the second connection behind
          ;; the first and this barrier never completes
          (is (srv-wait-for #(= k @entered) 9000)
              (str "only " @entered " of " k " handlers entered"))
          (doseq [p releases] (deliver p :go))
          (doseq [c conns]
            (let [x (srv-read-one c [])]
              (is (some? x))
              (is (= 200 (:code (:resp x))))
              (is (= (srv-bb "c") (:body (:resp x)))))
            (try (net-close c) (catch e nil))))))))

(deftest pool-keeps-serving-while-one-handler-parks
  (if-not (grant-affords? 4)
    (skip-small-grant "pool-keeps-serving-while-one-handler-parks" 4)
   (let [_ (srv-await-capacity 4)
        entered (atom false)
        release (promise)
        h (fn [req]
            (case (:uri req)
              "/park" (do (reset! entered true)
                          (deref release 9000 :t)
                          {:status 200 :body "parked"})
              "/boom" (throw (ex-info "boom" {}))
              {:status 200 :body "fine"}))]
    (rs-with h {:acceptors 2 :max-conns 4
                :idle-timeout 9000 :request-timeout 9000}
      (fn [s]
        (let [a (srv-connect (:port s))]
          (srv-send a (srv-req "GET" "/park"))
          (is (srv-wait-for #(true? @entered) 9000))
          ;; two full request/response cycles complete while the first
          ;; connection's handler is parked
          (doseq [path ["/b" "/c"]]
            (let [c (srv-connect (:port s))]
              (srv-send c (srv-close-req "GET" path))
              (let [x (srv-read-one c [])]
                (is (= 200 (:code (:resp x))) path)
                (is (= (srv-bb "fine") (:body (:resp x))) path))
              (try (net-close c) (catch e nil))))
          (deliver release :go)
          (let [x (srv-read-one a [])]
            (is (= 200 (:code (:resp x))))
            (is (= (srv-bb "parked") (:body (:resp x)))))
          (try (net-close a) (catch e nil)))
        ;; a throwing handler answers 500 and the pool serves the next
        ;; connection untouched
        (let [d (srv-connect (:port s))]
          (srv-send d (srv-close-req "GET" "/boom"))
          (let [x (srv-read-one d [])]
            (is (= 500 (:code (:resp x))))
            (is (= "close" (get (:headers (:resp x)) "connection"))))
          (try (net-close d) (catch e nil)))
        (let [e2 (srv-connect (:port s))]
          (srv-send e2 (srv-close-req "GET" "/after"))
          (let [x (srv-read-one e2 [])]
            (is (= 200 (:code (:resp x))))
            (is (= (srv-bb "fine") (:body (:resp x)))))
          (try (net-close e2) (catch e nil))))))))

(deftest pool-max-conns-one-serializes-and-holds-the-backlog
  (if-not (grant-affords? 4)
    (skip-small-grant "pool-max-conns-one-serializes-and-holds-the-backlog" 4)
   (let [_ (srv-await-capacity 4)
        st (atom {:inflight 0 :max 0 :served 0})
        release (promise)
        h (fn [req]
            (swap! st (fn [m]
                        (let [m2 (-> m (update :served inc) (update :inflight inc))]
                          (assoc m2 :max (max (:max m) (:inflight m2))))))
            (when (= "/park" (:uri req))
              (deref release 9000 :t))
            (swap! st update :inflight dec)
            {:status 200 :body "s"})]
    (rs-with h {:acceptors 2 :max-conns 1
                :idle-timeout 9000 :request-timeout 9000}
      (fn [s]
        ;; the parked request asks to close, so its connection and its
        ;; permit are released the moment the handler answers
        (let [a (srv-connect (:port s))]
          (srv-send a (srv-close-req "GET" "/park"))
          (is (srv-wait-for #(= 1 (:served @st)) 9000))
          ;; the kernel backlog takes the second connection even though
          ;; the single permit is held; its handler cannot start
          (let [b (srv-connect (:port s))]
            (srv-send b (srv-close-req "GET" "/b"))
            (is (not (srv-wait-for #(= 2 (:served @st)) 600))
                "the second handler started while the permit was held")
            (deliver release :go)
            (is (srv-wait-for #(= 2 (:served @st)) 9000)
                "the backlog connection was never served")
            (let [x (srv-read-one b [])]
              (is (= 200 (:code (:resp x)))))
            (try (net-close b) (catch e nil)))
          (let [x (srv-read-one a [])]
            (is (= 200 (:code (:resp x)))))
          (try (net-close a) (catch e nil)))
        (is (= 1 (:max @st)) "two handlers ran at once"))))))

(deftest pool-opts-are-validated
  (let [h (fn [req] {:status 200})]
    (is (thrown? (srv/run-server h {:acceptors 0})))
    (is (thrown? (srv/run-server h {:acceptors "two"})))
    (is (thrown? (srv/run-server h {:max-conns 0})))
    (is (thrown? (srv/run-server h {:max-conns -2})))
    (is (re-find #"acceptors"
                 (try (srv/run-server h {:acceptors 0})
                      (catch e (ex-message e)))))))

(deftest teardown-under-load-stops-bounded-and-drains
  (if-not (grant-affords? 4)
    (skip-small-grant "teardown-under-load-stops-bounded-and-drains" 4)
   (let [_ (srv-await-capacity 4)
        k 2
        inflight (atom 0)
        releases (vec (repeatedly k promise))
        h (fn [req]
            (let [i (swap! inflight inc)]
              (deref (nth releases (dec i)) 8000 :t)
              (swap! inflight dec)
              {:status 200 :body "done"}))
        s (srv/run-server h {:acceptors 2 :max-conns 4
                             :idle-timeout 9000 :request-timeout 9000})]
    (try
      (let [conns (mapv (fn [_] (srv-connect (:port s))) (range k))]
        (doseq [c conns] (srv-send c (srv-close-req "GET" "/load")))
        (is (srv-wait-for #(= k @inflight) 9000))
        (let [t0 (time-ms)
              r ((:stop s))
              elapsed (- (time-ms) t0)]
          (is (nil? r))
          (is (< elapsed 8000) (str "stop took " elapsed "ms"))
          ;; the straggler workers finish once released: every
          ;; connection gets its response and closes, and the
          ;; in-flight count drains to zero
          (doseq [p releases] (deliver p :go))
          (doseq [c conns]
            (let [x (srv-read-one c [])]
              (is (some? x))
              (is (= 200 (:code (:resp x)))))
            (is (contains? #{nil :err} (try (net-read c 65536) (catch e :err)))
                "connection did not close after stop and release")
            (try (net-close c) (catch e nil)))
          (is (srv-wait-for #(zero? @inflight) 9000)
              "in-flight handlers never drained")))
      (finally
        ((:stop s))
        (srv-wait-for #(zero? (mino-thread-count)) 2000))))))

(deftest permits-return-on-normal-throwing-and-reset-paths
  (let [_ (srv-await-capacity 4)
        h (fn [req]
            (if (= "/boom" (:uri req))
              (throw (ex-info "boom" {}))
              {:status 200 :body "ok"}))
        served (fn [s path code]
                 (let [c (srv-connect (:port s))]
                   (srv-send c (srv-close-req "GET" path))
                   (let [x (srv-read-one c [])]
                     (is (some? x) path)
                     (is (= code (:code (:resp x))) path))
                   (try (net-close c) (catch e nil))))]
    (rs-with h {:acceptors 2 :max-conns 1
                :idle-timeout 9000 :request-timeout 9000}
      (fn [s]
        ;; one permit: any exit path that fails to return it hangs
        ;; every leg after it
        (served s "/one" 200)
        (served s "/boom" 500)
        (served s "/two" 200)
        ;; a peer dies mid-request: partial headers, abrupt close; the
        ;; cushion covers one acceptor poll before the next leg
        (let [r (srv-connect (:port s))]
          (srv-send r "GET / HTTP/1.1\r\nHost: t.example\r\n")
          (try (net-close r) (catch e nil)))
        (thread-sleep 300)
        (served s "/three" 200)))))

;;;; the frozen public surface

(deftest ns-publics-are-run-server-and-the-serve-conn-seam
  (is (= #{'run-server 'serve-conn*}
         (set (keys (ns-publics 'mino.http.server))))))

(deftest every-public-var-carries-a-docstring
  (doseq [v (vals (ns-publics 'mino.http.server))]
    (is (string? (:doc (meta v)))
        (str (:name (meta v)) " lacks a docstring"))
    (is (pos? (count (:doc (meta v))))
        (str (:name (meta v)) " has an empty docstring"))))

(deftest request-map-carries-exactly-the-frozen-keys
  (let [seen (atom nil)
        h (fn [req] (reset! seen req) {:status 200})]
    (srv-with 1 h {}
      (fn [s]
        (let [c (srv-connect (:port s))]
          (srv-send c (srv-req "GET" "/x?k=v"))
          (let [x (srv-read-one c [])]
            (is (= 200 (:code (:resp x)))))
          (try (net-close c) (catch e nil)))))
    (is (= #{:request-method :uri :query-string :headers :body
             :scheme :http-version :conn}
           (set (keys @seen))))
    (is (not (contains? @seen :remote-addr))
        "the peer-address gap stays omitted until net-accept widens")))

(deftest serve-conn-seam-rejects-unknown-opts-naming-them
  (let [h (fn [req] {:status 200})]
    (is (thrown? (srv/serve-conn* nil h {:bogus 1 :worse 2})))
    (let [msg (try (srv/serve-conn* nil h {:bogus 1})
                   (catch e (ex-message e)))]
      (is (re-find #"bogus" msg))
      (is (re-find #"connection" msg)))))

(deftest run-server-names-every-unknown-opt-in-one-error
  (let [h (fn [req] {:status 200})
        msg (try (srv/run-server h {:bogus 1 :worse 2})
                 (catch e (ex-message e)))]
    (is (re-find #":bogus" msg))
    (is (re-find #":worse" msg))))

;;;; end to end against the mino.http client

;; The two halves prove each other: the real client stack (plain-map
;; surface, encoder, pool, parser) against the real server engine.
;; Client calls pin :keepalive 0 unless a test exercises pooling, so
;; no idle pooled connection pins a worker past its test.

(defn- e2e-base
  [s]
  (str "http://127.0.0.1:" (:port s)))

(defn- e2e-text
  [body]
  {:status 200 :headers [["Content-Type" "text/plain"]] :body body})

(deftest e2e-client-get-carries-query-params-to-the-handler
  (let [h (fn [req] (e2e-text (str "uri=" (:uri req) " qs="
                                   (or (:query-string req) ""))))]
    (rs-with h {}
      (fn [s]
        (let [r (http/get (str (e2e-base s) "/find")
                          {:query-params {:type "backend"} :keepalive 0})]
          (is (= 200 (:status r)))
          (is (= "uri=/find qs=type=backend" (:body r)))
          (is (= "text/plain" (get (:headers r) "content-type"))))))))

(deftest e2e-client-post-body-round-trips
  (let [h (fn [req] (e2e-text (:body req)))]
    (rs-with h {}
      (fn [s]
        (let [r (http/post (str (e2e-base s) "/echo")
                           {:body "upload payload 42" :keepalive 0})]
          (is (= 200 (:status r)))
          (is (= "upload payload 42" (:body r))))))))

(deftest e2e-chunked-request-body-dechunks-server-side
  ;; the client codec's :chunked? path (header by http-encode-request,
  ;; frames by http-encode-chunk) against the server's de-chunking
  (let [h (fn [req] (e2e-text (:body req)))]
    (rs-with h {}
      (fn [s]
        (let [c (srv-connect (:port s))]
          (net-write c (http-encode-request {:method "POST" :target "/up"
                                             :host "127.0.0.1"
                                             :chunked? true}))
          (net-write c (http-encode-chunk (srv-bb "hello ")))
          (net-write c (http-encode-chunk (srv-bb "world")))
          (net-write c (http-encode-chunk (srv-bb "")))
          (let [x (srv-read-one c [])]
            (is (some? x))
            (is (= 200 (:code (:resp x))))
            (is (= (srv-bb "hello world") (:body (:resp x)))))
          (try (net-close c) (catch e nil)))))))

(deftest e2e-keep-alive-connection-reuse-counted-server-side
  (let [seen (atom [])
        h (fn [req]
            (when-not (some #(identical? % (:conn req)) @seen)
              (swap! seen conj (:conn req)))
            (if (= "/count" (:uri req))
              (e2e-text (str (count @seen)))
              (e2e-text "ok")))]
    (rs-with h {:idle-timeout 600}
      (fn [s]
        (is (= 200 (:status (http/get (str (e2e-base s) "/a")))))
        (is (= 200 (:status (http/get (str (e2e-base s) "/b")))))
        (let [r (http/get (str (e2e-base s) "/count"))]
          (is (= 200 (:status r)))
          (is (= "1" (:body r))
              "three requests must share one connection"))))))

(deftest e2e-server-errors-reach-the-client-as-data
  (let [h (fn [req]
            (if (= "/boom" (:uri req))
              (throw (ex-info "e2e boom" {}))
              {:status 404 :headers [["Content-Type" "text/plain"]]
               :body "no such route"}))]
    (rs-with h {}
      (fn [s]
        (let [r404 (http/get (str (e2e-base s) "/missing")
                             {:throw false :keepalive 0})]
          (is (= 404 (:status r404)))
          (is (= "no such route" (:body r404))))
        (let [r500 (http/get (str (e2e-base s) "/boom")
                             {:throw false :keepalive 0})]
          (is (= 500 (:status r500)))
          (is (= "internal server error" (:body r500)))
          (is (= "text/plain" (get (:headers r500) "content-type"))))
        (let [e (try (http/get (str (e2e-base s) "/boom") {:keepalive 0})
                     (catch Throwable e e))]
          (is (= "HTTP 500" (ex-message e)))
          (is (= 500 (:status (ex-data e)))))))))

(deftest e2e-stop-restart-serves-on-a-fresh-port
  (let [h (fn [req] (e2e-text (str "gen:" (:uri req))))]
    (rs-with h {}
      (fn [s1]
        (is (= 200 (:status (http/get (str (e2e-base s1) "/one")
                                      {:keepalive 0}))))))
    (rs-with h {}
      (fn [s2]
        (let [r (http/get (str (e2e-base s2) "/two") {:keepalive 0})]
          (is (= 200 (:status r)))
          (is (= "gen:/two" (:body r))))))))

;;;; slow peers, caps, and resource exhaustion

(defn- srv-drop-outcome
  "One bounded read classifying how the server ended the connection:
  :served response bytes, :eof a clean close, :reset an aborted
  close, :held still open when the read budget lapsed."
  [c]
  (try
    (let [r (net-read c 65536)]
      (if (and r (pos? (count r))) :served :eof))
    (catch e
      (if (= :net/timeout (:mino/kind e)) :held :reset))))

(defn- srv-drip!
  "Write parts on c, one every ms, on its own future so the caller
  watches the connection while the peer is still dripping. A peer
  dripping past a working deadline gets its pipe broken mid-drip;
  broken writes are swallowed."
  [c parts ms]
  (future
    (doseq [p parts]
      (try (net-write c p) (catch e nil))
      (thread-sleep ms))))

(defn- srv-parts
  "Byte seq cut into slices of n for the drip writer."
  [bs n]
  (mapv byte-array (partition-all n (vec bs))))

(deftest drip-fed-headers-are-dropped-past-the-request-deadline
  ;; The server's pool (spawner + acceptor + worker) runs concurrently
  ;; with the client-side drip future -- four host threads. On a grant
  ;; too small to hold them the drip future is starved of a slot
  ;; (MTH001), so the shape is skipped there, exactly as the pool tests
  ;; are.
  (if-not (grant-affords? 4)
    (skip-small-grant "drip-fed-headers-are-dropped-past-the-request-deadline" 4)
   (let [_ (srv-await-capacity 5)
        served (atom 0)
        h (fn [req] (swap! served inc) {:status 200 :body "ok"})]
    (rs-with h {:acceptors 1 :max-conns 2
                 :idle-timeout 20000 :request-timeout 800}
      (fn [s]
        (let [c (srv-connect (:port s))
              req (srv-bb "GET /slow HTTP/1.1\r\nHost: t.example\r\n"
                          "X-Drip: aaaa\r\n")
              drip (srv-drip! c (srv-parts req 8) 100)
              t0 (time-ms)
              out (srv-drop-outcome c)
              elapsed (- (time-ms) t0)]
          (is (contains? #{:eof :reset} out)
              (str "a dripping peer is dropped, not served; got " out))
          ;; the drop cannot beat the deadline; only host stretch
          ;; moves it the other way
          (is (>= elapsed 700)
              (str "dropped at " elapsed "ms, before the deadline"))
          (is (< elapsed 6000) (str "drop took " elapsed "ms"))
          (try (deref drip 4000 :drip-timeout) (catch e nil))
          (try (net-close c) (catch e nil)))))
    (is (zero? @served) "the drip request was never served"))))

(deftest drip-fed-body-is-dropped-past-the-request-deadline
  ;; Four host threads (server pool + client drip future); skipped on a
  ;; grant that cannot hold them (see drip-fed-headers).
  (if-not (grant-affords? 4)
    (skip-small-grant "drip-fed-body-is-dropped-past-the-request-deadline" 4)
   (let [_ (srv-await-capacity 5)
        h (fn [req] {:status 200 :body "never"})]
    (rs-with h {:acceptors 1 :max-conns 2
                :idle-timeout 20000 :request-timeout 800
                :max-body-bytes 4096}
      (fn [s]
        (let [c (srv-connect (:port s))]
          (srv-send c "POST /slow HTTP/1.1\r\nHost: t.example\r\n"
                    "Content-Length: 12\r\n\r\n")
          (let [drip (srv-drip! c (srv-parts (srv-bb "xxxxxxxxxxxx") 1) 100)
                t0 (time-ms)
                out (srv-drop-outcome c)
                elapsed (- (time-ms) t0)]
            (is (contains? #{:eof :reset} out)
                (str "a body-dripping peer is dropped; got " out))
            (is (>= elapsed 700)
                (str "dropped at " elapsed "ms, before the deadline"))
            (is (< elapsed 6000) (str "drop took " elapsed "ms"))
            (try (deref drip 4000 :drip-timeout) (catch e nil)))
          (try (net-close c) (catch e nil))))))))

(deftest a-slow-request-inside-the-deadline-is-still-served
  ;; Four host threads (server pool + client drip future); skipped on a
  ;; grant that cannot hold them (see drip-fed-headers).
  (if-not (grant-affords? 4)
    (skip-small-grant "a-slow-request-inside-the-deadline-is-still-served" 4)
   (let [_ (srv-await-capacity 5)
        h (fn [req] {:status 200 :body "slow but legal"})]
    (rs-with h {:acceptors 1 :max-conns 2
                :idle-timeout 20000 :request-timeout 2500}
      (fn [s]
        (let [c (srv-connect (:port s))
              req (srv-bb "GET /ok HTTP/1.1\r\nHost: t.example\r\n"
                          "X-One: 1\r\nX-Two: 2\r\nX-Three: 3\r\n\r\n")
              drip (srv-drip! c (srv-parts req 10) 80)]
          (let [x (srv-read-one c [])]
            (is (some? x) "a request completing inside the deadline is served")
            (is (= 200 (:code (:resp x))))
            (is (= (srv-bb "slow but legal") (:body (:resp x)))))
          (try (deref drip 4000 :drip-timeout) (catch e nil))
          (try (net-close c) (catch e nil))))))))

(deftest oversized-header-section-answers-400-before-the-request-completes
  (let [h (fn [req] {:status 200 :body "never"})]
    (rs-with h {:acceptors 1 :max-conns 2
                :max-header-bytes 200
                :idle-timeout 20000 :request-timeout 3000}
      (fn [s]
        (let [c (srv-connect (:port s))]
          ;; the section crosses the cap mid-drip and the request is
          ;; never completed: the 400 must come from the cap, not from
          ;; an end of stream or a deadline
          (srv-send c "GET /big HTTP/1.1\r\nHost: t.example\r\n"
                    "X-Huge: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
          (dotimes [_ 4]
            (srv-send c "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
            (thread-sleep 60))
          (let [x (srv-read-one c [])]
            (is (some? x) "the cap answers without the request completing")
            (is (= 400 (:code (:resp x))))
            (is (= "close" (get (:headers (:resp x)) "connection"))))
          (try (net-close c) (catch e nil)))))))

(deftest oversized-header-count-answers-400-before-the-request-completes
  (let [h (fn [req] {:status 200 :body "never"})]
    (rs-with h {:acceptors 1 :max-conns 2
                :max-headers 3
                :idle-timeout 20000 :request-timeout 3000}
      (fn [s]
        (let [c (srv-connect (:port s))]
          ;; request line plus four header rows against a cap of three,
          ;; and the blank line never arrives
          (srv-send c "GET /many HTTP/1.1\r\nHost: t.example\r\n"
                    "A: 1\r\nB: 2\r\nC: 3\r\nD: 4\r\n")
          (let [x (srv-read-one c [])]
            (is (some? x))
            (is (= 400 (:code (:resp x))))
            (is (= "close" (get (:headers (:resp x)) "connection"))))
          (try (net-close c) (catch e nil)))))))

(deftest oversized-content-length-answers-400-on-the-live-path
  (let [h (fn [req] {:status 200 :body "never"})]
    (rs-with h {:acceptors 1 :max-conns 2
                :max-body-bytes 64
                :idle-timeout 20000 :request-timeout 3000}
      (fn [s]
        (let [c (srv-connect (:port s))]
          (srv-send c "POST /hoard HTTP/1.1\r\nHost: t.example\r\n"
                    "Content-Length: 999999999\r\n\r\n")
          (let [x (srv-read-one c [])]
            (is (some? x) "the declared length is rejected at the header")
            (is (= 400 (:code (:resp x))))
            (is (= "close" (get (:headers (:resp x)) "connection"))))
          (try (net-close c) (catch e nil)))))))

(deftest idle-keep-alive-connection-is-reaped-and-its-permit-returns
  (let [h (fn [req] {:status 200 :body "ok"})]
    (rs-with h {:acceptors 1 :max-conns 1
                :idle-timeout 700 :request-timeout 20000}
      (fn [s]
        (let [a (srv-connect (:port s))]
          (srv-send a (srv-req "GET" "/one"))
          (let [x (srv-read-one a [])]
            (is (= 200 (:code (:resp x)))))
          (let [t0 (time-ms)
                out (srv-drop-outcome a)
                elapsed (- (time-ms) t0)]
            (is (contains? #{:eof :reset} out)
                (str "a quiet keep-alive conn is reaped; got " out))
            (is (< elapsed 8000) (str "reap took " elapsed "ms")))
          (try (net-close a) (catch e nil)))
        ;; one permit in play: the next connection proves the reap
        ;; returned it
        (let [b (srv-connect (:port s))]
          (srv-send b (srv-close-req "GET" "/after"))
          (let [x (srv-read-one b [])]
            (is (= 200 (:code (:resp x)))))
          (try (net-close b) (catch e nil)))))))

(deftest silent-connection-hoard-is-reaped-and-the-server-stays-responsive
  (let [h (fn [req] {:status 200 :body "still here"})]
    (rs-with h {:acceptors 1 :max-conns 2
                :idle-timeout 600 :request-timeout 20000}
      (fn [s]
        (let [hoard (mapv (fn [_] (srv-connect (:port s))) (range 3))]
          (doseq [c hoard]
            (let [t0 (time-ms)
                  out (srv-drop-outcome c)
                  elapsed (- (time-ms) t0)]
              (is (contains? #{:eof :reset} out)
                  (str "an idle silent conn is reaped; got " out))
              (is (< elapsed 8000) (str "reap took " elapsed "ms")))
            (try (net-close c) (catch e nil))))
        (let [c (srv-connect (:port s))]
          (srv-send c (srv-close-req "GET" "/probe"))
          (let [x (srv-read-one c [])]
            (is (some? x) "the server serves after the hoard is reaped")
            (is (= 200 (:code (:resp x))))
            (is (= (srv-bb "still here") (:body (:resp x)))))
          (try (net-close c) (catch e nil)))))))

;;;; stop convergence: workers wind down at their next read tick

(deftest stop-winds-down-a-parked-keep-alive-worker-at-its-next-tick
  ;; A keep-alive connection whose worker is parked between requests
  ;; against a long idle budget: stop must reach it at its next read
  ;; tick (the socket poll interval), never at the idle deadline. On
  ;; a loaded 2-core CI runner the old behaviour let each stopped
  ;; server leak its parked workers for the full 30s idle budget,
  ;; overlapping across consecutive tests until the process thread
  ;; limit saturated.
  (srv-wait-for #(zero? (mino-thread-count)) 10000)
  (let [h (fn [req] {:status 200 :body "ok"})
        s (srv/run-server h {:acceptors 1
                             :idle-timeout 30000 :request-timeout 30000})
        c (srv-connect (:port s))]
    (try
      (srv-send c (srv-req "GET" "/warm"))
      (let [x (srv-read-one c [])]
        (is (= 200 (:code (:resp x)))))
      ;; the worker is now parked waiting for a next request
      ((:stop s))
      ;; the worker closed the connection at its wake, not at the idle
      ;; deadline: a :held outcome means the socket is still open
      (is (contains? #{:eof :reset} (srv-drop-outcome c))
          "the parked worker closed its connection after stop")
      (is (srv-wait-for #(zero? (mino-thread-count)) 8000)
          "the worker grant drained within the poll interval, not the idle budget")
      (finally
        (try (net-close c) (catch e nil))
        ((:stop s))))))

(deftest stop-lets-a-mid-request-exchange-finish-then-closes
  ;; A worker holding a partial request when stop lands must finish
  ;; that one exchange (its read is never cut) and then close instead
  ;; of continuing keep-alive; the response announces the close.
  (if-not (grant-affords? 4)
    (skip-small-grant "stop-lets-a-mid-request-exchange-finish-then-closes" 4)
   (let [_ (srv-await-capacity 4)
        h (fn [req] {:status 200 :body (str "late:" (:uri req))})
        s (srv/run-server h {:acceptors 1
                             :idle-timeout 30000 :request-timeout 30000})
        c (srv-connect (:port s))]
    (try
      (srv-send c "GET /mid HTTP/1.1\r\nHost: t.example\r\n")
      ;; give the engine a moment to park on the partial request
      (thread-sleep 300)
      (let [completer (future (thread-sleep 400)
                              (try (net-write c (srv-bb "\r\n"))
                                   (catch e nil)))]
        ((:stop s))
        (let [x (srv-read-one c [])]
          (is (some? x) "the in-flight exchange was served, not cut")
          (is (= 200 (:code (:resp x))))
          (is (= (srv-bb "late:/mid") (:body (:resp x))))
          (is (= "close" (get (:headers (:resp x)) "connection"))
              "no keep-alive continuation after stop"))
        (is (contains? #{:eof :reset} (srv-drop-outcome c))
            "the connection closed once its exchange finished")
        (is (srv-wait-for #(zero? (mino-thread-count)) 8000)
            "the worker grant drained once the exchange finished")
        (try (deref completer 4000 nil) (catch e nil)))
      (finally
        (try (net-close c) (catch e nil))
        ((:stop s)))))))

(run-tests-and-exit)
