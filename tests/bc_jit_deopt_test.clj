(require "tests/test")

;; Regression-protective tests for the CPJIT deopt contract.
;;
;; The runtime invalidates a JIT-compiled native region when its
;; `native_gen` snapshot no longer matches `S->ic_gen`. ic_gen advances
;; on def / ns-unmap / var_set_root / var_unintern; the dispatch path
;; in apply_callable detects the staleness and drops the runtime-
;; visible native pointer so the next call falls through to the
;; interpreter. The fn's hot counter resets to 0 -- a fresh compile
;; needs the full threshold of subsequent calls.
;;
;; These tests pin the externally-observable contract: identical
;; results before and after redefinition. The mechanism is the same
;; with or without -DMINO_CPJIT=1 because the deopt branch in
;; apply_callable lives outside the build flag.

(def ^:dynamic *warm-iterations* 200)

(defn- warm [f n]
  (loop [i 0] (when (< i n) (f i) (recur (+ i 1)))))

(deftest jit-deopt-survives-def
  (def jit-deopt-target (fn [x] x))
  (warm jit-deopt-target *warm-iterations*)
  (is (= (jit-deopt-target 11) 11))
  ;; Bump ic_gen via a fresh def. The next call hits the dispatch-
  ;; entry stale-snapshot check.
  (def jit-deopt-sentinel 1)
  (is (= (jit-deopt-target 12) 12))
  ;; Re-warming returns identical results post-recompile.
  (warm jit-deopt-target *warm-iterations*)
  (is (= (jit-deopt-target 13) 13)))

(deftest jit-deopt-survives-redefine
  (def jit-redef-f (fn [x] x))
  (warm jit-redef-f *warm-iterations*)
  (is (= (jit-redef-f :a) :a))
  ;; Redefining a different var still bumps ic_gen.
  (def jit-redef-other (fn [y] y))
  (is (= (jit-redef-f :b) :b))
  (warm jit-redef-f *warm-iterations*)
  (is (= (jit-redef-f :c) :c)))

(deftest jit-deopt-batch-of-defs
  (def jit-batch-f (fn [x] x))
  (warm jit-batch-f *warm-iterations*)
  ;; Many defs in a row: ic_gen jumps many times but the dispatch
  ;; path still observes mismatch exactly once per call, so the
  ;; deopt is idempotent.
  (def jit-batch-a 1)
  (def jit-batch-b 2)
  (def jit-batch-c 3)
  (is (= (jit-batch-f 42) 42))
  (warm jit-batch-f *warm-iterations*)
  (is (= (jit-batch-f 43) 43)))

(deftest jit-const-fn-deopt
  ;; Constant-returning fn: the body is OP_LOAD_K + OP_RETURN. After
  ;; ic_gen advances the JIT'd code is invalidated and the
  ;; interpreter takes over until the fn re-warms.
  (def jit-const (fn [] 99))
  (warm (fn [_] (jit-const)) *warm-iterations*)
  (is (= (jit-const) 99))
  (def jit-const-sentinel :x)
  (is (= (jit-const) 99))
  (warm (fn [_] (jit-const)) *warm-iterations*)
  (is (= (jit-const) 99)))
