(require "tests/test")
(require '[clojure.string :as str])
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

(defn- srv-connect [port]
  (net-connect "127.0.0.1" port {:read-timeout 8000 :write-timeout 8000}))

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
  {:idle-timeout 4000 :request-timeout 4000 :poll-ms 50})

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
                             (try (srv/serve-conn* c handler o)
                                  (catch e :engine-crash)))
                      (try (net-close c) (catch e nil)))
                    (recur (inc i)))))
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
                 ;; the server closes its side; the client sees end of
                 ;; stream without any error
                 (is (nil? (try (net-read c 65536) (catch e :err))))
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
                 (is (nil? (try (net-read c 65536) (catch e :err))))
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
                 (is (nil? (try (net-read c 65536) (catch e :err))))
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
                 (is (nil? (try (net-read c 65536) (catch e :err))))
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
                 (is (nil? (try (net-read c 65536) (catch e :err))))
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
  is stopped after the body whether it passed, threw, or errored."
  [handler opts body]
  (let [s (srv/run-server handler opts)]
    (try
      (body s)
      (finally
        ((:stop s))))))

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
    (is (thrown? (srv/run-server h :not-a-map)))))

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
      (is (nil? (try (net-read c 65536) (catch e :err))))
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

(run-tests-and-exit)
