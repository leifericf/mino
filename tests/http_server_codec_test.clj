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

;;; request parsing: golden fixtures

(defn http-req [b] (http-parse-request b))
(defn http-req-eof [b] (http-parse-request b {:eof true}))
(defn http-req-chunks [cs] (http-parse-request-chunks cs))

(deftest parse-simple-get
  (let [r (http-req (http-bb "GET /a?b=1 HTTP/1.1\r\nHost: h\r\n\r\n"))]
    (is (= :done (:status r)))
    (is (= "GET" (:method r)))
    (is (= "/a?b=1" (:target r)))
    (is (= "HTTP/1.1" (:http-version r)))
    (is (= {"host" "h"} (:headers r)))
    (is (= (http-bb "") (:body r)))
    (is (not (:chunked? r)))
    (is (= (http-bb "") (:leftover r)))
    (is (= {} (:trailers r)))))

(deftest parse-post-with-length-body
  (let [r (http-req (http-bb "POST /submit HTTP/1.1\r\n"
                             "Host: h\r\n"
                             "Content-Length: 6\r\n"
                             "\r\n"
                             "name=x"))]
    (is (= :done (:status r)))
    (is (= "POST" (:method r)))
    (is (= (http-bb "name=x") (:body r)))))

(deftest parse-http10-request
  (let [r (http-req (http-bb "GET / HTTP/1.0\r\nHost: h\r\n\r\n"))]
    (is (= :done (:status r)))
    (is (= "HTTP/1.0" (:http-version r)))))

(deftest request-without-framing-headers-is-bodiless
  ;; Requests are never close-delimited: neither framing header means
  ;; the message ends at the blank line.
  (let [r (http-req (http-bb "POST / HTTP/1.1\r\nHost: h\r\n\r\n"))]
    (is (= :done (:status r)))
    (is (= (http-bb "") (:body r)))))

(deftest parse-chunked-request-with-trailers
  (let [r (http-req (http-bb "POST /up HTTP/1.1\r\n"
                             "Host: h\r\n"
                             "Transfer-Encoding: chunked\r\n"
                             "\r\n"
                             "5\r\nhello\r\n"
                             "3\r\n wo\r\n"
                             "0\r\nX-Sum: 1\r\n\r\n"))]
    (is (= :done (:status r)))
    (is (:chunked? r))
    (is (= (http-bb "hello wo") (:body r)))
    (is (= {"x-sum" "1"} (:trailers r)))))

(deftest parse-request-header-duplicates-collect-into-vectors
  (let [r (http-req (http-bb "GET / HTTP/1.1\r\n"
                             "Host: h\r\n"
                             "X-Tag: a\r\n"
                             "X-Tag: b\r\n"
                             "\r\n"))]
    (is (= ["a" "b"] (get (:headers r) "x-tag")))))

(deftest parse-request-bare-lf-leniently
  (let [r (http-req (http-bb "GET / HTTP/1.1\nHost: h\nContent-Length: 2\n\nhi"))]
    (is (= :done (:status r)))
    (is (= (http-bb "hi") (:body r)))))

;;; keep-alive leftovers and pipelining

(deftest leftover-carries-bytes-past-the-message
  (let [r (http-req (http-bb "GET / HTTP/1.1\r\nHost: h\r\n\r\nNEXT"))]
    (is (= :done (:status r)))
    (is (= (http-bb "NEXT") (:leftover r)))))

(deftest pipelined-two-requests-split-exactly
  (let [two (str "GET /one HTTP/1.1\r\nHost: h\r\n\r\n"
                 "POST /two HTTP/1.1\r\nHost: h\r\n"
                 "Content-Length: 1\r\n\r\nx")
        first (http-req (http-bb two))]
    (is (= :done (:status first)))
    (is (= "/one" (:target first)))
    (is (= (http-bb (subs two 30)) (:leftover first)))
    (let [second (http-req (:leftover first))]
      (is (= :done (:status second)))
      (is (= "POST" (:method second)))
      (is (= "/two" (:target second)))
      (is (= (http-bb "x") (:body second)))
      (is (= (http-bb "") (:leftover second))))))

(deftest pipelined-garbage-after-valid-request-stays-in-leftover
  (let [r (http-req (http-bb "GET / HTTP/1.1\r\nHost: h\r\n\r\n"
                             "not a request at all"))]
    (is (= :done (:status r)))
    (is (= (http-bb "not a request at all") (:leftover r)))))

;;; split feeding: arbitrary read splits agree with one feed

