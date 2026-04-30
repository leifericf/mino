;; clojure.core.async — CSP channels, buffers, alts arbitration, and the
;; go macro state machine. One namespace, two halves: the channel
;; mechanics first, the high-level combinators after.
;;
;; Channel state lives inside an atom holding a map with keys:
;;
;;   :kind         -- :chan (marker used by chan?)
;;   :buf-kind     -- :fixed | :dropping | :sliding | :promise | nil
;;   :buf-capacity -- positive integer (1 for promise, 0 for unbuffered)
;;   :buf-items    -- vector of buffered values (non-promise)
;;   :promise-set? -- true once the promise latch has received a value
;;   :promise-val  -- latched value
;;   :takers       -- vector of pending take ops
;;   :putters      -- vector of pending put ops
;;   :closed?      -- true once close! has run
;;   :xform        -- transducer reducing function (or nil)
;;   :ex-handler   -- exception handler called when xform throws (or nil)
;;
;; A pending op is a map:
;;
;;   :val       -- value to put (nil for takes)
;;   :callback  -- fn to invoke with the op result (or nil)
;;   :flag      -- atom shared across alts siblings (nil for regular ops)
;;   :ch        -- channel handle (included in alts callback result)
;;
;; An alts flag is (atom :pending); committing is (reset! flag :committed).
;; Scheduling of pending callbacks goes through the C scheduler via
;; `async-sched-enqueue*`. Timers (`timeout*`) and the drain loop
;; (`drain!`, `drain-loop!`) stay in C so host embedders and timer
;; callbacks share a single run queue with the mino-level code.

(ns clojure.core.async
  (:refer-clojure :exclude [merge into reduce transduce partition-by]))

(def ^:private MAX-PENDING 1024)

;; ---------------------------------------------------------------------
;; Scheduling
;; ---------------------------------------------------------------------

(defn- schedule! [cb val]
  (when cb
    (async-sched-enqueue* cb val)))

(defn- schedule-op! [op val]
  (when-let [cb (:callback op)]
    (if (:flag op)
      (async-sched-enqueue* cb [val (:ch op)])
      (async-sched-enqueue* cb val))))

(defn- flag-active? [op]
  (let [f (:flag op)]
    (or (nil? f) (= :pending @f))))

(defn- try-commit! [op]
  (if-let [f (:flag op)]
    (if (= :pending @f)
      (do (reset! f :committed) true)
      false)
    true))

;; ---------------------------------------------------------------------
;; Buffer helpers (operate on the state map; no atom involved)
;; ---------------------------------------------------------------------

(defn- buf-count [state]
  (case (:buf-kind state)
    nil      0
    :promise (if (:promise-set? state) 1 0)
    (count (:buf-items state))))

(defn- buf-full? [state]
  (case (:buf-kind state)
    nil      true
    :fixed   (>= (count (:buf-items state)) (:buf-capacity state))
    :dropping false
    :sliding  false
    :promise (:promise-set? state)))

(defn- buf-add [state val]
  (case (:buf-kind state)
    :fixed
    (update state :buf-items conj val)

    :dropping
    (if (>= (count (:buf-items state)) (:buf-capacity state))
      state
      (update state :buf-items conj val))

    :sliding
    (let [items (:buf-items state)
          cap   (:buf-capacity state)]
      (if (>= (count items) cap)
        (assoc state :buf-items (conj (subvec items 1) val))
        (assoc state :buf-items (conj items val))))

    :promise
    (if (:promise-set? state)
      state
      (assoc state :promise-set? true :promise-val val))))

(defn- buf-remove [state]
  (case (:buf-kind state)
    :promise
    [(:promise-val state) state]
    (let [items (:buf-items state)]
      [(first items) (assoc state :buf-items (subvec items 1))])))

(defn- drop-committed
  "Trim leading ops whose alts flag is already committed."
  [ops]
  (loop [ops ops]
    (if (and (seq ops) (not (flag-active? (first ops))))
      (recur (subvec ops 1))
      ops)))

;; ---------------------------------------------------------------------
;; Predicates and constructors
;; ---------------------------------------------------------------------

(defn- chan-state? [x]
  (and (map? x) (= :chan (:kind x))))

(defn chan?
  "True if x is a channel."
  [x]
  (and (atom? x) (chan-state? @x)))

(defn buffer
  "Fixed-size buffer descriptor. Pairs with chan."
  [n]
  {:kind :buf :buf-kind :fixed :capacity n})

(defn dropping-buffer
  "Dropping buffer descriptor. New values are silently dropped once full."
  [n]
  {:kind :buf :buf-kind :dropping :capacity n})

(defn sliding-buffer
  "Sliding buffer descriptor. Oldest value is evicted when full."
  [n]
  {:kind :buf :buf-kind :sliding :capacity n})

(defn- buffer? [x]
  (and (map? x) (= :buf (:kind x))))

(defn- new-chan-state [buf-kind buf-cap xform ex-handler]
  {:kind         :chan
   :buf-kind     buf-kind
   :buf-capacity buf-cap
   :buf-items    []
   :promise-set? false
   :promise-val  nil
   :takers       []
   :putters      []
   :closed?      false
   :xform        xform
   :ex-handler   ex-handler})

(defn- install-xform!
  "Attach a transducer to a channel. The reducing step is wired to write
   results into the channel's buffer."
  [ch xform ex-handler]
  (let [add-fn (fn
                 ([result] result)
                 ([result input]
                  (swap! ch (fn [state] (buf-add state input)))
                  result))
        rf     (xform add-fn)]
    (swap! ch assoc :xform rf :ex-handler ex-handler)
    nil))

(defn chan
  "Create a channel.
     ()                           -- unbuffered
     (buf-or-n)                   -- buffered (int or buffer descriptor)
     (buf-or-n xform)             -- buffered with a transducer
     (buf-or-n xform ex-handler)  -- also with an exception handler"
  ([] (chan nil nil nil))
  ([buf-or-n] (chan buf-or-n nil nil))
  ([buf-or-n xform] (chan buf-or-n xform nil))
  ([buf-or-n xform ex-handler]
   (let [[kind cap]
         (cond
           (nil? buf-or-n)                       [nil 0]
           (and (integer? buf-or-n)
                (zero? buf-or-n))                [nil 0]
           (integer? buf-or-n)                   [:fixed buf-or-n]
           (buffer? buf-or-n)                    [(:buf-kind buf-or-n)
                                                  (:capacity buf-or-n)]
           :else
           (throw (str "chan: buffer-or-n must be nil, a non-negative "
                       "integer, or a buffer")))
         ch (atom (new-chan-state kind cap nil nil))]
     (when xform
       (install-xform! ch xform ex-handler))
     ch)))

(defn promise-chan
  "Create a promise channel. The first value latches; all takers see it."
  ([] (promise-chan nil nil))
  ([xform] (promise-chan xform nil))
  ([xform ex-handler]
   (let [ch (atom (new-chan-state :promise 1 nil nil))]
     (when xform
       (install-xform! ch xform ex-handler))
     ch)))

(defn closed?
  "True if the channel is closed."
  [ch]
  (:closed? @ch))

;; ---------------------------------------------------------------------
;; Buffer → takers flushing
;; ---------------------------------------------------------------------

