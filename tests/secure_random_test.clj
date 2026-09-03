(require "tests/test")

;; Secure random surface: (secure-rand-bytes n) draws n bytes from the
;; OS CSPRNG as MINO_BYTES; (rand-hex n) and (rand-token n) encode n
;; random bytes as a lowercase hex string and an unpadded base64url
;; token. All three install under the :random capability group.

(deftest secure-rand-bytes-exact-length
  (doseq [n [0 1 32 4096]]
    (let [b (secure-rand-bytes n)]
      (is (true? (bytes? b)))
      (is (= n (count b)))
      (is (= n (alength b))))))

(deftest secure-rand-bytes-successive-draws-differ
  ;; Two independent 32-byte draws collide with probability 2^-256;
  ;; equality here means a broken source, not bad luck. Compare the
  ;; unsigned byte contents, not identity.
  (is (not= (vec (seq (secure-rand-bytes 32)))
            (vec (seq (secure-rand-bytes 32))))))

(deftest secure-rand-bytes-rejects-negative
  (is (thrown? (secure-rand-bytes -1)))
  (is (thrown? (secure-rand-bytes -4096))))

(deftest rand-hex-length-and-alphabet
  ;; n counts random bytes; each byte prints as two lowercase hex
  ;; chars, so the string length is exactly 2n.
  (doseq [n [0 1 16 32]]
    (let [s (rand-hex n)]
      (is (string? s))
      (is (= (* 2 n) (count s)))
      (is (some? (re-matches #"[0-9a-f]*" s))))))

(deftest rand-hex-successive-draws-differ
  (is (not= (rand-hex 16) (rand-hex 16))))

(deftest rand-hex-rejects-negative
  (is (thrown? (rand-hex -1))))

(deftest rand-token-length-and-alphabet
  ;; n counts random bytes, encoded base64url without padding: each
  ;; 3-byte group yields 4 chars; a 1- or 2-byte tail yields 2 or 3.
  ;; The alphabet is URL-safe only: alphanumerics, underscore, and dash.
  ;; The dash sits in final class position, where it reads as a literal.
  (doseq [n [0 1 2 3 16 32]]
    (let [s (rand-token n)]
      (is (string? s))
      (is (= (+ (* 4 (quot n 3)) (nth [0 2 3] (rem n 3)))
             (count s)))
      (is (some? (re-matches #"[A-Za-z0-9_-]*" s))))))

(deftest rand-token-non-empty-for-positive-n
  (is (pos? (count (rand-token 1))))
  (is (pos? (count (rand-token 32)))))

(deftest rand-token-excludes-padding-and-standard-base64-chars
  ;; Pin the URL-safe promise on a wide draw: never '=', '+', or '/'.
  (let [s (rand-token 64)]
    (is (false? (clojure.string/includes? s "=")))
    (is (false? (clojure.string/includes? s "+")))
    (is (false? (clojure.string/includes? s "/")))))

(deftest rand-token-successive-draws-differ
  (is (not= (rand-token 32) (rand-token 32))))

(deftest rand-token-rejects-negative
  (is (thrown? (rand-token -1))))

(deftest secure-random-prims-carry-random-capability
  (is (= :random (mino-capability 'secure-rand-bytes)))
  (is (= :random (mino-capability 'rand-token)))
  (is (= :random (mino-capability 'rand-hex))))

(run-tests-and-exit)
