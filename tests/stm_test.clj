(require "tests/test")

;; Software Transactional Memory: refs, dosync, alter, commute, ensure, io!.

(deftest ref-construct
  (let [r (ref 0)]
    (is (= 0 @r))
    (is (ref? r))
    (is (not (ref? 0)))
    (is (not (ref? (atom 0))))))

(deftest ref-set-basic
  (let [r (ref 0)]
    (dosync (ref-set r 5))
    (is (= 5 @r))))

(deftest alter-fn
  (let [r (ref 10)]
    (dosync (alter r + 5))
    (is (= 15 @r))))

(deftest alter-multi-args
  (let [r (ref 10)]
    (dosync (alter r + 1 2 3))
    (is (= 16 @r))))

(deftest commute-basic
  (let [r (ref 0)]
    (dosync
      (commute r inc)
      (commute r inc))
    (is (= 2 @r))))

(deftest ensure-no-write
  (let [r (ref 0)]
    (dosync (ensure r))
    (is (= 0 @r))))

(deftest in-tx-predicate
  (is (false? (in-transaction?)))
  (let [r (ref 0)
        seen-in-tx (atom nil)]
    (dosync
      (reset! seen-in-tx (in-transaction?))
      (ref-set r 1))
    (is (true? @seen-in-tx))
    (is (false? (in-transaction?)))))

(deftest tx-only-mutations
  (let [r (ref 0)]
    (is (thrown? (ref-set r 1)))
    (is (thrown? (alter r inc)))
    (is (thrown? (commute r inc)))
    (is (thrown? (ensure r)))))

(deftest io-bang-rejected
  (let [r (ref 0)]
    (is (thrown? (dosync (alter r inc) (io! (println "side-effect")))))))

(deftest io-bang-passthrough
  ;; Outside dosync, io! just runs body.
  (let [a (atom 0)]
    (io! (swap! a inc))
    (is (= 1 @a))))

(deftest watch-fires-on-commit
  (let [r (ref 0)
        seen (atom [])]
    (add-watch r :w (fn [k _ o n] (swap! seen conj [k o n])))
    (dosync (alter r inc))
    (dosync (alter r inc))
    (is (= [[:w 0 1] [:w 1 2]] @seen))))

(deftest watch-removed
  (let [r (ref 0)
        seen (atom [])]
    (add-watch r :w (fn [_ _ _ n] (swap! seen conj n)))
    (dosync (alter r inc))
    (remove-watch r :w)
    (dosync (alter r inc))
    (is (= [1] @seen))))

(deftest validator-accepts
  (let [r (ref 0)]
    (set-validator! r number?)
    (is (= number? (get-validator r)))
    (dosync (alter r inc))
    (is (= 1 @r))))

(deftest validator-rejects
  (let [r (ref 0)]
    ;; Installing a validator that would fail the current value succeeds
    ;; (JVM canon); only subsequent in-tx transitions are checked.
    (set-validator! r pos?)
    (is (= 0 @r))
    (is (thrown? (dosync (ref-set r -1))))
    ;; Failed transaction leaves the ref untouched.
    (is (= 0 @r))
    (dosync (ref-set r 5))
    (is (= 5 @r))))

(deftest nested-dosync
  (let [r (ref 0)]
    (dosync
      (alter r inc)
      (dosync (alter r inc)))
    (is (= 2 @r))))

(deftest deref-in-tx-sees-tentative
  (let [r (ref 0)]
    (dosync
      (alter r inc)
      (is (= 1 @r))
      (alter r inc)
      (is (= 2 @r)))
    (is (= 2 @r))))

(deftest alter-after-commute-throws
  ;; JVM canon: ref-set / alter on a ref that already has a logged
  ;; commute throws "Can't set after commute". The commute log captures
  ;; replay-at-commit semantics that an explicit set would silently
  ;; discard, so the rule rejects the combination outright.
  (let [r (ref 0)]
    (is (thrown? (dosync
                   (commute r inc)
                   (alter r * 10))))
    (is (= 0 @r))
    (is (thrown? (dosync
                   (commute r inc)
                   (ref-set r 99))))
    (is (= 0 @r))))

