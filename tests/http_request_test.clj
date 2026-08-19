(require "tests/test")
(require "tests/fixtures/http/server")
(require '[clojure.string :as str])

;; Full HTTP request loop (http-request): the orchestration prim that
;; composes pool-checkout, net-connect, tls-connect, request encoding,
;; response parsing, redirects, and body decompression into one call.
;; The prim takes the already-normalized parts map and returns a
;; response map; mino.http builds on it.
;;
;; Loopback fixtures run against the in-process mino server
;; (tests/fixtures/http/server.clj): a route table over the net
;; listener prims, serial accept loop in a future, torn down in a
;; finally block. HTTPS legs live in the mino-tests satellite repo;
;; the port manifest is
;; ~/.agentic-sdk/mino/runs/http-client/moved-tls-tests.md.

(defn- hr-with-server
  "Run body with a fresh fixture server; teardown is guaranteed
  whether the body passed, threw, or errored."
  [body]
  (fx-with-server body))

(defn- hr-get
  "Minimal normalized GET against the fixture server."
  ([srv target] (hr-get srv target nil))
  ([srv target opts]
   (http-request (merge {:method "GET" :scheme :http
                         :host "127.0.0.1" :port (:port srv)
                         :target target
                         :connect-timeout 3000 :read-timeout 3000
                         :write-timeout 3000}
                        opts))))

(defn- hr-text
  "Fixture bodies are ASCII; widen bytes through char for assertions."
  [b]
  (apply str (map char (seq b))))

;; ---- capability metadata and validation (no server; every platform) ----

(deftest http-request-labelled-with-net-capability
  (is (= :net (mino-capability 'http-request))))

(deftest http-request-validates-arguments
  (is (thrown? (http-request)))
  (is (= :eval/type
         (try (http-request 42) (catch e (:mino/kind e)))))
  (is (= :eval/type
         (try (http-request "not-a-map") (catch e (:mino/kind e)))))
  (is (= :http/method
         (try (http-request {:method "GE T" :scheme :http :host "127.0.0.1"
                             :port 80 :target "/"}) (catch e (:mino/kind e)))))
  (is (= :http/method
         (try (http-request {:method :get :scheme :http :host "127.0.0.1"
                             :port 80 :target "/"}) (catch e (:mino/kind e)))))
  (is (= :http/method
         (try (http-request {:scheme :http :host "127.0.0.1"
                             :port 80 :target "/"}) (catch e (:mino/kind e)))))
  (is (= :http/request
         (try (http-request {:method "GET" :scheme :ftp :host "127.0.0.1"
                             :port 80 :target "/"}) (catch e (:mino/kind e)))))
  (is (= :http/request
         (try (http-request {:method "GET" :scheme :http :host "127.0.0.1"
                             :port 70000 :target "/"})
              (catch e (:mino/kind e)))))
  (is (= :http/request
         (try (http-request {:method "GET" :scheme :http :host 8080
                             :port 80 :target "/"}) (catch e (:mino/kind e)))))
  (is (= :http/request
         (try (http-request {:method "GET" :scheme :http :host "127.0.0.1"
                             :port 80 :target "/a b"})
              (catch e (:mino/kind e))))))

(deftest http-request-rejects-layer-owned-headers
  (doseq [n ["Host" "host" "Content-Length" "content-length"
             "Transfer-Encoding" "transfer-encoding"]]
    (is (= :http/headers
           (try (http-request {:method "GET" :scheme :http
                               :host "127.0.0.1" :port 80 :target "/"
                               :headers [[n "v"]]})
                (catch e (:mino/kind e)))
           (str n " must be rejected")))))

;; ---- loopback behaviour ----

(deftest get-plain-returns-full-response-map
  (hr-with-server
    (fn [srv]
      (let [r (hr-get srv "/hello")]
        (is (= 200 (:status r)))
        (is (= "hello world" (hr-text (:body-bytes r))))
        (is (= "1.1" (:http-version r)))
        (is (map? (:headers r)))
        (is (string? (get (:headers r) "content-type")))
        (is (false? (:from-pool? r)))
        (is (nat-int? (:request-time-ms r)))
        (is (= [] (:trace-redirects r)))
        (is (= "GET" (get (:request r) :method)))
        (is (= "/hello" (get (:request r) :target)))))))

(deftest post-body-round-trips
  (hr-with-server
    (fn [srv]
      (let [r (http-request {:method "POST" :scheme :http
                             :host "127.0.0.1" :port (:port srv)
                             :target "/echo"
                             :body "upload payload 123"
                             :connect-timeout 3000 :read-timeout 3000
                             :write-timeout 3000})]
        (is (= 200 (:status r)))
        (is (= "upload payload 123" (hr-text (:body-bytes r))))))))

(deftest chunked-response-body-dechunks
  (hr-with-server
    (fn [srv]
      (let [r (hr-get srv "/chunked")]
        (is (= 200 (:status r)))
        (is (= "hello world" (hr-text (:body-bytes r))))))))

(deftest gzip-response-decompresses-by-default
  (hr-with-server
    (fn [srv]
      (let [r (hr-get srv "/gzip")]
        (is (= 200 (:status r)))
        (is (= (apply str (repeat 40 "gz-integration-"))
               (hr-text (:body-bytes r))))
        (is (nil? (get r :content-encoding)))
        ;; Opt-out returns the raw gzip bytes and names the encoding.
        (let [raw (hr-get srv "/gzip" {:decompress-body? false})]
          (is (= 200 (:status raw)))
          (is (= "gzip" (get raw :content-encoding)))
          (is (not= (count (:body-bytes raw))
                    (count (:body-bytes r)))))))))

(deftest redirect-chain-follows-and-records-trace
  (hr-with-server
    (fn [srv]
      (let [r    (hr-get srv "/r1")
            base (str "http://127.0.0.1:" (:port srv))]
        (is (= 200 (:status r)))
        (is (= "final-landing" (hr-text (:body-bytes r))))
        (is (= 2 (count (:trace-redirects r))))
        (is (= (str base "/r2") (nth (:trace-redirects r) 0)))
        (is (= (str base "/final") (nth (:trace-redirects r) 1)))))))

(deftest redirects-off-return-the-3xx-as-data
  (hr-with-server
    (fn [srv]
      ;; Redirects off: the 3xx is data, not an error, and no trace
      ;; is recorded.
      (let [r (hr-get srv "/r1" {:follow-redirects false})]
        (is (= 301 (:status r)))
        (is (= [] (:trace-redirects r)))))))

(deftest redirect-307-preserves-post-body
  (hr-with-server
    (fn [srv]
      (let [r (http-request {:method "POST" :scheme :http
                             :host "127.0.0.1" :port (:port srv)
                             :target "/r307" :body "307-body"
                             :connect-timeout 3000 :read-timeout 3000
                             :write-timeout 3000})]
        (is (= 200 (:status r)))
        (is (= "307-body" (hr-text (:body-bytes r))))))))

(deftest keep-alive-reuses-pooled-connection
  (hr-with-server
    (fn [srv]
      (let [r1 (hr-get srv "/hello")
            r2 (hr-get srv "/echo-path?a=1")]
        (is (false? (:from-pool? r1)))
        (is (true? (:from-pool? r2))
            "second call must check out the pooled socket")
        (is (= "/echo-path?a=1" (hr-text (:body-bytes r2))))
        ;; The accept precedes every response, so the tally is settled
        ;; once both responses returned: one connection served both.
        (is (= 1 @(:accepts srv)))))))

(deftest connection-close-header-bypasses-pool
  (hr-with-server
    (fn [srv]
      (let [r1 (hr-get srv "/close")
            r2 (hr-get srv "/close")]
        (is (= 200 (:status r1)))
        (is (false? (:from-pool? r1)))
        (is (false? (:from-pool? r2)))
        (is (= 2 @(:accepts srv)))))))

(deftest read-timeout-classifies-and-fires-on-schedule
  (hr-with-server
    (fn [srv]
      (let [t0 (time-ms)
            r  (try (hr-get srv "/hold" {:read-timeout 400})
                    (catch e e))
            dt (- (time-ms) t0)]
        (is (= :net/timeout (:mino/kind r)))
        (is (< dt 2400) (str "read timeout fired at " dt " ms"))))))

(deftest body-cap-throws-limit
  (hr-with-server
    (fn [srv]
      (let [r (try (hr-get srv "/big" {:max-bytes 1000}) (catch e e))]
        (is (= :codec/limit (:mino/kind r)))
        (is (str/includes? (:mino/message r) "cap"))))))

(deftest dns-failure-passthrough
  (let [r (try (http-request {:method "GET" :scheme :http
                              :host "host-that-cannot-resolve.invalid"
                              :port 80 :target "/"
                              :connect-timeout 2000})
               (catch e e))]
    (is (= :net/dns (:mino/kind r)))
    (is (str/includes? (:mino/message r) "cannot resolve"))))

(deftest http10-close-delimited-response-parses
  (hr-with-server
    (fn [srv]
      (let [r (hr-get srv "/http10")]
        (is (= 200 (:status r)))
        (is (= "close-delimited-body" (hr-text (:body-bytes r))))
        (is (= "1.0" (:http-version r)))
        (is (false? (:from-pool? r))
            "a close-delimited response leaves nothing to pool")))))

(deftest head-response-with-content-length-completes-after-headers
  ;; The server advertises the entity length but sends no body; a
  ;; HEAD client that opened the Content-Length body window would
  ;; starve until :net/timeout.
  (hr-with-server
    (fn [srv]
      (let [t0 (time-ms)
            r  (http-request {:method "HEAD" :scheme :http
                              :host "127.0.0.1" :port (:port srv)
                              :target "/head-cl"
                              :connect-timeout 3000 :read-timeout 1500
                              :write-timeout 3000})
            dt (- (time-ms) t0)]
        (is (= 200 (:status r)))
        (is (= "11" (get (:headers r) "content-length")))
        (is (= 0 (count (:body-bytes r))))
        (is (< dt 1200)
            (str "HEAD must not wait for body bytes (took " dt
                 " ms)"))))))

(deftest head-response-without-content-length-completes-after-headers
  ;; No framing header at all: still complete at the blank line, not
  ;; stuck in close-framing waiting for an EOF that a keep-alive
  ;; peer never sends.
  (hr-with-server
    (fn [srv]
      (let [t0 (time-ms)
            r  (http-request {:method "HEAD" :scheme :http
                              :host "127.0.0.1" :port (:port srv)
                              :target "/head-no-cl"
                              :connect-timeout 3000 :read-timeout 1500
                              :write-timeout 3000})
            dt (- (time-ms) t0)]
        (is (= 200 (:status r)))
        (is (nil? (get (:headers r) "content-length")))
        (is (= 0 (count (:body-bytes r))))
        (is (< dt 1200)
            (str "HEAD must not wait for EOF (took " dt " ms)"))))))

(run-tests-and-exit)
