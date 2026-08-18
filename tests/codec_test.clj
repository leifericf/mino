(require "tests/test")
(require '[clojure.test.check :as tc])
(require '[clojure.test.check.properties :as prop])
(require '[clojure.test.check.generators :as gen])

;; Binary codecs: base64 (RFC 4648, padded, strict on decode) and
;; lowercase hex. Encoders take a string or bytes value and return a
;; string; decoders take a string or bytes value and return bytes.

(def trials 60)

(defn codec-qc [p seed]
  (:result (tc/quick-check trials p :seed seed)))

(def bytes-gen (gen/fmap byte-array (gen/vector (gen/choose 0 255) 0 48)))

(def garbage-gen
  (gen/fmap (fn [v] (apply str v))
            (gen/vector (gen/choose 32 126) 0 48)))

;;; base64

(deftest base64-rfc-4648-vectors
  (is (= "" (base64-encode "")))
  (is (= "Zg==" (base64-encode "f")))
  (is (= "Zm8=" (base64-encode "fo")))
  (is (= "Zm9v" (base64-encode "foo")))
  (is (= "Zm9vYg==" (base64-encode "foob")))
  (is (= "Zm9vYmE=" (base64-encode "fooba")))
  (is (= "Zm9vYmFy" (base64-encode "foobar"))))

(deftest base64-encodes-utf-8-bytes-and-byte-values
  (is (= "w6k=" (base64-encode "é")))
  (is (= "AP/++A==" (base64-encode (byte-array [0 255 254 248])))))

(deftest base64-decode-returns-bytes
  (is (= (byte-array []) (base64-decode "")))
  (is (= (byte-array [102]) (base64-decode "Zg==")))
  (is (= (byte-array [102 111 111 98 97 114]) (base64-decode "Zm9vYmFy")))
  (is (= (byte-array [195 169]) (base64-decode "w6k="))))

(deftest base64-wrong-length-throws
  (doseq [bad ["Z" "Zg" "Zg=" "Zm9vY" "Zm9vYg="]]
    (is (thrown-with-msg? #"multiple of 4" (base64-decode bad)))))

(deftest base64-rejects-whitespace-and-non-alphabet
  (is (thrown-with-msg? #"invalid character" (base64-decode "Z\ng=")))
  (is (thrown-with-msg? #"invalid character" (base64-decode "Z ==")))
  (is (thrown-with-msg? #"invalid character" (base64-decode "Z_==")))
  (is (thrown-with-msg? #"invalid character" (base64-decode " w6k"))))

(deftest base64-rejects-bad-padding-shapes
  (is (thrown-with-msg? #"invalid" (base64-decode "====")))
  (is (thrown-with-msg? #"invalid" (base64-decode "=AAA")))
  (is (thrown-with-msg? #"invalid" (base64-decode "A===")))
  (is (thrown-with-msg? #"invalid" (base64-decode "AB=C"))))

(deftest base64-rejects-non-canonical-trailing-bits
  ;; QR== has leftover bits set in the final data character: strict
  ;; RFC 4648 decoding refuses it; QQ== is the canonical form.
  (is (thrown-with-msg? #"canonical" (base64-decode "QR==")))
  (is (thrown-with-msg? #"canonical" (base64-decode "Zm9=")))
  (is (= (byte-array [102]) (base64-decode "Zg=="))))

;;; hex

(deftest hex-encode-vectors
  (is (= "" (hex-encode "")))
  (is (= "666f6f" (hex-encode "foo")))
  (is (= "c3a9" (hex-encode "é")))
  (is (= "0001feff" (hex-encode (byte-array [0 1 254 255])))
      "hex digits are lowercase"))

(deftest hex-decode-returns-bytes
  (is (= (byte-array []) (hex-decode "")))
  (is (= (byte-array [102 111 111]) (hex-decode "666f6f")))
  (is (= (byte-array [102 111 111]) (hex-decode "666F6F"))
      "decode accepts uppercase hex")
  (is (= (byte-array [195 169]) (hex-decode "c3a9"))))

(deftest hex-decode-malformed-input-throws
  (is (thrown-with-msg? #"odd" (hex-decode "abc")))
  (is (thrown-with-msg? #"odd" (hex-decode "0")))
  (is (thrown-with-msg? #"invalid character" (hex-decode "zz")))
  (is (thrown-with-msg? #"invalid character" (hex-decode "0x10"))))

;;; shared contract

(deftest type-and-arity-errors
  (is (thrown? (base64-encode 42)))
  (is (thrown? (base64-decode nil)))
  (is (thrown? (hex-encode :kw)))
  (is (thrown? (hex-decode 1.5)))
  (is (thrown? (base64-encode)))
  (is (thrown? (hex-decode "aa" "bb"))))

;;; properties

(deftest random-bytes-round-trip-through-base64
  (is (codec-qc (prop/for-all [b bytes-gen]
                  (= b (base64-decode (base64-encode b))))
                444441)))

(deftest random-bytes-round-trip-through-hex
  (is (codec-qc (prop/for-all [b bytes-gen]
                  (= b (hex-decode (hex-encode b))))
                444442)))

(deftest base64-output-has-alphabet-and-padding-shape
  (is (codec-qc (prop/for-all [b bytes-gen]
                  (let [out (base64-encode b)]
                    (and (= 0 (rem (count out) 4))
                         (re-matches #"[A-Za-z0-9+/]*={0,2}" out))))
                444443)))

(deftest hex-output-is-lowercase-and-double-length
  (is (codec-qc (prop/for-all [b bytes-gen]
                  (let [out (hex-encode b)]
                    (and (= (* 2 (count b)) (count out))
                         (re-matches #"[0-9a-f]*" out))))
                444444)))

(deftest garbage-decode-never-crashes
  ;; Printable garbage hits both decoders; every input either yields a
  ;; bytes value or throws a classified :eval/contract error.
  (is (codec-qc (prop/for-all [s garbage-gen]
                  (let [b64 (try (base64-decode s)
                                 (catch e (if (= :eval/contract (:mino/kind e))
                                            :ok
                                            (str "bad-kind:" (:mino/kind e)))))
                        hx  (try (hex-decode s)
                                 (catch e (if (= :eval/contract (:mino/kind e))
                                            :ok
                                            (str "bad-kind:" (:mino/kind e)))))]
                    (and (or (bytes? b64) (= :ok b64))
                         (or (bytes? hx) (= :ok hx)))))
                444445)))

(run-tests-and-exit)
