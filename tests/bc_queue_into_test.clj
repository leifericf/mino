(require "tests/test")

;; Regression test for: bytecode shape loses elements in (into q [3 4])
;; after cross-file load.
;;
;; The original symptom was that a BC-compiled function body containing
;; let bindings, inline map literals, and (apply = ...) could cause a
;; sibling queue test's (into q [3 4]) to silently drop elements.
;;
;; Root cause: a GC-root pinning gap in dyn_binding_make allowed
;; collection values to be swept between the symbol intern and the
;; binding snapshot under a minor GC cycle.  The queue's register
;; appeared correct at the call site but the cons-spine built by
;; argv_to_cons inside prim_conj could reference already-swept memory.
;;
;; The fixes on this branch (cdc17d39 and siblings) pin the affected
;; roots.  This file pins the observable contract so a regression
;; surfaces as a test failure rather than a silent wrong answer.

;; --- helpers used to exercise the original trigger shape ---

;; Simulates the assert-predicate expansion shape: let bindings,
;; (list ...) to build a values collection, (apply = ...) to compare,
;; and inline map literals for the two reporting branches.  This is the
;; compiled shape that, before the fix, could leave a sibling function's
;; queue register pointing at swept memory.
(defn- assert-pred-shape [a b]
  (let [values (list a b)
        result (apply = values)]
    (if result
      {:type :pass :expected a :actual b}
      {:type :fail :expected a :actual b})
    result))

;; --- queue construction via 3-arg conj ---

(deftest bc-queue-conj-three-arg
  (testing "three-arg conj on empty queue produces two-element queue"
    (defn q3 [] (conj clojure.lang.PersistentQueue/EMPTY 1 2))
    (is (= [1 2] (vec (q3)))))
  (testing "queue count is correct"
    (defn qc [] (count (conj clojure.lang.PersistentQueue/EMPTY 1 2)))
    (is (= 2 (qc)))))

;; --- into preserves all elements ---

(deftest bc-queue-into-retains-elements
  (testing "into adds all vector elements to a queue"
    (defn qi []
      (let [q (conj clojure.lang.PersistentQueue/EMPTY 1 2)]
        (into q [3 4])))
    (is (= [1 2 3 4] (vec (qi)))))
  (testing "into on empty queue produces queue of vector elements"
    (defn qi2 []
      (into clojure.lang.PersistentQueue/EMPTY [1 2 3]))
    (is (= [1 2 3] (vec (qi2)))))
  (testing "into with single element preserves original"
    (defn qi3 []
      (let [q (conj clojure.lang.PersistentQueue/EMPTY 10)]
        (into q [20])))
    (is (= [10 20] (vec (qi3))))))

;; --- original trigger shape: apply = with inline maps in same fn ---

(deftest bc-queue-survives-assert-pred-shape
  (testing "queue value correct after assert-predicate-style expansion in same fn"
    (defn qt []
      (let [q (conj clojure.lang.PersistentQueue/EMPTY 1 2)]
        ;; This assertion-style let/apply shape was the original trigger.
        (assert-pred-shape q [1 2])
        ;; After the trigger shape executes, q must still hold its value.
        (vec (into q [3 4]))))
    (is (= [1 2 3 4] (qt))))
  (testing "multiple apply-= calls before into do not corrupt queue register"
    (defn qt2 []
      (let [q  (conj clojure.lang.PersistentQueue/EMPTY 10 20)
            _1 (assert-pred-shape (count q) 2)
            _2 (assert-pred-shape (first q) 10)
            _3 (assert-pred-shape (vec q) [10 20])]
        (vec (into q [30 40]))))
    (is (= [10 20 30 40] (qt2)))))

;; --- sequential equality matches vectors and lists ---

(deftest bc-queue-sequential-eq
  (testing "queue equals vector with same elements"
    (defn qeq-vec []
      (let [q (conj clojure.lang.PersistentQueue/EMPTY 1 2)]
        (= q [1 2])))
    (is (qeq-vec)))
  (testing "queue equals list with same elements"
    (defn qeq-list []
      (let [q (conj clojure.lang.PersistentQueue/EMPTY 1 2)]
        (= q (list 1 2))))
    (is (qeq-list))))

(run-tests-and-exit)