(def req-fixtures
  ["GET /a?b=1 HTTP/1.1\r\nHost: h\r\n\r\n"
   "POST /up HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n"
   "GET / HTTP/1.0\r\nHost: h\r\n\r\n"
   "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\n\r\nxyz"
   "GET / HTTP/1.1\r\nHost: h\r\n\r\nGET /next HTTP/1.1\r\nHost: h\r\n\r\n"])

(deftest byte-by-byte-request-feeding-reaches-the-same-result
  (doseq [s req-fixtures]
    (let [whole (http-req (http-bb s))
          pieces (map http-bb (map str (seq s)))
          r (http-req-chunks (vec pieces))]
      (is (= whole r) (str "fixture diverged under byte-split: " s)))))

(deftest every-request-split-offset-agrees-with-single-feed
  (doseq [s req-fixtures
          k (range (inc (count s)))]
    (is (= (http-req (http-bb s))
           (http-req-chunks [(http-bb (subs s 0 k))
                             (http-bb (subs s k))]))
        (str "split at " k " diverged for " s))))

;;; round-trip law: the client request encoder is the generator oracle

(deftest random-requests-encode-then-parse-back
  (let [str* (fn [v] (apply str v))]
    (is (:result (tc/quick-check
                  http-trials
                  (prop/for-all [method (gen/fmap str*
                                                  (gen/vector
                                                   (gen/elements "GETPOSTUHDLE")
                                                   3 6))
                                  seg (gen/vector gen/string-alphanumeric 0 3)
                                  hname (gen/fmap str*
                                                  (gen/vector
                                                   (gen/elements "!#$%&'*+-.^_`|~0123456789")
                                                   1 8))
                                  hval (gen/fmap str*
                                                 (gen/vector
                                                  (gen/elements "abcdefghijklmnopqrstuvwxyz0123456789 ")
                                                  0 10))
                                  body (gen/fmap str*
                                                 (gen/vector
                                                  (gen/elements "abcdefghijklmnopqrstuvwxyz")
                                                  0 10))]
                    (let [target (str "/" (apply str (interpose "/" seg)))
                          req {:method method :target target :host "h.example"
                               :headers [[hname (str/trim hval)]]}
                          req (if (zero? (count body))
                                req
                                (assoc req :body body))
                          r (http-req (http-encode-request req))]
                      (and (= :done (:status r))
                           (= method (:method r))
                           (= target (:target r))
                           (= (http-bb (or body "")) (or (:body r) (http-bb "")))
                           (= (http-bb "") (:leftover r))
                           (= "h.example" (get (:headers r) "host"))
                           (= (str/trim hval)
                              (get (:headers r) (str/lower-case hname)))))))))))

(deftest random-chunked-request-bodies-round-trip
  (let [str* (fn [v] (apply str v))]
    (is (:result (tc/quick-check
                  http-trials
                  (prop/for-all [chunks (gen/vector
                                         (gen/fmap str*
                                                   (gen/vector
                                                    (gen/elements "abcdefghijklmnopqrstuvwxyz ")
                                                    1 12))
                                         1 6)]
                    (let [joined (apply str chunks)
                          frames (apply str
                                        (map (fn [c]
                                               (str (format "%x" (count c))
                                                    "\r\n" c "\r\n"))
                                             chunks))
                          head (http-encode-request
                                {:method "POST" :target "/up" :host "h"
                                 :chunked? true})
                          r (http-req (http-bb head frames "0\r\n\r\n"))]
                      (and (= :done (:status r))
                           (:chunked? r)
                           (= joined (str (apply str (map char (:body r)))))
                           (= {} (:trailers r))
                           (= (http-bb "") (:leftover r))))))))))

;;; adversarial: request targets, request lines, framing

(defn http-req-err-of [s]
  (let [r (http-req (http-bb s))]
    (is (= :error (:status r)) (str "expected error for: " s))
    (:error r)))

(deftest reject-non-origin-form-targets
  (doseq [line ["GET http://h/x HTTP/1.1"
                "GET https://h/x HTTP/1.1"
                "GET h.example:80 HTTP/1.1"
                "GET * HTTP/1.1"
                "GET x HTTP/1.1"]]
    (is (= :error (:status (http-req (http-bb (str line "\r\nHost: h\r\n\r\n")))))
        (str "target form must be rejected: " line))))

(deftest reject-malformed-request-lines
  (doseq [s ["GET  / HTTP/1.1\r\n\r\n"
             "GET /\r\n\r\n"
             "GET / HTTP/2.0\r\n\r\n"
             "GET / HTTP/10\r\n\r\n"
             "GET / HTTP/1x\r\n\r\n"
             "garbage\r\n\r\n"
             "\r\n\r\n"
             " GET / HTTP/1.1\r\n\r\n"
             "GE T / HTTP/1.1\r\n\r\n"
             "GET /a HTTP/1.1 extra\r\n\r\n"]]
    (is (= :error (:status (http-req (http-bb s))) s))))

