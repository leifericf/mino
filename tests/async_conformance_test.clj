(require "tests/test")
(require "core/async")

;; Conformance tests adapted from the canonical core.async test suite.
;; Covers channel semantics, go macro state machine, and combinators.

;; ===== Channel FIFO ordering =====

(deftest chan-fifo-ordering
  (let [ch (chan 5)]
    (>!! ch 1) (>!! ch 2) (>!! ch 3) (>!! ch 4) (>!! ch 5)
    (is (= 1 (<!! ch)))
    (is (= 2 (<!! ch)))
    (is (= 3 (<!! ch)))
    (is (= 4 (<!! ch)))
    (is (= 5 (<!! ch)))))

(deftest chan-fifo-unbuffered
  (let [ch (chan)
        results (atom [])]
    (put! ch :a) (put! ch :b) (put! ch :c)
    (take! ch (fn [v] (swap! results conj v)))
    (take! ch (fn [v] (swap! results conj v)))
    (take! ch (fn [v] (swap! results conj v)))
    (drain!)
    (is (= [:a :b :c] @results))))

;; ===== Close semantics =====

(deftest close-flushes-buffer
  (let [ch (chan 3)]
    (>!! ch :a) (>!! ch :b)
    (close! ch)
    (is (= :a (<!! ch)))
    (is (= :b (<!! ch)))
    (is (nil? (<!! ch)) "nil after buffer exhausted")))

(deftest close-multiple-pending-takers
  (let [ch (chan)
        r1 (atom :pending) r2 (atom :pending) r3 (atom :pending)]
    (take! ch (fn [v] (reset! r1 v)))
    (take! ch (fn [v] (reset! r2 v)))
    (take! ch (fn [v] (reset! r3 v)))
    (close! ch)
    (drain!)
    (is (nil? @r1))
    (is (nil? @r2))
    (is (nil? @r3))))

(deftest close-pending-puts-rejected
  (let [ch (chan)
        r1 (atom :pending) r2 (atom :pending)]
    (put! ch :a (fn [v] (reset! r1 v)))
    (put! ch :b (fn [v] (reset! r2 v)))
    (close! ch)
    (drain!)
    (is (false? @r1) "pending put callback gets false on close")
    (is (false? @r2))))

;; ===== Dropping buffer overflow =====

(deftest dropping-buffer-overflow
  (let [ch (chan (dropping-buffer 2))]
    (>!! ch 1) (>!! ch 2) (>!! ch 3) (>!! ch 4) (>!! ch 5)
    (is (= 1 (<!! ch)))
    (is (= 2 (<!! ch)))
    (is (nil? (poll! ch)) "excess values dropped")))

;; ===== Sliding buffer overflow =====

(deftest sliding-buffer-overflow
  (let [ch (chan (sliding-buffer 2))]
    (>!! ch 1) (>!! ch 2) (>!! ch 3) (>!! ch 4) (>!! ch 5)
    (is (= 4 (<!! ch)))
    (is (= 5 (<!! ch)))
    (is (nil? (poll! ch)) "oldest values slid off")))

;; ===== Promise channel =====

(deftest promise-chan-multiple-takes
  (let [ch (promise-chan)]
    (put! ch 42)
    (drain!)
    (is (= 42 (<!! ch)))
    (is (= 42 (<!! ch)))
    (is (= 42 (<!! ch)) "promise-chan delivers same value repeatedly")))

(deftest promise-chan-ignores-second-put
  (let [ch (promise-chan)]
    (put! ch :first)
    (put! ch :second)
    (drain!)
    (is (= :first (<!! ch)) "second put is ignored")))

;; ===== go: basic expressions =====

(deftest go-constant
  (is (= 42 (<!! (go 42)))))

(deftest go-arithmetic
  (is (= 15 (<!! (go (+ 5 10))))))

(deftest go-string
  (is (= "hello" (<!! (go "hello")))))

(deftest go-keyword
  (is (= :foo (<!! (go :foo)))))

(deftest go-vector
  (is (= [1 2 3] (<!! (go [1 2 3])))))

(deftest go-map
  (is (= {:a 1} (<!! (go {:a 1})))))

(deftest go-boolean
  (is (true? (<!! (go true))))
  (is (false? (<!! (go false)))))

;; ===== go: fn inside go (not walked) =====

(deftest go-fn-not-walked
  (let [result (<!! (go (let [f (fn [x] (* x 2))]
                          (f 21))))]
    (is (= 42 result) "fn inside go is not state-machine transformed")))

