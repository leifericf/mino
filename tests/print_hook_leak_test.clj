(require "tests/test")
(require '[clojure.string :as str])

;; A print-method that throws unwinds by longjmp from the hook call
;; straight to the enclosing catch, skipping every C frame in the
;; print path. These tests pin what that unwind must not leak.
;;
;; The caught-throw loops live inside their deftest bodies on
;; purpose: the suite runner's fixture-wrap closure is JIT-compiled
;; under the eager lane, so this shape also pins the catch landing
;; pads' jit_invoke_env restore (a torn hook-frame invoke once left
;; the published env stale and a later native env read failed with
;; an unbound gensym).

(defmethod print-method :pin-leak-probe
  [v]
  (throw {:mino/kind :test/pin-leak-probe}))

(def ^:private pin-bomb (with-meta {:x 2} {:type :pin-leak-probe}))

;; Each caught throw tears through the hook's C frame, which holds a
;; pinned capture sink. The catch landing pad must rewind the pin
;; watermark; before it did, this loop overflowed the fixed pin stack
;; (a loud abort under the sanitizer lanes, silent pin-array soft-loss
;; and a stale-root hazard otherwise).
(deftest pin-stack-rewound-per-caught-throw
  (is (= :caught (try
                   (with-out-str (pr "x" pin-bomb))
                   :not-thrown
                   (catch :test/pin-leak-probe e :caught))))
  (dotimes [_ 800]
    (try
      (with-out-str (pr "x" pin-bomb))
      (catch :test/pin-leak-probe e nil)))
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
  directly, so $PPID inside the child shell is this process. Qualified
  because the suite shares one namespace and the core.logic test file
  refers its own run macro into it."
  []
  (parse-long
   (str/trim (:out (clojure.core/run "sh" "-c" "ps -o rss= -p $PPID")))))

(def ^:private payload
  ;; 256 KiB of random printable ASCII, built in 1 KiB pieces so the
  ;; construction itself stays cheap in the shared suite process.
  (str/join (repeatedly 256 (fn []
                              (apply str (repeatedly 1024
                                                     #(char (+ 33 (rand-int 94)))))))))

(deftest print-chunk-reclaimed-when-hook-throws
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
      (let [growth (- (rss-kb) before)]
        (is (< growth (* 48 1024))
            (str "RSS grew " growth
                 " KiB across 300 caught print-method throws"))))))

(deftest print-usable-after-hook-throw
  (is (= "[1 2]" (with-out-str (pr [1 2]))))
  (is (= "1 2\n" (with-out-str (println 1 2)))))

;; A hook that pushes a thread-binding frame and never pops it must
;; not corrupt the dyn stack: the post-hook unwind is anchored on the
;; print path's own frame, freeing anything the hook left above it,
;; so the stray binding neither survives the call nor upsets later
;; binding forms.
(defmethod print-method :push-no-pop-probe
  [v]
  (push-thread-bindings {'*print-length* 1})
  (pr-builtin (:payload v)))

(def ^:private push-no-pop-obj
  (with-meta {:payload [1 2 3]} {:type :push-no-pop-probe}))

(deftest hook-pushed-bindings-are-unwound
  (is (= "[1 2 3]" (with-out-str (pr push-no-pop-obj))))
  ;; The stray *print-length* binding did not leak out of the call.
  (is (= "[1 2 3]" (pr-str [1 2 3])))
  (dotimes [_ 50]
    (with-out-str (pr push-no-pop-obj)))
  (is (= "[9 8]" (binding [*print-length* nil] (pr-str [9 8]))))
  (is (= "(0 1 ...)" (binding [*print-length* 2] (pr-str (range 5))))))

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
          out (:out (clojure.core/run
                     "sh" "-c"
                     (str "printf '%s' '" script "' | ./mino 2>&1")))]
      (is (str/includes? out ":alive")
          (str "top-level print after a caught-at-top hook throw"
               " produced: " (pr-str out))))))

(run-tests-and-exit)
