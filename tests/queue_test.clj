(require "tests/test")

;; PersistentQueue: two-list persistent FIFO.
;;
;; clojure.lang.PersistentQueue/EMPTY is the canonical empty value;
;; conj appends to the back, peek returns the next-to-pop, pop returns
;; the queue without its head. seq walks elements in deque order.

(deftest empty-queue-shape
  (let [q clojure.lang.PersistentQueue/EMPTY]
    (is (queue? q))
    (is (= 0 (count q)))
    (is (nil? (peek q)))
    (is (nil? (seq q)))
    (is (= "#queue []" (pr-str q)))
    (is (= :queue (type q)))))

(deftest conj-appends-to-back
  (let [q (-> clojure.lang.PersistentQueue/EMPTY
              (conj 1) (conj 2) (conj 3))]
    (is (= 3 (count q)))
    (is (= 1 (peek q)))
    (is (= '(1 2 3) (seq q)))
    (is (= "#queue [1 2 3]" (pr-str q)))))

(deftest conj-multiple-in-one-call
  (let [q (conj clojure.lang.PersistentQueue/EMPTY 1 2 3)]
    (is (= 3 (count q)))
    (is (= '(1 2 3) (seq q)))))

(deftest pop-removes-from-front
  (let [q (conj clojure.lang.PersistentQueue/EMPTY 1 2 3)]
    (is (= 1 (peek q)))
    (is (= '(2 3) (seq (pop q))))
    (is (= 2 (peek (pop q))))
    (is (= '(3) (seq (pop (pop q)))))
    (is (= 0 (count (pop (pop (pop q))))))))

(deftest pop-empty-returns-empty-queue
  ;; pop is total on queues: popping the empty queue yields the empty
  ;; queue (unlike vectors and lists, where it throws).
  (is (= clojure.lang.PersistentQueue/EMPTY
         (pop clojure.lang.PersistentQueue/EMPTY))))

(deftest seq-walks-in-deque-order
  (let [q (-> clojure.lang.PersistentQueue/EMPTY
              (conj :a) (conj :b) (conj :c) (conj :d))]
    (is (= '(:a :b :c :d) (seq q)))
    (is (= [:a :b :c :d] (vec q)))
    (is (= 4 (count q)))))

(deftest first-and-rest-on-queue
  (let [q (-> clojure.lang.PersistentQueue/EMPTY (conj 1) (conj 2))]
    ;; first is peek-equivalent; rest is the seq of everything after.
    (is (= 1 (first q)))
    (is (= '(2) (rest q)))))

(deftest empty-returns-empty-queue
  (let [q (conj clojure.lang.PersistentQueue/EMPTY 1 2)
        e (empty q)]
    (is (queue? e))
    (is (= 0 (count e)))
    (is (= "#queue []" (pr-str e)))))

(deftest queue-equality
  (let [a (-> clojure.lang.PersistentQueue/EMPTY (conj 1) (conj 2) (conj 3))
        b (conj clojure.lang.PersistentQueue/EMPTY 1 2 3)
        c (-> clojure.lang.PersistentQueue/EMPTY (conj 1) (conj 2))]
    (is (= a b))
    (is (not= a c))
    (is (= clojure.lang.PersistentQueue/EMPTY (empty a)))))

(deftest queue-equals-sequentials
  ;; A queue takes part in sequential equality: it is = to a list or
  ;; vector with the same elements in the same order.
  (is (= '(1 2 3) (conj clojure.lang.PersistentQueue/EMPTY 1 2 3)))
  (is (= [1 2 3] (conj clojure.lang.PersistentQueue/EMPTY 1 2 3)))
  (is (not= '(1 2) (conj clojure.lang.PersistentQueue/EMPTY 1 2 3))))

(deftest queue-as-fifo-after-pop-rotation
  ;; After several conj+pop cycles, the front-list eventually empties
  ;; and the back-list must rotate. Verify deque-order is preserved.
  (let [q1 (-> clojure.lang.PersistentQueue/EMPTY
               (conj 1) (conj 2) (conj 3) (conj 4) (conj 5))
        q2 (pop (pop (pop q1)))]
    (is (= 2 (count q2)))
    (is (= 4 (peek q2)))
    (is (= '(4 5) (seq q2)))
    ;; Conj after rotation puts new elements after the rotated front.
    (let [q3 (conj q2 6 7)]
      (is (= 4 (count q3)))
      (is (= '(4 5 6 7) (seq q3))))))

(deftest queue-empty?
  (is (empty? clojure.lang.PersistentQueue/EMPTY))
  (is (not (empty? (conj clojure.lang.PersistentQueue/EMPTY 1)))))