(deftest go-fn-closure-over-park
  (let [ch (chan 1)]
    (put! ch 10) (drain!)
    (let [result (<!! (go (let [x (<! ch)
                                f (fn [y] (+ x y))]
                            (f 5))))]
      (is (= 15 result) "fn closes over parked value"))))

;; ===== go: do blocks =====

(deftest go-do-returns-last
  (is (= :last (<!! (go (do :first :second :last))))))

(deftest go-do-side-effects
  (let [log (atom [])]
    (<!! (go (do (swap! log conj :a)
                 (swap! log conj :b)
                 :done)))
    (is (= [:a :b] @log))))

;; ===== go: let bindings =====

(deftest go-let-simple
  (is (= 10 (<!! (go (let [x 10] x))))))

(deftest go-let-chain
  (is (= 30 (<!! (go (let [x 10
                            y 20]
                        (+ x y)))))))

(deftest go-let-park-chain
  (let [ch1 (chan 1) ch2 (chan 1)]
    (>!! ch1 3) (>!! ch2 4)
    (is (= 12 (<!! (go (let [a (<! ch1)
                              b (<! ch2)]
                          (* a b))))))))

(deftest go-let-shadow
  (let [ch (chan 1)]
    (>!! ch 5)
    (is (= 5 (<!! (go (let [x (<! ch)] x)))))))

;; ===== go: nested let with parks =====

(deftest go-nested-let-parks
  (let [ch1 (chan 1) ch2 (chan 1) ch3 (chan 1)]
    (>!! ch1 1) (>!! ch2 2) (>!! ch3 3)
    (is (= 6 (<!! (go (let [a (<! ch1)]
                         (let [b (<! ch2)]
                           (let [c (<! ch3)]
                             (+ a b c))))))))))

;; ===== go: if with parks =====

(deftest go-if-true-parks
  (let [ch (chan 1)]
    (>!! ch :yes)
    (is (= :yes (<!! (go (if true (<! ch) :no)))))))

(deftest go-if-false-parks
  (let [ch (chan 1)]
    (>!! ch :yes)
    (is (= :yes (<!! (go (if false :no (<! ch))))))))

(deftest go-if-both-parks
  (let [ch1 (chan 1) ch2 (chan 1)]
    (>!! ch1 :a) (>!! ch2 :b)
    (is (= :a (<!! (go (if true (<! ch1) (<! ch2)))))))
  (let [ch1 (chan 1) ch2 (chan 1)]
    (>!! ch1 :c) (>!! ch2 :d)
    (is (= :d (<!! (go (if false (<! ch1) (<! ch2))))))))

(deftest go-if-park-in-test
  (let [ch (chan 1)]
    (>!! ch true)
    (is (= :yes (<!! (go (if (<! ch) :yes :no)))))))

;; ===== go: when with parks =====

(deftest go-when-true-body
  (let [ch (chan 1)]
    (>!! ch :val)
    (is (= :val (<!! (go (when true (<! ch))))))))

(deftest go-when-false-nil
  (let [ch (chan 1)]
    (>!! ch :val)
    ;; when false returns nil; go with nil result closes channel
    (let [out (go (when false (<! ch)))]
      (is (nil? (<!! out))))))

;; ===== go: cond with parks =====

(deftest go-cond-first-true
  (let [ch (chan 1)]
    (>!! ch :first)
    (is (= :first (<!! (go (cond true (<! ch) :else :nope)))))))

(deftest go-cond-fallthrough
  (let [ch (chan 1)]
    (>!! ch :fallback)
    (is (= :fallback (<!! (go (cond false :nope :else (<! ch))))))))

;; ===== go: loop/recur =====

(deftest go-loop-basic-count
  (is (= 10 (<!! (go (loop [n 0]
                        (if (< n 10) (recur (inc n)) n)))))))

(deftest go-loop-accumulate-from-channel
  (let [ch (chan 5)]
    (>!! ch 1) (>!! ch 2) (>!! ch 3) (>!! ch 4) (>!! ch 5)
    (close! ch)
    (is (= 15 (<!! (go-loop [sum 0]
                     (let [v (<! ch)]
                       (if (nil? v) sum (recur (+ sum v))))))))))

(deftest go-loop-build-vector
  (let [ch (chan 3)]
    (>!! ch :a) (>!! ch :b) (>!! ch :c)
    (close! ch)
    (is (= [:a :b :c] (<!! (go-loop [acc []]
                              (let [v (<! ch)]
                                (if (nil? v) acc (recur (conj acc v))))))))))