(deftest alter-then-commute-folds
  ;; alter-then-commute on the same ref: the commute folds into the
  ;; alter's tentative value (commute is observed at the call site as
  ;; (g (f current))), and the final committed value is (g (f orig)).
  ;; Matches JVM, which skips the commute log replay at commit when
  ;; the ref is already in the write set.
  (let [r (ref 1)
        seen (atom [])]
    (add-watch r :w (fn [_ _ o n] (swap! seen conj [o n])))
    (dosync
      (alter r + 10)            ; tentative = 11
      (let [v (commute r * 2)]  ; tentative = 22, returns 22
        (is (= 22 v))))
    (is (= 22 @r))
    (is (= [[1 22]] @seen))))

(deftest ref-history-stubs
  (let [r (ref 0)]
    (is (= 0 (ref-min-history r)))
    (is (= 10 (ref-max-history r)))
    (is (= 0 (ref-history-count r)))))

(deftest type-keyword
  (is (= :ref (type (ref 1)))))

(deftest validator-throw-propagates-original-exception
  ;; A validator that throws aborts the transaction with the
  ;; validator's original exception value (matching JVM Clojure).
  ;; Previously mino_pcall's catch arm published a diag via
  ;; set_eval_diag, which longjmped past tx_commit's stm_unlock and
  ;; leaked the commit lock; now pcall captures the exception via
  ;; out_ex and tx_commit threads it through tx_state_t so dosync_run
  ;; can re-throw the user's payload directly. The retry path is no
  ;; longer entered for validator throws -- they are hard failures.
  (let [r (ref 1)]
    (set-validator! r (fn [v] (if (zero? v)
                                 (throw (ex-info "validator-boom"
                                                  {:value v}))
                                 (pos? v))))
    ;; First tx: validator throws. Catch the throw and inspect the
    ;; original ex-info data.
    (let [caught (try (dosync (alter r dec)) nil
                      (catch e (ex-data e)))]
      (is (= {:value 0} caught)))
    (is (= 1 @r))
    ;; Second tx must complete normally (proves the commit lock
    ;; was released after the validator throw).
    (dosync (alter r inc))
    (is (= 2 @r))))

(deftest validator-falsy-reject-throws-MCT001
  ;; Validator that returns falsy (without throwing) still aborts
  ;; the transaction with the canonical MCT001 "Invalid reference
  ;; state" message -- distinct from a validator throw which carries
  ;; its own payload.
  (let [r (ref 1)]
    (set-validator! r pos?)
    (let [caught (try (dosync (ref-set r -1)) nil
                      (catch e (:mino/code e)))]
      (is (= "MCT001" caught)))
    (is (= 1 @r))))

(deftest mutate-during-commute-replay-throws
  ;; A commute fn that calls (alter / ref-set / commute) on another
  ;; ref during commit-phase replay used to silently lose the new
  ;; tentative -- the iterator in tx_commit had already moved past
  ;; the affected ref node. Now throws MST002.
  (let [r1 (ref 0)
        r3 (ref 0)]
    (is (thrown? (dosync
                   (commute r1 (fn [v]
                                 (dosync (alter r3 inc))
                                 (inc v))))))
    ;; Whole transaction aborted; r1 and r3 untouched.
    (is (= 0 @r1))
    (is (= 0 @r3))))

(deftest mutate-self-during-commute-replay-throws
  (let [r (ref 0)
        depth (atom 0)]
    (is (thrown? (dosync
                   (commute r (fn [v]
                                (swap! depth inc)
                                (when (< @depth 3)
                                  (commute r identity))
                                (inc v))))))
    (is (= 0 @r))))

(deftest commit-is-atomic-late-validator-reject
  ;; A late-iteration validator rejection used to leave earlier
  ;; iteration writes already committed (committed in order). The
  ;; two-pass commit stages every new value first, then applies
  ;; them as a single block -- so any rejection cancels the whole
  ;; transaction with no partial state.
  (let [r1 (ref 0)
        r2 (ref 0)]
    (set-validator! r2 pos?)
    ;; Order matters: r1 is touched first, so it appears last in the
    ;; LIFO refs_head iteration -- under single-pass commit it would
    ;; be committed before r2's validator rejected.
    (try (dosync (alter r2 (fn [_] -1))
                 (alter r1 inc))
         (catch e nil))
    (is (= 0 @r1) "r1 must not be committed when r2's validator rejects")
    (is (= 0 @r2)))
  ;; Inverse touch order: same expectation.
  (let [r1 (ref 0)
        r2 (ref 0)]
    (set-validator! r1 pos?)
    (try (dosync (alter r2 inc)
                 (alter r1 (fn [_] -1)))
         (catch e nil))
    (is (= 0 @r1))
    (is (= 0 @r2))))

