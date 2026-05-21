(ns clojure.test.check)

(require '[clojure.test.check.generators :as gen])
(require '[clojure.test.check.properties :as prop])

;; clojure.test.check port for mino. Provides quick-check against
;; clojure.test.check.properties/for-all properties, with real
;; rose-tree shrinking on failure. The walker descends greedily into
;; the first failing child of the current node; the first node whose
;; children are all passing is the minimal-counterexample tip.

(defn- now-ns []
  ;; mino doesn't expose nanoTime; use rand for a session-stable but
  ;; not reproducible default seed.
  (long (* 1.0e9 (rand))))

(defn- shrink-loop
  "Walk the rose tree of a failing args vec, descending into the first
  failing child at each level. Returns a map with :smallest, :depth,
  :total-nodes-visited."
  [pred initial-rose]
  (loop [current initial-rose
         depth   0
         visited 1]
    (let [children (gen/rose-children current)
          failing  (some
                     (fn [child]
                       (when-not (prop/pred-passes? pred (gen/rose-val child))
                         child))
                     children)
          v        (count (or children []))]
      (if (nil? failing)
        {:smallest (gen/rose-val current)
         :depth    depth
         :total-nodes-visited (+ visited v)}
        (recur failing (inc depth) (+ visited v))))))

(defn quick-check
  "Run num-tests samples of property `prop`. Options:
     :seed -- 64-bit integer; defaults to a fresh session seed
     :max-size -- upper bound on generator size (default 100)
   Returns a map describing the run. On failure, :shrunk is included
   with the minimal failing argument vector found by walking the
   rose tree."
  [num-tests prop & {:keys [seed max-size]
                     :or   {max-size 100}}]
  (when-not (prop/property? prop)
    (throw (ex-info "quick-check requires a property"
                    {:got prop})))
  (let [actual-seed (or seed (now-ns))
        pred        (:pred prop)]
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
            (let [shrunk (shrink-loop pred (:rose r))]
              {:result        false
               :num-tests     (inc i)
               :seed          actual-seed
               :failing-size  size
               :failing-args  (:args r)
               :shrunk        {:smallest (:smallest shrunk)
                               :depth    (:depth shrunk)
                               :total-nodes-visited
                                         (:total-nodes-visited shrunk)}})))))))
