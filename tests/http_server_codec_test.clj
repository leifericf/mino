(require "tests/test")
(require '[clojure.string :as str])
(require '[clojure.test.check :as tc])
(require '[clojure.test.check.properties :as prop])
(require '[clojure.test.check.generators :as gen])

;; HTTP server codec: response serialization (http-encode-response)
;; and, over the same wire discipline, request parsing
;; (http-parse-request). The request parser eats untrusted bytes; its
;; adversarial arms are the point of this file.

(defn http-bb [& ss]
  (byte-array (mapcat #(if (bytes? %) (vec %) (map int %)) ss)))

;;; response serialization: golden byte vectors

(deftest minimal-bodiless-response-bytes
  (is (= (http-bb "HTTP/1.1 200 OK\r\n\r\n")
         (http-encode-response {:status 200}))))

(deftest response-with-headers-and-body-bytes
  (is (= (http-bb "HTTP/1.1 200 OK\r\n"
              "Content-Type: text/html\r\n"
              "X-Custom: v\r\n"
              "Content-Length: 5\r\n"
              "\r\n"
              "hello")
         (http-encode-response {:status 200
                                :headers [["Content-Type" "text/html"]
                                          ["X-Custom" "v"]]
                                :body "hello"}))))

(deftest reason-table-covers-common-codes
  (is (= (http-bb "HTTP/1.1 404 Not Found\r\n\r\n")
         (http-encode-response {:status 404})))
  (is (= (http-bb "HTTP/1.1 201 Created\r\n\r\n")
         (http-encode-response {:status 201})))
  (is (= (http-bb "HTTP/1.1 204 No Content\r\n\r\n")
         (http-encode-response {:status 204})))
  (is (= (http-bb "HTTP/1.1 503 Service Unavailable\r\n\r\n")
         (http-encode-response {:status 503}))))

(deftest unknown-code-emits-no-reason-and-no-trailing-space
  (is (= (http-bb "HTTP/1.1 599\r\n\r\n")
         (http-encode-response {:status 599}))))

(deftest bodies-carry-byte-length
  (is (= (http-bb "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\n"
                  (byte-array [195 169 120]))
         (http-encode-response {:status 200
                                :body (byte-array [195 169 120])})))
  (is (= (http-bb "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc")
         (http-encode-response {:status 200 :body "abc"}))))

(deftest empty-body-emits-zero-content-length
  (is (= (http-bb "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")
         (http-encode-response {:status 200 :body ""}))))

(deftest present-body-is-always-length-framed-even-on-204
  ;; Bodiless emission for 204/304/HEAD is the handler's contract; the
  ;; encoder stays mechanical about a body it is handed.
  (is (= (http-bb "HTTP/1.1 204 No Content\r\nContent-Length: 1\r\n\r\nx")
         (http-encode-response {:status 204 :body "x"}))))

(deftest http10-flag-selects-version-line
  (is (= (http-bb "HTTP/1.0 200 OK\r\n\r\n")
         (http-encode-response {:status 200 :http10? true}))))

(deftest connection-and-date-emissions
  (is (= (http-bb "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n")
         (http-encode-response {:status 200 :close? true})))
  (is (= (http-bb "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\n\r\n")
         (http-encode-response {:status 200 :keep-alive? true})))
  (is (= (http-bb "HTTP/1.0 200 OK\r\nConnection: keep-alive\r\n\r\n")
         (http-encode-response {:status 200 :http10? true
                                :keep-alive? true})))
  (is (= (http-bb "HTTP/1.1 200 OK\r\n"
                  "Date: Sun, 06 Nov 1994 08:49:37 GMT\r\n\r\n")
         (http-encode-response {:status 200
                                :date "Sun, 06 Nov 1994 08:49:37 GMT"}))))

(deftest framing-headers-follow-handler-headers-in-fixed-order
  (is (= (http-bb "HTTP/1.1 200 OK\r\n"
              "X-A: 1\r\n"
              "Date: D\r\n"
              "Connection: close\r\n"
              "Content-Length: 1\r\n"
              "\r\n"
              "x")
         (http-encode-response {:status 200
                                :headers [["X-A" "1"]]
                                :date "D"
                                :close? true
                                :body "x"}))))

(deftest headers-map-form-is-accepted
  (is (= (http-bb "HTTP/1.1 200 OK\r\nX-One: 1\r\n\r\n")
         (http-encode-response {:status 200 :headers {:X-One "1"}}))))

;;; response serialization: rejection of malformed parts

(deftest encoder-rejects-bad-status
  (doseq [s [99 600 0 -1 "200" nil]]
    (is (thrown? (http-encode-response {:status s}))))
  (is (thrown? (http-encode-response {}))))

(deftest encoder-rejects-server-owned-header-names
  (doseq [n ["Content-Length" "content-length"
             "Transfer-Encoding" "transfer-encoding"
             "Connection" "connection"
             "Date" "date"]]
    (is (thrown? (http-encode-response {:status 200
                                        :headers [[n "v"]]})))))

(deftest encoder-rejects-bad-header-names
  (doseq [n ["" "Bad Name" "X@Y" "X\tY"]]
    (is (thrown? (http-encode-response {:status 200
                                        :headers [[n "v"]]})))))

(deftest encoder-rejects-control-characters-in-values
  (doseq [v ["a\r\nb" "a\nb" "a\rb" "a\0b" (str "a" (char 127) "b")]]
    (is (thrown? (http-encode-response {:status 200
                                        :headers [["X-A" v]]})))))

(deftest encoder-rejects-non-string-header-values
  (doseq [v [42 :kw (http-bb "x")]]
    (is (thrown? (http-encode-response {:status 200
                                        :headers [["X-A" v]]})))))

(deftest encoder-rejects-contradictory-connection-keys
  (is (thrown? (http-encode-response {:status 200 :close? true
                                      :keep-alive? true}))))

(deftest encoder-rejects-bad-argument-shapes
  (is (thrown? (http-encode-response "not-a-map")))
  (is (thrown? (http-encode-response)))
  (is (thrown? (http-encode-response {:status 200} {:status 201})))
  (is (thrown? (http-encode-response {:status 200 :headers "nope"})))
  (is (thrown? (http-encode-response {:status 200 :body 42})))
  (is (thrown? (http-encode-response {:status 200 :date 42}))))

;;; round-trip law: encoded responses parse back through the client
;;; parser with the same code, headers, and body

(def http-trials 60)

(def http-name-pool ["X-Aa" "X-Bb" "X-Cc" "X-Dd"])

(deftest random-responses-round-trip-through-the-response-parser
  (let [str* (fn [v] (apply str v))]
    (is (:result (tc/quick-check
                  http-trials
                  (prop/for-all [status (gen/elements
                                         [200 201 204 301 302 304 400 404
                                          418 429 500 503 599])
                                 nheaders (gen/choose 0 4)
                                 hvals (gen/vector
                                        (gen/fmap str*
                                                  (gen/vector
                                                   (gen/elements
                                                    "abcdefghijklmnopqrstuvwxyz0123456789 ")
                                                   0 10))
                                        4)
                                 body (gen/one-of
                                       [(gen/return nil)
                                        (gen/fmap str*
                                                  (gen/vector
                                                   (gen/elements "abcdefghijklmnopqrstuvwxyz")
                                                   0 12))])]
                    (let [pairs (map vector (take nheaders http-name-pool)
                                     hvals)
                          ;; 204 and 304 are bodiless at the parse edge;
                          ;; a handler never sends a body on them
                          body (if (contains? #{204 304} status)
                                 nil body)
                          m (if (> nheaders 0)
                              (assoc {:status status} :headers (vec pairs))
                              {:status status})
                          m (if (some? body) (assoc m :body body) m)
                          wire (http-encode-response m)
                          r (http-parse-response wire {:eof true})
                          expected (zipmap
                                    (map str/lower-case
                                         (map first pairs))
                                    (map (fn [p] (str/trim (second p)))
                                         pairs))
                          expected (if (some? body)
                                     (assoc expected
                                            "content-length"
                                            (str (count body)))
                                     expected)]
                      (and (= :done (:status r))
                           (= status (:code r))
                           (= (http-bb (or body ""))
                              (or (:body r) (http-bb "")))
                           (= expected (:headers r))))))))))

(run-tests-and-exit)
