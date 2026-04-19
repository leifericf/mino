(require "tests/test")
(require "core/async")

(deftest blocking-take-buffered
  (let [ch (chan 1)]
    (put! ch :val)
    (drain!)
    (is (= :val (<!! ch)))))

(deftest blocking-take-from-closed
  (let [ch (chan)]
    (close! ch)
    (is (nil? (<!! ch)))))

(deftest blocking-put-buffered
  (let [ch (chan 1)]
    (is (true? (>!! ch :x)))
    (is (= :x (<!! ch)))))

(deftest blocking-put-closed
  (let [ch (chan)]
    (close! ch)
    (is (false? (>!! ch :x)))))

(deftest blocking-take-deadlock-throws
  (let [ch (chan)]
    (is (thrown? (<!! ch)))))

(deftest blocking-put-deadlock-throws
  (let [ch (chan)]
    (is (thrown? (>!! ch :x)))))

(deftest blocking-round-trip
  (let [ch (chan 1)]
    (>!! ch 42)
    (is (= 42 (<!! ch)))))

(deftest blocking-multiple
  (let [ch (chan 3)]
    (>!! ch 1)
    (>!! ch 2)
    (>!! ch 3)
    (is (= 1 (<!! ch)))
    (is (= 2 (<!! ch)))
    (is (= 3 (<!! ch)))))

(deftest blocking-alts-take
  (let [ch (chan 1)]
    (>!! ch :x)
    (let [[val port] (alts!! [ch])]
      (is (= :x val))
      (is (identical? ch port)))))

(deftest blocking-alts-with-default
  (let [ch (chan)]
    (let [[val port] (alts!! [ch] {:default :nope})]
      (is (= :nope val))
      (is (= :default port)))))

;; --- Edge cases: delayed progress ---

(deftest blocking-delayed-producer
  (let [ch (chan)]
    (go (>! ch 99))
    (is (= 99 (<!! ch)))))

(deftest blocking-cascaded-go
  (is (= 42 (<!! (go (<! (go (<! (go 42)))))))))

(deftest blocking-multi-park-producer
  (let [ch (chan)]
    (go (let [a (<! (go 10))
              b (<! (go 20))
              c (<! (go 30))]
          (>! ch (+ a b c))))
    (is (= 60 (<!! ch)))))

(deftest blocking-alts-delayed-producer
  (let [ch1 (chan)
        ch2 (chan 1)]
    (go (>! ch2 :winner))
    (let [[val port] (alts!! [ch1 ch2])]
      (is (= :winner val))
      (is (identical? ch2 port)))))

(deftest blocking-put-slow-consumer
  (let [ch (chan)]
    (go (let [v (<! ch)]
          (>! (go v) v)))
    (is (true? (>!! ch :data)))))

(run-tests)
