(require "tests/test")

;; Agents (asynchronous mutable cells with serialized actions).
;; send / send-off enqueue an action onto a per-state run-queue and
;; return the agent immediately; a worker thread drains the queue
;; under state_lock. Tests await before reading agent state.

(deftest agent-construct
  (let [a (agent 0)]
    (is (agent? a))
    (is (not (agent? 0)))
    (is (not (agent? (atom 0))))
    (is (not (agent? (ref 0))))
    (is (= 0 @a))
    (is (= :agent (type a)))))

(deftest agent-send-basic
  (let [a (agent 0)]
    (send a inc)
    (await a)
    (is (= 1 @a))
    (send a + 5)
    (await a)
    (is (= 6 @a))))

(deftest agent-send-off
  ;; send-off routes onto the SOLO pool. mino's eval lock means
  ;; actions across send and send-off still serialize, but the
  ;; queues are independent.
  (let [a (agent 10)]
    (send-off a dec)
    (await a)
    (is (= 9 @a))))

(deftest agent-send-and-send-off-interleave
  ;; Both pools serve the same agent. await waits across both
  ;; queues -- the broadcast on agent_cv wakes await regardless of
  ;; which pool's worker drained the action.
  (let [a (agent 0)]
    (send a inc)
    (send-off a inc)
    (send a inc)
    (send-off a inc)
    (await a)
    (is (= 4 @a))))

(deftest agent-watches
  (let [a    (agent 0)
        seen (atom [])]
    (add-watch a :w (fn [_ _ o n] (swap! seen conj [o n])))
    (send a inc)
    (send a inc)
    (await a)
    (is (= [[0 1] [1 2]] @seen))))

(deftest agent-watch-removed
  (let [a    (agent 0)
        seen (atom [])]
    (add-watch a :w (fn [_ _ _ n] (swap! seen conj n)))
    (send a inc)
    (await a)
    (remove-watch a :w)
    (send a inc)
    (await a)
    (is (= [1] @seen))))

(deftest agent-validator-accepts
  (let [a (agent 0)]
    (set-validator! a number?)
    (send a inc)
    (await a)
    (is (= 1 @a))
    (is (nil? (agent-error a)))))

(deftest agent-validator-rejects
  ;; Validator rejection sets agent-error and does NOT publish the
  ;; new state. Mirrors the action-throw path.
  (let [a (agent 5)]
    (set-validator! a pos?)
    (send a (fn [_] -1))
    (await a)
    (is (some? (agent-error a)))
    (is (= 5 @a))))

(deftest agent-action-throws
  (let [a (agent 0)]
    (send a (fn [_] (throw (ex-info "boom" {:kind :test}))))
    (await a)
    (is (some? (agent-error a)))
    (is (= 0 @a))
    ;; Subsequent send on a faulted agent throws by default (:fail).
    (is (thrown? (send a inc)))))

(deftest agent-restart
  (let [a (agent 0)]
    (send a (fn [_] (throw (ex-info "boom" {}))))
    (await a)
    (is (some? (agent-error a)))
    (restart-agent a 99)
    (is (nil? (agent-error a)))
    (is (= 99 @a))
    (send a inc)
    (await a)
    (is (= 100 @a))))

(deftest agent-error-mode-continue
  (let [a (agent 0)]
    (set-error-mode! a :continue)
    (is (= :continue (error-mode a)))
    (send a (fn [_] (throw (ex-info "boom" {}))))
    (await a)
    (is (some? (agent-error a)))
    ;; In :continue mode, further sends do not throw on the failed
    ;; agent. The agent.err latch holds the most recent failure.
    (send a inc)
    (await a)))

(deftest agent-watch-throws-captured
  ;; Watch throws are captured into agent.err, matching JVM-ish
  ;; behavior where agent watch errors surface via agent-error.
  (let [a (agent 0)]
    (add-watch a :w (fn [_ _ _ _] (throw (ex-info "watch-boom" {}))))
    (send a inc)
    (await a)
    (is (some? (agent-error a)))
    ;; The publish itself succeeded; the watch threw after.
    (is (= 1 @a))))

