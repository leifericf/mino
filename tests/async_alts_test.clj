(require "tests/test")
(require '[clojure.core.async :as a :refer [chan chan? buffer dropping-buffer sliding-buffer promise-chan closed? close! put! take! alts! offer! poll! <!! >!! alts!! timeout go go-loop pipe onto-chan! to-chan! mult tap untap pub sub unsub unsub-all mix admix unmix unmix-all toggle solo-mode pipeline pipeline-async pipeline-blocking chan* chan?* chan-put* chan-take* chan-close* chan-closed?* offer!* poll!* alts* buf-fixed* buf-dropping* buf-sliding* buf-promise* chan-set-xform*]])

;; --- alts! immediate take ---

(deftest alts-take-from-buffered
  (let [ch (chan 1)]
    (put! ch :x)
    (drain!)
    (let [[val port] (alts! [ch])]
      (is (= :x val))
      (is (identical? ch port)))))

;; --- alts! immediate put ---

(deftest alts-put-to-buffered
  (let [ch (chan 1)
        [val port] (alts! [[ch :y]])]
    (is (true? val) "put returns true")
    (is (identical? ch port))))

;; --- alts! with default ---

(deftest alts-default-when-nothing-ready
  (let [ch (chan)
        [val port] (alts! [ch] {:default :nope})]
    (is (= :nope val))
    (is (= :default port))))

(deftest alts-no-default-when-ready
  (let [ch (chan 1)]
    (put! ch :ready)
    (drain!)
    (let [[val port] (alts! [ch] {:default :nope})]
      (is (= :ready val))
      (is (identical? ch port)))))

;; --- alts! with closed channel ---

(deftest alts-take-from-closed
  (let [ch (chan)]
    (close! ch)
    (let [[val port] (alts! [ch])]
      (is (nil? val))
      (is (identical? ch port)))))

(deftest alts-put-to-closed
  (let [ch (chan)]
    (close! ch)
    (let [[val port] (alts! [[ch :x]])]
      (is (false? val))
      (is (identical? ch port)))))

;; --- alts! with multiple channels ---

(deftest alts-multiple-one-ready
  (let [ch1 (chan 1)
        ch2 (chan 1)]
    (put! ch2 :from-2)
    (drain!)
    (let [[val port] (alts! [ch1 ch2] {:priority true})]
      ;; ch2 has a value, ch1 doesn't
      (is (= :from-2 val))
      (is (identical? ch2 port)))))

;; --- alts! pending take ---

(deftest alts-pending-take
  (let [ch1 (chan)
        ch2 (chan)]
    ;; Both empty -- alts registers pending takes
    ;; Then put to one
    (let [result (atom nil)]
      ;; We need to set up the alts pending, then put
      ;; alts! is synchronous in single-threaded mode,
      ;; so we need to set up the put first
      (chan-put* ch1 :val1 (fn [v] nil))
      (let [[val port] (alts! [ch1 ch2])]
        (is (= :val1 val))
        (is (identical? ch1 port))))))

;; --- alts! with priority ---

(deftest alts-priority
  (let [ch1 (chan 1)
        ch2 (chan 1)]
    (put! ch1 :first)
    (put! ch2 :second)
    (drain!)
    (let [[val port] (alts! [ch1 ch2] {:priority true})]
      (is (= :first val) "priority selects first ready channel")
      (is (identical? ch1 port)))))

;; --- alts! nil put rejection ---

(deftest alts-nil-put-throws
  (let [ch (chan 1)]
    (is (thrown? (alts! [[ch nil]])))))

(run-tests-and-exit)
