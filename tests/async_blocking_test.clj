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

;; Deadlock detection only fires when host threads are NOT granted.
;; With threads granted (the standalone default), <!!/>!!/alts!! park
;; forever waiting for another thread to fire the matching op — that's
;; canonical behaviour. Single-threaded embed mode keeps the throw so a
;; lone driver thread can't lock itself.
(deftest blocking-take-deadlock-throws
  (when (<= (mino-thread-limit) 1)
    (let [ch (chan)]
      (is (thrown? (<!! ch))))))

(deftest blocking-put-deadlock-throws
  (when (<= (mino-thread-limit) 1)
    (let [ch (chan)]
      (is (thrown? (>!! ch :x))))))

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

;; --- Cross-thread parking (host-thread grant required) ---

(deftest blocking-take-parks-until-other-thread-puts
  (when (> (mino-thread-limit) 1)
    (let [ch (chan)]
      (future (>!! ch :from-worker))
      (is (= :from-worker (<!! ch))))))

(deftest blocking-put-parks-until-other-thread-takes
  (when (> (mino-thread-limit) 1)
    (let [ch  (chan)
          got (promise)]
      (future (deliver got (<!! ch)))
      (is (true? (>!! ch :payload)))
      (is (= :payload @got)))))

(deftest blocking-alts-parks-until-other-thread-fires
  (when (> (mino-thread-limit) 1)
    (let [ch1 (chan)
          ch2 (chan)]
      (future (>!! ch2 :win))
      (let [[val port] (alts!! [ch1 ch2])]
        (is (= :win val))
        (is (identical? ch2 port))))))

(deftest blocking-many-cross-thread-pings
  ;; Stress the channel under genuine concurrency: N futures each push
  ;; M values, the test thread takes N*M values back. With <!! and >!!
  ;; bridging across OS threads, no values may be lost. Cap N at
  ;; (dec (mino-thread-limit)) so the test thread plus N producers fit
  ;; under the runtime's grant on low-CPU shared runners.
  (when (> (mino-thread-limit) 1)
    (let [ch (chan 8)
          n  (min 4 (max 1 (dec (mino-thread-limit))))
          m  50
          producers (doall (for [i (range n)]
                             (future (dotimes [j m]
                                       (>!! ch [i j])))))]
      (let [seen (loop [acc [] k 0]
                   (if (= k (* n m))
                     acc
                     (recur (conj acc (<!! ch)) (inc k))))]
        (is (= (* n m) (count seen)))
        (doseq [f producers] @f)))))

(run-tests-and-exit)