(deftest agent-await-no-op
  ;; await is a no-op in the sync MVP -- the queue is always drained.
  (let [a (agent 0)]
    (send a inc)
    (await a)
    (is (= 1 @a)))
  ;; await with no agents.
  (await))

(deftest agent-send-returns-agent
  (let [a (agent 0)]
    (is (= a (send a inc)))
    (is (= a (send-off a inc)))))

(deftest agent-constructor-options-validator
  ;; (agent state :validator pred) installs the validator at
  ;; construction time. JVM canon: the initial state is NOT
  ;; checked against the validator at install (matches set-validator!
  ;; behavior). Subsequent send actions are.
  (let [a (agent 5 :validator pos?)]
    (is (= pos? (get-validator a)))
    (send a (fn [_] -1))
    (await a)
    (is (some? (agent-error a)))
    (is (= 5 @a))))

(deftest agent-constructor-options-error-mode
  (let [a (agent 0 :error-mode :continue)]
    (is (= :continue (error-mode a))))
  (let [a (agent 0 :error-mode :fail)]
    (is (= :fail (error-mode a)))))

(deftest agent-constructor-options-error-handler
  (let [calls (atom [])
        h (fn [a e] (swap! calls conj :h))
        a (agent 0 :error-handler h)]
    (is (= h (error-handler a)))))

(deftest agent-constructor-options-unknown-throws
  ;; Unknown option keys must throw rather than be silently ignored.
  (is (thrown? (agent 0 :no-such-option 1))))

(deftest send-from-dosync-deferred-until-commit
  ;; JVM canon: send / send-off from inside dosync queue actions
  ;; that fire only on successful commit. Earlier mino ran them
  ;; synchronously, so the action saw mid-tx tentative state, fired
  ;; on every retry attempt, and falsely tripped io! checks.
  (let [a (agent 0)
        r (ref 0)
        action-runs (atom 0)]
    (dosync
      (alter r inc)
      (send a (fn [v] (swap! action-runs inc) (inc v)))
      (is (= 0 @a) "action must NOT have run while tx still in flight"))
    ;; After commit the action has fired exactly once.
    (await a)
    (is (= 1 @a))
    (is (= 1 @action-runs))))

(deftest send-from-dosync-cleared-on-abort
  (let [a (agent 0)]
    (try (dosync
           (send a inc)
           (throw (ex-info "boom" {})))
         (catch e nil))
    (await a)
    ;; Aborted dosync's pending sends must not fire.
    (is (= 0 @a))))

(deftest send-from-dosync-fires-once-per-call
  ;; Multiple sends in the body each fire once.
  (let [a (agent [])
        r (ref 0)]
    (dosync
      (alter r inc)
      (send a conj :a)
      (send a conj :b)
      (send a conj :c))
    (await a)
    (is (= [:a :b :c] @a))))

(deftest watch-throw-does-not-drop-pending-sends
  ;; A ref watch that throws used to longjmp past the pending-sends
  ;; drain, silently swallowing every queued agent dispatch. Drain
  ;; now runs BEFORE watch dispatch so a misbehaving watch can't
  ;; lose the agent sends a successful body queued.
  (let [a (agent 0)
        r (ref 0)]
    (add-watch r :crash (fn [_ _ _ _] (throw (ex-info "watch-crash" {}))))
    (try (dosync (alter r inc) (send a inc)) (catch e nil))
    (await a)
    (is (= 1 @a))))

(deftest release-pending-sends-counts-and-clears
  ;; Inside a tx, release-pending-sends returns the count of queued
  ;; sends and removes them so they will NOT fire on commit. Outside
  ;; a tx it's a no-op returning 0.
  (let [a (agent 0)
        r (ref 0)]
    (dosync
      (alter r inc)
      (send a inc)
      (send a inc)
      (send a inc)
      (is (= 3 (release-pending-sends)))
      ;; Subsequent sends in the same tx still queue.
      (send a inc)
      (is (= 1 (release-pending-sends))))
    (await a)
    ;; All sends were released; agent state is unchanged.
    (is (= 0 @a)))
  ;; Outside a tx, returns 0 without throwing.
  (is (= 0 (release-pending-sends))))

