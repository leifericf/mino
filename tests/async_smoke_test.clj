(require "tests/test")
(require '[clojure.core.async :as a])

;; Minimal async conformance smoke. Heavier tests (alts!, buffers,
;; combinators, mult/pub, conformance, blocking, go-stress,
;; multi-arity timers) live in the mino-tests satellite repo. This
;; file pins the three core shapes -- one go, one alts!, one promise
;; -- so a regression that broke them at the unit level still trips
;; mino's release-gate without depending on the satellite suite.

(deftest async-go-smoke
  (testing "go returns a channel that yields the body's value"
    (let [ch (a/go 42)]
      (is (= 42 (a/<!! ch))))))

(deftest async-alts-smoke
  (testing "alts!! over two channels takes from the ready one"
    (let [a (a/chan 1) b (a/chan 1)]
      (a/>!! a :hello)
      (let [[v c] (a/alts!! [a b])]
        (is (= :hello v))
        (is (= a c))))))

(deftest async-promise-smoke
  (testing "promise + deliver round-trip"
    (let [p (promise)]
      (deliver p :ok)
      (is (= :ok @p)))))
