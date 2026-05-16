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

(deftest async-realized-cross-thread
  ;; Regression: thread-sleep used to hold state_lock for the full
  ;; sleep duration. A worker thread spawned by (future ...) couldn't
  ;; deliver into a promise during that window because every call
  ;; into mino_call needs state_lock too. realized? observed the
  ;; promise as still pending even after a generous sleep; @p
  ;; reproduced the actual delivery because deref yields the lock.
  ;; Fix: thread-sleep now wraps the nanosleep in
  ;; mino_yield_lock/mino_resume_lock so workers can make progress.
  (testing "realized? observes cross-thread delivery after thread-sleep"
    (let [p (promise)]
      (future (deliver p 42))
      (thread-sleep 200)
      (is (realized? p))
      (is (= 42 @p)))))
