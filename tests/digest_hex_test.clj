(require "tests/test")
(require '[mino.digest :as digest])

;; Keyword sugar over the digest prims: digest-hex and hmac-hex answer
;; lowercase hex strings, dispatching on :sha256/:sha1/:md5/:sha512.
;; Anything else throws a diagnostic with :mino/kind :digest/alg and the
;; algorithm in the data map (ADR 37).

(def abc-sha256 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
(def abc-sha1 "a9993e364706816aba3e25717850c26c9cd0d89d")
(def abc-md5 "900150983cd24fb0d6963f7d28e17f72")
;; SHA-512("abc"), FIPS 180-4 example.
(def abc-sha512 (str "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eee"
                     "e64b55d39a2192992a274fc1a836ba3c23a3feebbd454d442364"
                     "3ce80e2a9ac94fa54ca49f"))
(def jefe-hmac "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843")
;; HMAC-SHA512(key "Jefe", "what do ya want for nothing?"), RFC 4231 case 2.
(def jefe-hmac-512 (str "164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd6102"
                        "70cd7ea2505549758bf75c05a994a6d034f65f8f0e6fdcaeab1"
                        "a34d4a6b4b636e070a38bce737"))

(deftest digest-hex-dispatches-on-the-algorithm-keyword
  (is (= abc-sha256 (digest/digest-hex :sha256 "abc")))
  (is (= abc-sha1 (digest/digest-hex :sha1 "abc")))
  (is (= abc-md5 (digest/digest-hex :md5 "abc")))
  (is (= abc-sha512 (digest/digest-hex :sha512 "abc"))))

(deftest digest-hex-passes-strings-and-bytes-through-like-the-prims
  (is (= (hex-encode (sha256 "abc")) (digest/digest-hex :sha256 "abc")))
  (is (= (hex-encode (sha1 (byte-array [0 1 254 255])))
         (digest/digest-hex :sha1 (byte-array [0 1 254 255])))))

(deftest digest-hex-rejects-unknown-algorithms
  (let [e (try (digest/digest-hex :sha3-256 "abc")
               (catch ex (do ex)))]
    (is (= :digest/alg (:mino/kind e)))
    (is (map? (ex-data e)) "unknown algorithm carries a data map")
    (is (= :sha3-256 (:alg (ex-data e)))))
  (is (thrown-with-msg? #"algorithm"
                        (digest/digest-hex "sha256" "abc")))
  (is (thrown-with-msg? #"algorithm"
                        (digest/digest-hex nil "abc"))))

(deftest hmac-hex-macs-with-sha256
  (is (= jefe-hmac
         (digest/hmac-hex :sha256 "Jefe"
                          "what do ya want for nothing?")))
  (is (= (hex-encode (hmac-sha256 "key" (byte-array [1 2 3])))
         (digest/hmac-hex :sha256 "key" (byte-array [1 2 3])))))

(deftest hmac-hex-macs-with-sha512
  (is (= jefe-hmac-512
         (digest/hmac-hex :sha512 "Jefe"
                          "what do ya want for nothing?")))
  (is (= (hex-encode (hmac-sha512 "key" (byte-array [1 2 3])))
         (digest/hmac-hex :sha512 "key" (byte-array [1 2 3])))))

(deftest hmac-hex-rejects-algorithms-without-an-hmac-prim
  (let [e (try (digest/hmac-hex :sha1 "k" "d")
               (catch ex (do ex)))]
    (is (= :digest/alg (:mino/kind e)))
    (is (= :sha1 (:alg (ex-data e)))))
  (is (thrown-with-msg? #"algorithm" (digest/hmac-hex :md5 "k" "d"))))

(run-tests-and-exit)
