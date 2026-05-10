(require "tests/test")

;; Agents (asynchronous mutable cells with serialized actions).
;; mino's MVP runs sends synchronously on the calling thread; await
;; is a no-op since the queue is always drained on send return.

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
  ;; send and send-off share the synchronous path in mino's MVP.
  (let [a (agent 10)]
    (send-off a dec)
    (is (= 9 @a))))

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
    (remove-watch a :w)
    (send a inc)
    (is (= [1] @seen))))

(deftest agent-validator-accepts
  (let [a (agent 0)]
    (set-validator! a number?)
    (send a inc)
    (is (= 1 @a))
    (is (nil? (agent-error a)))))

(deftest agent-validator-rejects
  ;; Validator rejection sets agent-error and does NOT publish the
  ;; new state. Mirrors the action-throw path.
  (let [a (agent 5)]
    (set-validator! a pos?)
    (send a (fn [_] -1))
    (is (some? (agent-error a)))
    (is (= 5 @a))))

(deftest agent-action-throws
  (let [a (agent 0)]
    (send a (fn [_] (throw (ex-info "boom" {:kind :test}))))
    (is (some? (agent-error a)))
    (is (= 0 @a))
    ;; Subsequent send on a faulted agent throws by default (:fail).
    (is (thrown? (send a inc)))))

(deftest agent-restart
  (let [a (agent 0)]
    (send a (fn [_] (throw (ex-info "boom" {}))))
    (is (some? (agent-error a)))
    (restart-agent a 99)
    (is (nil? (agent-error a)))
    (is (= 99 @a))
    (send a inc)
    (is (= 100 @a))))

(deftest agent-error-mode-continue
  (let [a (agent 0)]
    (set-error-mode! a :continue)
    (is (= :continue (error-mode a)))
    (send a (fn [_] (throw (ex-info "boom" {}))))
    (is (some? (agent-error a)))
    ;; In :continue mode, further sends do not throw on the failed
    ;; agent. mino's MVP captures only the latest err.
    (send a inc)))

(deftest agent-watch-throws-captured
  ;; Watch throws are captured into agent.err, matching JVM-ish
  ;; behavior where agent watch errors surface via agent-error.
  (let [a (agent 0)]
    (add-watch a :w (fn [_ _ _ _] (throw (ex-info "watch-boom" {}))))
    (send a inc)
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
  (is (thrown? (agent 0 :no-such-option 1)))
  ;; :meta is not yet supported on agents; throws explicitly.
  (is (thrown? (agent 0 :meta {:doc "x"}))))

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
    (is (= "boom" (second @seen)))
    (is (= a (first @seen)))
    (is (nil? (agent-error a)))
    ;; Subsequent sends still work since the agent isn't latched.
    (send a inc)
    (is (= 1 @a))))

(deftest agent-error-handler-throw-latches-agent
  ;; If the error-handler itself throws, the handler's exception
  ;; is captured into agent-error so the failure isn't silently
  ;; lost. The original action's exception is the one passed in;
  ;; this asserts only that *some* error is latched.
  (let [a (agent 0)]
    (set-error-handler! a (fn [_ _] (throw (ex-info "handler-boom" {}))))
    (send a (fn [_] (throw (ex-info "action-boom" {}))))
    (is (some? (agent-error a)))))

(deftest agent-error-handler-also-invoked-on-validator-reject
  ;; Validator failure is treated as an action failure.
  (let [seen (atom 0)
        a (agent 1)]
    (set-validator! a pos?)
    (set-error-handler! a (fn [_ _] (swap! seen inc)))
    (send a (fn [_] -1))
    (is (= 1 @seen))
    (is (nil? (agent-error a)))
    (is (= 1 @a))))

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
    (is (some? (agent-error a)))
    (is (thrown? (restart-agent a -99)))
    ;; Agent stays in failed state with original value untouched.
    (is (some? (agent-error a)))
    (is (= 1 @a))
    ;; A valid restart succeeds.
    (restart-agent a 42)
    (is (nil? (agent-error a)))
    (is (= 42 @a))))
