(require "tests/test")

;; Reader conditional tests

(deftest reader-cond-mino-branch
  (is (= 1 #?(:mino 1 :clj 2))))

(deftest reader-cond-clj-skipped
  ;; Mino is *not* a JVM dialect: :clj branches are skipped so JVM-only
  ;; assertions in cross-dialect tests (e.g., jank-lang/clojure-test-suite)
  ;; do not fire on mino. Cross-dialect code is expected to put a :default
  ;; branch as the catch-all for non-JVM runtimes.
  (is (= 99 #?(:clj 42 :default 99))))

(deftest reader-cond-default-when-no-active-key
  (is (= 99 #?(:cljs 42 :default 99))))

(deftest reader-cond-skipped-when-no-match
  (is (= 7 #?(:cljs 100 :default 7))))

(deftest reader-cond-mino-over-default
  (is (= :mino #?(:default :fallback :mino :mino))))

(deftest reader-cond-in-vector
  (is (= [1 2 3] [1 #?(:mino 2 :clj 99) 3])))

(deftest reader-cond-in-list
  (is (= 6 (+ #?(:mino 1 :clj 0) 2 3))))

(deftest reader-cond-splice-in-vector
  (is (= [1 2 3 4] [1 #?@(:mino [2 3] :clj [99]) 4])))

(deftest reader-cond-splice-in-list
  (is (= 10 (+ 1 #?@(:mino (2 3) :clj (0 0)) 4))))

(deftest reader-cond-splice-clj-skipped
  (is (= [1 4] [1 #?@(:clj [2 3]) 4])))

(deftest reader-cond-splice-skipped-when-no-match
  (is (= [1 4] [1 #?@(:cljs [2 3]) 4])))

(deftest reader-cond-nested
  (is (= :yes #?(:mino #?(:mino :yes :default :no) :default :nope))))

(deftest reader-cond-empty-after-deref-is-diagnosable
  ;; `@#?(:clj … :cljs …)` resolves to a bare `@` on mino's dialect.
  ;; The bare diagnostic ("expected form after @") doesn't name the
  ;; reader-conditional cause. The reader should mention it.
  (let [err (try (read-string "@#?(:clj :x :cljs :y)") nil
                 (catch e (if (map? e) (:mino/message e) (str e))))]
    (is (some? err))
    (is (some? (re-find #"reader conditional" err)))
    (is (some? (re-find #":mino" err)))))

(deftest reader-cond-empty-after-quote-is-diagnosable
  (let [err (try (read-string "'#?(:clj :x :cljs :y)") nil
                 (catch e (if (map? e) (:mino/message e) (str e))))]
    (is (some? err))
    (is (some? (re-find #"reader conditional" err)))))

(deftest reader-cond-empty-after-syntax-quote-is-diagnosable
  (let [err (try (read-string "`#?(:clj :x :cljs :y)") nil
                 (catch e (if (map? e) (:mino/message e) (str e))))]
    (is (some? err))
    (is (some? (re-find #"reader conditional" err)))))

(deftest reader-cond-empty-after-unquote-is-diagnosable
  (let [err (try (read-string "`(foo ~#?(:clj :x :cljs :y))") nil
                 (catch e (if (map? e) (:mino/message e) (str e))))]
    (is (some? err))
    (is (some? (re-find #"reader conditional" err)))))

(deftest reader-cond-empty-value-in-namespaced-map
  ;; In a plain map literal a no-match reader conditional in the value
  ;; slot drops the paired key entirely; the namespaced-map reader must
  ;; honor the same shape (and not silently corrupt the surrounding form).
  (is (= {} (read-string "#:foo{:a #?(:clj 1)}")))
  ;; Embedded in a vector: the parent reader keeps reading after the
  ;; closing brace -- it used to surface as "unexpected ')'" because the
  ;; namespaced-map reader bailed without consuming its own '}'.
  (is (= [1 {} 3] (read-string "[1 #:foo{:a #?(:clj 1)} 3]")))
  ;; Value matched -- the namespaced prefix qualifies the key.
  (is (= {:foo/a 99} (read-string "#:foo{:a #?(:default 99)}")))
  ;; Key dropped by the conditional: paired value is discarded too.
  (is (= {} (read-string "#:foo{#?(:clj :a) 1}")))
  ;; Mixed: one entry survives, one drops.
  (is (= {:foo/a 1} (read-string "#:foo{:a 1 :b #?(:clj 2)}"))))

(deftest reader-cond-empty-element-in-set
  ;; Same shape, in a set literal: the no-match element is dropped, the
  ;; surrounding form is not corrupted.
  (is (= #{1 3} (read-string "#{1 #?(:clj 2) 3}")))
  (is (= #{1 2 3} (read-string "#{1 #?(:default 2) 3}")))
  (is (= #{} (read-string "#{#?(:clj 1) #?(:cljs 2)}"))))
