(ns mino.retry
  "Retry with decorrelated jitter backoff.

  (require '[mino.retry :as retry])
  (retry/with-retry {:max-tries 5 :initial-ms 100}
    (http-get url))

  Retries a body expression when it throws. The default policy catches
  every thrown value; supply :retry? to narrow the gate.

  Options (keyword map):
    :max-tries   max number of attempts (default 3; must be >= 1)
    :max-ms      wall-clock budget in ms across all attempts (no default;
                 nil means no wall-clock limit)
    :initial-ms  first backoff delay in ms (default 100)
    :max-delay-ms cap on any single delay (default 30000)
    :retry?      one-arg predicate: given the thrown value, return true
                 to retry (default: retry on any thrown value)
    :on-retry    one-arg callback called with the thrown value before each
                 retry sleep (default: no callback)
    :sleep       one-arg fn called with ms to sleep (default thread-sleep;
                 injectable for deterministic tests)

  Backoff: decorrelated jitter per Bry-Baugh 2014 — each sleep is
  uniform in [initial-ms, min(max-delay-ms, prev * 3)]. The seed is
  the initial-ms, so the bounds are always well-defined regardless of
  prior history.

  Give-up: when max-tries is exhausted (or max-ms would be exceeded),
  the last thrown value is rethrown as-is. If the body never throws,
  the body's return value is returned.")

(require '[clojure.string :as str])

(defn retry-fail
  "Throws a classified mino.retry diagnostic."
  [msg data]
  (throw {:mino/kind :retry/opts
          :mino/code "MRT001"
          :mino/message msg
          :mino/data data}))

(defn validate-retry-opts
  "Validates the with-retry opts map; throws :mino/kind :retry/opts on
  any invalid value. Public so the macro can call it after expansion."
  [{:keys [max-tries initial-ms max-delay-ms max-ms]}]
  (when (and max-tries (or (not (integer? max-tries)) (< max-tries 1)))
    (retry-fail ":max-tries must be a positive integer" {:max-tries max-tries}))
  (when (and initial-ms (or (not (number? initial-ms)) (< initial-ms 0)))
    (retry-fail ":initial-ms must be a non-negative number" {:initial-ms initial-ms}))
  (when (and max-delay-ms (or (not (number? max-delay-ms)) (< max-delay-ms 0)))
    (retry-fail ":max-delay-ms must be a non-negative number" {:max-delay-ms max-delay-ms}))
  (when (and max-ms (or (not (number? max-ms)) (< max-ms 0)))
    (retry-fail ":max-ms must be a non-negative number" {:max-ms max-ms})))

;;;; Backoff math (pure)

(defn- next-delay-ms
  "Decorrelated jitter: uniform in [initial-ms, min(max-delay-ms, prev*3)]."
  [initial-ms max-delay-ms prev-ms]
  (let [lo initial-ms
        hi (min max-delay-ms (* prev-ms 3.0))
        hi (max lo hi)]
    (+ lo (* (rand) (- hi lo)))))

(defn- budget-exhausted?
  "True when sleeping sleep-ms would exceed the remaining budget."
  [start-ms max-ms sleep-ms]
  (when max-ms
    (> (+ (- (time-ms) start-ms) sleep-ms) max-ms)))

;;;; Core retry logic (function, not macro)

(defn- unwrap-mino-err
  "Extracts the original thrown value from the mino exception envelope.
  When mino wraps a non-map throw (e.g. (throw :kw)) it sets
  :mino/kind :user and :mino/data to the original value. When the
  throw was already a data map (e.g. (throw {:mino/kind :foo})) the
  caught value is the map itself. Return the most-specific value a
  retry? predicate should see."
  [ex]
  (if (and (map? ex) (= :user (:mino/kind ex)) (contains? ex :mino/data))
    (:mino/data ex)
    ex))

(defn retry*
  "Calls thunk, retrying per the validated opts map. Used by with-retry."
  [opts thunk]
  (let [max-tries  (get opts :max-tries 3)
        max-ms     (get opts :max-ms nil)
        initial-ms (double (get opts :initial-ms 100))
        max-delay  (double (get opts :max-delay-ms 30000))
        retry?     (get opts :retry? (constantly true))
        on-retry   (get opts :on-retry nil)
        sleep-fn   (get opts :sleep thread-sleep)
        start-ms   (time-ms)]
    (loop [attempt    1
           prev-delay initial-ms]
      (let [result (try {:ok (thunk)} (catch Exception ex {:err ex}))]
        (if-not (contains? result :err)
          (:ok result)
          (let [raw-err (:err result)
                err     (unwrap-mino-err raw-err)]
            (if (and (< attempt max-tries)
                     (retry? err))
              (let [delay (if (= attempt 1)
                             initial-ms
                             (next-delay-ms initial-ms max-delay prev-delay))
                    delay (min delay max-delay)]
                (if (budget-exhausted? start-ms max-ms delay)
                  (throw err)
                  (do
                    (when on-retry (on-retry err))
                    (sleep-fn (long delay))
                    (recur (inc attempt) delay))))
              (throw err))))))))

;;;; Public macro

(defmacro with-retry
  "Evaluates body, retrying on thrown values per opts. See ns docstring
  for option details. The body can be multiple forms."
  [opts & body]
  `(let [opts# (or ~opts {})]
     (mino.retry/validate-retry-opts opts#)
     (mino.retry/retry* opts# (fn [] ~@body))))
