(require "tests/test")
(require '[clojure.string :as str])

;; A print-method that throws unwinds by longjmp from the hook call
;; straight to the enclosing catch, skipping every C frame in the
;; print path. These tests pin what that unwind must not leak.
;;
;; The caught-throw loops run inline at the top level, not inside
;; deftest bodies or helper fns: the eager JIT lane today miscompiles
;; a caught print-method throw inside a compiled fn (an
;; unbound-gensym failure, tracked as its own bug), and keeping the
;; loops here keeps this file's assertions independent of that
;; defect.

(defmethod print-method :pin-leak-probe
  [v]
  (throw {:mino/kind :test/pin-leak-probe}))

(def ^:private pin-bomb (with-meta {:x 2} {:type :pin-leak-probe}))

(def ^:private first-round-result
  (try
    (with-out-str (pr "x" pin-bomb))
    :not-thrown
    (catch :test/pin-leak-probe e :caught)))

;; Each caught throw tears through the hook's C frame, which holds a
;; pinned capture sink. The catch landing pad must rewind the pin
;; watermark; before it did, this loop overflowed the fixed pin stack
;; (a loud abort under the sanitizer lanes, silent pin-array soft-loss
;; and a stale-root hazard otherwise).
(def ^:private pin-rounds-result
  (do (dotimes [_ 800]
        (try
          (with-out-str (pr "x" pin-bomb))
          (catch :test/pin-leak-probe e nil)))
      :completed))

(deftest pin-stack-rewound-per-caught-throw
  (is (= :caught first-round-result))
  (is (= :completed pin-rounds-result))
  (is (= "\"ok\"" (with-out-str (pr "ok")))))

;; The pr/prn output chunk must be reclaimed when the hook throws.
;; Asserted with a bounded RSS probe, not wall-clock: each round
;; below fills the chunk with ~256 KiB before the second arg's
;; print-method throws, and the leak was one chunk per caught throw
;; (about 100 MiB across the measured window) when the chunk lived
;; in a raw allocation the longjmp skipped. The payload is random
;; ASCII so a host's memory compressor cannot squeeze the leaked
;; pages out of the RSS number; the warm-up loop saturates allocator
;; arenas (and a sanitizer build's free quarantine) so the measured
;; window sees a steady state; the gc! per round runs the finalizers
;; of the GC-owned capture buffers the unwind abandoned, so only an
;; allocation the collector cannot see moves the number.

(def ^:private windows? (some? (getenv "OS")))

(defn- rss-kb
  "Resident set size of this process in KiB. run forks the probe
  directly, so $PPID inside the child shell is this process."
  []
  (parse-long (str/trim (:out (run "sh" "-c" "ps -o rss= -p $PPID")))))

(def ^:private payload
  ;; 256 KiB of random printable ASCII, built in 1 KiB pieces so the
  ;; construction itself stays cheap in the shared suite process.
  (str/join (repeatedly 256 (fn []
                              (apply str (repeatedly 1024
                                                     #(char (+ 33 (rand-int 94)))))))))

(def ^:private chunk-growth-kb
  (when-not windows?
    (dotimes [_ 200]
      (try
        (with-out-str (pr payload pin-bomb))
        (catch :test/pin-leak-probe e nil))
      (gc!))
    (let [before (rss-kb)]
      (dotimes [_ 300]
        (try
          (with-out-str (pr payload pin-bomb))
          (catch :test/pin-leak-probe e nil))
        (gc!))
      (- (rss-kb) before))))

(deftest print-chunk-reclaimed-when-hook-throws
  (when-not windows?
    (is (< chunk-growth-kb (* 48 1024))
        (str "RSS grew " chunk-growth-kb
             " KiB across 300 caught print-method throws"))))

(deftest print-usable-after-hook-throw
  (is (= "[1 2]" (with-out-str (pr [1 2]))))
  (is (= "1 2\n" (with-out-str (println 1 2)))))

;; With no script-side try at all, the throw lands on the top-level
;; eval pad. That pad must unwind the dyn frames the longjmp tore
;; through, or *out* stays bound to the hook's dead capture sink and
;; every later print on the session vanishes into it (and the frames
;; leak). Driven in a subprocess because the throw must reach the
;; top level uncaught, which would abort the shared suite process.
(deftest top-level-pad-unwinds-dyn-frames
  (when-not windows?
    (let [script (str "(defmethod print-method :d [v]"
                      " (throw {:mino/kind :t/d}))\n"
                      "(pr (with-meta {} {:type :d}))\n"
                      "(println :alive)\n")
          out (:out (run "sh" "-c"
                         (str "printf '%s' '" script "' | ./mino 2>&1")))]
      (is (str/includes? out ":alive")
          (str "top-level print after a caught-at-top hook throw"
               " produced: " (pr-str out))))))

(run-tests-and-exit)
