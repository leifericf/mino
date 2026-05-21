(require "tests/test")

;; JVM Clojure surface-parity statics: pure value constants, Math
;; methods, parsers/predicates, java.util collection factories, and
;; the embedded-host semantic remap (System/currentTimeMillis,
;; java.util.UUID/randomUUID, ...). The Clojure-level UX matches JVM;
;; mino's underlying primitive may differ.

(deftest jvm-statics-numeric-constants
  ;; Long/MAX_VALUE fits in a mino int (auto-promotes to bigint at
  ;; print time because it's outside mino's inline-int range; the
  ;; numeric VALUE is JVM-canon).
  (is (= 9223372036854775807 Long/MAX_VALUE))
  (is (= -9223372036854775808 Long/MIN_VALUE))
  (is (= 2147483647 Integer/MAX_VALUE))
  (is (= -2147483648 Integer/MIN_VALUE))
  (is (= 32767 Short/MAX_VALUE))
  (is (= -32768 Short/MIN_VALUE))
  (is (= 127 Byte/MAX_VALUE))
  (is (= -128 Byte/MIN_VALUE)))

(deftest jvm-statics-float-constants
  (is (< 1.79e308 Double/MAX_VALUE))
  (is (true? (Double/isInfinite Double/POSITIVE_INFINITY)))
  (is (true? (Double/isInfinite Double/NEGATIVE_INFINITY)))
  (is (true? (Double/isNaN Double/NaN))))

(deftest jvm-statics-boolean
  (is (= true Boolean/TRUE))
  (is (= false Boolean/FALSE))
  (is (true?  (Boolean/parseBoolean "true")))
  (is (true?  (Boolean/parseBoolean "TRUE")))
  (is (false? (Boolean/parseBoolean "false")))
  (is (false? (Boolean/parseBoolean "yes")))
  (is (false? (Boolean/parseBoolean nil))))

(deftest jvm-statics-math-constants
  (is (< 3.14 Math/PI 3.15))
  (is (< 2.71 Math/E  2.72)))

(deftest jvm-statics-math-methods
  (is (= 4.0  (Math/sqrt 16)))
  (is (= 2.0  (Math/floor 2.7)))
  (is (= 3.0  (Math/ceil  2.3)))
  (is (= 3    (Math/round 2.7)))
  (is (= 1.0  (Math/abs -1.0)))
  (is (= 5    (Math/abs -5)))
  (is (= 1    (Math/min 1 2)))
  (is (= 2    (Math/max 1 2)))
  (is (= 8.0  (Math/pow 2 3)))
  (is (= 0.0  (Math/sin 0))))

(deftest jvm-statics-parsers
  (is (= 42       (Long/parseLong "42")))
  (is (= 255      (Long/parseLong "FF" 16)))
  (is (= 7        (Integer/parseInt "7")))
  (is (= 3.14     (Double/parseDouble "3.14")))
  (is (thrown?    (Long/parseLong "not-a-number"))))

(deftest jvm-statics-java-util-of
  (is (= '(1 2 3)    (java.util.List/of 1 2 3)))
  (is (= #{1 2 3}    (java.util.Set/of 1 2 3)))
  (is (= {:a 1 :b 2} (java.util.Map/of :a 1 :b 2))))

(deftest jvm-statics-string-character
  (is (= "42"   (String/valueOf 42)))
  (is (= "abc"  (String/valueOf "abc")))
  (is (= "a"    (Character/toString \a))))

;; Embedded-host semantic remap: each Java surface routes to mino's
;; existing primitive. The Clojure-level contract is preserved.

(deftest jvm-host-remap-time
  (is (integer? (System/currentTimeMillis)))
  (is (integer? (System/nanoTime))))

(deftest jvm-host-remap-env
  ;; getenv on an unset variable returns nil per JVM canon.
  (is (nil? (System/getenv "ABSOLUTELY_NOT_SET_VARIABLE_XYZ"))))

(deftest jvm-host-remap-uuid
  (is (uuid? (java.util.UUID/randomUUID)))
  (let [u (java.util.UUID/randomUUID)]
    (is (= u (java.util.UUID/fromString (str u))))))

(deftest jvm-host-remap-thread-sleep
  ;; Should return without error after a near-zero sleep.
  (is (= nil (Thread/sleep 1))))

(deftest jvm-host-remap-get-property-throws
  ;; No JVM properties table in mino; surface :mino/unsupported.
  (is (thrown? (System/getProperty "user.home"))))
