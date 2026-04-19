(require "tests/test")
(require "core/async")

;; --- Phase 6a: straight-line go blocks ---

(deftest go-returns-channel
  (let [ch (go :hello)]
    (is (true? (chan? ch)) "go returns a channel")))

(deftest go-simple-value
  (let [ch (go 42)
        result (atom nil)]
    (take! ch (fn [v] (reset! result v)))
    (drain!)
    (is (= 42 @result))))

(deftest go-expression
  (let [ch (go (+ 1 2 3))
        result (atom nil)]
    (take! ch (fn [v] (reset! result v)))
    (drain!)
    (is (= 6 @result))))

(deftest go-nil-result
  (let [ch (go nil)]
    (let [result (atom :sentinel)]
      (take! ch (fn [v] (reset! result v)))
      (drain!)
      (is (nil? @result) "nil result closes channel without put"))))

;; --- go with <! ---

(deftest go-take-immediate
  (let [src (chan 1)
        result (atom nil)]
    (put! src :val)
    (drain!)
    (let [ch (go (<! src))]
      (take! ch (fn [v] (reset! result v)))
      (drain!)
      (is (= :val @result)))))

(deftest go-take-pending
  (let [src    (chan)
        result (atom nil)]
    (let [out (go (<! src))]
      (put! src :delivered)
      (drain!)
      (take! out (fn [v] (reset! result v)))
      (drain!)
      (is (= :delivered @result)))))

;; --- go with >! ---

(deftest go-put
  (let [dst    (chan 1)
        result (atom nil)]
    (let [ch (go (>! dst :sent) :done)]
      (take! dst (fn [v] (reset! result v)))
      (drain!)
      (is (= :sent @result)))))

(deftest go-put-pending
  (let [dst    (chan)
        result (atom nil)]
    (let [out (go (>! dst :val) :done)]
      (take! dst (fn [v] (reset! result v)))
      (drain!)
      (is (= :val @result)))))

;; --- go with sequential parks ---

(deftest go-sequential-takes
  (let [ch1 (chan 1)
        ch2 (chan 1)
        result (atom nil)]
    (put! ch1 :a)
    (put! ch2 :b)
    (drain!)
    (let [out (go
                (<! ch1)
                (<! ch2)
                :both-done)]
      (take! out (fn [v] (reset! result v)))
      (drain!)
      (is (= :both-done @result)))))

(deftest go-sequential-put-take
  (let [ch     (chan 1)
        result (atom nil)]
    (let [out (go
                (>! ch :x)
                (<! ch))]
      (take! out (fn [v] (reset! result v)))
      (drain!)
      (is (= :x @result) "put then take on same channel"))))

(deftest go-code-between-parks
  (let [src1   (chan 1)
        src2   (chan 1)
        side   (atom nil)
        result (atom nil)]
    (put! src1 10)
    (put! src2 20)
    (drain!)
    (let [out (go
                (<! src1)
                (reset! side :between)
                (<! src2))]
      (take! out (fn [v] (reset! result v)))
      (drain!)
      (is (= :between @side) "side effect runs between parks")
      (is (= 20 @result) "final take result returned"))))

;; --- Multiple go blocks ---

(deftest multiple-go-blocks
  (let [ch     (chan 1)
        result (atom nil)]
    ;; One go puts, another takes
    (go (>! ch :transferred))
    (let [out (go (<! ch))]
      (take! out (fn [v] (reset! result v)))
      (drain!)
      (is (= :transferred @result)))))

;; --- go with let bindings across parks ---

(deftest go-let-take
  (let [src (chan 1)
        result (atom nil)]
    (put! src 10)
    (drain!)
    (let [ch (go (let [x (<! src)]
                   (* x 2)))]
      (take! ch (fn [v] (reset! result v)))
      (drain!)
      (is (= 20 @result)))))

(deftest go-let-multiple-bindings
  (let [src (chan 1)
        result (atom nil)]
    (put! src 5)
    (drain!)
    (let [ch (go (let [x (<! src)
                       y (+ x 3)]
                   (* y 2)))]
      (take! ch (fn [v] (reset! result v)))
      (drain!)
      (is (= 16 @result)))))