(deftest watch-throw-does-not-abort-other-watches
  ;; A throwing watch on r1 used to abort dispatch entirely so a
  ;; watch on r2 wouldn't fire. Now every watch on every committed
  ;; ref runs first; the first thrown exception is then re-thrown
  ;; after dispatch finishes.
  (let [r1 (ref 0)
        r2 (ref 0)
        r2-watch-fired (atom 0)]
    (add-watch r1 :crash (fn [_ _ _ _] (throw (ex-info "r1-boom" {}))))
    (add-watch r2 :w (fn [_ _ _ _] (swap! r2-watch-fired inc)))
    (try (dosync (alter r1 inc) (alter r2 inc)) (catch e nil))
    ;; r2's watch fired even though r1's watch threw.
    (is (= 1 @r2-watch-fired))))

(deftest watch-throw-still-surfaces-to-caller
  (let [r (ref 0)]
    (add-watch r :crash (fn [_ _ _ _] (throw (ex-info "boom" {:k :v}))))
    (let [caught (try (dosync (alter r inc)) nil
                      (catch e (ex-data e)))]
      (is (= {:k :v} caught)))))

(deftest commit-is-atomic-late-validator-throw
  (let [r1 (ref 0)
        r2 (ref 0)]
    (set-validator! r2 (fn [v] (if (neg? v)
                                  (throw (ex-info "bad" {:value v}))
                                  true)))
    (try (dosync (alter r1 inc)
                 (alter r2 (fn [_] -1)))
         (catch e nil))
    (is (= 0 @r1))
    (is (= 0 @r2))))

(deftest commute-throw-during-replay-does-not-leak-commit-lock
  ;; A commute fn that succeeds during the body but throws during
  ;; commute_log_replay (under the commit lock) used to longjmp past
  ;; stm_unlock, leaving the lock held. Subsequent dosync calls on
  ;; another thread would deadlock; on the same thread, attempting
  ;; to re-acquire the non-recursive mutex is undefined. The fix
  ;; routes commute fns through pcall so the throw is captured and
  ;; the lock is released cleanly before the user's exception is
  ;; surfaced.
  (let [r (ref 0)
        trip (atom false)
        caught (atom nil)]
    (try
      (dosync
        (commute r (fn [v]
                     (if @trip
                       (throw (ex-info "replay-boom" {:value v}))
                       (do (reset! trip true) (inc v))))))
      (catch e (reset! caught (ex-data e))))
    ;; Replay sees the committed base value (0), not the body-time
    ;; tentative -- commute_log_replay walks log entries against the
    ;; latest committed value at commit time.
    (is (= {:value 0} @caught))
    ;; Lock must be released: subsequent transaction completes.
    (dosync (alter r inc))
    (is (= 1 @r))))

(deftest ref-accepts-validator-option
  ;; `(ref init :validator fn)` installs the validator at construction.
  ;; The next mutation runs the validator; a falsy return aborts the
  ;; transaction with an IllegalStateException-style throw.
  (let [r (ref 1 :validator pos?)]
    (is (= 1 @r))
    (is (= pos? (get-validator r)))
    ;; A valid mutation succeeds.
    (dosync (alter r inc))
    (is (= 2 @r))
    ;; An invalid mutation throws; ref stays at the last valid value.
    (is (thrown? (dosync (ref-set r -1))))
    (is (= 2 @r))))

(deftest ref-accepts-meta-option
  (let [r (ref 0 :meta {:doc "counter"})]
    (is (= 0 @r))
    (is (= {:doc "counter"} (meta r)))))

(deftest ref-accepts-validator-and-meta-together
  (let [r (ref 5 :validator pos? :meta {:tag :counter})]
    (is (= 5 @r))
    (is (= pos? (get-validator r)))
    (is (= {:tag :counter} (meta r)))))

(deftest ref-accepts-history-options-as-noop
  ;; mino's STM doesn't track ref history; these options are accepted
  ;; for source compatibility with JVM Clojure but have no observable
  ;; effect.
  (let [r (ref 0 :min-history 3 :max-history 10)]
    (is (= 0 @r))))

(deftest ref-rejects-unknown-options
  ;; JVM Clojure throws on unknown options; match that posture so a
  ;; typo like `:vaildator` doesn't silently no-op.
  (is (thrown? (ref 0 :not-a-real-option 42))))

(deftest ref-rejects-odd-trailing-args
  (is (thrown? (ref 0 :validator))))
