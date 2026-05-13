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

(deftest future-thread-count-not-stuck-under-tight-loop
  ;; Regression: pre-v0.103.0, the worker entry-link and exit-detach
  ;; took the recursive state_lock. A tight embedder loop holding
  ;; state_lock across a single form (e.g. a dotimes wrapper around
  ;; many dosyncs) could starve workers at both ends, leaving
  ;; thread_count inflated even after the workers' bodies had
  ;; finished. v0.103.0 moves this bookkeeping onto worker_list_lock,
  ;; so workers attach + detach concurrently with the embedder loop.
  (when (> (mino-thread-limit) 1)
    (let [n    (min 4 (max 1 (dec (mino-thread-limit))))
          futs (doall (for [_ (range n)] (future :done)))]
      ;; Tight loop: 200 trivial dosyncs on the main thread. The
      ;; whole dotimes runs as one form, so state_lock is held
      ;; continuously across all 200 iterations.
      (dotimes [_ 200] (dosync))
      ;; Confirm the workers' bodies completed.
      (doseq [f futs] (is (= :done @f)))
      ;; Bounded poll: with the lock split in place, exit-detach
      ;; happens off state_lock so this converges immediately.
      ;; Without the split, workers could still be stalled.
      (loop [iters 200]
        (when (and (pos? (mino-thread-count)) (pos? iters))
          (recur (dec iters))))
      (is (= 0 (mino-thread-count))))))

(deftest future-deref-preserves-thrown-string-message
  ;; A worker thunk that throws a string used to surface on the
  ;; consumer side as the generic "future failed" -- the original
  ;; message was lost in the BC VM's no-try path, and the deref
  ;; rethrow did not propagate any worker diagnostic. Both fixed
  ;; together: the consumer now sees the worker's "unhandled
  ;; exception: <thrown>" so the cause is greppable.
  (let [f (future (throw "original-msg"))
        err (try @f nil
                 (catch e (if (map? e) (:mino/message e) (str e))))]
    (is (some? err))
    (is (some? (re-find #"original-msg" err)))))

(deftest future-deref-preserves-thrown-map-message
  (let [f (future (throw (ex-info "boom" {:n 42})))
        err (try @f nil
                 (catch e (if (map? e) (:mino/message e) (str e))))]
    (is (some? err))
    (is (some? (re-find #"boom" err)))))

(deftest bc-throw-without-try-names-the-value
  ;; Same shape, single-threaded: the bytecode VM's no-try path
  ;; used to emit a bare "unhandled exception (no try)" with no
  ;; mention of the actual thrown value. Mirror the tree-walker
  ;; behaviour so the value is in the message.
  (let [err (try
              ;; Wrapped in an eval so the throw goes through the bc
              ;; vm's no-enclosing-try path even though the outer try
              ;; would otherwise catch it under the same compilation.
              (eval '(throw "bc-direct-throw"))
              nil
              (catch e (if (map? e) (:mino/message e) (str e))))]
    (is (some? err))
    (is (some? (re-find #"bc-direct-throw" err)))))

(deftest cancel-of-promise-unblocks-future-deref-on-it
  ;; A worker deref'ing an undelivered promise parks in cv_wait on
  ;; the promise's cv. Cancelling the promise must wake the worker
  ;; so its deref returns with "future was cancelled" -- without
  ;; this, the embedder side of state_free would block on
  ;; pthread_join forever because the worker thread never returns.
  (when (> (mino-thread-limit) 1)
    (let [gate (promise)
          f    (future @gate :unreached)]
      (is (true? (future-cancel gate)))
      (let [err (try @f nil
                     (catch e (if (map? e) (:mino/message e) (str e))))]
        (is (some? err))
        (is (some? (re-find #"cancelled" err)))))))

(run-tests-and-exit)