(deftest go-let-nested-takes
  (let [ch1 (chan 1)
        ch2 (chan 1)
        result (atom nil)]
    (put! ch1 10)
    (put! ch2 20)
    (drain!)
    (let [out (go (let [a (<! ch1)]
                    (let [b (<! ch2)]
                      (+ a b))))]
      (take! out (fn [v] (reset! result v)))
      (drain!)
      (is (= 30 @result)))))

;; --- go with if/cond across park points (Phase 6b) ---

(deftest go-if-both-branches-park
  (let [ch1 (chan 1)
        ch2 (chan 1)
        result (atom nil)]
    (put! ch1 :from-ch1)
    (put! ch2 :from-ch2)
    (drain!)
    (let [out (go (if true (<! ch1) (<! ch2)))]
      (take! out (fn [v] (reset! result v)))
      (drain!)
      (is (= :from-ch1 @result) "if true takes from ch1"))))

(deftest go-if-else-branch
  (let [ch1 (chan 1)
        ch2 (chan 1)
        result (atom nil)]
    (put! ch1 :from-ch1)
    (put! ch2 :from-ch2)
    (drain!)
    (let [out (go (if false (<! ch1) (<! ch2)))]
      (take! out (fn [v] (reset! result v)))
      (drain!)
      (is (= :from-ch2 @result) "if false takes from ch2"))))

(deftest go-if-one-park-one-value
  (let [ch (chan 1)
        result (atom nil)]
    (put! ch :parked)
    (drain!)
    (let [out (go (if true (<! ch) :default))]
      (take! out (fn [v] (reset! result v)))
      (drain!)
      (is (= :parked @result) "true branch parks"))))

(deftest go-if-value-then-park-else
  (let [ch (chan 1)
        result (atom nil)]
    (put! ch :parked)
    (drain!)
    (let [out (go (if false :default (<! ch)))]
      (take! out (fn [v] (reset! result v)))
      (drain!)
      (is (= :parked @result) "else branch parks"))))

(deftest go-if-nil-branch
  (let [ch (chan 1)
        result (atom :sentinel)]
    (put! ch :val)
    (drain!)
    (let [out (go (if true (<! ch) nil))]
      (take! out (fn [v] (reset! result v)))
      (drain!)
      (is (= :val @result) "nil in else branch handled"))))

(deftest go-if-park-as-statement
  (let [ch1 (chan 1)
        ch2 (chan 1)
        result (atom nil)]
    (put! ch1 :a)
    (drain!)
    (let [out (go
                (if true (<! ch1) (<! ch2))
                :done)]
      (take! out (fn [v] (reset! result v)))
      (drain!)
      (is (= :done @result) "if-park as statement, not return value"))))

(deftest go-if-with-put-park
  (let [ch (chan 1)
        result (atom nil)]
    (let [out (go (if true (>! ch :sent) :skip)
                  :done)]
      (take! ch (fn [v] (reset! result v)))
      (drain!)
      (is (= :sent @result) "if branch with >! park"))))

(deftest go-if-dynamic-condition
  (let [src (chan 1)
        ch1 (chan 1)
        ch2 (chan 1)
        result (atom nil)]
    (put! src 10)
    (put! ch1 :big)
    (put! ch2 :small)
    (drain!)
    (let [out (go (let [x (<! src)]
                    (if (> x 5) (<! ch1) (<! ch2))))]
      (take! out (fn [v] (reset! result v)))
      (drain!)
      (is (= :big @result) "dynamic condition from earlier park"))))

(deftest go-when-park
  (let [ch (chan 1)
        result (atom nil)]
    (put! ch :val)
    (drain!)
    (let [out (go (when true (<! ch)))]
      (take! out (fn [v] (reset! result v)))
      (drain!)
      (is (= :val @result) "when with park"))))

(deftest go-when-false-park
  (let [ch (chan 1)
        result (atom :sentinel)]
    (put! ch :val)
    (drain!)
    (let [out (go (when false (<! ch)))]
      (take! out (fn [v] (reset! result v)))
      (drain!)
      (is (nil? @result) "when false returns nil"))))

