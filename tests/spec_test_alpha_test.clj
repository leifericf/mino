(require "tests/test")

(require '[clojure.spec.alpha :as s])
(require '[clojure.spec.test.alpha :as stest])

;; ---------------------------------------------------------------------------
;; A correct fdef'd fn and a deliberately broken one.  myinc returns an
;; int; broken-inc claims an int :ret but returns a string, so a check
;; must find a counterexample.
;; ---------------------------------------------------------------------------

(defn myinc [n] (+ n 1))
(s/fdef myinc :args (s/cat :n int?) :ret int?)

(defn broken-inc [n] (str n))
(s/fdef broken-inc :args (s/cat :n int?) :ret int?)

;; ---------------------------------------------------------------------------
;; instrument / unstrument.
;; ---------------------------------------------------------------------------

(deftest instrument-catches-bad-arg
  (is (= 'user/myinc (stest/instrument 'user/myinc)))
  (try
    (is (= 6 (myinc 5)))
    (is (thrown? (myinc "not-an-int")))
    (finally
      (stest/unstrument 'user/myinc))))

(deftest unstrument-restores-original
  (stest/instrument 'user/myinc)
  (is (= 'user/myinc (stest/unstrument 'user/myinc)))
  ;; After unstrument the wrapper is gone, so a bad arg is no longer
  ;; caught by spec; the raw fn runs (and here +'s type error, not a
  ;; spec error, would surface).  A good arg still works.
  (is (= 3 (myinc 2))))

(deftest instrument-disabled-skips-check
  (stest/instrument 'user/myinc)
  (try
    (is (= 4 (stest/with-instrument-disabled (myinc 3))))
    (finally
      (stest/unstrument 'user/myinc))))

;; ---------------------------------------------------------------------------
;; check.
;; ---------------------------------------------------------------------------

(deftest check-passes-correct-fn
  (let [results (stest/check 'user/myinc)
        r       (first results)
        qc      (:clojure.spec.test.check/ret r)]
    (is (= 1 (count results)))
    (is (= 'user/myinc (:sym r)))
    (is (true? (:result qc)))))

(deftest check-reports-broken-ret
  (let [results (stest/check 'user/broken-inc)
        r       (first results)
        qc      (:clojure.spec.test.check/ret r)]
    (is (= 'user/broken-inc (:sym r)))
    (is (false? (:result qc)))
    (is (some? (:shrunk qc)))
    (is (contains? (:shrunk qc) :smallest))))

;; ---------------------------------------------------------------------------
;; Symbol enumeration.
;; ---------------------------------------------------------------------------

(deftest enumerate-namespace-finds-fdefs
  (let [syms (stest/enumerate-namespace 'user)]
    (is (contains? syms 'user/myinc))
    (is (contains? syms 'user/broken-inc))))

(deftest instrumentable-and-checkable-include-fdefs
  (is (contains? (stest/instrumentable-syms) 'user/myinc))
  (is (contains? (stest/checkable-syms) 'user/myinc)))

;; ---------------------------------------------------------------------------
;; Summary.
;; ---------------------------------------------------------------------------

(deftest summarize-counts-results
  (let [results (stest/check ['user/myinc 'user/broken-inc])
        summary (stest/summarize-results results)]
    (is (= 2 (:total summary)))
    (is (= 1 (:check-passed summary)))
    (is (= 1 (:check-failed summary)))))

(deftest abbrev-result-condenses
  (let [r   (first (stest/check 'user/myinc))
        abr (stest/abbrev-result r)]
    (is (= 'user/myinc (:sym abr)))
    (is (true? (:result abr)))))

(run-tests-and-exit)
