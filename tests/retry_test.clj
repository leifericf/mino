(require "tests/test")

;; Retry with decorrelated jitter backoff.
;;
;; (mino.retry/with-retry opts body) retries body on thrown values.
;; Policy: max-tries, max-ms wall-clock budget, decorrelated jitter
;; backoff between attempts, retry? predicate to filter exceptions,
;; injectable sleep for deterministic tests.

(require '[mino.retry :as retry])

(defn- run-retry
  "Runs a thunk in a fn so catch Throwable syntax works; unwraps the
  mino :user envelope to get the original thrown value."
  [_opts thunk]
  (let [r (try (thunk) (catch Throwable e e))]
    (if (and (map? r) (= :user (:mino/kind r)) (contains? r :mino/data))
      (:mino/data r)
      r)))

;;;; Happy path: no retry needed

(deftest retry-succeeds-without-retrying
  (let [calls (atom 0)]
    (is (= 42 (retry/with-retry {}
                (swap! calls inc)
                42)))
    (is (= 1 @calls))))

;;;; Retry on error

(deftest retry-retries-on-thrown-value
  (let [calls (atom 0)]
    (is (= :ok
           (retry/with-retry {:max-tries 3 :sleep (constantly nil)}
             (swap! calls inc)
             (if (< @calls 3) (throw :transient) :ok))))
    (is (= 3 @calls))))

(deftest retry-rethrows-when-max-tries-exhausted
  (let [calls (atom 0)
        result (run-retry {:max-tries 2 :sleep (constantly nil)}
                          (fn []
                            (retry/with-retry {:max-tries 2 :sleep (constantly nil)}
                              (swap! calls inc)
                              (throw :boom))))]
    (is (= :boom result))
    (is (= 2 @calls))))

;;;; retry? predicate

(deftest retry-predicate-gates-retry
  ;; :retry? returning false means give up immediately on the first failure
  (let [calls (atom 0)
        result (run-retry {} (fn []
                               (retry/with-retry {:max-tries 5 :sleep (constantly nil)
                                                  :retry? (constantly false)}
                                 (swap! calls inc)
                                 (throw :nope))))]
    (is (= :nope result))
    ;; only one attempt despite max-tries 5
    (is (= 1 @calls))))

(deftest retry-predicate-matches-on-mino-kind
  ;; :retry? can inspect :mino/kind on the thrown map;
  ;; it retries :retryable errors and gives up on :give-up
  (let [calls (atom 0)
        result (run-retry {} (fn []
                               (retry/with-retry
                                 {:max-tries 4 :sleep (constantly nil)
                                  :retry? #(= :retryable (:mino/kind %))}
                                 (swap! calls inc)
                                 (if (< @calls 3)
                                   (throw {:mino/kind :retryable})
                                   (throw {:mino/kind :give-up})))))]
    (is (= :give-up (:mino/kind result)))
    ;; two retryable throws then the give-up on attempt 3
    (is (= 3 @calls))))

;;;; on-retry callback

(deftest retry-calls-on-retry-with-thrown-value
  (let [seen (atom [])
        _ (run-retry {} (fn []
                          (retry/with-retry {:max-tries 3 :sleep (constantly nil)
                                             :on-retry #(swap! seen conj %)}
                            (throw :err))))]
    ;; on-retry fires before each retry (attempts 2 and 3 trigger it)
    (is (= 2 (count @seen)))
    (is (every? #(= :err %) @seen))))

;;;; sleep injection for deterministic tests

(deftest retry-uses-injectable-sleep
  (let [slept (atom [])
        _ (run-retry {} (fn []
                          (retry/with-retry {:max-tries 3
                                             :sleep #(swap! slept conj %)}
                            (throw :boom))))]
    ;; two sleep calls (before retries 2 and 3)
    (is (= 2 (count @slept)))
    (is (every? number? @slept))))

;;;; max-ms wall-clock budget

(deftest retry-gives-up-when-max-ms-would-be-exceeded
  ;; With a very tight max-ms, the first retry delay would exceed the
  ;; budget, so we should give up after the first failure.
  (let [calls (atom 0)
        result (run-retry {} (fn []
                               (retry/with-retry {:max-tries 10 :max-ms 1
                                                  :initial-ms 100
                                                  :sleep (constantly nil)}
                                 (swap! calls inc)
                                 (throw :err))))]
    (is (= :err result))
    ;; only 1 attempt because the budget check fires before sleeping
    (is (= 1 @calls))))

;;;; Backoff bounds

(deftest retry-delay-stays-within-max-delay
  ;; Verify that sleep calls don't exceed max-delay-ms
  (let [slept (atom [])
        _ (run-retry {} (fn []
                          (retry/with-retry {:max-tries 5
                                             :initial-ms 100
                                             :max-delay-ms 200
                                             :sleep #(swap! slept conj %)}
                            (throw :err))))]
    (is (every? #(<= % 200) @slept))))

;;;; opts validation

(deftest retry-rejects-invalid-max-tries
  (let [r1 (try (retry/with-retry {:max-tries 0 :sleep (constantly nil)} :ok)
                (catch Throwable e e))
        r2 (try (retry/with-retry {:max-tries -1 :sleep (constantly nil)} :ok)
                (catch Throwable e e))]
    (is (= :retry/opts (:mino/kind r1)))
    (is (= :retry/opts (:mino/kind r2)))))

(deftest retry-rejects-invalid-initial-ms
  (let [r (try (retry/with-retry {:initial-ms -1 :sleep (constantly nil)} :ok)
               (catch Throwable e e))]
    (is (= :retry/opts (:mino/kind r)))))

(run-tests-and-exit)
