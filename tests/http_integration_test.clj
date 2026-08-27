(require "tests/test")
(require "tests/fixtures/http/server")
(require '[clojure.string :as str])
(require '[clojure.data.json :as json])
(require '[mino.http :as http])

;; End-to-end HTTP client lane: the full stack (mino.http plain-map
;; surface, http-request prim, pool, net, codecs) against the
;; in-process mino server (tests/fixtures/http/server.clj) on a
;; loopback port. The scenarios mirror real client work: parametered
;; GETs with headers, JSON POST echo with server-side validation, the
;; maintainer's pagination gist as plain maps, error statuses as
;; ex-data, redirect chains with 307 body preservation, keep-alive
;; connection reuse counted by the server, gzip and chunked bodies,
;; async deref, and read timeouts. TLS end-to-end legs live in the
;; mino-tests satellite repo; the port manifest is
;; ~/.agentic-sdk/mino/runs/http-client/moved-tls-tests.md.

(defn- hi-with-server
  "Run body with the plain-base URL; the fixture is torn down after
  the body whether it passed, threw, or errored."
  [body]
  (fx-with-server
    (fn [srv]
      (body (str "http://127.0.0.1:" (:port srv))))))

;; ---- loopback end to end ----

(deftest get-carries-query-params-and-headers-to-the-server
  (hi-with-server
    (fn [base]
      (let [b (:body (http/get (str base "/echo-headers")
                               {:query-params {:type "backend" :n 2}
                                :headers {:x-probe "yes"}
                                :user-agent "integration-probe"
                                :accept :json
                                :as :json}))]
        (is (= ["backend"] (:type (:query b))))
        (is (= ["2"] (:n (:query b))))
        (is (= "integration-probe" (:user-agent b)))
        (is (= "application/json" (:accept b)))
        (is (= "yes" (:x-probe b)))))))

(deftest post-json-echo-round-trips-and-validates-content-type
  (hi-with-server
    (fn [base]
      (let [payload {:name "mino"
                     :tags ["http" "integration"]
                     :nested {:ok true}}
            r (http/post (str base "/echo-json")
                         {:body (json/write-str payload)
                          :content-type :json
                          :as :json})
            b (:body r)]
        (is (= 200 (:status r)))
        (is (= payload (:echo b)))
        (is (= "application/json" (:seen b))))
      (let [e (try (http/post (str base "/echo-json")
                              {:body "plain text" :content-type :text})
                   (catch Throwable e e))]
        (is (= "HTTP 400" (ex-message e)))
        (is (= 400 (:status (ex-data e)))))
      (let [e (try (http/post (str base "/echo-json")
                              {:body "{\"oops\": " :content-type :json})
                   (catch Throwable e e))]
        (is (= 400 (:status (ex-data e))))))))

(deftest gist-pagination-iterates-two-pages-via-continuation-param
  ;; The maintainer's bb gist shape: plain maps into http/request,
  ;; pages walked by a continuation query param, items concatenated
  ;; on the client.
  (hi-with-server
    (fn [base]
      (let [pages (loop [page 1, acc []]
                    (let [b (:body (http/request
                                     {:method :get
                                      :uri (str base "/items")
                                      :headers {:accept "application/json"}
                                      :query-params {:page page}
                                      :as :json
                                      :throw false}))]
                      (if (:next_page b)
                        (recur (:next_page b) (conj acc b))
                        (conj acc b))))
            all (mapcat :items pages)]
        (is (= [1 2] (map :page pages)))
        (is (= ["alpha" "beta" "gamma"] (vec all)))
        (is (nil? (:next_page (nth pages 1))))))))

(deftest not-found-throws-with-the-response-as-ex-data
  (hi-with-server
    (fn [base]
      (let [e (try (http/get (str base "/missing")) (catch Throwable e e))
            d (ex-data e)]
        (is (= "HTTP 404" (ex-message e)))
        (is (= 404 (:status d)))
        (is (= "not here" (:body d))))
      (let [r (http/get (str base "/missing") {:throw false})]
        (is (= 404 (:status r)))
        (is (= "not here" (:body r)))))))

(deftest redirect-chain-follows-and-records-the-trace
  (hi-with-server
    (fn [base]
      (let [r (http/get (str base "/r1"))]
        (is (= 200 (:status r)))
        (is (= "final-landing" (:body r)))
        (is (= [(str base "/r2") (str base "/final")]
               (:trace-redirects r)))))))

(deftest redirect-307-preserves-post-body-end-to-end
  (hi-with-server
    (fn [base]
      (let [r (http/post (str base "/r307")
                         {:body "307-integration-body"})]
        (is (= 200 (:status r)))
        (is (= "307-integration-body" (:body r)))
        (is (= [(str base "/echo")] (:trace-redirects r)))))))

(deftest keep-alive-serves-every-request-over-one-connection
  ;; The fixture counts accepts and serves the tally at /conncount;
  ;; the accept happens before each response, so the number is
  ;; settled once the responses return. Three requests over one
  ;; pooled connection read back 1.
  (hi-with-server
    (fn [base]
      (is (= 200 (:status (http/get (str base "/hello")))))
      (is (= 200 (:status (http/get (str base "/hello")))))
      (is (= 1 (parse-long (:body (http/get (str base "/conncount"))))
             "three requests must share one connection")))))

(deftest gzip-body-round-trips-and-raw-opt-out-stays-gzip
  (hi-with-server
    (fn [base]
      (let [r (http/get (str base "/gzip"))]
        (is (= 200 (:status r)))
        (is (= (apply str (repeat 40 "gz-integration-")) (:body r))))
      (let [raw (:body (http/get (str base "/gzip")
                                 {:decompress-body? false
                                  :as :bytes}))]
        (is (= 0x1f (nth raw 0)))
        (is (= 0x8b (nth raw 1)))))))

(deftest chunked-body-dechunks-to-the-full-payload
  (hi-with-server
    (fn [base]
      (let [r (http/get (str base "/chunked"))]
        (is (= 200 (:status r)))
        (is (= "hello world" (:body r)))))))

(deftest async-deref-matches-the-sync-result
  (hi-with-server
    (fn [base]
      (let [sync  (http/get (str base "/hello"))
            fut   (http/get (str base "/hello") {:async true})
            async (deref fut)]
        (is (= (:status sync) (:status async)))
        (is (= (:body sync) (:body async)))))))

(deftest read-timeout-classifies-and-fires-on-schedule
  (hi-with-server
    (fn [base]
      (let [t0 (time-ms)
            e  (try (http/get (str base "/hold") {:timeout 400})
                    (catch Throwable e e))
            dt (- (time-ms) t0)]
        (is (= :timeout (-> (ex-data e) :error :kind)))
        (is (< dt 2400) (str "read timeout fired at " dt " ms"))))))

(run-tests-and-exit)
