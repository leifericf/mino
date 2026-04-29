(require "tests/test")
(require '[clojure.core.async :as a :refer [chan chan? buffer dropping-buffer sliding-buffer promise-chan closed? close! put! take! alts! offer! poll! <!! >!! alts!! timeout go go-loop pipe onto-chan! to-chan! mult tap untap pub sub unsub unsub-all mix admix unmix unmix-all toggle solo-mode pipeline pipeline-async pipeline-blocking chan* chan?* chan-put* chan-take* chan-close* chan-closed?* offer!* poll!* alts* buf-fixed* buf-dropping* buf-sliding* buf-promise* chan-set-xform*]])

(deftest timeout-returns-channel
  (let [ch (timeout 0)]
    (is (true? (chan? ch)))))

(deftest timeout-zero-closes-immediately
  (let [ch     (timeout 0)
        result (atom :pending)]
    ;; timeout(0) should close on next drain
    (take! ch (fn [v] (reset! result v)))
    (drain!)
    (is (nil? @result) "timeout(0) delivers nil via close")))

(deftest timeout-alts-default
  (let [ch  (chan)
        tch (timeout 0)
        [val port] (alts! [ch tch] {:priority true})]
    ;; timeout(0) fires immediately, ch has nothing
    ;; With priority, ch is tried first (no value), then tch (closed = nil)
    (is (nil? val))
    (is (identical? tch port))))

(run-tests-and-exit)
