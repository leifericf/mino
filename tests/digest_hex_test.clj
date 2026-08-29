(require "tests/test")
(require '[mino.digest :as digest])

;; Keyword sugar over the digest prims: digest-hex and hmac-hex answer
;; lowercase hex strings, dispatching on :sha256/:sha1/:md5. Anything
;; else throws a diagnostic with :mino/kind :digest/alg and the
;; algorithm in the data map (ADR 37).

(def abc-sha256 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
(def abc-sha1 "a9993e364706816aba3e25717850c26c9cd0d89d")
(def abc-md5 "900150983cd24fb0d6963f7d28e17f72")
(def jefe-hmac "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843")

(deftest digest-hex-dispatches-on-the-algorithm-keyword
  (is (= abc-sha256 (digest/digest-hex :sha256 "abc")))
  (is (= abc-sha1 (digest/digest-hex :sha1 "abc")))
  (is (= abc-md5 (digest/digest-hex :md5 "abc"))))

(deftest digest-hex-passes-strings-and-bytes-through-like-the-prims
  (is (= (hex-encode (sha256 "abc")) (digest/digest-hex :sha256 "abc")))
  (is (= (hex-encode (sha1 (byte-array [0 1 254 255])))
         (digest/digest-hex :sha1 (byte-array [0 1 254 255])))))

(deftest digest-hex-rejects-unknown-algorithms
  (let [e (try (digest/digest-hex :sha512 "abc")
               (catch ex (do ex)))]
    (is (= :digest/alg (:mino/kind e)))
    (is (map? (ex-data e)) "unknown algorithm carries a data map")
    (is (= :sha512 (:alg (ex-data e)))))
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

(deftest hmac-hex-rejects-algorithms-without-an-hmac-prim
  (let [e (try (digest/hmac-hex :sha1 "k" "d")
               (catch ex (do ex)))]
    (is (= :digest/alg (:mino/kind e)))
    (is (= :sha1 (:alg (ex-data e)))))
  (is (thrown-with-msg? #"algorithm" (digest/hmac-hex :md5 "k" "d"))))

(run-tests-and-exit)
