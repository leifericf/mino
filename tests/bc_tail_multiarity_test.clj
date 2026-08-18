(require "tests/test")

;; Tail calls through multi-arity fns must stay flat on the C stack.
;; The bc trampoline used to bail to the recursive apply path for any
;; target whose params field is NULL (the multi-arity shape), growing
;; one native frame per hop and raising MLM004 near ten thousand
;; iterations. mino_bc_run dispatches clauses itself, so the
;; trampoline can re-enter a multi-arity target directly.

(defn countdown
  "Self tail call that hops between the two fixed arities."
  ([n] (countdown n 0))
  ([n acc] (if (pos? n) (countdown (dec n) (inc acc)) acc)))

(defn countdown-rest
  "Self tail call into the variadic clause."
  ([n] (countdown-rest n 0))
  ([n acc & _ignored] (if (pos? n) (countdown-rest (dec n) (inc acc)) acc)))

(defn ping
  "Mutual tail recursion between a single-arity and a multi-arity fn."
  [n]
  (if (pos? n) (pong (dec n)) [:ping 0]))

(defn pong
  ([n] (ping (dec n)))
  ([n tag] (if (pos? n) (ping (dec n)) [:pong n tag])))

(def deep 100000)

(deftest multi-arity-self-tail-stays-flat
  (is (= deep (countdown deep))))

(deftest cross-arity-tail-hops-stay-flat
  ;; Every iteration crosses the ([n]) -> ([n acc]) boundary.
  (is (= deep (countdown deep 0))))

(deftest variadic-clause-tail-stays-flat
  (is (= deep (countdown-rest deep))))

(deftest mutual-tail-recursion-with-multi-arity-stays-flat
  (is (= [:ping 0] (ping deep))))

(deftest multi-arity-arity-errors-preserved
  (is (thrown? (countdown)))
  (is (thrown? (countdown 1 2 3))))

(run-tests-and-exit)
