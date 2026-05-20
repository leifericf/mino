(require "tests/test")

;; Reader-conditional :preserve round-trip.
;;
;; (read-string {:read-cond :preserve} src) returns a reader-conditional
;; record (a map carrying {:mino/reader-conditional true} meta). pr-str
;; on that value emits the original #?(:k v ...) shape so str → read →
;; pr-str → read is idempotent.

(deftest preserve-non-splicing-form
  (let [v (clojure.core/read-string {:read-cond :preserve}
                                    "#?(:mino 1 :clj 2)")]
    (is (reader-conditional? v))
    (is (= "#?(:mino 1 :clj 2)" (pr-str v)))
    (is (false? (boolean (:splicing? v))))
    (is (= '(:mino 1 :clj 2) (:form v)))))

(deftest preserve-splicing-form
  (let [v (clojure.core/read-string {:read-cond :preserve}
                                    "#?@(:mino [1 2 3])")]
    (is (reader-conditional? v))
    (is (= "#?@(:mino [1 2 3])" (pr-str v)))
    (is (true? (boolean (:splicing? v))))))

(deftest preserve-round-trip-idempotent
  (let [src "#?(:mino 1 :clj 2)"
        v1  (clojure.core/read-string {:read-cond :preserve} src)
        s1  (pr-str v1)
        v2  (clojure.core/read-string {:read-cond :preserve} s1)
        s2  (pr-str v2)]
    (is (= src s1))
    (is (= s1 s2))))

(deftest preserve-via-clojure-edn-read-string
  ;; clojure.edn/read-string defaults to :preserve in mino so EDN
  ;; readers transparently support reader-conditional values.
  (let [v (clojure.edn/read-string "#?(:mino 1 :clj 2)")]
    (is (reader-conditional? v))
    (is (= "#?(:mino 1 :clj 2)" (pr-str v)))))

(deftest reader-conditional-fn-builds-same-shape
  (let [v1 (reader-conditional '(:mino 1 :clj 2) false)
        v2 (clojure.core/read-string {:read-cond :preserve}
                                     "#?(:mino 1 :clj 2)")]
    (is (reader-conditional? v1))
    (is (= (:form v1) (:form v2)))
    (is (= (boolean (:splicing? v1)) (boolean (:splicing? v2))))))