(deftest go-cond-park
  (let [ch1 (chan 1)
        ch2 (chan 1)
        ch3 (chan 1)
        result (atom nil)]
    (put! ch1 :a)
    (put! ch2 :b)
    (put! ch3 :c)
    (drain!)
    (let [out (go (cond
                    false (<! ch1)
                    true  (<! ch2)
                    :else (<! ch3)))]
      (take! out (fn [v] (reset! result v)))
      (drain!)
      (is (= :b @result) "cond selects correct park branch"))))

(deftest go-cond-else-park
  (let [ch (chan 1)
        result (atom nil)]
    (put! ch :fallthrough)
    (drain!)
    (let [out (go (cond
                    false :a
                    false :b
                    :else (<! ch)))]
      (take! out (fn [v] (reset! result v)))
      (drain!)
      (is (= :fallthrough @result) "cond :else with park"))))

(deftest go-if-pending-park
  (let [ch1 (chan)
        ch2 (chan)
        result (atom nil)]
    (let [out (go (if true (<! ch1) (<! ch2)))]
      (put! ch1 :delayed)
      (drain!)
      (take! out (fn [v] (reset! result v)))
      (drain!)
      (is (= :delayed @result) "if-park with pending channel"))))

;; --- go with loop/recur across park points (Phase 6d) ---

(deftest go-loop-collect
  (let [ch (chan 3) result (atom nil)]
    (put! ch 1) (put! ch 2) (put! ch 3) (drain!)
    (close! ch)
    (let [out (go (loop [acc []]
                    (let [v (<! ch)]
                      (if (nil? v) acc (recur (conj acc v))))))]
      (take! out (fn [v] (reset! result v)))
      (drain!)
      (is (= [1 2 3] @result)))))

(deftest go-loop-sum
  (let [ch (chan 2) result (atom nil)]
    (put! ch 10) (put! ch 20) (drain!)
    (close! ch)
    (let [out (go-loop [sum 0]
                (let [v (<! ch)]
                  (if (nil? v) sum (recur (+ sum v)))))]
      (take! out (fn [v] (reset! result v)))
      (drain!)
      (is (= 30 @result)))))

(deftest go-loop-recur-park
  (let [ch (chan 3) result (atom nil)]
    (put! ch 1) (put! ch 2) (put! ch 3) (drain!)
    (let [out (go (loop [x 0 n 3]
                    (if (> n 0) (recur (<! ch) (dec n)) x)))]
      (take! out (fn [v] (reset! result v)))
      (drain!)
      (is (= 3 @result) "park in recur argument"))))

(deftest go-loop-multi-bindings
  (let [ch (chan 2) result (atom nil)]
    (put! ch :a) (put! ch :b) (drain!)
    (close! ch)
    (let [out (go-loop [items [] n 0]
                (let [v (<! ch)]
                  (if (nil? v)
                    {:items items :count n}
                    (recur (conj items v) (inc n)))))]
      (take! out (fn [v] (reset! result v)))
      (drain!)
      (is (= {:items [:a :b] :count 2} @result)))))

(deftest go-loop-swap-bindings
  (let [result (atom nil)]
    (let [out (go (loop [a :a b :b n 1]
                    (if (pos? n)
                      (recur b a (dec n))
                      [a b])))]
      (take! out (fn [v] (reset! result v)))
      (drain!)
      (is (= [:b :a] @result) "loop swaps bindings"))))

(deftest go-loop-take-in-binding
  (let [ch (chan 1) result (atom nil)]
    (put! ch 42) (drain!)
    (let [out (go (loop [x (<! ch)] x))]
      (take! out (fn [v] (reset! result v)))
      (drain!)
      (is (= 42 @result) "park in loop binding"))))

(deftest go-loop-pending-channels
  (let [ch (chan) result (atom nil)]
    (let [out (go-loop [acc []]
                (let [v (<! ch)]
                  (if (nil? v) acc (recur (conj acc v)))))]
      (put! ch :x) (drain!)
      (put! ch :y) (drain!)
      (close! ch) (drain!)
      (take! out (fn [v] (reset! result v)))
      (drain!)
      (is (= [:x :y] @result) "loop with pending channels"))))

(run-tests)
