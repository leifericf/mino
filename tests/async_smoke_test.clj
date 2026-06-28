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
    (let [p (promise)
          f (future (deliver p 42))]
      (thread-sleep 200)
      (is (realized? p))
      (is (= 42 @p))
      ;; deref the worker so it has fully returned before the next test
      ;; runs (avoids leaking a PENDING future into the save-image
      ;; quiesce check).
      @f)))

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
    (let [p (promise)
          f (future (thread-sleep 30) (deliver p :ok))]
      (is (= :ok (deref p 500 :timeout)))
      ;; deref the worker so it has fully returned (avoids leaking a
      ;; PENDING future into the save-image quiesce check).
      @f))
  (testing "3-arg deref poll (ms=0) on already-delivered promise"
    (let [p (promise)]
      (deliver p :delivered)
      (is (= :delivered (deref p 0 :timeout)))))
  (testing "3-arg deref returns result for a fast future"
    (let [f (future 42)]
      (is (= 42 (deref f 500 :timeout)))))
  (testing "3-arg deref times out on a slow future"
    (let [f (future (thread-sleep 200) 99)]
      (is (= :timeout (deref f 10 :timeout)))
      ;; Cleanup: wait for the slow future to resolve so it doesn't
      ;; leak a pending worker into subsequent tests (notably the
      ;; save-image quiesce check).
      (is (= 99 (deref f 500 :timeout))))))

(deftest async-concurrent-fn-yield-preserves-args
  ;; Regression: two workers calling the same fn that yields state_lock
  ;; (e.g. via thread-sleep) previously corrupted each other's argument
  ;; slots. Root cause: S->bc_regs / S->bc_top were per-state, so the
  ;; second worker's bc_push grew the stack above the first worker's
  ;; frame, and the first worker's bc_pop dropped bc_top below the
  ;; second worker's frame, NULLing the second worker's slots before
  ;; it could read them back on resume.
  ;;
  ;; Fix: mino_lock/mino_unlock snapshot the BC register stack into the
  ;; current ctx on outermost transitions; mino_yield_lock/resume_lock
  ;; do the same across the yield window. Each ctx owns its own bc_regs
  ;; pointer / cap / top in its bc_*_storage fields, and the GC root
  ;; walker visits every yielded ctx's snapshot so a peer's allocation
  ;; pressure can't collect another worker's still-live values.
  (testing "concurrent (fn [x] (thread-sleep) x) calls return distinct args"
    (let [f  (fn [x] (thread-sleep 5) x)
          f1 (future-call (fn [] (f 1)))
          f2 (future-call (fn [] (f 2)))]
      (is (= [1 2] [@f1 @f2]))))
  (testing "pmap over a yielding f returns one result per input"
    (let [results (pmap (fn [x] (thread-sleep 2) (* x x)) (range 1 6))]
      (is (= [1 4 9 16 25] (vec results))))))

(deftest async-future-cancel-interrupts-cpu-bound
  ;; Regression: future-cancel previously only flipped the impl's
  ;; CANCELLED tag and broadcast its cv. A CPU-bound worker running
  ;; a tight (loop ... recur) had no enclosing cv_wait, so the cancel
  ;; was invisible to the worker and the subsequent state_destroy
  ;; hung forever waiting on pthread_join.
  ;;
  ;; Fix: BC VM safepoint poll at every backward jump reads through
  ;; a TLS pointer to the worker's owning impl->cancel_flag and throws
  ;; :mino/cancelled when set. The worker unwinds, worker_run
  ;; publishes CANCELLED, quiesce's join completes.
  (testing "future-cancel breaks a tight recur loop and lets script exit"
    (let [f (future (loop [i 0] (recur (inc i))))]
      (thread-sleep 50)
      (future-cancel f)
      ;; Give the worker a moment to observe the cancel, unwind, and
      ;; release its thread_count slot. CI runners with tight CPU
      ;; budgets fail the next test's spawn otherwise.
      (thread-sleep 200)
      (is (future-cancelled? f)))))