(deftest reject-request-smuggling-shapes
  (is (re-find #"content-length"
               (http-req-err-of (str "POST / HTTP/1.1\r\nHost: h\r\n"
                                "Content-Length: 5\r\n"
                                "Transfer-Encoding: chunked\r\n\r\n"))))
  (is (re-find #"conflicting"
               (http-req-err-of (str "POST / HTTP/1.1\r\nHost: h\r\n"
                                "Content-Length: 5\r\n"
                                "Content-Length: 6\r\n\r\n"))))
  (is (= :done (:status (http-req (http-bb (str "POST / HTTP/1.1\r\nHost: h\r\n"
                                          "Content-Length: 5\r\n"
                                          "Content-Length: 5\r\n\r\n"
                                          "hello")))))
      "identical duplicate content-length is borne")
  (doseq [cl ["5a" "-1" "1e5" "0x10" "+5" ""]]
    (is (= :error (:status (http-req (http-bb (str "POST / HTTP/1.1\r\nHost: h\r\n"
                                            "Content-Length: " cl
                                            "\r\n\r\n")))))
        (str "content-length [" cl "] must be rejected")))
  (is (= :error (:status (http-req (http-bb (str "POST / HTTP/1.1\r\nHost: h\r\n"
                                          "Transfer-Encoding: gzip\r\n\r\n"))))))
  (is (= :error (:status (http-req (http-bb (str "POST / HTTP/1.1\r\nHost: h\r\n"
                                          "Transfer-Encoding: chunked\r\n"
                                          "Transfer-Encoding: chunked\r\n"
                                          "\r\n0\r\n\r\n")))))))

(deftest reject-chunked-request-on-http10
  (is (= :error (:status (http-req (http-bb (str "POST / HTTP/1.0\r\nHost: h\r\n"
                                          "Transfer-Encoding: chunked\r\n"
                                          "\r\n0\r\n\r\n")))))))

