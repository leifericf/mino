(require "tests/test")
(require "core/async")

;; --- Fixed buffer ---

(deftest fixed-buffer-basics
  (let [b (buf-fixed* 3)]
    (is (map? b) "buffer is a descriptor map")))

;; --- Channel + buffer integration ---

(deftest chan-unbuffered-create
  (let [ch (chan*)]
    (is (chan? ch) "is a channel")
    (is (false? (chan-closed?* ch)))))

(deftest chan-buffered-create
  (let [ch (chan* (buf-fixed* 2))]
    (is (chan? ch) "is a channel")
    (is (false? (chan-closed?* ch)))))

(deftest chan-close
  (let [ch (chan* (buf-fixed* 1))]
    (chan-close* ch)
    (is (true? (chan-closed?* ch)))))

(deftest chan-put-take-buffered
  (let [ch     (chan* (buf-fixed* 2))
        result (atom nil)]
    ;; put two values into buffered channel
    (chan-put* ch 1 (fn [v] (reset! result v)))
    (drain!)
    (is (true? @result) "put callback receives true")
    ;; take the value back
    (reset! result nil)
    (chan-take* ch (fn [v] (reset! result v)))
    (drain!)
    (is (= 1 @result) "take receives the put value")))

(deftest chan-put-take-unbuffered
  (let [ch     (chan*)
        result (atom nil)]
    ;; put on unbuffered channel -- no taker, so it pends
    (chan-put* ch :hello (fn [v] nil))
    ;; now take -- should match the pending put
    (chan-take* ch (fn [v] (reset! result v)))
    (drain!)
    (is (= :hello @result) "unbuffered transfer works")))

(deftest chan-take-then-put
  (let [ch     (chan*)
        result (atom nil)]
    ;; take first -- pends
    (chan-take* ch (fn [v] (reset! result v)))
    ;; put -- matches the pending take
    (chan-put* ch 42 (fn [v] nil))
    (drain!)
    (is (= 42 @result) "pending take receives put value")))

(deftest chan-nil-put-rejected
  (let [ch (chan* (buf-fixed* 1))]
    (is (thrown? (chan-put* ch nil (fn [v] nil)))
        "putting nil throws")))

(deftest chan-close-delivers-nil-to-takers
  (let [ch     (chan*)
        result (atom :sentinel)]
    (chan-take* ch (fn [v] (reset! result v)))
    (chan-close* ch)
    (drain!)
    (is (nil? @result) "pending taker gets nil on close")))

(deftest chan-put-on-closed
  (let [ch     (chan*)
        result (atom :sentinel)]
    (chan-close* ch)
    (chan-put* ch 1 (fn [v] (reset! result v)))
    (drain!)
    (is (false? @result) "put callback receives false on closed channel")))

(deftest chan-offer-poll
  (let [ch (chan* (buf-fixed* 2))]
    (is (true? (offer!* ch :a)) "offer succeeds with buffer room")
    (is (= :a (poll!* ch)) "poll returns buffered value")
    (is (nil? (poll!* ch)) "poll returns nil when empty")))

(deftest chan-dropping-buffer
  (let [ch     (chan* (buf-dropping* 2))
        result (atom [])]
    (chan-put* ch 1 (fn [v] nil))
    (chan-put* ch 2 (fn [v] nil))
    (chan-put* ch 3 (fn [v] nil))  ;; dropped
    (chan-take* ch (fn [v] (swap! result conj v)))
    (chan-take* ch (fn [v] (swap! result conj v)))
    (drain!)
    (is (= [1 2] @result) "dropping buffer keeps first N values")))

(deftest chan-sliding-buffer
  (let [ch     (chan* (buf-sliding* 2))
        result (atom [])]
    (chan-put* ch 1 (fn [v] nil))
    (chan-put* ch 2 (fn [v] nil))
    (chan-put* ch 3 (fn [v] nil))  ;; slides: drops 1, keeps 2,3
    (chan-take* ch (fn [v] (swap! result conj v)))
    (chan-take* ch (fn [v] (swap! result conj v)))
    (drain!)
    (is (= [2 3] @result) "sliding buffer keeps last N values")))

(deftest chan-promise-buffer
  (let [ch     (chan* (buf-promise*))
        result (atom [])]
    (chan-put* ch :first (fn [v] nil))
    (chan-put* ch :second (fn [v] nil))  ;; ignored
    ;; promise buffer delivers same value multiple times
    (chan-take* ch (fn [v] (swap! result conj v)))
    (chan-take* ch (fn [v] (swap! result conj v)))
    (drain!)
    (is (= [:first :first] @result) "promise buffer delivers first value repeatedly")))

(deftest chan-multiple-pending-takes
  (let [ch (chan* (buf-fixed* 1))
        r1 (atom nil)
        r2 (atom nil)]
    (chan-take* ch (fn [v] (reset! r1 v)))
    (chan-take* ch (fn [v] (reset! r2 v)))
    (chan-put* ch :a (fn [v] nil))
    (chan-put* ch :b (fn [v] nil))
    (drain!)
    (is (= :a @r1) "first taker gets first value")
    (is (= :b @r2) "second taker gets second value")))

(deftest chan-take-from-closed-empty
  (let [ch     (chan*)
        result (atom :sentinel)]
    (chan-close* ch)
    (chan-take* ch (fn [v] (reset! result v)))
    (drain!)
    (is (nil? @result) "take from closed empty channel returns nil")))

(deftest chan-take-from-closed-buffered
  (let [ch     (chan* (buf-fixed* 2))
        result (atom nil)]
    (chan-put* ch :val (fn [v] nil))
    (drain!)
    (chan-close* ch)
    (chan-take* ch (fn [v] (reset! result v)))
    (drain!)
    (is (= :val @result) "take from closed channel with buffered value returns that value")))

(run-tests-and-exit)
