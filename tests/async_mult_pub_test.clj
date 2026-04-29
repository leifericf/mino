(require "tests/test")
(require '[clojure.core.async :as a :refer [chan chan? buffer dropping-buffer sliding-buffer promise-chan closed? close! put! take! alts! offer! poll! <!! >!! alts!! timeout go go-loop pipe onto-chan! to-chan! mult tap untap pub sub unsub unsub-all mix admix unmix unmix-all toggle solo-mode pipeline pipeline-async pipeline-blocking chan* chan?* chan-put* chan-take* chan-close* chan-closed?* offer!* poll!* alts* buf-fixed* buf-dropping* buf-sliding* buf-promise* chan-set-xform*]])

;; --- mult ---

(deftest mult-distributes
  (let [src (chan 3)
        t1  (chan 3)
        t2  (chan 3)
        m   (mult src)]
    (tap m t1)
    (tap m t2)
    (put! src :a)
    (put! src :b)
    (drain!)
    (is (= :a (<!! t1)))
    (is (= :a (<!! t2)))
    (is (= :b (<!! t1)))
    (is (= :b (<!! t2)))))

(deftest mult-close-propagates
  (let [src (chan)
        t1  (chan 1)
        m   (mult src)]
    (tap m t1)
    (close! src)
    (drain!)
    (is (true? (closed? t1)))))

(deftest untap-stops-delivery
  (let [src (chan 3)
        t1  (chan 3)
        t2  (chan 3)
        m   (mult src)]
    (tap m t1)
    (tap m t2)
    (untap m t2)
    (put! src :x)
    (drain!)
    (is (= :x (<!! t1)))
    (is (nil? (poll! t2)) "untapped channel gets nothing")))

;; --- pub / sub ---

(deftest pub-routes-by-topic
  (let [src (chan 3)
        a-ch (chan 3)
        b-ch (chan 3)
        p   (pub src :topic)]
    (sub p :a a-ch)
    (sub p :b b-ch)
    (put! src {:topic :a :data 1})
    (put! src {:topic :b :data 2})
    (put! src {:topic :a :data 3})
    (drain!)
    (is (= {:topic :a :data 1} (<!! a-ch)))
    (is (= {:topic :b :data 2} (<!! b-ch)))
    (is (= {:topic :a :data 3} (<!! a-ch)))))

(deftest pub-close-propagates
  (let [src (chan)
        a-ch (chan 1)
        p   (pub src :topic)]
    (sub p :a a-ch)
    (close! src)
    (drain!)
    (is (true? (closed? a-ch)))))

(deftest unsub-stops-delivery
  (let [src (chan 3)
        a-ch (chan 3)
        p   (pub src :topic)]
    (sub p :a a-ch)
    (unsub p :a a-ch)
    (put! src {:topic :a :data 1})
    (drain!)
    (is (nil? (poll! a-ch)) "unsubbed channel gets nothing")))

;; --- mix ---

(deftest mix-basic
  (let [out (chan 10)
        ch1 (chan 2)
        ch2 (chan 2)
        m   (mix out)
        result (atom #{})]
    (admix m ch1)
    (admix m ch2)
    (put! ch1 :a) (put! ch2 :b) (drain!)
    (take! out (fn [v] (swap! result conj v)))
    (take! out (fn [v] (swap! result conj v)))
    (drain!)
    (is (= #{:a :b} @result) "mix forwards from all inputs")))

(deftest mix-unmix
  (let [out (chan 10)
        ch1 (chan 2)
        m   (mix out)
        result (atom [])]
    (admix m ch1)
    (put! ch1 :before) (drain!)
    (take! out (fn [v] (swap! result conj v)))
    (drain!)
    (unmix m ch1)
    (put! ch1 :after) (drain!)
    (is (= [:before] @result) "unmixed channel no longer forwarded")))

(deftest mix-mute
  (let [out (chan 10)
        ch1 (chan 2)
        m   (mix out)]
    (admix m ch1)
    (toggle m {ch1 {:mute true}})
    (put! ch1 :muted) (drain!)
    (is (nil? (poll! out)) "muted channel values not forwarded")))

(deftest mix-close-removes
  (let [out (chan 10)
        ch1 (chan 1)
        m   (mix out)]
    (admix m ch1)
    (close! ch1)
    (drain!)
    (is (= {} (:channels @(:state m))) "closed channel removed from mix")))

(run-tests-and-exit)
