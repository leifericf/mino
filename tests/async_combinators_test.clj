(require "tests/test")
(require "core/async")

;; --- pipe ---

(deftest pipe-transfers-values
  (let [from (chan 3)
        to   (chan 3)]
    (put! from :a)
    (put! from :b)
    (put! from :c)
    (drain!)
    (close! from)
    (pipe from to)
    (drain!)
    (let [result (atom [])]
      (take! to (fn [v] (swap! result conj v)))
      (take! to (fn [v] (swap! result conj v)))
      (take! to (fn [v] (swap! result conj v)))
      (drain!)
      (is (= [:a :b :c] @result)))))

(deftest pipe-closes-to
  (let [from (chan 1)
        to   (chan 1)]
    (close! from)
    (pipe from to)
    (drain!)
    (is (true? (closed? to)) "pipe closes to channel when from is done")))

(deftest pipe-no-close
  (let [from (chan 1)
        to   (chan 1)]
    (close! from)
    (pipe from to false)
    (drain!)
    (is (false? (closed? to)) "pipe does not close to when close? is false")))

;; --- to-chan! ---

(deftest to-chan!-basics
  (let [ch (to-chan! [1 2 3])
        result (atom [])]
    (take! ch (fn [v] (swap! result conj v)))
    (take! ch (fn [v] (swap! result conj v)))
    (take! ch (fn [v] (swap! result conj v)))
    (drain!)
    (is (= [1 2 3] @result))))

(deftest to-chan!-closes
  (let [ch (to-chan! [:x])]
    (take! ch (fn [v] nil))  ;; consume the value
    (drain!)
    (is (true? (closed? ch)))))

;; --- into ---

(deftest into-collects
  (let [ch  (to-chan! [1 2 3])
        out (async-into [] ch)
        result (atom nil)]
    (take! out (fn [v] (reset! result v)))
    (drain!)
    (is (= [1 2 3] @result))))

(deftest into-empty
  (let [ch  (chan)]
    (close! ch)
    (let [out (async-into [] ch)
          result (atom nil)]
      (take! out (fn [v] (reset! result v)))
      (drain!)
      (is (= [] @result)))))

;; --- merge ---

(deftest merge-combines-channels
  (let [ch1 (to-chan! [:a :b])
        ch2 (to-chan! [:c :d])
        out (merge-chans [ch1 ch2] 4)
        result (atom #{})]
    (dotimes [_ 4]
      (take! out (fn [v] (swap! result conj v))))
    (drain!)
    (is (= #{:a :b :c :d} @result))))

(deftest merge-closes-when-all-done
  (let [ch1 (chan)
        ch2 (chan)
        out (merge-chans [ch1 ch2])]
    (close! ch1)
    (drain!)
    (is (false? (closed? out)) "not closed while one source remains")
    (close! ch2)
    (drain!)
    (is (true? (closed? out)) "closed when all sources done")))

;; --- mix edge cases ---

(deftest mix-toggle-during-flow
  (let [out (chan 10)
        m   (mix out)
        ch1 (chan 10)]
    (admix m ch1)
    (put! ch1 :a)
    (drain!)
    (is (= :a (poll! out)))
    ;; Mute during flow
    (toggle m {ch1 {:solo false :mute true :pause false}})
    (put! ch1 :b)
    (drain!)
    (is (= nil (poll! out)))
    ;; Unmute
    (toggle m {ch1 {:solo false :mute false :pause false}})
    (put! ch1 :c)
    (drain!)
    (is (= :c (poll! out)))))

(deftest mix-solo-mode-pause
  (let [out (chan 10)
        m   (mix out)
        ch1 (chan 10)
        ch2 (chan 10)]
    (admix m ch1)
    (admix m ch2)
    ;; Solo ch1 with pause mode: ch2 paused (consumed but not forwarded)
    (solo-mode m :pause)
    (toggle m {ch1 {:solo true :mute false :pause false}})
    (put! ch1 :a)
    (put! ch2 :b)
    (drain!)
    (is (= :a (poll! out)))
    (is (= nil (poll! out)))))

;; --- mult edge cases ---

(deftest mult-close-propagates
  (let [src (chan 1)
        m   (mult src)
        t1  (chan 1)
        t2  (chan 1)]
    (tap m t1)
    (tap m t2)
    (close! src)
    (drain!)
    (is (true? (closed? t1)))
    (is (true? (closed? t2)))))

;; --- merge edge cases ---

(deftest merge-zero-channels
  (let [out (merge-chans [] 1)]
    (drain!)
    (is (true? (closed? out)))))

;; --- pipeline edge cases ---

(deftest pipeline-ordering-n4
  (let [from (to-chan! [1 2 3 4 5 6 7 8])
        to   (chan 8)]
    (pipeline 4 to (fn [x] (* x 10)) from)
    (drain!)
    (let [result (atom [])]
      (dotimes [_ 8]
        (take! to (fn [v] (swap! result conj v))))
      (drain!)
      (is (= [10 20 30 40 50 60 70 80] @result)))))

(deftest pipeline-async-ordering-n4
  (let [from (to-chan! [1 2 3 4 5 6 7 8])
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

(run-tests-and-exit)
