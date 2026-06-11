(require "tests/test")

;; Safety tests for value constructors: overflow guards and null-return
;; paths in mino_keyword_ns_n / mino_symbol_ns_n / intern table growth.
;;
;; The overflow paths require inputs near SIZE_MAX/2 which are not
;; constructible in a language-level test; those code paths are confirmed
;; structurally and covered by the embedder ABI tests (tests/embed_*.c).
;; The tests here exercise the large-string heap path (>= 256 combined
;; bytes) so the malloc branch runs and is exercised for correct output.

(deftest values-safety-keyword-large-ns-name
  ;; Combined ns+name >= 256 bytes forces the heap-allocated path in
  ;; mino_keyword_ns_n.  The result must be a valid namespaced keyword.
  ;; (Before the malloc-null fix, a NULL buf was immediately dereferenced;
  ;; under ASAN this segfaults on an OOM injection.)
  (let [ns   (apply str (repeat 130 "a"))   ; 130 chars
        name (apply str (repeat 130 "b"))   ; 130 chars, total 261 > 255
        kw   (keyword ns name)]
    (is (keyword? kw))
    (is (= ns   (namespace kw)))
    (is (= name (clojure.core/name kw)))))

(deftest values-safety-symbol-large-ns-name
  ;; Same path in mino_symbol_ns_n.
  (let [ns   (apply str (repeat 130 "c"))
        name (apply str (repeat 130 "d"))
        sym  (symbol ns name)]
    (is (symbol? sym))
    (is (= ns   (namespace sym)))
    (is (= name (clojure.core/name sym)))))

(deftest values-safety-keyword-roundtrip-normal
  ;; Normal-sized keywords still work after the overflow guard was added.
  (is (= :foo/bar   (keyword "foo" "bar")))
  (is (= "foo"      (namespace :foo/bar)))
  (is (= "bar"      (clojure.core/name :foo/bar)))
  (is (= :hello     (keyword nil "hello")))
  (is (= nil        (namespace :hello))))

(deftest values-safety-symbol-roundtrip-normal
  ;; Normal-sized symbols still work.
  (is (= 'foo/bar   (symbol "foo" "bar")))
  (is (= "foo"      (namespace 'foo/bar)))
  (is (= "bar"      (clojure.core/name 'foo/bar)))
  (is (= 'hello     (symbol nil "hello")))
  (is (= nil        (namespace 'hello))))

(deftest values-safety-intern-table-many-keywords
  ;; Exercise intern-table doubling across multiple resizes.  With 1000
  ;; distinct keywords the entries[] array doubles from 64 to 1024 and
  ;; the ht_buckets array rehashes at least four times.  After the
  ;; overflow-guard additions, each doubling must still produce a
  ;; correctly functioning table (no lost entries, no stale look-ups).
  (let [n     1000
        kws   (mapv (fn [i] (keyword (str "intern-safety-kw-" i))) (range n))
        kw-set (set kws)]
    (is (= n (count kw-set)))
    ;; Spot-check first, middle, last.
    (is (contains? kw-set :intern-safety-kw-0))
    (is (contains? kw-set :intern-safety-kw-499))
    (is (contains? kw-set :intern-safety-kw-999))
    ;; Interning is idempotent: same input returns the same object.
    (is (identical? (keyword "intern-safety-kw-0")
                    (keyword "intern-safety-kw-0")))))

(run-tests-and-exit)