(deftest io-bang-allowed-in-action-from-dosync
  ;; Because the action runs post-commit (current_tx cleared), io! does
  ;; NOT trip even when the send was queued from inside dosync.
  (let [a (agent 0)
        side (atom :unset)]
    (dosync
      (send a (fn [v] (io! (reset! side :ran)) (inc v))))
    (await a)
    (is (= :ran @side))
    (is (= 1 @a))
    (is (nil? (agent-error a)))))

(deftest agent-dynamic-var-bound-in-action
  ;; Inside an action body *agent* resolves to the dispatching agent.
  ;; Matches JVM canon. Outside any dispatch it's nil.
  (let [a (agent 0)
        seen (atom :unset)]
    (send a (fn [v] (reset! seen *agent*) (inc v)))
    (await a)
    (is (= a @seen)))
  (is (nil? *agent*)))

(deftest agent-dynamic-var-visible-in-validator
  ;; Validator runs while *agent* is still bound.
  (let [a (agent 0)
        seen (atom :unset)]
    (set-validator! a (fn [_] (reset! seen *agent*) true))
    (send a inc)
    (await a)
    (is (= a @seen))))

(deftest agent-dynamic-var-visible-in-watch
  ;; Agent watch dispatch happens while *agent* is still bound.
  (let [a (agent 0)
        seen (atom :unset)]
    (add-watch a :w (fn [_ _ _ _] (reset! seen *agent*)))
    (send a inc)
    (await a)
    (is (= a @seen))))

(deftest drain-skips-pending-on-failed-agent-in-fail-mode
  ;; Agent A enters :fail state mid-drain because pending action #1
  ;; throws; pending action #2 targets the same agent. Match
  ;; prim_send's contract: skip the second action silently. The
  ;; agent.err latch is the surviving record of the first failure.
  (let [a (agent 0)
        b (agent :init)]
    (dosync
      (send a (fn [_] (throw (ex-info "boom" {}))))
      (send a inc)        ;; this one must NOT run; a is now failed
      (send b (fn [_] :survived)))
    (await a)
    (await b)
    (is (some? (agent-error a)))
    (is (= 0 @a) "second pending send to failed-:fail agent dropped")
    (is (= :survived @b) "unrelated agent's send still fires")))

(deftest drain-runs-pending-on-continue-mode-failed-agent
  ;; In :continue mode, failed agents keep accepting actions. Match
  ;; prim_send: drain must also run them.
  (let [a (agent 0)]
    (set-error-mode! a :continue)
    (dosync
      (send a (fn [_] (throw (ex-info "boom" {}))))
      (send a (fn [_] :recovered)))
    (await a)
    (is (= :recovered @a))))

(deftest with-meta-on-stateful-throws
  ;; with-meta on atom or agent used to do a shallow cell copy --
  ;; the sibling got its own val/err pointers and diverged from the
  ;; original on the next mutation. Throw with a clear directive
  ;; pointing at alter-meta! / constructor :meta instead.
  (is (thrown? (with-meta (atom 0) {:m 1})))
  (is (thrown? (with-meta (agent 0) {:m 1})))
  (is (thrown? (vary-meta (atom 0) assoc :m 1)))
  (is (thrown? (vary-meta (agent 0) assoc :m 1))))

(deftest alter-meta-on-stateful-still-works
  ;; alter-meta! is in-place; identity is preserved and state is
  ;; not duplicated. Stays available for stateful types.
  (let [a (atom 0)]
    (alter-meta! a (constantly {:doc "x"}))
    (is (= {:doc "x"} (meta a))))
  (let [a (agent 0)]
    (alter-meta! a (constantly {:doc "y"}))
    (is (= {:doc "y"} (meta a)))))

(deftest meta-on-stateful-reads-cell-meta
  ;; (meta x) returns the cell-level meta on stateful types.
  (let [a (atom 0)]
    (is (nil? (meta a)))
    (alter-meta! a (constantly {:k :v}))
    (is (= {:k :v} (meta a)))))

