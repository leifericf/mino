(require "tests/test")
(require '[clojure.test.check :as tc])
(require '[clojure.test.check.properties :as prop])
(require '[clojure.test.check.generators :as gen])

;; HTTP message codec: request serialization (http-encode-request,
;; http-encode-chunk) and response parsing (http-parse-response,
;; http-parse-response-chunks). The parser eats untrusted bytes, so
;; the adversarial and fuzz arms below are the core of this file.
;;
;; Fixtures are written as ASCII strings and widened to bytes with bb
;; so request/response bytes stay readable; split feeding uses
;; http-parse-response-chunks, which feeds one parser across many
;; buffers exactly like a socket read loop would.

(defn http-bb [& ss]
  (byte-array (mapcat #(if (bytes? %) (vec %) (map int %)) ss)))

(defn http-parse [b] (http-parse-response b))
(defn http-parse-eof [b] (http-parse-response b {:eof true}))
(defn http-parse-chunks [chunks] (http-parse-response-chunks chunks))
(defn http-parse-chunks-eof [chunks] (http-parse-response-chunks chunks {:eof true}))

;;; request serialization: golden byte vectors

(deftest minimal-get-request-bytes
  (is (= (http-bb "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n")
         (http-encode-request {:method "GET" :target "/"
                               :host "example.com"}))))

(deftest post-request-with-headers-and-body-bytes
  (is (= (http-bb "POST /submit?a=1 HTTP/1.1\r\n"
             "Host: h.example\r\n"
             "User-Agent: mino\r\n"
             "Accept: */*\r\n"
             "Content-Length: 6\r\n"
             "\r\n"
             "name=x")
         (http-encode-request {:method "POST" :target "/submit?a=1"
                               :host "h.example"
                               :headers [["User-Agent" "mino"]
                                         ["Accept" "*/*"]]
                               :body (http-bb "name=x")}))))

(deftest string-body-uses-utf-8-byte-length
  (is (= (http-bb "PUT /x HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\n\r\n"
             (byte-array [195 169 120]))
         (http-encode-request {:method "PUT" :target "/x" :host "h"
                               :body (byte-array [195 169 120])})))
  (is (= (http-bb "PUT /x HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\n\r\nabc")
         (http-encode-request {:method "PUT" :target "/x" :host "h"
                               :body "abc"}))))

(deftest empty-body-emits-zero-content-length
  (is (= (http-bb "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 0\r\n\r\n")
         (http-encode-request {:method "POST" :target "/" :host "h"
                               :body ""}))))

(deftest http10-flag-selects-version-line
  (is (= (http-bb "GET / HTTP/1.0\r\nHost: h\r\n\r\n")
         (http-encode-request {:method "GET" :target "/" :host "h"
                               :http10? true}))))

(deftest headers-map-form-is-accepted
  (is (= (http-bb "GET / HTTP/1.1\r\nHost: h\r\nX-One: 1\r\n\r\n")
         (http-encode-request {:method "GET" :target "/" :host "h"
                               :headers {:X-One "1"}}))))

(deftest chunked-marker-emits-transfer-encoding-and-no-body
  (is (= (http-bb "POST /stream HTTP/1.1\r\n"
             "Host: h\r\n"
             "Transfer-Encoding: chunked\r\n"
             "\r\n")
         (http-encode-request {:method "POST" :target "/stream" :host "h"
                               :chunked? true}))))

(deftest chunk-frame-encoder-vectors
  (is (= (http-bb "3\r\nabc\r\n") (http-encode-chunk (http-bb "abc"))))
  (is (= (http-bb "5\r\nhello\r\n") (http-encode-chunk (http-bb "hello"))))
  (is (= (http-bb "0\r\n\r\n") (http-encode-chunk (http-bb ""))))
  (is (= (http-bb (str "ff\r\n" (apply str (repeat 255 "x")) "\r\n"))
         (http-encode-chunk (http-bb (apply str (repeat 255 "x")))))))

;;; request serialization: rejection of malformed parts

(deftest encoder-rejects-bad-header-names
  (is (thrown? (http-encode-request {:method "GET" :target "/" :host "h"
                                     :headers [["Bad Name" "v"]]})))
  (is (thrown? (http-encode-request {:method "GET" :target "/" :host "h"
                                     :headers [["X@Y" "v"]]})))
  (is (thrown? (http-encode-request {:method "GET" :target "/" :host "h"
                                     :headers [["" "v"]]}))))

(deftest encoder-rejects-control-characters-in-values
  (doseq [v ["a\r\nb" "a\nb" "a\rb" "a\0b"]]
    (is (thrown? (http-encode-request {:method "GET" :target "/" :host "h"
                                       :headers [["X-A" v]]})))))

(deftest encoder-rejects-layer-owned-header-names
  (doseq [n ["Host" "host" "Content-Length" "content-length"
             "Transfer-Encoding" "transfer-encoding"]]
    (is (thrown? (http-encode-request {:method "GET" :target "/" :host "h"
                                       :headers [[n "v"]]})))))

(deftest encoder-rejects-injection-in-target-method-host
  (is (thrown? (http-encode-request {:method "GET" :target "/a b" :host "h"})))
  (is (thrown? (http-encode-request {:method "GET" :target "/a\r\nX: 1" :host "h"})))
  (is (thrown? (http-encode-request {:method "GE T" :target "/" :host "h"})))
  (is (thrown? (http-encode-request {:method "GET" :target "/" :host "h\r\nX: 1"}))))

(deftest encoder-rejects-contradictory-framing
  (is (thrown? (http-encode-request {:method "POST" :target "/" :host "h"
                                     :chunked? true :body "x"})))
  (is (thrown? (http-encode-request {:method "POST" :target "/" :host "h"
                                     :chunked? true :http10? true}))))

(deftest encoder-rejects-missing-required-parts
  (is (thrown? (http-encode-request {:target "/" :host "h"})))
  (is (thrown? (http-encode-request {:method "GET" :host "h"})))
  (is (thrown? (http-encode-request {:method "GET" :target "/"})))
  (is (thrown? (http-encode-request {:method "GET" :target "/" :host "h"
                                     :headers "not-a-collection"}))))

;;; response parsing: golden fixtures

(def resp-normal
  "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 5\r\n\r\nhello")

(deftest parse-normal-response
  (let [r (http-parse (http-bb resp-normal))]
    (is (= :done (:status r)))
    (is (= 200 (:code r)))
    (is (= "OK" (:reason r)))
    (is (= "HTTP/1.1" (:http-version r)))
    (is (= "text/html" (get (:headers r) "content-type")))
    (is (= "5" (get (:headers r) "content-length")))
    (is (= (http-bb "hello") (:body r)))
    (is (not (:chunked? r)))
    (is (= {} (:trailers r)))))

(deftest parse-response-without-reason-phrase
  (let [r (http-parse (http-bb "HTTP/1.1 200\r\nContent-Length: 0\r\n\r\n"))]
    (is (= :done (:status r)))
    (is (= "" (:reason r)))
    (is (= (http-bb "") (:body r)))))

(deftest informational-opt-surfaces-a-101-head
  ;; The Upgrade handshake needs the 101 itself: :informational true
  ;; delivers the 1xx head as a bodiless :done message instead of
  ;; skipping it as interim traffic.
  (let [b (http-bb "HTTP/1.1 101 Switching Protocols\r\n"
                   "Upgrade: websocket\r\n"
                   "Sec-WebSocket-Accept: xyz\r\n\r\n")]
    (is (= :need-more (:status (http-parse b)))
        "without the opt a 1xx is still skipped")
    (let [r (http-parse-response b {:informational true})]
      (is (= :done (:status r)))
      (is (= 101 (:code r)))
      (is (= "Switching Protocols" (:reason r)))
      (is (= "websocket" (get (:headers r) "upgrade")))
      (is (= "xyz" (get (:headers r) "sec-websocket-accept")))
      (is (= (http-bb "") (:body r)) "a 1xx head is bodiless"))))

(deftest parse-bare-lf-line-endings-leniently
  ;; HTTP requires CRLF; real servers emit bare LF often enough that a
  ;; client must take both (documented parser leniency).
  (let [r (http-parse (http-bb "HTTP/1.1 200 OK\nContent-Length: 2\n\nhi"))]
    (is (= :done (:status r)))
    (is (= (http-bb "hi") (:body r)))
    (is (= "2" (get (:headers r) "content-length")))))

(def resp-chunked
  (str "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
       "5\r\nhello\r\n"
       "3\r\n wo\r\n"
       "3\r\nrld\r\n"
       "0\r\nX-Checksum: abc\r\n\r\n"))

(deftest parse-chunked-response-with-trailers
  (let [r (http-parse (http-bb resp-chunked))]
    (is (= :done (:status r)))
    (is (:chunked? r))
    (is (= (http-bb "hello world") (:body r)))
    (is (= "chunked" (get (:headers r) "transfer-encoding")))
    (is (= "abc" (get (:trailers r) "x-checksum")))))

(deftest parse-header-duplicates-collect-into-vectors
  (let [r (http-parse (http-bb (str "HTTP/1.1 200 OK\r\n"
                          "Set-Cookie: a=1\r\n"
                          "Set-Cookie: b=2\r\n"
                          "X-One: 1\r\n"
                          "Content-Length: 0\r\n\r\n")))]
    (is (= ["a=1" "b=2"] (get (:headers r) "set-cookie")))
    (is (= "1" (get (:headers r) "x-one")))))

(deftest parse-204-has-no-body
  (let [r (http-parse (http-bb "HTTP/1.1 204 No Content\r\nServer: x\r\n\r\n"))]
    (is (= :done (:status r)))
    (is (= 204 (:code r)))
    (is (= (http-bb "") (:body r)))))

(deftest parse-204-ignores-content-length
  ;; 204 is bodiless by definition; a content-length header on one is
  ;; server noise and never opens a body window.
  (let [r (http-parse (http-bb "HTTP/1.1 204 No Content\r\nContent-Length: 5\r\n\r\n"))]
    (is (= :done (:status r)))
    (is (= (http-bb "") (:body r)))))

(deftest parse-close-delimited-waits-for-eof
  (let [base "HTTP/1.1 200 OK\r\nServer: x\r\n\r\nsome body"]
    (is (= :need-more (:status (http-parse (http-bb base)))))
    (let [r (http-parse-eof (http-bb base))]
      (is (= :done (:status r)))
      (is (= (http-bb "some body") (:body r))))))

(deftest parse-http10-response
  (let [r (http-parse-eof (http-bb "HTTP/1.0 200 OK\r\n\r\nbody"))]
    (is (= :done (:status r)))
    (is (= "HTTP/1.0" (:http-version r)))
    (is (= (http-bb "body") (:body r)))))

(deftest parse-skips-informational-responses
  (let [r (http-parse (http-bb (str "HTTP/1.1 100 Continue\r\n\r\n"
                          "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi")))]
    (is (= :done (:status r)))
    (is (= 200 (:code r)))
    (is (= (http-bb "hi") (:body r)))))

(deftest parse-accepts-upper-case-framing-headers
  (let [r (http-parse (http-bb (str "HTTP/1.1 200 OK\r\nCONTENT-LENGTH: 2\r\n\r\nhi")))]
    (is (= :done (:status r)))
    (is (= "2" (get (:headers r) "content-length")))))

;;; split feeding: the core incremental surface

(def all-fixtures
  [resp-normal
   resp-chunked
   "HTTP/1.1 200\r\nContent-Length: 0\r\n\r\n"
   "HTTP/1.1 200 OK\r\nSet-Cookie: a=1\r\nSet-Cookie: b=2\r\nContent-Length: 0\r\n\r\n"
   "HTTP/1.1 204 No Content\r\n\r\n"
   "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nxyz"
   "HTTP/1.1 200 OK\nContent-Length: 2\n\nhi"])

(deftest byte-by-byte-feeding-reaches-the-same-result
  (doseq [s all-fixtures]
    (let [whole (http-parse (http-bb s))
          pieces (map http-bb (map str (seq s)))
          r (http-parse-chunks-eof (vec pieces))]
      (is (= whole r) (str "fixture diverged under byte-split: " s)))))

(deftest every-split-offset-agrees-with-single-feed
  (doseq [k (range (inc (count resp-chunked)))]
    (let [two (http-parse-chunks [(http-bb (subs resp-chunked 0 k))
                             (http-bb (subs resp-chunked k))])
          one (http-parse (http-bb resp-chunked))]
      (is (= one two) (str "split at " k " diverged"))))
  (doseq [k (range (inc (count resp-normal)))]
    (is (= (http-parse (http-bb resp-normal))
           (http-parse-chunks [(http-bb (subs resp-normal 0 k))
                          (http-bb (subs resp-normal k))])))))

(deftest need-more-progresses-to-done-at-the-exact-final-byte
  (loop [i 1]
    (when (< i (count resp-normal))
      (is (= :need-more (:status (http-parse (http-bb (subs resp-normal 0 i))))))
      (recur (inc i))))
  (is (= :done (:status (http-parse (http-bb resp-normal))))))

;;; adversarial: smuggling and malformed messages

(defn http-err-of [s]
  (let [r (http-parse (http-bb s))]
    (is (= :error (:status r)) (str "expected error for: " s))
    (:error r)))

(deftest reject-both-content-length-and-transfer-encoding
  (let [e (http-err-of (str "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
                       "Transfer-Encoding: chunked\r\n\r\n"))]
    (is (re-find #"content-length" e))
    (is (re-find #"transfer-encoding" e))))

(deftest reject-conflicting-duplicate-content-length
  (is (re-find #"conflicting"
               (http-err-of (str "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
                            "Content-Length: 6\r\n\r\nhello")))))

(deftest accept-identical-duplicate-content-length
  (let [r (http-parse (http-bb (str "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
                          "Content-Length: 5\r\n\r\nhello")))]
    (is (= :done (:status r)))
    (is (= (http-bb "hello") (:body r)))))

(deftest reject-malformed-content-length-values
  (doseq [cl ["5a" "-1" "1e5" "0x10" "5 6" "+5" ""]]
    (is (= :error (:status (http-parse (http-bb (str "HTTP/1.1 200 OK\r\nContent-Length: "
                                           cl "\r\n\r\n")))))
        (str "content-length [" cl "] must be rejected"))))

(deftest reject-out-of-range-content-length
  ;; 1e19 and beyond cannot be a body length; even 2^53 exactly is
  ;; past the accumulation cap.
  (doseq [cl ["10000000000000000000" "9007199254740993" "99999999999999999999999"]]
    (is (= :error (:status (http-parse (http-bb (str "HTTP/1.1 200 OK\r\nContent-Length: "
                                           cl "\r\n\r\n"))))))))

(deftest reject-non-hex-chunk-size
  (is (= :error (:status (http-parse (http-bb (str "HTTP/1.1 200 OK\r\n"
                                         "Transfer-Encoding: chunked\r\n\r\n"
                                         "ZZ\r\n"))))))
  (is (= :error (:status (http-parse (http-bb (str "HTTP/1.1 200 OK\r\n"
                                         "Transfer-Encoding: chunked\r\n\r\n"
                                         "1x\r\n"))))))
  (is (= :error (:status (http-parse (http-bb (str "HTTP/1.1 200 OK\r\n"
                                         "Transfer-Encoding: chunked\r\n\r\n"
                                         "\r\n")))))))

(deftest reject-missing-terminal-chunk
  (is (= :need-more
         (:status (http-parse (http-bb (str "HTTP/1.1 200 OK\r\n"
                                  "Transfer-Encoding: chunked\r\n\r\n"
                                  "5\r\nhello")))))
      "an unterminated chunked body is incomplete, not yet an error")
  (is (= :error
         (:status (http-parse-eof (http-bb (str "HTTP/1.1 200 OK\r\n"
                                      "Transfer-Encoding: chunked\r\n\r\n"
                                      "5\r\nhello")))))))

(deftest reject-obs-fold-line-folding
  (is (re-find #"fold"
               (http-err-of (str "HTTP/1.1 200 OK\r\nX-A: 1\r\n"
                            "  folded-continuation\r\n"
                            "Content-Length: 0\r\n\r\n")))))

(deftest reject-control-bytes-in-header-values
  ;; RFC 7230 field-content: HTAB is legal padding, every other C0
  ;; control and DEL are not (header values are untrusted structure).
  (let [with-byte (fn [b] (http-bb "HTTP/1.1 200 OK\r\nX-A: a"
                                   (byte-array [b])
                                   "b\r\nContent-Length: 0\r\n\r\n"))]
    (doseq [b [0 1 8 12 27 127]]
      (is (= :error (:status (http-parse (with-byte b)))
             (str "value byte " b " must be rejected")))))
  (let [r (http-parse (http-bb "HTTP/1.1 200 OK\r\nX-A: a\tb\r\n"
                               "Content-Length: 0\r\n\r\n"))]
    (is (= :done (:status r)))
    (is (= "a\tb" (get (:headers r) "x-a")))))

(deftest reject-header-name-with-space
  (is (= :error (:status (http-parse (http-bb (str "HTTP/1.1 200 OK\r\nBad Name: v\r\n"
                                         "Content-Length: 0\r\n\r\n")))))))

(deftest reject-header-line-without-colon
  (is (= :error (:status (http-parse (http-bb (str "HTTP/1.1 200 OK\r\nBadHeaderLine\r\n"
                                         "Content-Length: 0\r\n\r\n")))))))

(deftest reject-status-line-garbage
  (doseq [s ["GARBAGE\r\n\r\n"
             "HTTP/2.0 200 OK\r\n\r\n"
             "HTTP/1.1 ABC OK\r\n\r\n"
             "HTTP/1.1 099 Weird\r\n\r\n"
             "HTTP/1.1 600 Six Hundred\r\n\r\n"
             "http/1.1 200 OK\r\n\r\n"
             "HTTP/1.1 2000 OK\r\n\r\n"]]
    (is (= :error (:status (http-parse (http-bb s)))) s)))

(deftest reject-six-informational-responses
  (is (= :error
         (:status (http-parse (http-bb (str (apply str (repeat 6 "HTTP/1.1 100 Continue\r\n\r\n"))
                                  "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")))))))

(deftest five-informational-responses-are-borne
  (let [r (http-parse (http-bb (str (apply str (repeat 5 "HTTP/1.1 100 Continue\r\n\r\n"))
                           "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")))]
    (is (= :done (:status r)))
    (is (= 200 (:code r)))))

(deftest header-block-cap-fires
  (let [big (apply str (concat ["HTTP/1.1 200 OK\r\nX-Big: "]
                               (repeat 200000 "a")
                               ["\r\n\r\n"]))]
    (is (= :error (:status (http-parse (http-bb big)))))))

(deftest header-count-cap-fires
  (let [many (apply str (concat ["HTTP/1.1 200 OK\r\n"]
                                (map #(str "X-H" % ": v\r\n") (range 150))
                                ["\r\n"]))]
    (is (= :error (:status (http-parse (http-bb many)))))))

(deftest caps-are-raisable-via-opts
  (let [many (apply str (concat ["HTTP/1.1 200 OK\r\n"]
                                (map #(str "X-H" % ": v\r\n") (range 150))
                                ["Content-Length: 0\r\n\r\n"]))]
    (is (= :done (:status (http-parse-response (http-bb many)
                                               {:max-headers 200}))))))

(deftest eof-before-complete-message-is-an-error
  (is (= :error (:status (http-parse-eof (http-bb "HTTP/1.1 200 OK\r\nContent-Len")))))
  (is (= :error (:status (http-parse-eof (http-bb "")))))
  (is (= :error (:status (http-parse-eof (http-bb "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhel"))))))

;;; response header injection: body bytes never become structure

(def inject-data "x\r\n\r\nStatus: 500 Oops\r\n")

(deftest close-delimited-body-cannot-forge-headers
  (let [s (str "HTTP/1.1 200 OK\r\nServer: real\r\n\r\n" inject-data)
        r (http-parse-eof (http-bb s))]
    (is (= :done (:status r)))
    (is (= 200 (:code r)))
    (is (= {"server" "real"} (:headers r)))
    (is (= (http-bb inject-data) (:body r)))))

(deftest chunk-data-cannot-forge-headers-or-trailers
  (is (= 23 (count inject-data)) "fixture self-check")
  (let [s (str "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
               "17\r\n" inject-data "\r\n"
               "0\r\n\r\n")
        r (http-parse (http-bb s))]
    (is (= :done (:status r)))
    (is (= (http-bb inject-data) (:body r)))
    (is (= {} (:trailers r)))
    (is (= 1 (count (:headers r))))))

;;; type and arity contract

(deftest parse-rejects-non-bytes-input
  (is (thrown? (http-parse-response 42)))
  (is (thrown? (http-parse-response nil)))
  (is (thrown? (http-parse-response)))
  (is (thrown? (http-parse-response-chunks "not-a-vector")))
  (is (thrown? (http-encode-request "not-a-map")))
  (is (thrown? (http-encode-request)))
  (is (thrown? (http-encode-chunk 42))))

;;; seeded fuzz: no crash, stable classification across splits

(defn http-xorshift [x]
  (let [x (bit-xor x (bit-shift-left x 13))
        x (bit-xor x (unsigned-bit-shift-right x 7))
        x (bit-xor x (bit-shift-left x 17))]
    (bit-and x 0x7FFFFFFFFFFFFFFF)))

(defn http-rand-range [x lo hi] (+ lo (rem x (- hi lo))))

(def http-soup-alphabets
  [(vec (map int "HTTP/1.1 200 OK\r\nContent-Length: chunked;\r\n abc"))
   (vec (map int "0123456789abcdefABCDEF\r\n; "))
   (vec (range 0 256))])

(defn http-classified? [r]
  (contains? #{:need-more :done :error} (:status r)))

(deftest random-byte-soups-never-crash-the-parser
  (let [bad (atom [])]
    (loop [i 0, x 88172645463325252]
      (when (< i 3000)
        (let [x1 (http-xorshift x)
              n (http-rand-range x1 0 64)
              alphabet (nth http-soup-alphabets (rem x1 3))
              soup (loop [j 0, y x1, acc []]
                     (if (= j n)
                       acc
                       (let [y2 (http-xorshift y)]
                         (recur (inc j) y2
                                (conj acc (nth alphabet
                                               (rem y2 (count alphabet))))))))
              whole (http-parse (byte-array soup))
              cut (http-rand-range (http-xorshift x1) 0 (inc n))
              split (http-parse-chunks [(byte-array (take cut soup))
                                   (byte-array (drop cut soup))])]
          (when-not (http-classified? whole) (swap! bad conj [:unclassified i]))
          (when (and (http-classified? whole) (http-classified? split)
                     (not= (:status whole) (:status split)))
            (swap! bad conj [:split-mismatch i]))
          (recur (inc i) (http-xorshift x1)))))
    (is (= [] @bad) "every soup classifies, and split feeding agrees")))

;;; round-trip law: random bodies through chunk frames parse back

(def http-trials 60)

(deftest random-bodies-round-trip-through-chunked-responses
  (let [str* (fn [v] (apply str v))]
    (is (:result (tc/quick-check
                  http-trials
                  (prop/for-all [chunks (gen/vector
                                         (gen/fmap str*
                                                   (gen/vector
                                                    (gen/elements "abcdefghijklmnopqrstuvwxyz \r\n")
                                                    1 12))
                                         1 6)]
                    (let [joined (apply str chunks)
                          frames (apply str
                                        (map (fn [c]
                                               (str (format "%x" (count c))
                                                    "\r\n" c "\r\n"))
                                             chunks))
                          r (http-parse (http-bb (str "HTTP/1.1 200 OK\r\n"
                                            "Transfer-Encoding: chunked\r\n\r\n"
                                            frames "0\r\n\r\n")))]
                      (and (= :done (:status r))
                           (= joined (apply str (map char (:body r))))
                           (= {} (:trailers r))))))))))

(deftest random-valid-requests-encode-to-exact-expected-bytes
  (let [str* (fn [v] (apply str v))]
    (is (:result (tc/quick-check
                  http-trials
                  (prop/for-all [method (gen/fmap str*
                                                  (gen/vector (gen/elements "GETPOSTUHDLE")
                                                              3 6))
                                 seg (gen/vector gen/string-alphanumeric 0 3)
                                 hname (gen/fmap str*
                                                 (gen/vector (gen/elements "!#$%&'*+-.^_`|~0123456789")
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
                               :headers [[hname hval]]}
                          req (if (zero? (count body))
                                req
                                (assoc req :body body))
                          expected (http-bb (apply str (concat
                                                   [method " " target " HTTP/1.1\r\n"
                                                    "Host: h.example\r\n"
                                                    hname ": " hval "\r\n"]
                                                   (if (zero? (count body))
                                                     ["\r\n"]
                                                     [(str "Content-Length: "
                                                           (count body) "\r\n\r\n" body)]))))]
                      (= expected (http-encode-request req)))))))))

(run-tests-and-exit)