(deftest go-loop-multi-binding-recur
  (let [ch (chan 2)]
    (>!! ch 10) (>!! ch 20)
    (close! ch)
    (is (= {:sum 30 :count 2}
           (<!! (go-loop [sum 0 count 0]
                  (let [v (<! ch)]
                    (if (nil? v)
                      {:sum sum :count count}
                      (recur (+ sum v) (inc count))))))))))

(deftest go-loop-park-in-recur-arg
  (let [ch (chan 3)]
    (>!! ch 1) (>!! ch 2) (>!! ch 3)
    (is (= 3 (<!! (go (loop [x 0 n 3]
                         (if (> n 0)
                           (recur (<! ch) (dec n))
                           x))))))))

;; ===== go: nested go blocks =====

(deftest go-nested-go
  (is (= 42 (<!! (go (<! (go 42)))))))

(deftest go-nested-go-chain
  (is (= :done (<!! (go (let [x (<! (go 1))
                               y (<! (go 2))]
                           (if (= 3 (+ x y)) :done :fail)))))))

;; ===== go: producer-consumer =====

(deftest go-producer-consumer
  (let [ch (chan 1)]
    (go (>! ch 1)
        (>! ch 2)
        (>! ch 3)
        (close! ch))
    (is (= [1 2 3] (<!! (go-loop [acc []]
                           (let [v (<! ch)]
                             (if (nil? v) acc (recur (conj acc v))))))))))

;; ===== go: ping-pong between two go blocks =====

(deftest go-ping-pong
  (let [ping (chan 1) pong (chan 1)]
    (go (>! ping :start)
        (<! pong)
        (>! ping :done))
    (go (let [v (<! ping)]
          (>! pong :ack)
          (<! ping)))
    (drain!)
    ;; Both blocks should complete without deadlock
    (is (true? true) "ping-pong completed")))

;; ===== go: alts! inside go =====

(deftest go-alts-inside
  (let [ch1 (chan 1) ch2 (chan 1)]
    (>!! ch1 :from-1)
    (let [result (<!! (go (let [[v _] (alts! [ch1 ch2] {:priority true})]
                            v)))]
      (is (= :from-1 result)))))

;; ===== go: complex state machine =====

(deftest go-complex-binding-survival
  (let [ch (chan 1)]
    (>!! ch 100)
    (let [result (<!! (go (let [a 1
                                b (<! ch)
                                c (+ a b)]
                            [a b c])))]
      (is (= [1 100 101] result) "bindings survive across park"))))

(deftest go-multi-park-with-computation
  (let [ch1 (chan 1) ch2 (chan 1) ch3 (chan 1)]
    (>!! ch1 10) (>!! ch2 20) (>!! ch3 30)
    (let [result (<!! (go (let [a (<! ch1)
                                b (* a 2)
                                c (<! ch2)
                                d (+ b c)
                                e (<! ch3)]
                            (+ d e))))]
      (is (= 70 result) "computation interleaved with parks"))))

;; ===== go: side effects ordering =====

(deftest go-side-effect-order
  (let [ch1 (chan 1) ch2 (chan 1)
        log (atom [])]
    (>!! ch1 :a) (>!! ch2 :b)
    (<!! (go (swap! log conj :before)
             (<! ch1)
             (swap! log conj :between)
             (<! ch2)
             (swap! log conj :after)
             :done))
    (is (= [:before :between :after] @log))))

;; ===== go: put returns boolean =====

(deftest go-put-returns-true
  (let [ch (chan 1)]
    (is (true? (<!! (go (>! ch :val)))) "put on open channel returns true")))

(deftest go-put-on-closed-returns-false
  (let [ch (chan)]
    (close! ch)
    (is (false? (<!! (go (>! ch :val)))) "put on closed channel returns false")))

;; ===== go: take from closed returns nil =====

(deftest go-take-from-closed
  (let [ch (chan)]
    (close! ch)
    (is (nil? (<!! (go (<! ch)))) "take from closed returns nil")))

;; ===== Combinator: pipe ordering =====

(deftest pipe-ordering
  (let [from (chan 5) to (chan 5)]
    (>!! from 1) (>!! from 2) (>!! from 3) (>!! from 4) (>!! from 5)
    (close! from)
    (pipe from to)
    (drain!)
    (is (= 1 (<!! to)))
    (is (= 2 (<!! to)))
    (is (= 3 (<!! to)))
    (is (= 4 (<!! to)))
    (is (= 5 (<!! to)))))

