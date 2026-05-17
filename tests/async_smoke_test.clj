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

(deftest async-parking-ops-are-referable
  ;; Regression: clojure.core.async previously did not expose `>!` or
  ;; `<!` as vars (only the blocking `>!!` / `<!!`). Script code that
  ;; wrote `(:require [clojure.core.async :refer [<! >!]])` got a
  ;; require-time error before its (go ...) block could compile,
  ;; because :refer needs a resolvable var per name. They're now
  ;; defined as stub macros that throw when called outside a go, and
  ;; the go-transformer intercepts them by literal symbol before the
  ;; stub would expand. Real Clojure core.async ships the same stubs
  ;; for the same reason.
  (testing ":refer [<! >!] resolves; usage inside go still parks correctly"
    (let [ch (a/chan 1)]
      (a/>!! ch :inside)
      (is (= :inside (a/<!! (a/go (a/<! ch))))))))

(deftest async-deref-timed-promise
  ;; Regression: (deref ref ms timeout-val) returns timeout-val if a
  ;; blocking ref isn't realized within ms milliseconds. Routes
  ;; through mino_future_deref_timed which uses pthread_cond_timedwait
  ;; (Win32 SleepConditionVariableCS) under a yielded state_lock so
  ;; sibling workers can still deliver during the wait.
  (testing "3-arg deref returns timeout-val on pending promise"
    (let [p (promise)]
      (is (= :timeout (deref p 50 :timeout)))))
  (testing "3-arg deref returns the value if delivered in time"
    (let [p (promise)]
      (future (thread-sleep 30) (deliver p :ok))
      (is (= :ok (deref p 500 :timeout)))))
  (testing "3-arg deref poll (ms=0) on already-delivered promise"
    (let [p (promise)]
      (deliver p :delivered)
      (is (= :delivered (deref p 0 :timeout)))))
  (testing "3-arg deref returns result for a fast future"
    (let [f (future 42)]
      (is (= 42 (deref f 500 :timeout)))))
  (testing "3-arg deref times out on a slow future"
    (let [f (future (thread-sleep 200) 99)]
      (is (= :timeout (deref f 10 :timeout))))))

(deftest async-future-ex-info-data-preserved
  ;; Regression: when a future body throws (ex-info "..." {:k :v}),
  ;; (deref fut) inside a try/catch lost the :data payload. prim_throw
  ;; at try_depth==0 (inside the worker) routed through
  ;; prim_throw_classified, which drops the data field, and
  ;; mino_future_deref's rethrow path likewise rebuilt the exception
  ;; via prim_throw_classified rather than longjmp'ing the original
  ;; map. Both paths fixed: prim_throw now extracts :data (or :mino/data)
  ;; and uses set_eval_diag_with_data; mino_future_deref now longjmps
  ;; the captured map verbatim when try_depth>0.
  (testing "ex-info :data survives future -> deref -> catch"
    (let [f (future (throw (ex-info "boom" {:n 42 :tag :test})))
          result (try (deref f) (catch e e))
          data   (when (map? result) (get result :mino/data))]
      (is (map? result))
      (is (= "boom" (get result :mino/message)))
      (is (map? data))
      (is (= 42 (get data :n)))
      (is (= :test (get data :tag))))))
