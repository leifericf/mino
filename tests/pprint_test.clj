(require "tests/test")
(require '[clojure.pprint :as pp])
(require '[clojure.string :as str])

;; clojure.pprint: pretty-printer engine, printer control vars, and
;; the dispatch-function interface (logical blocks, conditional
;; newlines, indents, custom dispatch).

;; --- helpers ---

(defn- pprint-lines [s] (str/split-lines s))

(defn- pprint-fits? [s width]
  (every? (fn [line] (<= (count line) width)) (pprint-lines s)))

;; Custom dispatch functions for the dispatch-interface tests. Each
;; writes through the pretty writer the engine binds to *out*.

(defn- pprint-sentinel-dispatch [_]
  (print "SENTINEL"))

(defn- pprint-triple-dispatch [v]
  (pp/pprint-logical-block :prefix "[" :suffix "]"
    (pp/write-out (nth v 0))
    (pp/pprint-newline :mandatory)
    (pp/write-out (nth v 1))
    (pp/pprint-newline :mandatory)
    (pp/write-out (nth v 2))))

(defn- pprint-indent-dispatch [v]
  (pp/pprint-logical-block :prefix "[" :suffix "]"
    (pp/write-out (nth v 0))
    (pp/pprint-indent :block 2)
    (pp/pprint-newline :mandatory)
    (pp/write-out (nth v 1))))

(defn- pprint-pll-dispatch [coll]
  (pp/pprint-logical-block :prefix "(" :suffix ")"
    (pp/print-length-loop [xs (seq coll)]
      (when xs
        (pp/write-out (first xs))
        (when (next xs)
          (print " ")
          (recur (next xs)))))))

;; --- public API surface ---

(deftest pprint-public-api-presence
  (doseq [s '[clojure.pprint/*print-base*
              clojure.pprint/*print-miser-width*
              clojure.pprint/*print-pprint-dispatch*
              clojure.pprint/*print-pretty*
              clojure.pprint/*print-radix*
              clojure.pprint/*print-right-margin*
              clojure.pprint/*print-suppress-namespaces*
              clojure.pprint/fresh-line
              clojure.pprint/get-pretty-writer
              clojure.pprint/pp
              clojure.pprint/pprint
              clojure.pprint/pprint-indent
              clojure.pprint/pprint-logical-block
              clojure.pprint/pprint-newline
              clojure.pprint/pprint-tab
              clojure.pprint/print-length-loop
              clojure.pprint/set-pprint-dispatch
              clojure.pprint/simple-dispatch
              clojure.pprint/with-pprint-dispatch
              clojure.pprint/write
              clojure.pprint/write-out]]
    (is (some? (resolve s)) (str s))))

(deftest pprint-control-var-defaults
  (is (= 72 pp/*print-right-margin*))
  (is (= 40 pp/*print-miser-width*))
  (is (= 10 pp/*print-base*))
  (is (true? pp/*print-pretty*))
  (is (not pp/*print-radix*))
  (is (not pp/*print-suppress-namespaces*))
  (is (some? pp/*print-pprint-dispatch*)))

;; --- pprint pretty-prints ---

(deftest pprint-small-values-stay-on-one-line
  (is (= "{:a 1}\n" (with-out-str (pp/pprint {:a 1}))))
  (is (= "[1 2 3]\n" (with-out-str (pp/pprint [1 2 3]))))
  (is (= "\"påske\"\n" (with-out-str (pp/pprint "påske"))))
  (is (= ":k\n" (with-out-str (pp/pprint :k))))
  (is (= "42\n" (with-out-str (pp/pprint 42))))
  (is (= "nil\n" (with-out-str (pp/pprint nil))))
  (is (= "[]\n" (with-out-str (pp/pprint []))))
  (is (= "{}\n" (with-out-str (pp/pprint {})))))

(deftest pprint-wide-vector-wraps-at-default-margin
  (let [v (vec (range 30))
        out (with-out-str (pp/pprint v))
        lines (pprint-lines out)]
    (is (str/ends-with? out "\n"))
    (is (> (count lines) 1))
    (is (pprint-fits? out 72))
    (is (= v (read-string out)))))

(deftest pprint-right-margin-rebinding-wraps-narrower
  (let [v (vec (range 5))]
    (is (= "[0 1 2 3 4]\n" (with-out-str (pp/pprint v))))
    (let [out (binding [pp/*print-right-margin* 8]
                (with-out-str (pp/pprint v)))
          lines (pprint-lines out)]
      (is (> (count lines) 1))
      (is (pprint-fits? out 8))
      (is (= v (read-string out))))))

(deftest pprint-map-breaks-between-pairs
  ;; Pair order in the printed map is not specified; assert the
  ;; structure (one pair per line, margin honored) and roundtrip.
  (let [m {:aaaa 1 :bbbb 2 :cccc 3}
        out (binding [pp/*print-right-margin* 20]
              (with-out-str (pp/pprint m)))
        lines (pprint-lines out)]
    (is (= 3 (count lines)))
    (is (pprint-fits? out 20))
    (is (every? (fn [line] (some? (re-find #":[a-z]+ \d" line))) lines))
    (is (= m (read-string out)))))

(deftest pprint-nested-collections-indent
  (let [v [[:alpha :beta] [:gamma :delta] [:epsilon :zeta]]
        out (binding [pp/*print-right-margin* 20]
              (with-out-str (pp/pprint v)))
        lines (pprint-lines out)]
    (is (= 3 (count lines)))
    (is (str/starts-with? (first lines) "[["))
    (is (every? (fn [line] (str/starts-with? line " ")) (rest lines)))
    (is (pprint-fits? out 20))
    (is (= v (read-string out)))))

;; --- write ---

(deftest pprint-write-stream-nil-returns-string
  (is (= "{:a 1}" (pp/write {:a 1} :stream nil)))
  (let [captured (atom nil)
        out (with-out-str (reset! captured (pp/write [1 2 3] :stream nil)))]
    (is (= "" out))
    (is (= "[1 2 3]" @captured))))

(deftest pprint-write-prints-to-out-by-default
  (let [ret (atom ::unset)
        out (with-out-str (reset! ret (pp/write {:a 1})))]
    (is (= "{:a 1}" out))
    (is (nil? @ret))))

(deftest pprint-write-pretty-and-right-margin-options
  (let [v (vec (range 30))]
    (is (= (pr-str v) (pp/write v :pretty false :stream nil)))
    (is (= (pr-str v)
           (binding [pp/*print-pretty* false] (pp/write v :stream nil))))
    (let [out (pp/write v :pretty true :right-margin 20 :stream nil)]
      (is (> (count (pprint-lines out)) 1))
      (is (pprint-fits? out 20))
      (is (= v (read-string out))))))

(deftest pprint-write-base-and-radix-options
  (is (= "10" (pp/write 10 :stream nil)))
  (is (= "ff" (pp/write 255 :base 16 :stream nil)))
  (is (= "#xff" (pp/write 255 :base 16 :radix true :stream nil)))
  (is (= "#o12" (pp/write 10 :base 8 :radix true :stream nil)))
  (is (= "#b10" (pp/write 2 :base 2 :radix true :stream nil)))
  (is (= "10." (pp/write 10 :radix true :stream nil))))

(deftest pprint-base-and-radix-vars-affect-pprint
  (is (= "255\n" (with-out-str (pp/pprint 255))))
  (is (= "ff\n" (binding [pp/*print-base* 16]
                  (with-out-str (pp/pprint 255)))))
  (is (= "#o12\n" (binding [pp/*print-base* 8
                            pp/*print-radix* true]
                    (with-out-str (pp/pprint 10))))))

(deftest pprint-suppress-namespaces-var
  (is (= "clojure.core/map\n" (with-out-str (pp/pprint 'clojure.core/map))))
  (is (= "map\n" (binding [pp/*print-suppress-namespaces* true]
                   (with-out-str (pp/pprint 'clojure.core/map))))))

;; --- dispatch ---

(deftest pprint-with-pprint-dispatch-scopes-and-restores
  (is (= "SENTINEL\n"
         (with-out-str
           (pp/with-pprint-dispatch pprint-sentinel-dispatch
             (pp/pprint 42)))))
  (is (= "42\n" (with-out-str (pp/pprint 42)))))

(deftest pprint-simple-dispatch-is-the-default
  (is (= "{:a 1}\n"
         (with-out-str
           (pp/with-pprint-dispatch pp/simple-dispatch
             (pp/pprint {:a 1}))))))

(deftest pprint-set-pprint-dispatch-global-and-restore
  (try
    (pp/set-pprint-dispatch pprint-sentinel-dispatch)
    (is (= "SENTINEL\n" (with-out-str (pp/pprint :anything))))
    (finally
      (pp/set-pprint-dispatch pp/simple-dispatch)))
  (is (= ":anything\n" (with-out-str (pp/pprint :anything)))))

(deftest pprint-logical-block-mandatory-newlines
  (let [out (with-out-str
              (pp/with-pprint-dispatch pprint-triple-dispatch
                (pp/pprint [1 2 3])))
        lines (pprint-lines out)]
    (is (= 3 (count lines)))
    (is (str/starts-with? (first lines) "[1"))
    (is (= "2" (str/trim (nth lines 1))))
    (is (str/ends-with? (nth lines 2) "3]"))))

(deftest pprint-indent-shifts-continuation-lines
  (let [out (with-out-str
              (pp/with-pprint-dispatch pprint-indent-dispatch
                (pp/pprint [10 20])))
        lines (pprint-lines out)]
    (is (= 2 (count lines)))
    (is (= "[10" (first lines)))
    (is (str/starts-with? (nth lines 1) "  "))
    (is (= "20]" (str/trim (nth lines 1))))))

(deftest pprint-print-length-loop-honors-print-length
  (let [out (binding [*print-length* 3]
              (with-out-str
                (pp/with-pprint-dispatch pprint-pll-dispatch
                  (pp/pprint '(1 2 3 4 5 6)))))]
    (is (= "(1 2 3 ...)" (str/trim out))))
  (let [out (with-out-str
              (pp/with-pprint-dispatch pprint-pll-dispatch
                (pp/pprint '(1 2 3))))]
    (is (= "(1 2 3)" (str/trim out)))))

;; --- column-aware helpers ---

(deftest pprint-fresh-line-only-when-mid-line
  (is (= "a\n" (with-out-str (print "a") (pp/fresh-line))))
  (is (= "a\n" (with-out-str (println "a") (pp/fresh-line)))))

(deftest pprint-get-pretty-writer-wraps-out
  (let [result (atom nil)]
    (with-out-str (reset! result (some? (pp/get-pretty-writer *out*))))
    (is (true? @result))))

(run-tests-and-exit)
