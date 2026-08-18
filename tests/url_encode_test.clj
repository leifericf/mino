(require "tests/test")
(require '[clojure.test.check :as tc])
(require '[clojure.test.check.properties :as prop])
(require '[clojure.test.check.generators :as gen])

;; Percent-encoding over RFC 3986: percent-encode leaves the
;; unreserved set literal and escapes every other byte as %XX with
;; uppercase hex; percent-decode is its inverse for well-formed input
;; and classifies malformed escapes and invalid UTF-8 as errors.

(def trials 60)

;; Suite-mode deftests run after every file has loaded, so top-level
;; helper names must stay clear of other test files' defs (a plain
;; `qc` collides with bc_queue_into_test's deftest-local redefinition).
(defn pct-qc
  ([p] (pct-qc p 20260818))
  ([p seed] (:result (tc/quick-check trials p :seed seed))))

(def unreserved-bytes
  (set (concat (range 65 91) (range 97 123) (range 48 58) [45 46 95 126])))

(defn encoded-charset-ok? [s]
  (every? (fn [c] (or (contains? unreserved-bytes (int c))
                      (= (int c) 37)))
          s))

(def bytes-gen (gen/fmap byte-array (gen/vector (gen/choose 0 255) 0 48)))

(def garbage-gen
  (gen/fmap (fn [v] (apply str (map char v)))
            (gen/vector (gen/choose 32 126) 0 48)))

(deftest unreserved-characters-stay-literal
  (is (= "AZaz09-._~" (percent-encode "AZaz09-._~"))))

(deftest reserved-and-unsafe-characters-are-escaped
  (is (= "a%20b" (percent-encode "a b")))
  (is (= "a%2Fb" (percent-encode "a/b")))
  (is (= "%3A%2F%3F%23%5B%5D%40" (percent-encode ":/?#[]@")))
  (is (= "%2541" (percent-encode "%41")))
  (is (= "a%2Bb%3Dc" (percent-encode "a+b=c")))
  (is (= "%0A" (percent-encode "\n")))
  (is (= "%3C%3E%22%27" (percent-encode "<>\"'"))
      "escape hex digits are uppercase"))

(deftest multibyte-utf-8-encodes-byte-by-byte
  (is (= "%C3%A9" (percent-encode "é")))
  (is (= "%E4%B8%AD" (percent-encode "中")))
  (is (= "%F0%9F%90%B1" (percent-encode "🐱"))))

(deftest encode-input-kind-determines-output-kind
  (is (string? (percent-encode "abc")))
  (is (bytes? (percent-encode (byte-array [104 105]))))
  (is (= (byte-array [65 37 50 48]) (percent-encode (byte-array [65 32])))))

(deftest decode-vectors
  (is (= "a b" (percent-decode "a%20b")))
  (is (= "é" (percent-decode "%C3%A9")))
  (is (= "é" (percent-decode "%c3%a9"))
      "decode accepts lowercase hex digits")
  (is (= "AZaz09-._~" (percent-decode "AZaz09-._~")))
  (is (= "" (percent-decode "")))
  (is (= "" (percent-encode ""))))

(deftest decode-input-kind-determines-output-kind
  (is (= (byte-array [195 169])
         (percent-decode (byte-array [37 67 51 37 65 57])))
      "bytes input skips the UTF-8 check"))

(deftest malformed-escapes-throw
  (doseq [bad ["%" "a%" "%A" "%1" "%G1" "%0z" "abc%XY" "%%"]]
    (is (thrown-with-msg? #"percent-decode" (percent-decode bad))))
  (is (= :eval/contract
         (try (percent-decode "%G1") (catch e (:mino/kind e))))))

(deftest invalid-utf-8-decoded-as-string-throws
  (is (thrown-with-msg? #"UTF-8" (percent-decode "%FF")))
  (is (thrown? (percent-decode "%C3")))
  (is (thrown? (percent-decode "%C3%28")))
  (is (thrown? (percent-decode "%ED%A0%80")))
  (is (thrown? (percent-decode "%C0%80"))))

(deftest type-and-arity-errors
  (is (thrown? (percent-encode 42)))
  (is (thrown? (percent-decode nil)))
  (is (thrown? (percent-decode :kw)))
  (is (thrown? (percent-encode)))
  (is (thrown? (percent-encode "a" "b"))))

(deftest strings-round-trip
  (is (pct-qc (prop/for-all [s gen/string]
           (= s (percent-decode (percent-encode s))))
       424241)))

(deftest bytes-round-trip
  (is (pct-qc (prop/for-all [b bytes-gen]
           (= b (percent-decode (percent-encode b))))
       424242)))

(deftest encoded-output-uses-only-legal-characters
  (is (pct-qc (prop/for-all [s gen/string]
           (encoded-charset-ok? (percent-encode s)))
       424243)))

(deftest garbage-decode-never-crashes
  ;; Random printable garbage runs through decode; every input either
  ;; yields a string or throws a classified :eval/contract error.
  (is (pct-qc (prop/for-all [s garbage-gen]
           (let [r (try (percent-decode s) (catch e e))]
             (or (string? r)
                 (and (map? r) (= :eval/contract (:mino/kind r))))))
       424244)))

(run-tests-and-exit)