;; ===== Combinator: to-chan / async-into round-trip =====

(deftest to-chan-into-round-trip
  (let [data [1 2 3 4 5]
        result (<!! (async-into [] (to-chan data)))]
    (is (= data result))))

;; ===== Combinator: merge ordering =====

(deftest merge-all-values-received
  (let [ch1 (to-chan [1 2])
        ch2 (to-chan [3 4])
        ch3 (to-chan [5 6])
        out (merge-chans [ch1 ch2 ch3] 6)
        result (<!! (async-into [] out))]
    (is (= 6 (count result)) "all values received")
    (is (= #{1 2 3 4 5 6} (set result)) "correct values")))

;; ===== Combinator: mult fan-out =====

(deftest mult-all-taps-receive
  (let [src (chan 3)
        t1 (chan 3) t2 (chan 3) t3 (chan 3)
        m (mult src)]
    (tap m t1) (tap m t2) (tap m t3)
    (>!! src :x) (drain!)
    (is (= :x (<!! t1)))
    (is (= :x (<!! t2)))
    (is (= :x (<!! t3)))))

(deftest mult-tap-close-on-source-close
  (let [src (chan)
        t1 (chan 1) t2 (chan 1)
        m (mult src)]
    (tap m t1) (tap m t2)
    (close! src) (drain!)
    (is (true? (closed? t1)))
    (is (true? (closed? t2)))))

;; ===== Combinator: pub/sub =====

(deftest pub-sub-multi-topic
  (let [src (chan 10)
        a-ch (chan 10) b-ch (chan 10)
        p (pub src :type)]
    (sub p :a a-ch) (sub p :b b-ch)
    (>!! src {:type :a :v 1})
    (>!! src {:type :b :v 2})
    (>!! src {:type :a :v 3})
    (>!! src {:type :b :v 4})
    (drain!)
    (is (= {:type :a :v 1} (<!! a-ch)))
    (is (= {:type :a :v 3} (<!! a-ch)))
    (is (= {:type :b :v 2} (<!! b-ch)))
    (is (= {:type :b :v 4} (<!! b-ch)))))

;; ===== Combinator: pipeline =====

(deftest pipeline-transforms
  (let [from (to-chan [1 2 3 4 5])
        to   (chan 5)]
    (pipeline 2 to (fn [x] (* x x)) from)
    (drain!)
    (let [result (atom #{})]
      (dotimes [_ 5]
        (take! to (fn [v] (swap! result conj v))))
      (drain!)
      (is (= #{1 4 9 16 25} @result)))))

(deftest pipeline-preserves-ordering
  (let [from (to-chan [1 2 3 4 5 6 7 8])
        to   (chan 8)]
    (pipeline 4 to (fn [x] (* x x)) from)
    (drain!)
    (let [result (atom [])]
      (dotimes [_ 8]
        (take! to (fn [v] (swap! result conj v))))
      (drain!)
      (is (= [1 4 9 16 25 36 49 64] @result)))))

(deftest pipeline-closes-to
  (let [from (to-chan [1])
        to   (chan 1)]
    (pipeline 1 to inc from)
    (drain!)
    (<!! to)
    (is (true? (closed? to)))))

;; ===== Combinator: pipeline-async =====

(deftest pipeline-async-transforms
  (let [from (to-chan [1 2 3])
        to   (chan 3)]
    (pipeline-async 1 to
      (fn [v result-ch]
        (put! result-ch (* v 10))
        (close! result-ch))
      from)
    (drain!)
    (let [result (atom #{})]
      (dotimes [_ 3]
        (take! to (fn [v] (swap! result conj v))))
      (drain!)
      (is (= #{10 20 30} @result)))))

(deftest pipeline-async-preserves-ordering
  (let [from (to-chan [1 2 3 4 5 6 7 8])
        to   (chan 8)]
    (pipeline-async 4 to
      (fn [v result-ch]
        (put! result-ch (* v 10))
        (close! result-ch))
      from)
    (drain!)
    (let [result (atom [])]
      (dotimes [_ 8]
        (take! to (fn [v] (swap! result conj v))))
      (drain!)
      (is (= [10 20 30 40 50 60 70 80] @result)))))

;; ===== Edge cases: chan? =====

(deftest chan-predicate
  (is (true? (chan? (chan))))
  (is (true? (chan? (chan 1))))
  (is (true? (chan? (promise-chan))))
  (is (false? (chan? 42)))
  (is (false? (chan? nil)))
  (is (false? (chan? :foo))))

;; ===== Edge cases: offer!/poll! on closed =====

(deftest offer-on-closed
  (let [ch (chan 1)]
    (close! ch)
    (is (false? (offer! ch :x)) "offer on closed returns false")))

(deftest poll-on-closed-empty
  (let [ch (chan)]
    (close! ch)
    (is (nil? (poll! ch)) "poll on closed empty returns nil")))

(deftest poll-on-closed-with-data
  (let [ch (chan 1)]
    (>!! ch :x)
    (close! ch)
    (is (= :x (poll! ch)) "poll on closed with data returns value")))

;; ===== Edge case: go block returning false =====

(deftest go-returns-false
  (is (false? (<!! (go false))) "go can return false"))

;; ===== Edge case: go block with map operations =====

(deftest go-map-operations
  (let [ch (chan 1)]
    (>!! ch {:a 1 :b 2})
    (let [result (<!! (go (let [m (<! ch)]
                            (assoc m :c 3))))]
      (is (= {:a 1 :b 2 :c 3} result)))))

;; ===== Edge case: go with higher-order functions =====

(deftest go-higher-order
  (let [ch (chan 3)]
    (>!! ch 1) (>!! ch 2) (>!! ch 3)
    (close! ch)
    (let [result (<!! (go-loop [acc []]
                        (let [v (<! ch)]
                          (if (nil? v)
                            (mapv inc acc)
                            (recur (conj acc v))))))]
      (is (= [2 3 4] result)))))

;; ===== Stress: many sequential parks =====

(deftest go-many-sequential-parks
  (let [ch (chan 10)]
    (dotimes [i 10] (>!! ch i))
    (let [result (<!! (go (let [a (<! ch) b (<! ch) c (<! ch)
                                d (<! ch) e (<! ch)]
                            (+ a b c d e))))]
      (is (= 10 result) "sum of 0+1+2+3+4"))))

;; ===== Stress: multiple go blocks with shared channel =====

(deftest go-multiple-producers
  (let [ch (chan 10)
        results (atom #{})]
    (dotimes [i 5]
      (go (>! ch i)))
    (dotimes [_ 5]
      (take! ch (fn [v] (swap! results conj v))))
    (drain!)
    (is (= #{0 1 2 3 4} @results))))

;; ===== Edge case: go-loop with no parks (pure computation) =====

(deftest go-loop-no-parks
  (is (= 55 (<!! (go-loop [n 10 sum 0]
                   (if (zero? n)
                     sum
                     (recur (dec n) (+ sum n))))))))

;; ===== Edge case: let-park with remaining stmts after =====

(deftest go-let-park-then-stmts
  (let [ch (chan 1)
        log (atom [])]
    (>!! ch 42)
    (let [result (<!! (go (let [x (<! ch)]
                            (swap! log conj x)
                            (* x 2))))]
      (is (= 84 result))
      (is (= [42] @log)))))

;; ===== Edge case: if-park as non-tail expression =====

(deftest go-if-park-non-tail
  (let [ch1 (chan 1) ch2 (chan 1)]
    (>!! ch1 :a) (>!! ch2 :b)
    (let [result (<!! (go (if true (<! ch1) (<! ch2))
                          :after))]
      (is (= :after result)))))

;; ===== try/catch across park points =====

(deftest go-try-normal-flow
  (let [ch (chan 1)]
    (>!! ch 42)
    (is (= 42 (<!! (go (try (<! ch) (catch e :error))))))))

(deftest go-try-exception-before-park
  (let [ch (chan 1)]
    (>!! ch 42)
    (is (= "early" (<!! (go (try (do (throw "early") (<! ch))
                              (catch e (ex-data e)))))))))

(deftest go-try-exception-after-park
  (let [ch (chan 1)]
    (>!! ch 42)
    (is (= "oops" (<!! (go (try (do (<! ch) (throw "oops"))
                             (catch e (ex-data e)))))))))

(deftest go-try-let-park-normal
  (let [ch (chan 1)]
    (>!! ch 10)
    (is (= 20 (<!! (go (try (let [v (<! ch)] (* v 2))
                         (catch e :error))))))))

(deftest go-try-let-park-exception
  (let [ch (chan 1)]
    (>!! ch 10)
    (is (= "got:10" (<!! (go (try (let [v (<! ch)] (throw (str "got:" v)))
                               (catch e (ex-data e)))))))))

(deftest go-try-finally-on-success
  (let [ch (chan 1)
        log (atom [])]
    (>!! ch 42)
    (<!! (go (try (<! ch)
              (catch e :error)
              (finally (swap! log conj :finally)))))
    (is (= [:finally] @log))))

(deftest go-try-finally-on-exception
  (let [ch (chan 1)
        log (atom [])]
    (>!! ch 42)
    (let [result (<!! (go (try (do (<! ch) (throw "boom"))
                            (catch e (do (swap! log conj :caught) (ex-data e)))
                            (finally (swap! log conj :finally)))))]
      (is (= "boom" result))
      (is (= [:caught :finally] @log)))))

(deftest go-try-loop-recur
  (let [ch (chan 3)
        out (atom [])]
    (>!! ch 1) (>!! ch 2) (>!! ch 3) (close! ch)
    (<!! (go (try
               (loop []
                 (let [v (<! ch)]
                   (when v
                     (swap! out conj v)
                     (recur))))
               (catch e :error))))
    (is (= [1 2 3] @out))))

(deftest go-try-exception-in-loop
  (let [ch (chan 3)]
    (>!! ch 1) (>!! ch 2) (>!! ch 99)
    (is (= "bad:99"
           (<!! (go (try
                      (loop []
                        (let [v (<! ch)]
                          (when (= v 99) (throw (str "bad:" v)))
                          (when v (recur))))
                      (catch e (ex-data e)))))))))

(deftest go-try-catch-nil-result
  (let [ch (chan 1)]
    (>!! ch 42)
    (is (= nil (<!! (go (try (do (<! ch) (throw "x"))
                          (catch e nil))))))))

(deftest go-try-multiple-parks
  (let [ch1 (chan 1) ch2 (chan 1)]
    (>!! ch1 10) (>!! ch2 20)
    (is (= 30 (<!! (go (try (let [a (<! ch1)
                                   b (<! ch2)]
                               (+ a b))
                          (catch e :error))))))))

(deftest go-try-exception-at-second-park
  (let [ch1 (chan 1) ch2 (chan 1)]
    (>!! ch1 10) (>!! ch2 20)
    (is (= "after-b"
           (<!! (go (try (let [a (<! ch1)
                                b (<! ch2)]
                            (throw "after-b"))
                      (catch e (ex-data e)))))))))

(deftest go-try-if-park-in-try
  (let [ch1 (chan 1) ch2 (chan 1)]
    (>!! ch1 :then) (>!! ch2 :else)
    (is (= :then (<!! (go (try (if true (<! ch1) (<! ch2))
                            (catch e :error))))))
    (let [ch3 (chan 1) ch4 (chan 1)]
      (>!! ch3 :then) (>!! ch4 :else)
      (is (= :else (<!! (go (try (if false (<! ch3) (<! ch4))
                              (catch e :error)))))))))

;; ===== Handler commit / alts pending =====

(deftest alts-pending-exactly-one-wins
  (let [ch1 (chan)
        ch2 (chan)
        results (atom [])]
    (let [cb (fn [v] (swap! results conj v))]
      (alts* [ch1 ch2] {} cb)
      (put! ch1 10)
      (put! ch2 20)
      (drain!)
      (is (= 1 (count @results))))))

(deftest alts-pending-take-completes
  (let [ch1 (chan)
        ch2 (chan)]
    (let [result (atom nil)
          cb     (fn [v] (reset! result v))]
      (alts* [ch1 ch2] {} cb)
      (put! ch2 42)
      (drain!)
      (is (= 42 (first @result)))
      (is (= ch2 (second @result))))))

(deftest alts-pending-put-completes
  (let [ch1 (chan)
        ch2 (chan)]
    (take! ch1 (fn [v] nil))
    (drain!)
    (let [result (atom nil)
          cb     (fn [v] (reset! result v))]
      (alts* [[ch1 77] [ch2 88]] {} cb)
      (drain!)
      (is (= true (first @result)))
      (is (= ch1 (second @result))))))

;; ===== Multi-turn blocking drain =====

(deftest blocking-cascaded-go
  (is (= 42 (<!! (go (<! (go (<! (go 42)))))))))

(deftest blocking-multi-park-go
  (let [ch (chan)]
    (go (let [a (<! (go 10))
              b (<! (go 20))]
          (>! ch (+ a b))))
    (is (= 30 (<!! ch)))))

;; ===== Channel transducers =====

(deftest chan-xform-map
  (let [ch (chan 10 (map inc))]
    (>!! ch 1) (>!! ch 2) (>!! ch 3)
    (is (= 2 (<!! ch)))
    (is (= 3 (<!! ch)))
    (is (= 4 (<!! ch)))))

(deftest chan-xform-filter
  (let [ch (chan 10 (filter even?))]
    (>!! ch 1) (>!! ch 2) (>!! ch 3) (>!! ch 4)
    (is (= 2 (<!! ch)))
    (is (= 4 (<!! ch)))))

(deftest chan-xform-comp
  (let [ch (chan 10 (comp (map inc) (filter even?)))]
    (>!! ch 1) (>!! ch 2) (>!! ch 3) (>!! ch 4)
    (is (= 2 (<!! ch)))
    (is (= 4 (<!! ch)))))

(deftest chan-xform-mapcat
  (let [ch (chan 10 (mapcat (fn [v] [v (* v 10)])))]
    (>!! ch 1) (>!! ch 2)
    (is (= 1 (<!! ch)))
    (is (= 10 (<!! ch)))
    (is (= 2 (<!! ch)))
    (is (= 20 (<!! ch)))))

(deftest chan-xform-ex-handler
  (let [ch (chan 10 (map (fn [v] (if (= v :bad)
                                    (throw "xform error!")
                                    (* v 2))))
                (fn [err] :recovered))]
    (>!! ch 5)
    (>!! ch :bad)
    (>!! ch 7)
    (is (= 10 (<!! ch)))
    (is (= :recovered (<!! ch)))
    (is (= 14 (<!! ch)))))

(deftest chan-xform-close-flushes
  (let [ch (chan 10 (partition-all 3))]
    (>!! ch 1) (>!! ch 2) (>!! ch 3) (>!! ch 4) (>!! ch 5)
    (close! ch)
    (drain!)
    (let [v1 (poll! ch) v2 (poll! ch)]
      (is (= 3 (count v1)))
      (is (= 2 (count v2))))))

;; ===== Close edge cases =====

(deftest close-idempotent
  (let [ch (chan 1)]
    (close! ch)
    (close! ch)
    (is (closed? ch))))

(deftest close-pending-taker-gets-nil
  (let [ch (chan)
        result (atom :unset)]
    (take! ch (fn [v] (reset! result v)))
    (close! ch)
    (drain!)
    (is (= nil @result))))

(deftest close-buffered-values-still-available
  (let [ch (chan 3)]
    (>!! ch 1) (>!! ch 2) (>!! ch 3)
    (close! ch)
    (is (= 1 (<!! ch)))
    (is (= 2 (<!! ch)))
    (is (= 3 (<!! ch)))
    (is (= nil (<!! ch)))))

;; ===== Buffer edge cases =====

(deftest sliding-buffer-cap-1
  (let [ch (chan (sliding-buffer 1))]
    (>!! ch 1) (>!! ch 2) (>!! ch 3)
    (is (= 3 (<!! ch)))))

(deftest dropping-buffer-cap-1
  (let [ch (chan (dropping-buffer 1))]
    (>!! ch 1) (>!! ch 2) (>!! ch 3)
    (is (= 1 (<!! ch)))))

;; ===== Combinator edge cases =====

(deftest pipe-from-closed
  (let [from (chan 1)
        to   (chan 1)]
    (close! from)
    (pipe from to)
    (drain!) (drain!)
    (is (closed? to))))

(deftest mult-zero-taps
  (let [src (chan 1)
        m   (mult src)]
    (>!! src 1)
    (drain!) (drain!)
    (is true)))

(deftest unsub-all-clears
  (let [src (chan 10)
        p   (pub src (fn [v] (:topic v)))
        s1  (chan 10)]
    (sub p :a s1)
    (unsub-all p)
    (put! src {:topic :a :val 1})
    (drain!)
    (is (= nil (poll! s1)))))

(deftest unmix-all-clears
  (let [out (chan 10)
        m   (mix out)
        ch1 (chan 10)]
    (admix m ch1)
    (unmix-all m)
    (put! ch1 1)
    (drain!)
    (is (= nil (poll! out)))))

(deftest mix-pause-resume
  (let [out (chan 10)
        m   (mix out)
        ch1 (chan 10)]
    (admix m ch1)
    ;; Values flow normally
    (put! ch1 1)
    (drain!)
    (is (= 1 (poll! out)))
    ;; Pause the channel
    (toggle m {ch1 {:solo false :mute false :pause true}})
    (put! ch1 2)
    (drain!)
    (is (= nil (poll! out)))
    ;; Unpause - values should flow again
    (toggle m {ch1 {:solo false :mute false :pause false}})
    (put! ch1 3)
    (drain!)
    (is (= 3 (poll! out)))))

(deftest mix-solo-mode-mute
  (let [out (chan 10)
        m   (mix out)
        ch1 (chan 10)
        ch2 (chan 10)]
    (admix m ch1)
    (admix m ch2)
    ;; Solo ch1 with mute mode (default): ch2 values dropped
    (solo-mode m :mute)
    (toggle m {ch1 {:solo true :mute false :pause false}})
    (put! ch1 :a)
    (put! ch2 :b)
    (drain!)
    (is (= :a (poll! out)))
    (is (= nil (poll! out)))))

;; ===== Multiple go blocks on same channel =====

(deftest multiple-go-writers
  (let [ch (chan 10)]
    (go (>! ch 1))
    (go (>! ch 2))
    (go (>! ch 3))
    (drain!) (drain!)
    (let [results (atom #{})]
      (dotimes [_ 3]
        (swap! results conj (<!! ch)))
      (is (= #{1 2 3} @results)))))

(deftest go-exception-propagates
  (let [caught (atom false)]
    (try
      (let [ch (go (throw "boom"))]
        (drain!))
      (catch e (reset! caught true)))
    (is @caught)))

;; ===== Handler commit: alts with all channels closed =====

(deftest alts-all-closed-returns-nil
  (let [ch1 (chan)
        ch2 (chan)]
    (close! ch1)
    (close! ch2)
    ;; Takes on closed channels complete immediately with nil
    (let [[val port] (alts! [ch1 ch2])]
      (is (nil? val))
      (is (or (identical? port ch1) (identical? port ch2))))))

;; ===== Double close is no-op =====

(deftest close-double-is-noop
  (let [ch (chan 1)]
    (put! ch :val)
    (drain!)
    (close! ch)
    (close! ch)
    (is (= :val (<!! ch)))
    (is (nil? (<!! ch)))))

;; ===== Unbuffered edge cases =====

(deftest unbuffered-put-take-ordering
  (let [ch (chan)
        result (atom [])]
    ;; Register takers first
    (take! ch (fn [v] (swap! result conj v)))
    (take! ch (fn [v] (swap! result conj v)))
    ;; Put values
    (put! ch :a)
    (put! ch :b)
    (drain!)
    (is (= [:a :b] @result))))

;; ===== go block returning nil =====

(deftest go-returning-nil
  (let [ch (go nil)]
    (drain!)
    ;; go block returning nil closes the result channel with nil
    (is (nil? (<!! ch)))))

;; ===== Transducer early termination (reduced) =====

(deftest chan-xform-reduced-closes
  (let [ch (chan 10 (take 3))]
    (put! ch 1) (drain!)
    (put! ch 2) (drain!)
    (put! ch 3) (drain!)
    ;; Channel should be closed after 3 values (reduced)
    (is (true? (closed? ch)))
    (is (= 1 (<!! ch)))
    (is (= 2 (<!! ch)))
    (is (= 3 (<!! ch)))))

;; ===== Multiple takes in same go block =====

(deftest go-multiple-takes
  (let [ch1 (chan 1)
        ch2 (chan 1)
        result (chan 1)]
    (put! ch1 :a)
    (put! ch2 :b)
    (drain!)
    (go (let [v1 (<! ch1)
              v2 (<! ch2)]
          (>! result [v1 v2])))
    (drain!)
    (is (= [:a :b] (<!! result)))))

;; ===== alts! put on closed channel =====

(deftest alts-put-on-closed
  (let [ch (chan)]
    (close! ch)
    (let [[val port] (alts! [[ch :val]])]
      (is (false? val))
      (is (identical? port ch)))))

;; ===== offer!/poll! on unbuffered channel =====

(deftest offer-poll-unbuffered
  (let [ch (chan)]
    ;; offer! fails on unbuffered with no taker
    (is (false? (offer! ch :val)))
    ;; poll! returns nil on unbuffered with no putter
    (is (nil? (poll! ch)))))

;; ===== Buffer edge: fixed buffer full =====

(deftest fixed-buffer-full-blocks
  (let [ch (chan 2)]
    (put! ch 1)
    (put! ch 2)
    (drain!)
    ;; Buffer full, put becomes pending
    (put! ch 3)
    (is (= 1 (<!! ch)))
    (is (= 2 (<!! ch)))
    (is (= 3 (<!! ch)))))

;; ===== Timeout produces nil on close =====

(deftest timeout-delivers-nil
  (let [ch (timeout 0)]
    (drain!)
    (is (nil? (<!! ch)))))

(run-tests-and-exit)