(deftest send-via-throws-not-implemented
  ;; send-via takes an Executor in JVM; mino's sync MVP has no
  ;; Executor type. Throw with a clear MST008 rather than alias to
  ;; send (which would silently lose the executor argument).
  (is (thrown? (send-via :exec (agent 0) inc))))

(deftest agent-print-form-includes-identity
  ;; Two agents with the same value must print as distinct strings
  ;; -- otherwise they're indistinguishable in logs / debug output.
  (let [a1 (agent 0)
        a2 (agent 0)]
    (is (not= (pr-str a1) (pr-str a2)))
    ;; Format check: starts with #agent[0x and contains the value.
    (is (re-find #"#agent\[0x[0-9a-f]+ 0\]" (pr-str a1)))))

(deftest agent-constructor-options-meta
  ;; :meta is wired through to the cell's meta field. (meta a) reads
  ;; it; with-meta is intentionally not supported for agents (shallow
  ;; copy of the cell would diverge on first send).
  (let [m {:doc "counter"}
        a (agent 0 :meta m)]
    (is (= m (meta a))))
  (let [a (agent 0)]
    (is (nil? (meta a))))
  (is (thrown? (agent 0 :meta 5))))

(deftest agent-constructor-options-bad-mode-throws
  (is (thrown? (agent 0 :error-mode :silent)))
  (is (thrown? (agent 0 :error-mode "fail"))))

(deftest agent-constructor-options-odd-args-throws
  (is (thrown? (agent 0 :validator))))

(deftest agent-error-handler-invoked-on-action-throw
  ;; JVM canon: when error-handler is installed, an action throw
  ;; routes through it instead of latching the agent into a failed
  ;; state. The agent stays clean (agent-error returns nil) unless
  ;; the handler itself throws.
  (let [seen (atom nil)
        a (agent 0)]
    (set-error-handler! a (fn [agent ex] (reset! seen [agent (ex-message ex)])))
    (send a (fn [_] (throw (ex-info "boom" {}))))
    (await a)
    (is (= "boom" (second @seen)))
    (is (= a (first @seen)))
    (is (nil? (agent-error a)))
    ;; Subsequent sends still work since the agent isn't latched.
    (send a inc)
    (await a)
    (is (= 1 @a))))

(deftest agent-error-handler-throw-latches-agent
  ;; If the error-handler itself throws, the handler's exception
  ;; is captured into agent-error so the failure isn't silently
  ;; lost. The original action's exception is the one passed in;
  ;; this asserts only that *some* error is latched.
  (let [a (agent 0)]
    (set-error-handler! a (fn [_ _] (throw (ex-info "handler-boom" {}))))
    (send a (fn [_] (throw (ex-info "action-boom" {}))))
    (await a)
    (is (some? (agent-error a)))))

(deftest agent-error-handler-also-invoked-on-validator-reject
  ;; Validator failure is treated as an action failure.
  (let [seen (atom 0)
        a (agent 1)]
    (set-validator! a pos?)
    (set-error-handler! a (fn [_ _] (swap! seen inc)))
    (send a (fn [_] -1))
    (await a)
    (is (= 1 @seen))
    (is (nil? (agent-error a)))
    (is (= 1 @a))))

(deftest set-validator-rejects-non-callable
  (let [a (atom 0)] (is (thrown? (set-validator! a 5))))
  (let [r (ref 0)] (is (thrown? (set-validator! r "fn"))))
  (let [a (agent 0)] (is (thrown? (set-validator! a :keyword))))
  ;; Real fns and nil work.
  (let [a (atom 0)]
    (set-validator! a number?)
    (set-validator! a nil)
    (is (nil? (get-validator a)))))

(deftest add-watch-rejects-non-callable
  (let [a (atom 0)] (is (thrown? (add-watch a :w 5))))
  (let [r (ref 0)] (is (thrown? (add-watch r :w "fn"))))
  (let [a (agent 0)] (is (thrown? (add-watch a :w :keyword)))))

(deftest agent-constructor-validator-must-be-fn
  (is (thrown? (agent 0 :validator 5)))
  (is (thrown? (agent 0 :error-handler "fn"))))

(deftest set-error-handler-validates-fn
  ;; set-error-handler! used to silently store any value -- (set-error-handler! a 5)
  ;; would put 5 in the slot, which then crashed on the call site
  ;; when an action failed. Throw at install time. nil clears.
  (let [a (agent 0)]
    (is (thrown? (set-error-handler! a 5)))
    (is (thrown? (set-error-handler! a "not-a-fn")))
    (is (thrown? (set-error-handler! a :keyword)))
    (set-error-handler! a (fn [_ _] :ok))
    (is (some? (error-handler a)))
    (set-error-handler! a nil)
    (is (nil? (error-handler a)))))

(deftest set-error-mode-validates-arg
  ;; Only :fail and :continue are accepted. mino used to either
  ;; silently re-route an invalid keyword to :fail (e.g. :silent
  ;; flipped a previously :continue agent to :fail) or silently
  ;; ignore non-keywords. Both modes are silent surprises; throw.
  (let [a (agent 0)]
    (set-error-mode! a :continue)
    (is (= :continue (error-mode a)))
    (is (thrown? (set-error-mode! a :silent)))
    (is (= :continue (error-mode a)))
    (is (thrown? (set-error-mode! a "fail")))
    (is (= :continue (error-mode a)))
    (is (thrown? (set-error-mode! a 99)))
    (is (= :continue (error-mode a)))
    (set-error-mode! a :fail)
    (is (= :fail (error-mode a)))))

(deftest restart-agent-runs-validator
  ;; JVM canon: restart-agent validates the new state. mino used to
  ;; bypass the validator, so a failed agent could be restarted into
  ;; a state the validator forbids -- silent corruption that would
  ;; only surface on the next send. Reject before clearing the error.
  (let [a (agent 1)]
    (set-validator! a pos?)
    (send a (fn [_] (throw (ex-info "boom" {}))))
    (await a)
    (is (some? (agent-error a)))
    (is (thrown? (restart-agent a -99)))
    ;; Agent stays in failed state with original value untouched.
    (is (some? (agent-error a)))
    (is (= 1 @a))
    ;; A valid restart succeeds.
    (restart-agent a 42)
    (is (nil? (agent-error a)))
    (is (= 42 @a))))

(deftest restart-agent-clear-actions-drops-queued
  ;; With :clear-actions true, queued actions targeting the failed
  ;; agent are dropped. After restart the agent's value is exactly
  ;; the restart value with no later actions applying. We exercise
  ;; the deeper invariant by piling several sends behind a slow
  ;; throwing action; the throw fails the agent, which makes the
  ;; trailing sends parking candidates the worker would skip with
  ;; :fail mode. :clear-actions then drops them outright instead
  ;; of leaving them as no-op skips.
  (let [a (agent 0)
        ran (atom 0)]
    (send a (fn [_] (throw (ex-info "boom" {}))))
    ;; Sends queued behind the failing action. With :fail mode and
    ;; an err-set agent the worker silently drops these, but they
    ;; still count toward in_flight until the worker pops them.
    ;; clear-actions true should remove them from the queue
    ;; outright so the agent's print-time identity shape and
    ;; await semantics line up with JVM canon.
    (send a (fn [v] (swap! ran inc) (inc v)))
    (send a (fn [v] (swap! ran inc) (inc v)))
    (await a)
    (is (some? (agent-error a)))
    ;; All queued sends drained as silent skips at this point.
    (is (= 0 @ran))
    (restart-agent a 100 :clear-actions true)
    (is (nil? (agent-error a)))
    (is (= 100 @a))))

;; shutdown-agents joins the worker and seals the state's agent surface.
;; The flag is permanent (no reverse), so this test runs only in
;; embed_stm_test.c against a private mino_state_t. Re-running it here
;; in the shared test state would poison every subsequent agent test.
