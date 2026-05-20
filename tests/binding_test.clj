(require "tests/test")

;; Bindings: def, let, lexical scope, redefinition.

(deftest def-then-read
  (def x__bt 41)
  (is (= 42 (+ x__bt 1))))

(deftest def-redefine
  (def r__bt 1)
  (is (= 1 r__bt))
  (def r__bt 2)
  (is (= 2 r__bt)))

(deftest let-binding
  (testing "single"
    (is (= 5 (let [x 5] x))))
  (testing "multi"
    (is (= 3 (let [x 1 y 2] (+ x y)))))
  (testing "sequential"
    (is (= 11 (let [x 1 y (+ x 10)] y))))
  (testing "shadow"
    (def a__bt 1)
    (is (= 99 (let [a__bt 99] a__bt)))
    (is (= 1 a__bt))))

(deftest var-redef-closure
  (def vrc-f (fn [] vrc-x))
  (def vrc-x 10)
  (is (= 10 (vrc-f)))
  (def vrc-x 20)
  (is (= 20 (vrc-f))))

(def ^:dynamic *bt-dyn-x* :outer)
(def bt-plain-x :outer)

(deftest binding-on-dynamic-var-rebinds
  (is (= :inner (binding [*bt-dyn-x* :inner] *bt-dyn-x*)))
  (is (= :outer *bt-dyn-x*)))

(deftest binding-on-non-dynamic-var-throws
  ;; JVM Clojure throws "Can't dynamically bind non-dynamic var" when
  ;; `binding` rebinds a var that wasn't declared ^:dynamic. mino used
  ;; to accept this silently, which masks the difference and lets
  ;; subtle bugs propagate to a real Clojure deployment that then
  ;; fails. Match the canon behaviour: fail loud at the binding site.
  (let [err (try (eval '(binding [bt-plain-x :inner] bt-plain-x)) nil
                 (catch e (if (map? e) (:mino/message e) (str e))))]
    (is (some? err))
    (is (some? (re-find #"dynamic" err)))
    (is (some? (re-find #"bt-plain-x" err)))))

;; Canon parity: bound? / thread-bound? / with-bindings / push+pop-thread-bindings.
;; The C primitives -var-root-bound? + -thread-bound? + with-bindings* +
;; get-thread-bindings + push/pop-thread-bindings* back the Clojure-level wrappers.

(def ^:dynamic *bt-dyn-y* :root)

(deftest bound?-checks-root-or-thread
  (is (bound? (var *bt-dyn-y*)))
  (binding [*bt-dyn-y* :inner]
    (is (bound? (var *bt-dyn-y*))))
  (is (bound? (var *bt-dyn-y*) (var bt-plain-x))))

(deftest thread-bound?-checks-only-thread
  (is (not (thread-bound? (var *bt-dyn-y*))))
  (binding [*bt-dyn-y* :inner]
    (is (thread-bound? (var *bt-dyn-y*))))
  (is (not (thread-bound? (var *bt-dyn-y*)))))

(deftest with-bindings-installs-and-pops
  (is (= :root *bt-dyn-y*))
  (with-bindings {(var *bt-dyn-y*) :wb-inner}
    (is (= :wb-inner *bt-dyn-y*)))
  (is (= :root *bt-dyn-y*)))

(deftest push-pop-thread-bindings-pair
  (is (= :root *bt-dyn-y*))
  (push-thread-bindings {(var *bt-dyn-y*) :pushed})
  (try
    (is (= :pushed *bt-dyn-y*))
    (finally
      (pop-thread-bindings)))
  (is (= :root *bt-dyn-y*)))

(deftest with-bindings-snapshot-via-get-thread-bindings
  ;; get-thread-bindings returns a map keyed by symbols; with-bindings*
  ;; accepts that snapshot directly. Used by bound-fn* to replay
  ;; captured bindings.
  (binding [*bt-dyn-y* :snap]
    (let [snap (get-thread-bindings)]
      (is (map? snap))
      ;; Re-apply outside the binding: with-bindings* takes the snapshot
      ;; and runs the thunk with those names installed.
      (is (= :snap (with-bindings* snap (fn [] *bt-dyn-y*)))))))

(deftest binding-frame-unwinds-on-throw
  ;; A throw inside a binding body must restore the outer value.
  (try
    (binding [*bt-dyn-y* :will-unwind]
      (throw :boom))
    (catch _e nil))
  (is (= :root *bt-dyn-y*)))