(deftest async-busy-spin-does-not-starve-siblings
  ;; Regression: a worker future running a busy spin without explicit
  ;; yield monopolized state_lock, so sibling workers couldn't make
  ;; progress and the script deadlocked. The BC VM safepoint now
  ;; auto-yields state_lock periodically (~64K backward jumps) so
  ;; siblings get scheduling time.
  (testing "busy-spin reader doesn't block writer futures from delivering"
    ;; Adapt n to the host's thread grant. Reserve one slot for the
    ;; reader future; on a 2-CPU GHA runner that leaves 1 writer
    ;; (still enough to exercise the auto-yield path). Higher-budget
    ;; hosts spawn more siblings, up to 3.
    (let [n       (max 1 (min 3 (- (mino-thread-limit) 1)))
          ps      (vec (repeatedly n promise))
          writers (vec (for [i (range n)]
                         (future (dotimes [_ 200] :work)
                                 (deliver (nth ps i) :done))))
          reader  (future
                    (loop [it 0]
                      (if (every? realized? ps)
                        :done
                        (recur (inc it)))))]
      (doseq [p ps] (is (= :done @p)))
      (is (= :done @reader))
      ;; Deref each writer so its body has returned (publish complete),
      ;; then sleep briefly so the worker thread can finish its post-
      ;; publish cleanup and release its thread_count slot. Without
      ;; this two-step, CI runners with tight CPU budgets can hit
      ;; MTH001 on the following test's spawn even though the test
      ;; itself only spawns one future -- the prior workers are still
      ;; in the worker_run cleanup path with their slots reserved.
      (doseq [w writers] (deref w))
      (thread-sleep 200))))

(deftest async-future-ex-info-data-preserved
  ;; When a future body throws (ex-info "..." {:k :v}), deref inside a
  ;; try/catch must preserve both the message and the data payload, the
  ;; same way a main-thread catch does. The worker runs the thunk under
  ;; a protected call, so the throw normalizes through the same path as
  ;; any other catch: (ex-data caught) returns the original data map.
  (testing "ex-info :data survives future -> deref -> catch"
    (let [f (future (throw (ex-info "boom" {:n 42 :tag :test})))
          result (try (deref f) (catch e e))
          data   (ex-data result)]
      (is (map? result))
      (is (= "boom" (get result :mino/message)))
      (is (map? data))
      (is (= 42 (get data :n)))
      (is (= :test (get data :tag))))))

(defn- note-arg
  "Record k on the order atom and return v."
  [order k v]
  (swap! order conj k)
  v)

(deftest go-parks-in-argument-position
  (testing "take in function argument position"
    (let [ch (a/chan 1)]
      (a/>!! ch 41)
      (is (= 42 (a/<!! (a/go (inc (a/<! ch))))))))
  (testing "sibling takes as arguments evaluate left to right"
    (let [c1 (a/chan 1) c2 (a/chan 1)]
      (a/>!! c1 40)
      (a/>!! c2 2)
      (is (= 42 (a/<!! (a/go (+ (a/<! c1) (a/<! c2))))))))
  (testing "park nested two calls deep"
    (let [ch (a/chan 1)]
      (a/>!! ch 20)
      (is (= 42 (a/<!! (a/go (+ 2 (* 2 (a/<! ch)))))))))
  (testing "take in put value position"
    (let [src (a/chan 1) dst (a/chan 1)]
      (a/>!! src 42)
      (a/<!! (a/go (a/>! dst (a/<! src))))
      (is (= 42 (a/<!! dst)))))
  (testing "take inside the if test"
    (let [ch (a/chan 1)]
      (a/>!! ch 4)
      (is (= :even (a/<!! (a/go (if (even? (a/<! ch)) :even :odd)))))))
  (testing "take inside a let binding computation"
    (let [ch (a/chan 1)]
      (a/>!! ch 20)
      (is (= 42 (a/<!! (a/go (let [x (+ 1 (a/<! ch))] (* 2 x))))))))
  (testing "take inside a vector literal argument"
    (let [ch (a/chan 1)]
      (a/>!! ch 1)
      (is (= 2 (a/<!! (a/go (count [(a/<! ch) 2])))))))
  (testing "argument evaluation order is preserved around parks"
    (let [ch    (a/chan 1)
          order (atom [])]
      (a/>!! ch 1)
      (is (= 43 (a/<!! (a/go (+ (note-arg order :a 40)
                                (+ (note-arg order :b 2) (a/<! ch)))))))
      (is (= [:a :b] @order)))))

(deftest timeout-channels-wake-blocking-takes
  (testing "blocking take on a timeout channel returns nil at deadline"
    (is (nil? (a/<!! (a/timeout 30)))))
  (testing "go block parked on a timeout completes"
    (is (= :timed (a/<!! (a/go (a/<! (a/timeout 30)) :timed)))))
  (testing "alts!! with a timeout channel selects it at deadline"
    (let [t (a/timeout 30)
          [v ch] (a/alts!! [(a/chan) t])]
      (is (nil? v))
      (is (= t ch)))))
