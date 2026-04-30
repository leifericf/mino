(ns clojure.test.check)

(require '[clojure.test.check.generators :as gen])
(require '[clojure.test.check.properties :as prop])

;; Minimal clojure.test.check port for mino. Provides quick-check
;; against clojure.test.check.properties/for-all properties. Threads
;; size 0..(num-tests-1) through generators. Shrinking deferred --
;; failure reports the unshrunk failing args.

(defn- now-ns []
  ;; mino doesn't expose nanoTime; use rand for a session-stable but
  ;; not reproducible default seed.
  (long (* 1.0e9 (rand))))

(defn quick-check
  "Run num-tests samples of property `prop`. Options:
     :seed -- 64-bit integer; defaults to a fresh session seed
     :max-size -- upper bound on generator size (default 100)
   Returns a map describing the run."
  [num-tests prop & {:keys [seed max-size]
                     :or   {max-size 100}}]
  (when-not (prop/property? prop)
    (throw (ex-info "quick-check requires a property"
                    {:got prop})))
  (let [actual-seed (or seed (now-ns))]
    (random-seed! actual-seed)
    (loop [i 0]
      (if (>= i num-tests)
        {:result    true
         :num-tests num-tests
         :seed      actual-seed}
        (let [size (mod i max-size)
              r    (prop/run-property prop size)]
          (if (:result r)
            (recur (inc i))
            {:result        false
             :num-tests     (inc i)
             :seed          actual-seed
             :failing-size  size
             :failing-args  (:args r)
             :note          "shrinking is not implemented in this build"}))))))
