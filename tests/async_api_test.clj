(require "tests/test")
(require '[clojure.core.async :as a :refer [chan chan? buffer dropping-buffer sliding-buffer promise-chan closed? close! put! take! alts! offer! poll! <!! >!! alts!! timeout go go-loop pipe onto-chan! to-chan! mult tap untap pub sub unsub unsub-all mix admix unmix unmix-all toggle solo-mode pipeline pipeline-async pipeline-blocking chan* chan?* chan-put* chan-take* chan-close* chan-closed?* offer!* poll!* alts* buf-fixed* buf-dropping* buf-sliding* buf-promise* chan-set-xform*]])

;; --- Channel constructors ---

(deftest chan-no-args
  (let [ch (chan)]
    (is (chan? ch))
    (is (false? (closed? ch)))))

(deftest chan-with-number
  (let [ch (chan 5)]
    (is (chan? ch))))

(deftest chan-with-buffer
  (let [ch (chan (buffer 3))]
    (is (chan? ch))))

(deftest chan-with-dropping
  (let [ch (chan (dropping-buffer 3))]
    (is (chan? ch))))

(deftest chan-with-sliding
  (let [ch (chan (sliding-buffer 3))]
    (is (chan? ch))))

(deftest promise-chan-test
  (let [ch (promise-chan)]
    (is (chan? ch))))

;; --- put! / take! ---

(deftest put-take-buffered
  (let [ch (chan 1)
        result (atom nil)]
    (put! ch :hello)
    (take! ch (fn [v] (reset! result v)))
    (drain!)
    (is (= :hello @result))))

(deftest put-take-unbuffered
  (let [ch (chan)
        result (atom nil)]
    (put! ch :world)
    (take! ch (fn [v] (reset! result v)))
    (drain!)
    (is (= :world @result))))

(deftest put-with-callback
  (let [ch (chan 1)
        ok (atom nil)]
    (put! ch :x (fn [v] (reset! ok v)))
    (drain!)
    (is (true? @ok))))

(deftest put-on-closed-callback
  (let [ch (chan)]
    (close! ch)
    (let [ok (atom :pending)]
      (put! ch :x (fn [v] (reset! ok v)))
      (drain!)
      (is (false? @ok)))))

;; --- close! ---

(deftest close-delivers-nil
  (let [ch (chan)
        result (atom :pending)]
    (take! ch (fn [v] (reset! result v)))
    (close! ch)
    (drain!)
    (is (nil? @result))))

(deftest close-idempotent
  (let [ch (chan)]
    (close! ch)
    (close! ch)
    (is (true? (closed? ch)))))

;; --- offer! / poll! ---

(deftest offer-poll-cycle
  (let [ch (chan 2)]
    (is (true? (offer! ch :a)))
    (is (true? (offer! ch :b)))
    (is (= :a (poll! ch)))
    (is (= :b (poll! ch)))
    (is (nil? (poll! ch)))))

(deftest offer-to-waiting-taker
  (let [ch (chan)
        result (atom nil)]
    (take! ch (fn [v] (reset! result v)))
    (is (true? (offer! ch :delivered)))
    (drain!)
    (is (= :delivered @result))))

(deftest offer-full-returns-false
  (let [ch (chan)]
    ;; unbuffered, no taker waiting
    (is (false? (offer! ch :nope)))))

;; --- nil put rejection ---

(deftest nil-put-throws
  (let [ch (chan 1)]
    (is (thrown? (put! ch nil)))))

(deftest nil-offer-throws
  (let [ch (chan 1)]
    (is (thrown? (offer! ch nil)))))

;; --- promise-chan semantics ---

(deftest promise-chan-delivers-same-value
  (let [ch (promise-chan)
        r1 (atom nil)
        r2 (atom nil)]
    (put! ch :once)
    (take! ch (fn [v] (reset! r1 v)))
    (take! ch (fn [v] (reset! r2 v)))
    (drain!)
    (is (= :once @r1))
    (is (= :once @r2))))

(run-tests-and-exit)