(defn- flush-buf-to-takers
  "Move buffered values into waiting takers. Returns [new-state pairs]
   where pairs is [[op val] ...]; caller commits flags and schedules."
  [state]
  (loop [st state pairs []]
    (let [ts (drop-committed (:takers st))]
      (if (and (pos? (buf-count st)) (seq ts))
        (let [[v st'] (buf-remove st)
              taker   (first ts)
              rest-t  (subvec ts 1)]
          (recur (assoc st' :takers rest-t)
                 (conj pairs [taker v])))
        [(assoc st :takers ts) pairs]))))

(defn- commit-and-schedule-pairs! [pairs]
  (doseq [[op v] pairs]
    (try-commit! op)
    (schedule-op! op v)))

;; ---------------------------------------------------------------------
;; Transducer step on a channel
;; ---------------------------------------------------------------------

(defn- run-xform-step!
  "Invoke the channel's xform step with val, then flush newly buffered
   values to waiting takers. Handles the exception path via ex-handler
   and closes the channel if the step returned (reduced ...)."
  [ch val]
  (let [state-before @ch
        rf           (:xform state-before)
        ex-handler   (:ex-handler state-before)
        result       (atom nil)]
    (try
      (reset! result (rf nil val))
      (catch e
        (when ex-handler
          (let [recovery (try (ex-handler (str e))
                              (catch _ nil))]
            (when-not (nil? recovery)
              (swap! ch (fn [s] (buf-add s recovery))))))))
    (let [[s1 pairs] (flush-buf-to-takers @ch)]
      (reset! ch s1)
      (commit-and-schedule-pairs! pairs))
    (when (and @result (reduced? @result))
      (close! ch))))

;; ---------------------------------------------------------------------
;; offer! / poll!
;; ---------------------------------------------------------------------

(defn offer!
  "Put val on ch if immediately possible. Returns true/false. Never
   enqueues as pending."
  [ch val]
  (when (nil? val)
    (throw "cannot put nil on a channel"))
  (cond
    (:closed? @ch)
    false

    (:xform @ch)
    (do (run-xform-step! ch val)
        true)

    :else
    (let [accepted   (atom false)
          taker-pair (atom nil)]
      (swap! ch
        (fn [state]
          (let [ts (drop-committed (:takers state))]
            (cond
              (seq ts)
              (let [t      (first ts)
                    rest-t (subvec ts 1)]
                (reset! taker-pair [t val])
                (reset! accepted true)
                (assoc state :takers rest-t))

              (and (:buf-kind state) (not (buf-full? state)))
              (do (reset! accepted true)
                  (buf-add state val))

              :else
              (assoc state :takers ts)))))
      (when-let [pair @taker-pair]
        (try-commit! (first pair))
        (schedule-op! (first pair) (second pair)))
      @accepted)))

(defn poll!
  "Take from ch if immediately available. Returns the value or nil."
  [ch]
  (let [val         (atom nil)
        putter-pair (atom nil)]
    (swap! ch
      (fn [state]
        (cond
          (pos? (buf-count state))
          (let [[v s1]          (buf-remove state)
                active-putters  (drop-committed (:putters s1))]
            (reset! val v)
            (if (seq active-putters)
              (let [p      (first active-putters)
                    rest-p (subvec active-putters 1)]
                (reset! putter-pair [p true])
                (buf-add (assoc s1 :putters rest-p) (:val p)))
              (assoc s1 :putters active-putters)))

          :else
          (let [active-putters (drop-committed (:putters state))]
            (if (seq active-putters)
              (let [p      (first active-putters)
                    rest-p (subvec active-putters 1)]
                (reset! val (:val p))
                (reset! putter-pair [p true])
                (assoc state :putters rest-p))
              (assoc state :putters active-putters))))))
    (when-let [pair @putter-pair]
      (try-commit! (first pair))
      (schedule-op! (first pair) (second pair)))
    @val))

;; ---------------------------------------------------------------------
;; put! / take!
;; ---------------------------------------------------------------------

(defn put!
  "Asynchronously put val on ch. Optional cb receives true (delivered) or
   false (channel closed). Returns nil."
  ([ch val] (put! ch val nil))
  ([ch val cb]
   (when (nil? val)
     (throw "cannot put nil on a channel"))
   (cond
     (:closed? @ch)
     (do (schedule! cb false) nil)

     (:xform @ch)
     (do (run-xform-step! ch val)
         (schedule! cb (if (:closed? @ch) false true))
         nil)

     :else
     (let [taker-pair (atom nil)
           outcome    (atom nil)]
       (swap! ch
         (fn [state]
           (let [ts (drop-committed (:takers state))]
             (cond
               (seq ts)
               (let [t      (first ts)
                     rest-t (subvec ts 1)]
                 (reset! taker-pair [t val])
                 (reset! outcome true)
                 (assoc state :takers rest-t))

               (and (:buf-kind state) (not (buf-full? state)))
               (do (reset! outcome true)
                   (buf-add state val))

               (>= (count (:putters state)) MAX-PENDING)
               (do (reset! outcome :too-many) state)

               :else
               (let [op {:val val :callback cb :flag nil :ch ch}]
                 (reset! outcome :pending)
                 (-> state
                     (assoc :takers ts)
                     (update :putters conj op)))))))
       (case @outcome
         :too-many (throw "channel has too many pending puts (> 1024)")
         :pending  nil
         (schedule! cb @outcome))
       (when-let [pair @taker-pair]
         (try-commit! (first pair))
         (schedule-op! (first pair) (second pair)))
       nil))))

(defn take!
  "Asynchronously take from ch. cb receives the value, or nil if ch is
   closed and empty. Returns nil."
  [ch cb]
  (let [putter-pair (atom nil)
        done?       (atom false)
        val         (atom nil)
        err         (atom nil)]
    (swap! ch
      (fn [state]
        (cond
          (pos? (buf-count state))
          (let [[v s1]         (buf-remove state)
                active-putters (drop-committed (:putters s1))]
            (reset! val v)
            (reset! done? true)
            (if (seq active-putters)
              (let [p      (first active-putters)
                    rest-p (subvec active-putters 1)]
                (reset! putter-pair [p true])
                (buf-add (assoc s1 :putters rest-p) (:val p)))
              (assoc s1 :putters active-putters)))

          :else
          (let [ps (drop-committed (:putters state))]
            (cond
              (seq ps)
              (let [p      (first ps)
                    rest-p (subvec ps 1)]
                (reset! val (:val p))
                (reset! done? true)
                (reset! putter-pair [p true])
                (assoc state :putters rest-p))

              (:closed? state)
              (do (reset! val nil) (reset! done? true) state)

              (>= (count (:takers state)) MAX-PENDING)
              (do (reset! err :too-many) state)

              :else
              (let [op {:val nil :callback cb :flag nil :ch ch}]
                (-> state
                    (assoc :putters ps)
                    (update :takers conj op))))))))
    (case @err
      :too-many (throw "channel has too many pending takes (> 1024)")
      nil)
    (when @done?
      (schedule! cb @val))
    (when-let [pair @putter-pair]
      (try-commit! (first pair))
      (schedule-op! (first pair) (second pair)))
    nil))

;; ---------------------------------------------------------------------
;; close!
;; ---------------------------------------------------------------------

(defn close!
  "Close the channel. Pending takers receive nil; pending putters false.
   Buffered values still flow to waiting takers."
  [ch]
  (when-not (:closed? @ch)
    ;; Mark closed and run the xform completion step (outside swap! so
    ;; the step can freely swap! again).
    (swap! ch assoc :closed? true)
    (when-let [rf (:xform @ch)]
      (try (rf nil) (catch _ nil)))
    (let [[st1 buf-pairs]   (flush-buf-to-takers @ch)
          active-takers     (drop-committed (:takers st1))
          active-putters    (drop-committed (:putters st1))]
      (reset! ch (assoc st1 :takers [] :putters []))
      (commit-and-schedule-pairs! buf-pairs)
      (doseq [op active-takers]
        (try-commit! op)
        (schedule-op! op nil))
      (doseq [op active-putters]
        (try-commit! op)
        (schedule-op! op false))
      ;; Drain the wake-callbacks we just enqueued. Parked takers and
      ;; putters block on promise deref, so nothing else will pull
      ;; them off the run queue; without this drain, blocking <!!/>!!
      ;; calls deadlock when close! is the only signal that can release
      ;; them.
      (drain!)))
  nil)

;; ---------------------------------------------------------------------
;; alts!
;; ---------------------------------------------------------------------

(defn- try-immediate-put
  "Try to complete a [ch val] put op immediately. Returns [result ch] or
   nil if pending."
  [ch val]
  (when (nil? val)
    (throw "cannot put nil on a channel"))
  (cond
    (:closed? @ch)
    [false ch]

    (:xform @ch)
    (do (run-xform-step! ch val)
        [true ch])

    :else
    (let [outcome    (atom :pending)
          taker-pair (atom nil)]
      (swap! ch
        (fn [state]
          (let [ts (drop-committed (:takers state))]
            (cond
              (seq ts)
              (let [t      (first ts)
                    rest-t (subvec ts 1)]
                (reset! taker-pair [t val])
                (reset! outcome :delivered)
                (assoc state :takers rest-t))

              (and (:buf-kind state) (not (buf-full? state)))
              (do (reset! outcome :delivered)
                  (buf-add state val))

              :else
              (assoc state :takers ts)))))
      (when-let [pair @taker-pair]
        (try-commit! (first pair))
        (schedule-op! (first pair) (second pair)))
      (when (= :delivered @outcome)
        [true ch]))))

(defn- try-immediate-take
  "Try to complete a take op on ch immediately. Returns [val ch] or nil."
  [ch]
  (let [outcome     (atom :pending)
        val         (atom nil)
        putter-pair (atom nil)]
    (swap! ch
      (fn [state]
        (cond
          (pos? (buf-count state))
          (let [[v s1]         (buf-remove state)
                active-putters (drop-committed (:putters s1))]
            (reset! val v)
            (reset! outcome :delivered)
            (if (seq active-putters)
              (let [p      (first active-putters)
                    rest-p (subvec active-putters 1)]
                (reset! putter-pair [p true])
                (buf-add (assoc s1 :putters rest-p) (:val p)))
              (assoc s1 :putters active-putters)))

          :else
          (let [ps (drop-committed (:putters state))]
            (cond
              (seq ps)
              (let [p      (first ps)
                    rest-p (subvec ps 1)]
                (reset! val (:val p))
                (reset! outcome :delivered)
                (reset! putter-pair [p true])
                (assoc state :putters rest-p))

              (:closed? state)
              (do (reset! val nil)
                  (reset! outcome :delivered) state)

              :else
              (assoc state :putters ps))))))
    (when-let [pair @putter-pair]
      (try-commit! (first pair))
      (schedule-op! (first pair) (second pair)))
    (when (= :delivered @outcome)
      [@val (second [nil ch])])))

(defn- try-immediate
  "Walk ops in index order, return [result ch] for the first op that can
   complete immediately; nil if none can."
  [ops indexed]
  (loop [i 0]
    (if (>= i (count indexed))
      nil
      (let [k  (nth indexed i)
            op (nth ops k)
            r  (if (vector? op)
                 (try-immediate-put (nth op 0) (nth op 1))
                 (try-immediate-take op))]
        (if r
          (if (vector? op)
            [(first r) (nth op 0)]
            [(first r) op])
          (recur (+ i 1)))))))

(defn- register-pending-alts [ops flag cb]
  (doseq [op ops]
    (if (vector? op)
      (let [ch  (nth op 0)
            val (nth op 1)]
        (when (nil? val) (throw "cannot put nil on a channel"))
        (let [new-op {:val val :callback cb :flag flag :ch ch}]
          (swap! ch (fn [state]
                      (if (:closed? state)
                        state
                        (update state :putters conj new-op))))))
      (let [ch     op
            new-op {:val nil :callback cb :flag flag :ch ch}]
        (swap! ch (fn [state]
                    (if (:closed? state)
                      state
                      (update state :takers conj new-op))))))))

(defn- alts-start
  "Core alts machinery. Tries immediate completion; if nothing ready and
   a :default is given, returns [default :default]; otherwise registers
   pending ops on each channel with a shared arbitration flag.
   Returns [result ch] or nil (= pending)."
  [ops opts cb]
  (when (zero? (count ops))
    (throw "alts!: ops vector must not be empty"))
  (let [n         (count ops)
        base      (vec (range n))
        indices   (if (:priority opts) base (shuffle base))
        immediate (try-immediate ops indices)]
    (cond
      immediate             immediate
      (contains? opts :default)
      [(:default opts) :default]

      :else
      (let [flag (atom :pending)]
        (register-pending-alts ops flag cb)
        nil))))

(defn- alts-opts-map
  "Normalises alts!/alts-callback trailing args to an opts map. Accepts
   the canon kwargs surface (:priority true :default val), the legacy
   single-map form ({:priority true :default val}), or none."
  [opts]
  (cond
    (empty? opts)                                 {}
    (and (= 1 (count opts)) (map? (first opts)))  (first opts)
    :else                                         (apply hash-map opts)))

(defn alts!
  "Atomically complete one of several channel operations.
   ops is a vector. Each element is either a channel (take) or
   [ch val] (put). opts (kwargs or a single map):
     :priority true  -- try in order (no shuffle)
     :default val    -- return [val :default] if no op is ready
   Returns [result channel]."
  [ops & opts]
  (let [opts-m (alts-opts-map opts)
        result (atom nil)
        cb     (fn [r] (reset! result r))]
    (or (alts-start ops opts-m cb)
        (do (drain!) @result))))

(defn alts-callback
  "Callback-style alts matching the old C alts* contract. cb fires with
   [val ch]. If an op completes immediately, cb is scheduled; otherwise
   ops register pending on a shared flag and cb fires when one wins.
   Returns 1 (matches the old C contract)."
  [ops opts cb]
  (when-let [immediate (alts-start ops opts cb)]
    (async-sched-enqueue* cb immediate))
  1)

;; ---------------------------------------------------------------------
;; Compatibility aliases
;;
;; The go macro transform (in core/async) emits calls to chan-take* /
;; chan-put* / alts* / chan* / chan?* / buf-fixed* etc. These used to be
;; C primitives. Point them at the mino-level functions so the transform
;; keeps working once the C primitives are removed.
;; ---------------------------------------------------------------------

(def chan*         chan)
(def chan?*        chan?)
(def chan-put*     put!)
(def chan-take*    take!)
(def chan-close*   close!)
(def chan-closed?* closed?)
(def offer!*       offer!)
(def poll!*        poll!)
(def alts*         alts-callback)
(def buf-fixed*    buffer)
(def buf-dropping* dropping-buffer)
(def buf-sliding*  sliding-buffer)

(defn buf-promise*
  "Kept for compatibility: returns a promise buffer descriptor. In the
   old C layer this was a buffer value passed to chan*; with pure-mino
   channels we collapse chan + promise buffer into promise-chan."
  []
  {:kind :buf :buf-kind :promise :capacity 1})

(defn chan-set-xform*
  "Set transducer rf (and optional ex-handler) on an existing channel."
  [ch rf ex-handler]
  (swap! ch assoc :xform rf :ex-handler ex-handler)
  nil)

(defn chan-buf-add*
  "Raw buffer add, called from a transducer reducing step."
  [ch val]
  (swap! ch (fn [state] (buf-add state val)))
  nil)

;; ---------------------------------------------------------------------
;; High-level combinators (timeout, go, blocking bridges, mult/pub/sub,
;; pipe, mix, pipeline). The go macro transforms its body into a state
;; machine where each <! and >! call is a park point.
;; ---------------------------------------------------------------------


(defn timeout
  "Returns a channel that closes after ms milliseconds."
  [ms]
  (let [ch (chan)]
    (async-schedule-timer* ms (fn [_] (close! ch)))
    ch))

;; --- go macro ---
;;
;; The go macro transforms its body into a state machine where each
;; <! and >! call is a park point. The body is split into states at
;; each park point, and each state is a callback that runs when the
;; channel operation completes.
;;
;; Supported constructs across park points:
;;   - Sequential parks: (go (<! ch1) (<! ch2))
;;   - let bindings:     (go (let [a (<! ch1) b (<! ch2)] (+ a b)))
;;   - Conditionals:     (go (if cond (<! ch1) (<! ch2)))
;;   - loop/recur:       (go (loop [x (<! ch)] (recur (<! ch))))
;;   - try/catch/finally: (go (try (<! ch) (catch e (handle e))))
;;
;; Known limitations (scope boundaries, not bugs):
;;   - try with parks must be in tail position of the go body
;;   - Parks in catch or finally bodies are not supported
;;   - Nested parks in function call args are not supported;
;;     use let bindings instead:
;;       BAD:  (+ (<! ch1) (<! ch2))
;;       GOOD: (let [a (<! ch1) b (<! ch2)] (+ a b))
;;   - go does not walk into fn or nested go forms

(defn go-park?
  "Returns true if form is a parking operation (<! or >!)."
  [form]
  (and (cons? form)
       (symbol? (first form))
       (let [n (name (first form))]
         (or (= n "<!") (= n ">!")))))

(defn go-contains-park?
  "Returns true if the form tree contains any <! or >! park point.
   Does not descend into fn or go forms (those are separate scopes)."
  [form]
  (cond
    (go-park? form) true
    (and (cons? form)
         (symbol? (first form))
         (let [n (name (first form))]
           (or (= n "fn") (= n "fn*") (= n "go"))))
    false
    (cons? form) (boolean (some go-contains-park? form))
    (vector? form) (boolean (some go-contains-park? (seq form)))
    :else false))

(defn go-if-park?
  "Returns true if form is (if test then else?) where a branch contains a park
   but NOT recur (recur branches are handled by the loop body exit transform)."
  [form]
  (and (cons? form)
       (= 'if (first form))
       (let [fv (vec form)]
         (and (>= (count fv) 3)
              (or (go-contains-park? (nth fv 2))
                  (and (>= (count fv) 4)
                       (go-contains-park? (nth fv 3))))
              ;; Exclude if branches contain recur
              (not (go-contains-recur? (nth fv 2)))
              (not (and (>= (count fv) 4)
                        (go-contains-recur? (nth fv 3))))))))

(defn go-recur?
  "Returns true if form is a (recur ...) call."
  [form]
  (and (cons? form) (= 'recur (first form))))

(defn go-contains-recur?
  "Returns true if form contains a recur call.
   Does not descend into fn, go, or loop forms."
  [form]
  (cond
    (go-recur? form) true
    (and (cons? form) (symbol? (first form))
         (let [n (name (first form))]
           (or (= n "fn") (= n "fn*") (= n "go") (= n "loop"))))
    false
    (cons? form) (boolean (some go-contains-recur? form))
    (vector? form) (boolean (some go-contains-recur? (seq form)))
    :else false))

(defn go-loop-park?
  "Returns true if form is (loop [bindings] body) where body contains parks."
  [form]
  (and (cons? form)
       (= 'loop (first form))
       (vector? (second form))
       (go-contains-park? form)))

(defn go-expand-if
  "Recursively expand when/cond forms to if within a go body,
   but only in positions where parks might appear. Does not descend
   into fn or go forms."
  [form]
  (cond
    (not (cons? form)) form
    (and (symbol? (first form))
         (let [n (name (first form))]
           (or (= n "fn") (= n "fn*") (= n "go"))))
    form
    ;; (when test body...) -> (if test body nil) or (if test (do body...) nil)
    (= 'when (first form))
    (let [body-forms (vec (rest (rest form)))]
      (go-expand-if
        (if (= 1 (count body-forms))
          (list 'if (second form) (first body-forms) nil)
          (list 'if (second form) (cons 'do body-forms) nil))))
    ;; (when-not test body...) -> (if test nil body) or (if test nil (do body...))
    (= 'when-not (first form))
    (let [body-forms (vec (rest (rest form)))]
      (go-expand-if
        (if (= 1 (count body-forms))
          (list 'if (second form) nil (first body-forms))
          (list 'if (second form) nil (cons 'do body-forms)))))
    ;; (cond pairs...) -> nested if
    (= 'cond (first form))
    (let [pairs (vec (rest form))]
      (if (< (count pairs) 2)
        nil
        (let [test-form (nth pairs 0)
              then-form (nth pairs 1)
              rest-pairs (subvec pairs 2)]
          (if (= :else test-form)
            (go-expand-if then-form)
            (go-expand-if
              (list 'if test-form
                    then-form
                    (if (empty? rest-pairs)
                      nil
                      (go-expand-if (apply list 'cond (seq rest-pairs))))))))))
    ;; if where test is a park -> lift to let-park
    (and (= 'if (first form))
         (go-park? (nth (vec form) 1)))
    (let [fv   (vec form)
          tmp  (gensym "if_test_")
          rest-forms (subvec fv 2)]
      (go-expand-if
        `(let [~tmp ~(nth fv 1)]
           (if ~tmp ~@(seq rest-forms)))))
    ;; recur with park args -> lift to let-park
    (and (= 'recur (first form))
         (some go-park? (rest form)))
    (let [args (vec (rest form))
          pairs (vec (map-indexed
                       (fn [i a]
                         (if (go-park? a)
                           (let [tmp (gensym "recur_")]
                             {:sym tmp :park a})
                           {:sym a :park nil}))
                       args))
          park-lets (vec (mapcat (fn [p] (when (:park p) [(:sym p) (:park p)])) pairs))
          new-args  (vec (map :sym pairs))]
      (go-expand-if `(let ~park-lets (recur ~@(seq new-args)))))
    ;; loop with parks in init bindings -> lift to let-park before loop
    (and (= 'loop (first form))
         (vector? (second form))
         (go-bindings-have-park? (second form)))
    (let [bindings (second form)
          pairs (partition 2 bindings)
          lifted (vec (map (fn [pair]
                             (let [bname (first pair)
                                   init  (second pair)]
                               (if (go-park? init)
                                 {:name bname :init (gensym "loop_init_") :park init}
                                 {:name bname :init init :park nil})))
                           pairs))
          outer-lets (vec (mapcat (fn [l] (when (:park l) [(:init l) (:park l)])) lifted))
          new-bindings (vec (mapcat (fn [l] [(:name l) (:init l)]) lifted))
          body (rest (rest form))]
      (go-expand-if `(let ~outer-lets (loop ~new-bindings ~@(seq body)))))
    ;; Recurse into other forms
    :else (apply list (map go-expand-if form))))

(defn go-try-clause?
  "Returns true if form is a (catch ...) or (finally ...) clause."
  [form]
  (and (cons? form)
       (or (= 'catch (first form))
           (= 'finally (first form)))))

(defn go-parse-try
  "Parse a try form into {:body-forms [...] :catch-sym sym :catch-body [...] :finally-body [...]}."
  [form]
  (let [args (vec (rest form))
        body-forms (vec (take-while (complement go-try-clause?) args))
        rest-clauses (drop (count body-forms) args)
        catch-clause (first (filter (fn [c] (and (cons? c) (= 'catch (first c)))) rest-clauses))
        finally-clause (first (filter (fn [c] (and (cons? c) (= 'finally (first c)))) rest-clauses))]
    {:body-forms body-forms
     :catch-sym (when catch-clause (second catch-clause))
     :catch-body (when catch-clause (vec (rest (rest catch-clause))))
     :finally-body (when finally-clause (vec (rest finally-clause)))}))

(defn go-try-park?
  "Returns true if form is (try body... (catch e handler)) where body contains parks."
  [form]
  (and (cons? form)
       (= 'try (first form))
       (boolean (some go-contains-park? (:body-forms (go-parse-try form))))))

(defn go-bindings-have-park?
  "Returns true if a binding vector [name val name val ...] contains
   any park operation in the value positions."
  [bindings]
  (boolean (some go-park?
             (map (fn [i] (nth bindings i))
                  (filter odd? (range (count bindings)))))))

(defn go-first-park-idx
  "Given a seq of binding pairs, return the index of the first pair
   whose value is a park operation."
  [pairs]
  (loop [i 0 ps (seq pairs)]
    (when ps
      (if (go-park? (second (first ps)))
        i
        (recur (inc i) (next ps))))))

(defn go-let-park?
  "Returns true if form is (let [... x (<! ch) ...] body) where any binding
   value is a park operation."
  [form]
  (and (cons? form)
       (= 'let (first form))
       (vector? (second form))
       (>= (count (second form)) 2)
       (go-bindings-have-park? (second form))))

(defn go-transform
  "Transform a go body into a vector of states.
   Each state is {:body [...] :park park-info :use-val bool :bind sym}.
   :bind sym means the next state should bind val# to sym via let.
   Park info: {:kind :take/:put :ch expr :val expr}.
   Recursively processes nested let-park and loop/recur forms.
   loop-ctx: nil or {:start-idx n :binds [names]} for loop context."
  ([body result-ch] (go-transform body result-ch nil))
  ([body result-ch loop-ctx]
  (let [raw-stmts (if (and (cons? body) (= 'do (first body)))
                    (vec (rest body))
                    [body])
        ;; Flatten nested do forms so (do (do a b) c) becomes [a b c]
        stmts (vec (mapcat (fn [s]
                             (if (and (cons? s) (= 'do (first s)))
                               (rest s)
                               [s]))
                           raw-stmts))]
    (loop [i       0
           states  []
           current []
           has-park false
           bind-sym nil]
      (if (>= i (count stmts))
        ;; Done: add final state
        (conj states {:body current :park nil
                      :use-val has-park :bind bind-sym})
        (let [stmt (nth stmts i)]
          (cond
            ;; Direct park: (<! ch) or (>! ch val)
            (go-park? stmt)
            (let [op   (name (first stmt))
                  kind (if (= op "<!") :take :put)
                  park (if (= kind :take)
                         {:kind :take :ch (second stmt)}
                         {:kind :put :ch (second stmt) :val (nth stmt 2)})
                  new-state {:body current :park park
                             :use-val has-park :bind bind-sym}]
              (recur (inc i) (conj states new-state) []
                     true nil))

            ;; Let with park in any binding: (let [... x (<! ch) ...] body)
            (go-let-park? stmt)
            (let [bindings    (second stmt)
                  pairs       (vec (partition 2 bindings))
                  park-idx    (go-first-park-idx pairs)
                  ;; Split into pre-park, park, and post-park
                  pre-pairs   (subvec pairs 0 park-idx)
                  park-pair   (nth pairs park-idx)
                  post-pairs  (subvec pairs (inc park-idx))
                  sym         (first park-pair)
                  park-form   (second park-pair)
                  op          (name (first park-form))
                  kind        (if (= op "<!") :take :put)
                  park        (if (= kind :take)
                                {:kind :take :ch (second park-form)}
                                {:kind :put :ch (second park-form) :val (nth park-form 2)})
                  rest-bindings (vec (mapcat identity post-pairs))
                  body-forms  (vec (rest (rest stmt)))
                  ;; Pre-park binding pairs (flattened [name val ...]) for
                  ;; threading through the state machine locals map
                  pre-bind-lets (vec (mapcat identity pre-pairs))
                  new-state   {:body current :park park
                               :use-val has-park :bind bind-sym
                               :pre-bind-lets pre-bind-lets}
                  ;; Build the continuation body
                  cont-body   (if (empty? rest-bindings)
                                body-forms
                                [`(let ~(apply vector (seq rest-bindings))
                                   ~@(seq body-forms))])
                  ;; Recursively transform continuation + remaining stmts
                  remaining   (vec (concat cont-body (subvec stmts (inc i))))
                  sub-states  (go-transform (cons 'do remaining) result-ch loop-ctx)
                  first-sub   (assoc (first sub-states) :use-val true :bind sym)
                  rest-subs   (rest sub-states)]
              (vec (concat (conj states new-state first-sub) rest-subs)))

            ;; If with park in branch: (if test (<! ch1) (<! ch2))
            (go-if-park? stmt)
            (let [fv          (vec stmt)
                  test-form   (nth fv 1)
                  then-form   (nth fv 2)
                  else-form   (if (>= (count fv) 4) (nth fv 3) nil)
                  bridge-sym  (gensym "if_bridge_")
                  cb-val      (gensym "if_v_")
                  make-park-cb (fn [form]
                                (let [op (name (first form))]
                                  (if (= op "<!")
                                    `(chan-take* ~(second form)
                                       (fn [~cb-val] (chan-put* ~bridge-sym [~cb-val] nil)))
                                    `(chan-put* ~(second form) ~(nth form 2)
                                       (fn [~cb-val] (chan-put* ~bridge-sym [~cb-val] nil))))))
                  make-branch (fn make-branch [form]
                                (cond
                                  (go-park? form)
                                  (make-park-cb form)
                                  ;; Nested if with parks -> recurse with same bridge
                                  (go-if-park? form)
                                  (let [ifv (vec form)
                                        t   (nth ifv 1)
                                        th  (nth ifv 2)
                                        el  (if (>= (count ifv) 4) (nth ifv 3) nil)]
                                    `(if ~t ~(make-branch th) ~(make-branch el)))
                                  ;; (do ... park) -> execute stmts then park
                                  (and (cons? form) (= 'do (first form)))
                                  (let [body-forms (vec (rest form))
                                        last-form  (last body-forms)]
                                    (if (= 1 (count body-forms))
                                      (make-branch (first body-forms))
                                      (let [init (butlast body-forms)]
                                        `(do ~@(seq init) ~(make-branch last-form)))))
                                  :else
                                  `(chan-put* ~bridge-sym [~form] nil)))
                  if-body     `(if ~test-form
                                 ~(make-branch then-form)
                                 ~(make-branch else-form))
                  park        {:kind :take :ch bridge-sym :unwrap true}
                  new-state   {:body (conj current if-body)
                               :park park
                               :use-val has-park :bind bind-sym}]
              (recur (inc i) (conj states new-state) []
                     true nil))

            ;; Loop with parks in body
            (go-loop-park? stmt)
            (let [bindings-vec (second stmt)
                  pairs       (vec (partition 2 bindings-vec))
                  bind-names  (vec (map first pairs))
                  bind-inits  (vec (map second pairs))
                  ;; Pre-loop state: accumulated body so far
                  pre-state   {:body current :park nil
                               :use-val has-park :bind bind-sym}
                  ;; Loop-start state index: after pre-state
                  loop-start  (inc (count states))
                  ;; Process body with loop context
                  body-form   (cons 'do (rest (rest stmt)))
                  body-states (go-transform body-form result-ch
                                {:start-idx loop-start :binds bind-names})
                  ;; Add recur context to all body states
                  body-with-ctx (mapv (fn [s] (assoc s :recur-ctx
                                               {:target loop-start :binds bind-names}))
                                     body-states)
                  ;; Mark first body state with loop info
                  first-body  (assoc (first body-with-ctx)
                                     :loop-id true
                                     :loop-binds bind-names
                                     :loop-inits bind-inits
                                     :loop-start-idx loop-start)
                  rest-body   (rest body-with-ctx)
                  ;; Remaining stmts after the loop
                  remaining   (subvec stmts (inc i))]
              (if (empty? remaining)
                ;; Loop is the last expression
                (vec (concat (conj states pre-state first-body) rest-body))
                ;; There are stmts after the loop - TODO
                (vec (concat (conj states pre-state first-body) rest-body))))

            ;; Try with parks in body
            (go-try-park? stmt)
            (let [{:keys [body-forms catch-sym catch-body finally-body]}
                    (go-parse-try stmt)
                  pre-state   {:body current :park nil
                               :use-val has-park :bind bind-sym}
                  inner-body  (if (= 1 (count body-forms))
                                (first body-forms)
                                (cons 'do body-forms))
                  sub-states  (go-transform inner-body result-ch loop-ctx)
                  try-ctx     {:catch-sym (or catch-sym (gensym "e_"))
                               :catch-body (or catch-body ['nil])
                               :finally-body finally-body}
                  tagged      (mapv (fn [s] (assoc s :try-ctx try-ctx))
                                    sub-states)]
              (vec (concat (conj states pre-state) tagged)))

            ;; Recur (inside a loop)
            (go-recur? stmt)
            (let [args      (vec (rest stmt))
                  new-state {:body current :park nil
                             :use-val has-park :bind bind-sym
                             :recur-target (:start-idx loop-ctx)
                             :recur-args args
                             :recur-binds (:binds loop-ctx)}]
              ;; Recur is terminal - no more states after it
              (conj states new-state))

            ;; Let with non-park bindings but park-containing body:
            ;; wrap the let around the inner body and recurse, preserving
            ;; the let scope by prepending it as pre-bind-lets
            (and (cons? stmt) (= 'let (first stmt))
                 (vector? (second stmt))
                 (not (go-bindings-have-park? (second stmt)))
                 (go-contains-park? stmt))
            (let [let-binds   (second stmt)
                  body-forms  (vec (rest (rest stmt)))
                  ;; Wrap body forms into a do, recurse with remaining stmts
                  remaining   (vec (concat body-forms (subvec stmts (inc i))))
                  sub-body    (cons 'do remaining)
                  sub-states  (go-transform sub-body result-ch loop-ctx)
                  ;; Inject the let bindings as pre-bind-lets on the first
                  ;; sub-state so they're in scope for the park operations
                  first-sub   (update (first sub-states) :pre-bind-lets
                                (fn [existing]
                                  (vec (concat let-binds (or existing [])))))
                  ;; Build: current accumulated body → first-sub (with let) → rest
                  pre-state   {:body current :park nil
                               :use-val has-park :bind bind-sym}]
              (vec (concat (conj states pre-state first-sub) (rest sub-states))))

            ;; Normal statement
            :else
            (recur (inc i) states (conj current stmt)
                   has-park bind-sym))))))))

(defn go-loop-body-exit
  "Transform the last expression of a loop body:
   - (recur args) → machine self-call back to loop start
   - (if test recur-expr exit-expr) → if with both paths handled
   - non-recur exit value → put on result-ch and close
   Does not descend into fn, go, or loop forms."
  [form machine-sym target binds result-ch val-sym]
  (cond
    ;; recur → jump back
    (go-recur? form)
    (let [args (vec (rest form))
          locals-map (apply hash-map
                       (mapcat (fn [b a] [`'~b a]) binds args))]
      `(~machine-sym ~target {:val nil :locals ~locals-map}))
    ;; Direct park (<! or >!) as exit value
    (go-park? form)
    (let [op (name (first form))
          cb-sym (gensym "park_exit_")]
      (if (= op "<!")
        `(chan-take* ~(second form)
           (fn [~cb-sym]
             (when (not (nil? ~cb-sym))
               (chan-put* ~result-ch ~cb-sym (fn [~val-sym] nil)))
             (chan-close* ~result-ch)))
        `(chan-put* ~(second form) ~(nth form 2)
           (fn [~cb-sym]
             (chan-close* ~result-ch)))))
    ;; Don't descend into fn/go/loop
    (and (cons? form) (symbol? (first form))
         (let [n (name (first form))]
           (or (= n "fn") (= n "fn*") (= n "go") (= n "loop"))))
    ;; Treat as exit value
    `(let [exit-val# ~form]
       (when (not (nil? exit-val#))
         (chan-put* ~result-ch exit-val# (fn [~val-sym] nil)))
       (chan-close* ~result-ch))
    ;; if → transform both branches
    (and (cons? form) (= 'if (first form)))
    (let [fv (vec form)
          test-form (nth fv 1)
          then-form (nth fv 2)
          else-form (if (>= (count fv) 4) (nth fv 3) nil)]
      `(if ~test-form
         ~(go-loop-body-exit then-form machine-sym target binds result-ch val-sym)
         ~(go-loop-body-exit else-form machine-sym target binds result-ch val-sym)))
    ;; do → check for parks in non-last position, transform last expression
    (and (cons? form) (= 'do (first form)))
    (let [body-forms (vec (rest form))
          ;; Find first park in non-last position
          park-idx (loop [idx 0]
                     (if (>= idx (dec (count body-forms)))
                       nil
                       (if (go-park? (nth body-forms idx))
                         idx
                         (recur (inc idx)))))]
      (if park-idx
        ;; Park in non-last position: chain as callback
        (let [before   (subvec body-forms 0 park-idx)
              park-form (nth body-forms park-idx)
              after    (subvec body-forms (inc park-idx))
              op       (name (first park-form))
              cb-sym   (gensym "do_cb_")
              cont     (go-loop-body-exit
                         (if (= 1 (count after))
                           (first after)
                           (cons 'do (seq after)))
                         machine-sym target binds result-ch val-sym)
              park-expr (if (= op "<!")
                          `(chan-take* ~(second park-form) (fn [~cb-sym] ~cont))
                          `(chan-put* ~(second park-form) ~(nth park-form 2) (fn [~cb-sym] ~cont)))]
          (if (empty? before)
            park-expr
            `(do ~@(seq before) ~park-expr)))
        ;; No park in non-last position: just transform last
        (let [init (butlast body-forms)
              last-form (last body-forms)]
          `(do ~@(seq init)
               ~(go-loop-body-exit last-form machine-sym target binds result-ch val-sym)))))
    ;; let with park binding → chan-take* callback with continuation
    (and (cons? form) (= 'let (first form))
         (vector? (second form))
         (>= (count (second form)) 2)
         (go-park? (nth (second form) 1)))
    (let [let-binds (second form)
          sym (nth let-binds 0)
          park-form (nth let-binds 1)
          op (name (first park-form))
          rest-binds (subvec let-binds 2)
          body-forms (vec (rest (rest form)))
          cont (if (empty? rest-binds)
                 (let [init-forms (butlast body-forms)
                       last-form  (last body-forms)]
                   `(do ~@(seq init-forms)
                        ~(go-loop-body-exit last-form machine-sym target binds result-ch val-sym)))
                 (go-loop-body-exit
                   `(let ~(apply vector (seq rest-binds)) ~@(seq body-forms))
                   machine-sym target binds result-ch val-sym))
          cb-sym (gensym "lcb_")]
      (if (= op "<!")
        `(chan-take* ~(second park-form) (fn [~cb-sym] (let [~sym ~cb-sym] ~cont)))
        `(chan-put* ~(second park-form) ~(nth park-form 2) (fn [~cb-sym] (let [~sym ~cb-sym] ~cont)))))
    ;; let → transform body (no park in bindings)
    (and (cons? form) (= 'let (first form)))
    (let [bindings-form (second form)
          body-forms (vec (rest (rest form)))
          init (butlast body-forms)
          last-form (last body-forms)]
      `(let ~bindings-form
         ~@(seq init)
         ~(go-loop-body-exit last-form machine-sym target binds result-ch val-sym)))
    ;; Non-recur exit value → put on result-ch
    :else
    `(let [exit-val# ~form]
       (when (not (nil? exit-val#))
         (chan-put* ~result-ch exit-val# (fn [~val-sym] nil)))
       (chan-close* ~result-ch))))

(defn go-wrap-try
  "If state has :try-ctx, wrap expr in try/catch that puts the caught value
   on result-ch. Otherwise return expr unchanged.
   last-try? controls finally placement: only the last try state gets
   a proper (finally ...) clause. Intermediate states run finally inline
   in the catch handler (only on exception, not on normal park)."
  [expr state result-ch last-try?]
  (if-let [tc (:try-ctx state)]
    (let [catch-sym    (:catch-sym tc)
          catch-body   (:catch-body tc)
          finally-body (:finally-body tc)
          catch-result (gensym "catch_r_")
          cb-sym       (gensym "tc_")
          catch-core    `(let [~catch-result (do ~@(seq catch-body))]
                           (when (not (nil? ~catch-result))
                             (chan-put* ~result-ch ~catch-result (fn [~cb-sym] nil)))
                           (chan-close* ~result-ch))
          catch-handler (if (and finally-body (not last-try?))
                          `(do ~catch-core ~@(seq finally-body))
                          catch-core)]
      (if (and finally-body last-try?)
        `(try ~expr (catch ~catch-sym ~catch-handler) (finally ~@(seq finally-body)))
        `(try ~expr (catch ~catch-sym ~catch-handler))))
    expr))

(defn go-emit-machine
  "Emit the state machine function for a go block.
   The machine function takes (state, val) where val is either nil
   (initial) or the result of the previous park operation.
   Bindings that cross state boundaries are stored in a locals map
   threaded through val as {:val v :locals {...}}."
  [states result-ch]
  (let [n           (count states)
        machine-sym (gensym "go_machine_")
        ;; Collect all bind syms to know which states need locals
        all-binds   (vec (keep :bind states))]
    (let [arms (atom [])]
      (dotimes [i n]
        (let [state    (nth states i)
              body     (:body state)
              park     (:park state)
              use-val  (:use-val state)
              bind     (:bind state)
              val-sym  (gensym "v_")
              ;; Is this the last state with :try-ctx? (for finally placement)
              last-try? (and (:try-ctx state)
                             (or (= i (dec n))
                                 (not (:try-ctx (nth states (inc i))))))
              ;; Previous binds: all :bind syms and pre-bind names from states before this one
              prev-binds  (vec (concat (keep :bind (take i states))
                                       (mapcat (fn [s] (take-nth 2 (or (:pre-bind-lets s) [])))
                                               (take i states))))
              ;; Loop context bindings to restore (for states after loop-start)
              loop-ctx-binds (if (and (:recur-ctx state) (not (:loop-id state)))
                               (:binds (:recur-ctx state))
                               [])
              ;; Loop-start bindings to pack (for the loop-start state)
              loop-own-binds (if (:loop-id state) (:loop-binds state) [])
              ;; Pre-bind names from current state (need to be packed into locals)
              cur-pre-binds (vec (take-nth 2 (or (:pre-bind-lets state) [])))
              ;; All bindings that need to be in scope
              has-locals  (or (not (empty? prev-binds)) (not (empty? loop-ctx-binds)))
              ;; Destructure locals from val#
              ;; For loop-start states, guard with (:locals val#) check so
              ;; first entry (raw val#) doesn't clobber init values with nil
              restore-lets (if (:loop-id state)
                             (mapcat (fn [s] [s `(when (:locals ~'val#)
                                                   (get (:locals ~'val#) '~s))])
                                     (concat loop-ctx-binds prev-binds))
                             (mapcat (fn [s] [s `(get (:locals ~'val#) '~s)])
                                     (concat loop-ctx-binds prev-binds)))
              ;; Current value from park
              cur-val     (if use-val
                            (if has-locals '(:val val#) 'val#)
                            nil)
              ;; Bridge channel for if-park states (unwrap parks)
              bridge-lets (if (and park (:unwrap park))
                            [(:ch park) `(chan* (buf-fixed* 1))]
                            [])
              ;; Build the value to pass to next state (with locals map)
              pack-val    (fn [v]
                            (let [cur-binds (vec (concat loop-own-binds
                                                         prev-binds
                                                         cur-pre-binds
                                                         (when bind [bind])))]
                              (if (empty? cur-binds)
                                v
                                (let [base (if has-locals '(:locals val#) {})
                                      locals-expr (clojure.core/reduce
                                                    (fn [m b] `(assoc ~m '~b ~b))
                                                    base cur-binds)]
                                  `{:val ~v :locals ~locals-expr}))))]
          ;; Loop-start: add loop bindings to all-lets
          ;; On first entry, use init values. On re-entry (val# has :locals), use locals.
          ;; First entry is detected by absence of :locals key (not by nil? val#)
          ;; because val# may carry a value from a preceding park point.
          ;; When an init value is a prev-bind sym (from a park in the loop init
          ;; bindings), substitute val# directly since the sym is only meaningful
          ;; in the previous state's scope.
          ;; restore-lets must run BEFORE loop-lets so init syms are in scope.
          (let [prev-bind-set (set prev-binds)
                loop-lets  (if (:loop-id state)
                             (vec (mapcat
                                    (fn [bname init]
                                      (let [actual-init (if (and (symbol? init)
                                                                 (prev-bind-set init))
                                                          'val#
                                                          init)]
                                        [bname `(if (:locals ~'val#)
                                                  (get (:locals ~'val#) '~bname)
                                                  ~actual-init)]))
                                    (:loop-binds state)
                                    (:loop-inits state)))
                             [])
                pre-lets   (or (:pre-bind-lets state) [])
                all-lets   (vec (concat bridge-lets restore-lets loop-lets
                                       (when bind [bind cur-val])
                                       pre-lets))]
          (swap! arms conj i)
          (cond
            ;; Recur state: jump back to loop start
            (:recur-target state)
            (let [target      (:recur-target state)
                  r-args      (:recur-args state)
                  r-binds     (:recur-binds state)
                  ;; Pack new bindings into locals map
                  pack-recur  `{:val nil
                                :locals ~(zipmap
                                           (map (fn [b] `'~b) r-binds)
                                           r-args)}
                  recur-expr  `(~machine-sym ~target ~pack-recur)
                  full-expr   (if (empty? all-lets)
                                `(do ~@(seq body) ~recur-expr)
                                `(let ~(apply vector (seq all-lets))
                                   ~@(seq body)
                                   ~recur-expr))]
              (swap! arms conj (go-wrap-try full-expr state result-ch last-try?)))

            ;; Park state
            park
            (let [next-state (inc i)
                  unwrap?    (:unwrap park)
                  pack-arg   (if unwrap? `(first ~val-sym) val-sym)
                  park-expr  (if (= (:kind park) :take)
                               `(chan-take* ~(:ch park)
                                  (fn [~val-sym]
                                    (~machine-sym ~next-state ~(pack-val pack-arg))))
                               `(chan-put* ~(:ch park) ~(:val park)
                                  (fn [~val-sym]
                                    (~machine-sym ~next-state ~(pack-val pack-arg)))))
                  ;; Wrap body + park in let if we have bindings to restore
                  full-expr  (if (empty? all-lets)
                               `(do ~@(seq body) ~park-expr)
                               `(let ~(apply vector (seq all-lets))
                                  ~@(seq body)
                                  ~park-expr))]
              (swap! arms conj (go-wrap-try full-expr state result-ch last-try?)))

            ;; Loop body state with recur: use go-loop-body-exit on last body expr
            (and (:recur-ctx state) (not (empty? body))
                 (go-contains-recur? (last body)))
            (let [ctx        (:recur-ctx state)
                  init-body  (butlast body)
                  last-expr  (last body)
                  exit-expr  (go-loop-body-exit last-expr machine-sym
                               (:target ctx) (:binds ctx) result-ch val-sym)
                  full-expr  (if (empty? all-lets)
                               `(do ~@(seq init-body) ~exit-expr)
                               `(let ~(apply vector (seq all-lets))
                                  ~@(seq init-body)
                                  ~exit-expr))]
              (swap! arms conj (go-wrap-try full-expr state result-ch last-try?)))

            ;; Pass-through state (no park, no recur, not the last state)
            (< i (dec n))
            (let [next-state (inc i)
                  full-expr  (if (empty? all-lets)
                               (if (empty? body)
                                 `(~machine-sym ~next-state ~'val#)
                                 `(do ~@(seq body) (~machine-sym ~next-state ~'val#)))
                               `(let ~(apply vector (seq all-lets))
                                  ~@(seq body)
                                  (~machine-sym ~next-state ~'val#)))]
              (swap! arms conj (go-wrap-try full-expr state result-ch last-try?)))

            ;; Final state (no park, no recur, last state)
            :else
            (let [wrapped    (if (empty? all-lets)
                               body
                               (list `(let ~(apply vector (seq all-lets))
                                       ~@(seq body))))
                  final-body (if (and use-val (empty? body) (not bind))
                               (if (empty? prev-binds) 'val# '(:val val#))
                               `(do ~@(seq wrapped)))
                  full-expr  `(let [final-result# ~final-body]
                                (when (not (nil? final-result#))
                                  (chan-put* ~result-ch final-result#
                                    (fn [~val-sym] nil)))
                                (chan-close* ~result-ch))]
              (swap! arms conj (go-wrap-try full-expr state result-ch last-try?)))))))
      `(fn ~machine-sym [state# val#]
         (case state# ~@(seq (deref arms)))))))

(defmacro go
  "Asynchronously executes the body in a lightweight state machine.
   Returns a channel which will receive the result of the body
   when it completes. <! and >! are parking operations that
   suspend the go block until the channel operation completes."
  [& body]
  (let [result-ch-sym (gensym "go_result_")
        machine-sym   (gensym "go_fn_")
        expanded      (go-expand-if (cons 'do body))
        states        (go-transform expanded result-ch-sym)]
    `(let [~result-ch-sym (chan* (buf-fixed* 1))
           ~machine-sym   ~(go-emit-machine states result-ch-sym)]
       (~machine-sym 0 nil)
       ~result-ch-sym)))

(defmacro go-loop
  "Like (go (loop bindings body...))."
  [bindings & body]
  `(go (loop ~bindings ~@(seq body))))

;; --- Blocking bridge ---
;;
;; <!!, >!!, and alts!! adapt to the host-thread grant. When threads
;; are granted, the callback delivers a promise and the calling thread
;; parks on @promise — another thread doing the matching put/take fires
;; the callback (via the scheduler drain on its own side), which wakes
;; the parked thread. When threads are not granted, the calling thread
;; drains the scheduler in a loop and throws on no progress, since no
;; other thread can ever supply the value.

(defn <!!
  "Blocking take from a channel. When host threads are granted, parks
   the calling thread until a value is available (or the channel
   closes). Without threads, drains the scheduler in a loop and throws
   if no progress can be made. Returns the taken value (nil if closed)."
  [ch]
  (let [p (promise)]
    (take! ch (fn [v] (deliver p [v])))
    (drain!)
    (cond
      (realized? p)
      (first @p)

      (> (mino-thread-limit) 1)
      (first @p)

      :else
      (if (drain-loop! (fn [] (realized? p)))
        (first @p)
        (throw "<!!: would deadlock -- no producer for this channel")))))

(defn >!!
  "Blocking put onto a channel. When host threads are granted, parks
   the calling thread until the put completes. Without threads, drains
   the scheduler in a loop and throws if no progress can be made.
   Returns true if successful, false if the channel is closed."
  [ch val]
  (let [p (promise)]
    (put! ch val (fn [v] (deliver p [v])))
    (drain!)
    (cond
      (realized? p)
      (first @p)

      (> (mino-thread-limit) 1)
      (first @p)

      :else
      (if (drain-loop! (fn [] (realized? p)))
        (first @p)
        (throw ">!!: would deadlock -- no consumer for this channel")))))

(defn alts!!
  "Blocking version of alts!. When host threads are granted, parks the
   calling thread until one operation completes. Without threads,
   drains the scheduler in a loop and throws on no progress.
   Returns [val ch]."
  ([ops] (alts!! ops {}))
  ([ops opts]
   (let [p (promise)]
     (alts* ops opts (fn [v] (deliver p [v])))
     (drain!)
     (cond
       (realized? p)
       (first @p)

       (> (mino-thread-limit) 1)
       (first @p)

       :else
       (if (drain-loop! (fn [] (realized? p)))
         (first @p)
         (throw "alts!!: would deadlock -- no operations can complete"))))))

;; --- Combinators ---

(defn pipe
  "Takes elements from the from channel and puts them on the to channel.
   Closes the to channel when from is exhausted (unless close? is false).
   Returns the to channel."
  ([from to] (pipe from to true))
  ([from to close?]
   (let [do-pipe (fn do-pipe [v]
                   (if (nil? v)
                     (when close? (close! to))
                     (do (put! to v)
                         (take! from do-pipe))))]
     (take! from do-pipe))
   to))

(defn onto-chan!
  "Puts each element of coll onto the channel ch, then closes ch
   (unless close? is false). Returns ch."
  ([ch coll] (onto-chan! ch coll true))
  ([ch coll close?]
   (doseq [v coll]
     (put! ch v))
   (when close? (close! ch))
   ch))

(defn to-chan!
  "Creates and returns a channel that receives all elements of coll,
   then closes."
  [coll]
  (let [ch (chan (count coll))]
    (onto-chan! ch coll)))

(defn into
  "Returns a channel containing the single result of reducing
   all values from ch into init using conj."
  [init ch]
  (let [result-ch (chan 1)
        acc       (atom init)
        do-take   (fn do-take [v]
                    (if (nil? v)
                      (do (put! result-ch @acc)
                          (close! result-ch))
                      (do (swap! acc conj v)
                          (take! ch do-take))))]
    (take! ch do-take)
    result-ch))

(defn merge
  "Takes a collection of source channels and returns a channel that
   contains all values from all source channels. Closes when all
   source channels are closed."
  ([chs] (merge chs nil))
  ([chs buf-or-n]
   (let [out       (if buf-or-n (chan buf-or-n) (chan))
         remaining (atom (count chs))]
     (if (= 0 (count chs))
       (close! out)
       (doseq [ch chs]
         (let [do-take (fn do-take [v]
                         (if (nil? v)
                           (when (= 0 (swap! remaining dec))
                             (close! out))
                           (do (put! out v)
                               (take! ch do-take))))]
           (take! ch do-take))))
     out)))

(defn reduce
  "Asynchronously reduces ch with f, starting from init. Returns a
   channel that yields the final accumulated value when ch closes.
   Behaves like clojure.core/reduce but without the 2-arg form: in a
   channel context the seeded form is the only one that makes sense."
  [f init ch]
  (let [result-ch (chan 1)
        acc       (atom init)
        do-take   (fn do-take [v]
                    (cond
                      (nil? v)
                      (do (put! result-ch @acc)
                          (close! result-ch))

                      (reduced? @acc)
                      (do (put! result-ch (deref @acc))
                          (close! result-ch))

                      :else
                      (do (swap! acc f v)
                          (if (reduced? @acc)
                            (do (put! result-ch (deref @acc))
                                (close! result-ch))
                            (take! ch do-take)))))]
    (take! ch do-take)
    result-ch))

(defn transduce
  "Asynchronously reduces ch with the transducer xform applied to f,
   starting from init. Returns a channel that yields the result of
   the completing arity of the transducing reducer."
  [xform f init ch]
  (let [xf        (xform f)
        result-ch (chan 1)
        acc       (atom init)
        finish    (fn finish []
                    (let [final (xf @acc)]
                      (put! result-ch final)
                      (close! result-ch)))
        do-take   (fn do-take [v]
                    (if (nil? v)
                      (finish)
                      (let [next-acc (xf @acc v)]
                        (reset! acc next-acc)
                        (if (reduced? next-acc)
                          (do (reset! acc (deref next-acc))
                              (finish))
                          (take! ch do-take)))))]
    (take! ch do-take)
    result-ch))

(defn split
  "Splits ch into two channels by predicate p. Values for which (p v)
   is truthy go to the first channel; the rest go to the second.
   Returns a vector [t-ch f-ch]. Both channels close when ch closes.
   Optional t-buf and f-buf set buffer sizes (or buffer instances)."
  ([p ch] (split p ch nil nil))
  ([p ch t-buf f-buf]
   (let [t-ch    (if t-buf (chan t-buf) (chan))
         f-ch    (if f-buf (chan f-buf) (chan))
         do-take (fn do-take [v]
                   (if (nil? v)
                     (do (close! t-ch)
                         (close! f-ch))
                     (let [out (if (p v) t-ch f-ch)]
                       (put! out v)
                       (take! ch do-take))))]
     (take! ch do-take)
     [t-ch f-ch])))

(defn partition-by
  "Returns a channel of vectors of consecutive items from ch with the
   same (f item). Closes when ch closes; flushes the in-progress
   partition before closing."
  ([f ch] (partition-by f ch nil))
  ([f ch buf-or-n]
   (let [out      (if buf-or-n (chan buf-or-n) (chan))
         current  (atom [])
         last-key (atom ::none)
         flush!   (fn []
                    (when (seq @current)
                      (put! out @current)
                      (reset! current [])))
         do-take  (fn do-take [v]
                    (if (nil? v)
                      (do (flush!)
                          (close! out))
                      (let [k (f v)]
                        (if (or (= ::none @last-key) (= k @last-key))
                          (do (swap! current conj v)
                              (reset! last-key k)
                              (take! ch do-take))
                          (do (flush!)
                              (swap! current conj v)
                              (reset! last-key k)
                              (take! ch do-take))))))]
     (take! ch do-take)
     out)))

;; --- mult ---

(defn mult
  "Creates a mult on source channel ch. Values taken from ch are
   distributed to all tapped channels. Returns a mult handle (map)."
  [ch]
  (let [taps   (atom #{})
        m      {:ch ch :taps taps}
        do-take (fn do-take [v]
                  (if (nil? v)
                    (doseq [t @taps]
                      (close! t))
                    (do (doseq [t @taps]
                          (put! t v))
                        (take! ch do-take))))]
    (take! ch do-take)
    m))

(defn tap
  "Registers channel ch as a tap on mult m. Returns ch."
  ([m ch] (tap m ch true))
  ([m ch close?]
   (swap! (:taps m) conj ch)
   ch))

(defn untap
  "Unregisters channel ch from mult m."
  [m ch]
  (swap! (:taps m) disj ch)
  nil)

;; --- pub / sub ---

(defn pub
  "Creates a pub on source channel ch with a topic-fn that extracts
   the topic from each value. Returns a pub handle (map)."
  [ch topic-fn]
  (let [subs    (atom {})
        p       {:ch ch :topic-fn topic-fn :subs subs}
        do-take (fn do-take [v]
                  (if (nil? v)
                    (doseq [[_ chs] @subs]
                      (doseq [c chs]
                        (close! c)))
                    (let [topic (topic-fn v)
                          chs   (get @subs topic)]
                      (when chs
                        (doseq [c chs]
                          (put! c v)))
                      (take! ch do-take))))]
    (take! ch do-take)
    p))

(defn sub
  "Subscribes channel ch to topic on pub p. Returns ch."
  ([p topic ch] (sub p topic ch true))
  ([p topic ch close?]
   (swap! (:subs p) update topic
     (fn [chs] (conj (or chs #{}) ch)))
   ch))

(defn unsub
  "Unsubscribes channel ch from topic on pub p."
  [p topic ch]
  (swap! (:subs p) update topic
    (fn [chs] (disj (or chs #{}) ch)))
  nil)

(defn unsub-all
  "Unsubscribes all channels from pub p, or all channels from a
   specific topic if provided."
  ([p] (reset! (:subs p) {}) nil)
  ([p topic]
   (swap! (:subs p) dissoc topic)
   nil))

;; --- mix ---

(defn mix
  "Creates a mix on the output channel out. Multiple input channels can
   be added with admix. Each input channel can have modes:
   :solo, :mute, :pause (all default false).
   solo-mode controls what happens to non-soloed channels: :mute or :pause."
  [out]
  {:out out
   :state (atom {:channels {} :solo-mode :mute})})

(defn mix-should-pass?
  "Returns true if a value from ch should be forwarded to the mix output."
  [state ch]
  (let [modes    (get (:channels state) ch)
        any-solo (some (fn [entry] (:solo (val entry))) (:channels state))]
    (cond
      (nil? modes)    false
      (:pause modes)  false
      any-solo        (and (:solo modes) (not (:mute modes)))
      :else           (not (:mute modes)))))

(defn mix-paused?
  "Returns true if ch is paused in the mix."
  [state ch]
  (let [modes    (get (:channels state) ch)
        any-solo (some (fn [entry] (:solo (val entry))) (:channels state))]
    (cond
      (nil? modes) true
      any-solo     (and (not (:solo modes))
                        (= :pause (:solo-mode state)))
      :else        (:pause modes))))

(defn admix
  "Adds ch as an input to the mix. Starts reading from it.
   Always reads from the channel while it is in the mix; paused/muted
   values are consumed but not forwarded."
  [m ch]
  (swap! (:state m) update :channels assoc ch
         {:solo false :mute false :pause false})
  (let [do-read (fn do-read [v]
                  (let [s @(:state m)]
                    (if (nil? v)
                      ;; Channel closed: remove from mix
                      (swap! (:state m) update :channels dissoc ch)
                      (do
                        (when (mix-should-pass? s ch)
                          (put! (:out m) v))
                        ;; Continue reading while still in mix
                        (when (get (:channels @(:state m)) ch)
                          (take! ch do-read))))))]
    (take! ch do-read))
  nil)

(defn unmix
  "Removes ch from the mix."
  [m ch]
  (swap! (:state m) update :channels dissoc ch)
  nil)

(defn unmix-all
  "Removes all inputs from the mix."
  [m]
  (swap! (:state m) assoc :channels {})
  nil)

(defn toggle
  "Sets modes on channels in the mix.
   state-map is {ch {:solo bool :mute bool :pause bool}}."
  [m state-map]
  (doseq [entry state-map]
    (let [ch    (key entry)
          modes (val entry)]
      (swap! (:state m) update :channels
        (fn [channels]
          (if (get channels ch)
            (update channels ch (fn [old] (clojure.core/into old modes)))
            channels)))))
  nil)

(defn solo-mode
  "Sets the solo mode for the mix. mode is :mute or :pause."
  [m mode]
  (swap! (:state m) assoc :solo-mode mode)
  nil)

;; --- pipeline ---

(defn pipeline
  "Takes items from the from channel, applies xf to each (using n
   parallel go blocks), and puts results on the to channel.
   Closes to when from is exhausted (unless close? is false).
   Preserves input ordering regardless of worker completion order.

   ex-handler, when supplied, is invoked with any exception thrown by
   xf; its return value is used as the replacement output (nil drops)."
  ([n to xf from] (pipeline n to xf from true nil))
  ([n to xf from close?] (pipeline n to xf from close? nil))
  ([n to xf from close? ex-handler]
   (let [jobs    (chan n)
         results (chan n)
         done    (atom 0)
         apply-xf (fn [v]
                    (if ex-handler
                      (try (xf v)
                           (catch e (ex-handler e)))
                      (xf v)))]
     ;; Feed: for each input, create a result channel, send [val res-ch]
     ;; to workers, and send res-ch to collector (in order).
     ;; Uses callbacks to wait for puts, preventing stalls when channels fill.
     (let [feeder (fn feeder [v]
                    (if (nil? v)
                      (close! jobs)
                      (let [res-ch (chan 1)]
                        (put! jobs [v res-ch]
                          (fn [_] (put! results res-ch
                                    (fn [_] (take! from feeder))))))))]
       (take! from feeder))
     ;; Workers: take [val res-ch], apply xf, put result on res-ch
     (dotimes [_ n]
       (let [worker (fn worker [job]
                      (if (nil? job)
                        (when (= n (swap! done inc))
                          (close! results))
                        (let [v      (first job)
                              res-ch (second job)
                              out    (apply-xf v)]
                          (when (some? out)
                            (put! res-ch out))
                          (close! res-ch)
                          (take! jobs worker))))]
         (take! jobs worker)))
     ;; Collector: take res-chs in order, drain each to output
     (let [collector (fn collector [res-ch]
                       (if (nil? res-ch)
                         (when close? (close! to))
                         (let [drain (fn drain [v]
                                       (if (nil? v)
                                         (take! results collector)
                                         (do (put! to v)
                                             (take! res-ch drain))))]
                           (take! res-ch drain))))]
       (take! results collector))
     to)))

(defn pipeline-async
  "Like pipeline, but af is an async function that takes [val result-ch].
   af should put results on result-ch and close it when done.
   Preserves input ordering regardless of worker completion order."
  ([n to af from] (pipeline-async n to af from true))
  ([n to af from close?]
   (let [jobs    (chan n)
         results (chan n)
         done    (atom 0)]
     ;; Feed: for each input, create a result channel, send [val res-ch]
     ;; to workers, and send res-ch to collector (in order).
     ;; Uses callbacks to wait for puts, preventing stalls when channels fill.
     (let [feeder (fn feeder [v]
                    (if (nil? v)
                      (close! jobs)
                      (let [res-ch (chan 1)]
                        (put! jobs [v res-ch]
                          (fn [_] (put! results res-ch
                                    (fn [_] (take! from feeder))))))))]
       (take! from feeder))
     ;; Workers: take [val res-ch], call af which puts results on res-ch
     (dotimes [_ n]
       (let [worker (fn worker [job]
                      (if (nil? job)
                        (when (= n (swap! done inc))
                          (close! results))
                        (let [v      (first job)
                              res-ch (second job)]
                          (af v res-ch)
                          (take! jobs worker))))]
         (take! jobs worker)))
     ;; Collector: take res-chs in order, drain each to output
     (let [collector (fn collector [res-ch]
                       (if (nil? res-ch)
                         (when close? (close! to))
                         (let [drain (fn drain [v]
                                       (if (nil? v)
                                         (take! results collector)
                                         (do (put! to v)
                                             (take! res-ch drain))))]
                           (take! res-ch drain))))]
       (take! results collector))
     to)))

;; mino has no separate blocking-IO scheduler, so pipeline-blocking
;; collapses into pipeline. When blocking IO lands, this should split
;; into its own implementation that releases the worker to a blocking
;; pool for the duration of the call.
(def pipeline-blocking pipeline)
