(require "tests/test")
(require "core/channel")
(require "core/async")

;; Stress-spawn tests: exercise the bot-fleet pattern at counts that
;; used to hit a precise-GC heap-use-after-free inside hamt_assoc.
;; Each go-loop parks on (<! in) and the outer loop retains every
;; channel so the scheduler's run queue, the atom-held roster, and
;; the go-state-machine closures all accumulate under GC pressure.

(defn- spawn-channels
  "Create n chan+go-loop pairs, return a vector of the input channels.
  Each go-loop parks on (<! in) and exits when the channel closes."
  [n]
  (let [roster (atom [])]
    (loop [i 0]
      (when (< i n)
        (let [in (chan 8)]
          (go-loop []
            (let [msg (<! in)]
              (when msg (recur))))
          (swap! roster conj in))
        (recur (inc i))))
    @roster))

(deftest spawn-1000-goloops-with-roster
  (let [roster (spawn-channels 1000)]
    (is (= 1000 (count roster)))
    (is (true? (chan? (first roster))))))

(deftest spawn-many-goloops-no-roster
  (let [n 1000]
    (loop [i 0]
      (when (< i n)
        (let [in (chan 8)]
          (go-loop []
            (let [msg (<! in)]
              (when msg (recur)))))
        (recur (inc i))))
    (is (true? true) "spawn-without-retention completed")))

(deftest spawn-1000-with-per-bot-atom
  (let [n      1000
        roster (atom [])]
    (loop [i 0]
      (when (< i n)
        (let [in (chan 8)
              st (atom {:hp 100 :xp 0})]
          (go-loop []
            (let [msg (<! in)]
              (when msg
                (swap! st update :hp dec)
                (recur))))
          (swap! roster conj {:in in :st st}))
        (recur (inc i))))
    (is (= n (count @roster)))
    (is (= 100 (:hp @(:st (first @roster)))))))

(deftest gc-bang-returns-nil-and-ticks-major
  (let [before (:collections-major (gc-stats))]
    (is (nil? (gc!)))
    (is (> (:collections-major (gc-stats)) before))))