(deftest reject-obs-fold-and-control-bytes-in-request-headers
  (let [line (fn [v] (str "GET / HTTP/1.1\r\nHost: h\r\n" v "\r\n\r\n"))]
    (is (re-find #"fold" (http-req-err-of (line "X-A: 1\r\n folded")))))
  (let [with-byte (fn [b] (http-bb "GET / HTTP/1.1\r\nHost: h\r\nX-A: a"
                                   (byte-array [b])
                                   "b\r\n\r\n"))]
    (doseq [b [0 1 12 27 127]]
      (is (= :error (:status (http-req (with-byte b))))
          (str "value byte " b " must be rejected"))))
  (is (= :done (:status (http-req (http-bb "GET / HTTP/1.1\r\nHost: h\r\nX-A: a\tb\r\n\r\n")))))
  (is (= :error (:status (http-req (http-bb "GET / HTTP/1.1\r\nHost: h\r\nBad Name: v\r\n\r\n")))))
  (is (= :error (:status (http-req (http-bb "GET / HTTP/1.1\r\nHost: h\r\nBadHeaderLine\r\n\r\n"))))))

(deftest request-cap-edges
  (is (= :error (:status (http-req (http-bb (str "GET / HTTP/1.1\r\nHost: h\r\n"
                                           "X-Big: "
                                           (apply str (repeat 200000 "a"))
                                           "\r\n\r\n"))))))
  (is (= :error (:status (http-req (http-bb (apply str (concat
                                           ["GET / HTTP/1.1\r\nHost: h\r\n"]
                                           (map #(str "X-H" % ": v\r\n")
                                                (range 150))
                                           ["\r\n"]))))))
    )
  (is (= :done (:status (http-parse-request
                        (http-bb (apply str (concat
                                         ["GET / HTTP/1.1\r\nHost: h\r\n"]
                                         (map #(str "X-H" % ": v\r\n")
                                              (range 150))
                                         ["\r\n"])))
                        {:max-headers 200}))))
  ;; body caps: at the cap is done, past it errors, on both framings
  (let [body-at-cap (apply str (repeat 16 "ab"))]
    (is (= :done (:status (http-req (http-bb (str "POST / HTTP/1.1\r\nHost: h\r\n"
                                            "Content-Length: 32\r\n\r\n"
                                            body-at-cap))))))
    (is (= :error (:status (http-parse-request
                           (http-bb (str "POST / HTTP/1.1\r\nHost: h\r\n"
                                    "Content-Length: 33\r\n\r\n" body-at-cap
                                    "x"))
                           {:max-body-bytes 32}))))
    (is (= :done (:status (http-parse-request
                          (http-bb (str "POST / HTTP/1.1\r\nHost: h\r\n"
                                   "Transfer-Encoding: chunked\r\n\r\n"
                                   "20\r\n" body-at-cap "\r\n0\r\n\r\n"))
                          {:max-body-bytes 32}))))
    (is (= :error (:status (http-parse-request
                          (http-bb (str "POST / HTTP/1.1\r\nHost: h\r\n"
                                   "Transfer-Encoding: chunked\r\n\r\n"
                                   "20\r\n" body-at-cap "\r\n"
                                   "1\r\nx\r\n0\r\n\r\n"))
                          {:max-body-bytes 32}))))))

(deftest request-eof-and-type-contract
  (is (= :error (:status (http-req-eof (http-bb "GET / HTT")))))
  (is (= :error (:status (http-req-eof (http-bb "GET / HTTP/1.1\r\nHost: h\r\n"
                                          "Content-Length: 5\r\n\r\nhel")))))
  (is (= :done (:status (http-req-eof (http-bb "GET / HTTP/1.1\r\nHost: h\r\n\r\n")))))
  (is (= :error (:status (http-req-eof (http-bb "POST / HTTP/1.1\r\nHost: h\r\n"
                                           "Content-Length: 2\r\n\r\nx")))))
  (is (thrown? (http-parse-request 42)))
  (is (thrown? (http-parse-request)))
  (is (thrown? (http-parse-request-chunks "not-a-vector"))))

;;; bounded-exhaustive: request-line variants x framing x versions

(deftest request-line-matrix-parities
  (doseq [method ["GET" "POST" "X-CUSTOM"]
          target ["/" "/a?b=1" "/a%20b/c"]
          [version http10] [["HTTP/1.1" false] ["HTTP/1.0" true]]
          [framing body] [[:cl "ab"]
                          [:none ""]]]
    (let [framing-hdr (case framing
                        :cl (str "Content-Length: " (count body) "\r\n")
                        :none "")
          r (http-req (http-bb (str method " " target " " version "\r\n"
                                   "Host: h\r\n" framing-hdr "\r\n" body)))]
      (is (= :done (:status r)) (str method " " target " " version))
      (is (= method (:method r)))
      (is (= target (:target r)))
      (is (= version (:http-version r)))
      (is (= (http-bb body) (or (:body r) (http-bb "")))))))

;;; seeded fuzz: request soups never crash, splits agree

(defn http-sc-xorshift [x]
  (let [x (bit-xor x (bit-shift-left x 13))
        x (bit-xor x (unsigned-bit-shift-right x 7))
        x (bit-xor x (bit-shift-left x 17))]
    (bit-and x 0x7FFFFFFFFFFFFFFF)))

(defn http-sc-range [x lo hi] (+ lo (rem x (- hi lo))))

(defn http-sc-classified? [r]
  (contains? #{:need-more :done :error} (:status r)))

(def http-req-soup-alphabets
  [(vec (map int "GETPOST * HTTP/1.01\r\n:;/\r\n"))
   (vec (map int "0123456789abcdefABCDEF\r\n; "))
   (vec (range 0 256))])

(deftest random-byte-soups-never-crash-the-request-parser
  (let [bad (atom [])]
    (loop [i 0, x 42424242424242424]
      (when (< i 3000)
        (let [x1 (http-sc-xorshift x)
              n (http-sc-range x1 0 64)
              alphabet (nth http-req-soup-alphabets (rem x1 3))
              soup (loop [j 0, y x1, acc []]
                     (if (= j n)
                       acc
                       (let [y2 (http-sc-xorshift y)]
                         (recur (inc j) y2
                                (conj acc (nth alphabet
                                               (rem y2 (count alphabet))))))))
              whole (http-req (byte-array soup))
              cut (http-sc-range (http-sc-xorshift x1) 0 (inc n))
              split (http-req-chunks [(byte-array (take cut soup))
                                      (byte-array (drop cut soup))])]
          (when-not (http-sc-classified? whole) (swap! bad conj [:unclassified i]))
          (when (and (http-sc-classified? whole) (http-sc-classified? split)
                     (not= (:status whole) (:status split)))
            (swap! bad conj [:split-mismatch i]))
          (recur (inc i) (http-sc-xorshift x1)))))
    (is (= [] @bad) "every soup classifies, and split feeding agrees")))

(run-tests-and-exit)
