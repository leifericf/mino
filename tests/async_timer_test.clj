(require "tests/test")
(require "core/async")

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
