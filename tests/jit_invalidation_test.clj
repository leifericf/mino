(require "tests/test")

;; CPJIT invalidation / deopt torture suite.
;;
;; Each test warms a fn past the JIT threshold so the assertion sees
;; the native path on the JIT-enabled binary, then exercises a
;; cache-busting REPL operation (def rebind, var-set-root, ns-unmap
;; equivalent, binding cascades). The post-operation behavior must
;; match what JIT-off would produce on the same source. The runner
;; checks `./mino` and `./mino_nojit` stdout byte-for-byte; any
;; divergence surfaces in its own output.

(def +warm+ 200)

;; ---- def rebind invalidation -----------------------------------------

(defn caller-of-target [x] (target x))
(defn target [x] (* x 2))

(deftest def-rebind-invalidation
  ;; Warm caller and target.
  (dotimes [_ +warm+] (caller-of-target 5))
  (is (= 10 (caller-of-target 5)))
  ;; Rebind target.
  (def target (fn [x] (* x 100)))
  (is (= 500 (caller-of-target 5))))

;; ---- var-set-root invalidation ---------------------------------------

(defn rooted-callee [x] (+ x 1))
(defn rooted-caller [x] (rooted-callee x))

(deftest var-set-root-invalidation
  (dotimes [_ +warm+] (rooted-caller 7))
  (is (= 8 (rooted-caller 7)))
  ;; alter-var-root style rebind via def.
  (def rooted-callee (fn [x] (* x 10)))
  (is (= 70 (rooted-caller 7))))

;; ---- binding cascade with dyn-var ------------------------------------

(def ^:dynamic *amp* 1)
(defn dyn-callee [x] (* x *amp*))

(deftest binding-cascade
  (dotimes [_ +warm+] (dyn-callee 3))
  (is (= 3 (dyn-callee 3)))
  ;; Binding scope changes *amp* dynamically; jit'd dyn-callee must
  ;; see the binding-thread-local value, not a stale snapshot.
  (binding [*amp* 5]
    (is (= 15 (dyn-callee 3))))
  ;; After binding pops, original root restored.
  (is (= 3 (dyn-callee 3))))

;; ---- protocol-cached call after impl redef ---------------------------

(defprotocol IShape (area [s]))
(defrecord Square [side])
(extend-type Square IShape (area [s] (* (:side s) (:side s))))

(defn shape-area [s] (area s))

(deftest protocol-impl-redef
  (let [sq (->Square 4)]
    (dotimes [_ +warm+] (shape-area sq))
    (is (= 16 (shape-area sq)))
    ;; Redefine the impl with a different formula.
    (extend-type Square IShape (area [s] (* 2 (:side s))))
    (is (= 8 (shape-area sq)))))

;; ---- recursive fn redef while running --------------------------------

(defn rec-fn [n] (if (<= n 0) 0 (+ n (rec-fn (- n 1)))))

(deftest recursive-redef-after-warm
  (dotimes [_ +warm+] (rec-fn 5))
  (is (= 15 (rec-fn 5)))
  ;; Replace with a non-recursive impl.
  (def rec-fn (fn [n] (* n 100)))
  (is (= 500 (rec-fn 5))))

;; ---- caller chain across def rebind ----------------------------------

(defn chain-leaf [x] x)
(defn chain-mid  [x] (chain-leaf x))
(defn chain-top  [x] (chain-mid  x))

(deftest chain-leaf-redef
  (dotimes [_ +warm+] (chain-top 9))
  (is (= 9 (chain-top 9)))
  ;; Redef the LEAF; both jit'd top and jit'd mid must observe.
  (def chain-leaf (fn [x] (+ x 1000)))
  (is (= 1009 (chain-top 9))))

;; ---- repeated rebind churn -------------------------------------------

(defn churn-target [n] n)
(defn churn-caller [n] (churn-target n))

(deftest churn-rebind
  (dotimes [_ +warm+] (churn-caller 0))
  (is (= 0 (churn-caller 0)))
  ;; Rebind multiple times back-to-back; each must invalidate the
  ;; cached IC slot in churn-caller.
  (dotimes [i 5]
    (def churn-target (fn [n] (+ n (* i 100))))
    (is (= (* i 100) (churn-caller 0)))))

;; ---- argc-shift redef --------------------------------------------------

(defn shift-target [a] (* a 2))
(defn shift-caller [a] (shift-target a))

(deftest argc-shift-redef
  (dotimes [_ +warm+] (shift-caller 5))
  (is (= 10 (shift-caller 5)))
  ;; Redef with same arity but different shape.
  (def shift-target (fn [a] (* a 3)))
  (is (= 15 (shift-caller 5))))

;; ---- JIT loop cancellability -----------------------------------------

;; Each loop stencil polls mino_bc_safepoint every 256 iterations.
;; A spinning JIT'd loop must wake on (future-cancel f) within bounded
;; time even when the body is entirely native.

(defn long-dec-spin [n]
  (loop [i n] (if (zero? i) :done (recur (dec i)))))
(defn long-lt-spin [n]
  (loop [i 0] (if (< i n) (recur (inc i)) :done)))

(deftest jit-loop-cancel-dec
  (dotimes [_ +warm+] (long-dec-spin 100))
  (let [f     (future (long-dec-spin 1000000000))
        start (time-ms)]
    (thread-sleep 30)
    (future-cancel f)
    (thread-sleep 100)
    (is (future-cancelled? f))
    (is (future-done? f))
    ;; Full run would take seconds; cancel must land well below that.
    (is (< (- (time-ms) start) 500))))

(deftest jit-loop-cancel-lt
  (dotimes [_ +warm+] (long-lt-spin 100))
  (let [f     (future (long-lt-spin 1000000000))
        start (time-ms)]
    (thread-sleep 30)
    (future-cancel f)
    (thread-sleep 100)
    (is (future-cancelled? f))
    (is (future-done? f))
    (is (< (- (time-ms) start) 500))))

;; The bit-xor accumulator keeps this shape out of the fused-loop
;; matcher, so the body compiles to generic ops with a direct-emit
;; backward jump -- the OP_SAFEPOINT_POLL path.
(defn long-generic-spin [n]
  (loop [i 0 acc 0]
    (if (< i n) (recur (inc i) (bit-xor acc i)) acc)))

(deftest jit-loop-cancel-generic
  ;; Regression: generic (non-fused) JIT'd loops carried no safepoint
  ;; poll on their backward jumps, so a spinning future was
  ;; uncancellable and never yielded the state lock. The emit pass now
  ;; plants a safepoint stencil before every backward OP_JMP /
  ;; OP_JMPIFNOT, mirroring the interpreter's poll-on-backward-jump
  ;; rule.
  (dotimes [_ +warm+] (long-generic-spin 100))
  (let [f     (future (long-generic-spin 1000000000))
        start (time-ms)]
    (thread-sleep 30)
    (future-cancel f)
    (thread-sleep 100)
    (is (future-cancelled? f))
    (is (future-done? f))
    (is (< (- (time-ms) start) 500))))

(run-tests-and-exit)
