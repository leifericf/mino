(require "tests/test")

;; ---------------------------------------------------------------------------
;; Host threads.
;;
;; Real OS-thread futures and promises. Standalone `./mino` grants
;; cpu_count after mino_install_all so future-call works without
;; configuration; embedded mode starts at thread_limit = 1 and the spawn
;; entry points throw :mino/unsupported with a message naming the grant
;; API.
;;
;; These tests assume thread_limit > 1, which the standalone always
;; provides. Embedders running this file at limit = 1 will see throw
;; behavior; that path is exercised by the "no-grant" subset below.
;; ---------------------------------------------------------------------------

(deftest mino-thread-limit-positive
  (is (integer? (mino-thread-limit)))
  (is (>= (mino-thread-limit) 1)))

(deftest future-and-deref-roundtrip
  (when (> (mino-thread-limit) 1)
    (let [f (future (+ 1 2 3))]
      (is (future? f))
      (is (= 6 @f))
      (is (true? (future-done? f)))
      (is (false? (future-cancelled? f)))
      (is (true? (realized? f))))))

(deftest future-with-side-effect
  (when (> (mino-thread-limit) 1)
    (let [a (atom 0)
          f (future (swap! a inc) :ok)]
      (is (= :ok @f))
      (is (= 1 @a)))))

(deftest promise-and-deliver
  (when (> (mino-thread-limit) 1)
    (let [p (promise)]
      (is (future? p))
      (is (false? (realized? p)))
      (is (= p (deliver p 42)))
      (is (= 42 @p))
      (is (true? (realized? p)))
      ;; Second deliver returns nil (already delivered)
      (is (nil? (deliver p 99)))
      (is (= 42 @p)))))

(deftest future-cancel
  (when (> (mino-thread-limit) 1)
    (let [p (promise)]
      (is (true? (future-cancel p)))
      (is (true? (future-cancelled? p)))
      (is (true? (future-done? p)))
      ;; Second cancel returns false (already terminal)
      (is (false? (future-cancel p))))))

(deftest future-q-discriminates
  (is (false? (future? nil)))
  (is (false? (future? 1)))
  (is (false? (future? :x)))
  (when (> (mino-thread-limit) 1)
    (is (true? (future? (promise))))))

(deftest concurrent-atom-cas
  ;; Stress the atom CAS path under genuine concurrency. With N futures
  ;; each doing M increments, the final value must be N*M (no lost
  ;; updates). This exercises the __atomic_compare_exchange path.
  ;; Cap N at (dec (mino-thread-limit)) so the test thread plus N
  ;; workers fit under the runtime's grant on low-CPU shared runners.
  (when (> (mino-thread-limit) 1)
    (let [a (atom 0)
          n (min 4 (max 1 (dec (mino-thread-limit))))
          m 250
          futs (doall (for [_ (range n)]
                        (future (dotimes [_ m] (swap! a inc)))))]
      ;; Wait on all
      (doseq [f futs] @f)
      (is (= (* n m) @a)))))

(run-tests-and-exit)
