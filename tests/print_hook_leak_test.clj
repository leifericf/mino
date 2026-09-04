(require "tests/test")
(require '[clojure.string :as str])

;; A print-method that throws unwinds by longjmp from the hook call
;; straight to the enclosing catch, skipping every C frame in the
;; print path. These tests pin what that unwind must not leak.
;;
;; The caught-throw loops run inline at the top level, not inside
;; deftest bodies or helper fns: the eager JIT lane today miscompiles
;; a caught print-method throw inside a compiled fn (an
;; unbound-gensym failure, tracked as its own bug), and keeping the
;; loops here keeps this file's assertions independent of that
;; defect.

(defmethod print-method :pin-leak-probe
  [v]
  (throw {:mino/kind :test/pin-leak-probe}))

(def ^:private pin-bomb (with-meta {:x 2} {:type :pin-leak-probe}))

(def ^:private first-round-result
  (try
    (with-out-str (pr "x" pin-bomb))
    :not-thrown
    (catch :test/pin-leak-probe e :caught)))

;; Each caught throw tears through the hook's C frame, which holds a
;; pinned capture sink. The catch landing pad must rewind the pin
;; watermark; before it did, this loop overflowed the fixed pin stack
;; (a loud abort under the sanitizer lanes, silent pin-array soft-loss
;; and a stale-root hazard otherwise).
(def ^:private pin-rounds-result
  (do (dotimes [_ 800]
        (try
          (with-out-str (pr "x" pin-bomb))
          (catch :test/pin-leak-probe e nil)))
      :completed))

(deftest pin-stack-rewound-per-caught-throw
  (is (= :caught first-round-result))
  (is (= :completed pin-rounds-result))
  (is (= "\"ok\"" (with-out-str (pr "ok")))))

(deftest print-usable-after-hook-throw
  (is (= "[1 2]" (with-out-str (pr [1 2]))))
  (is (= "1 2\n" (with-out-str (println 1 2)))))

(run-tests-and-exit)
