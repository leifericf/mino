(require "tests/test")
(require '[clojure.data.json :as json])
(require '[clojure.test.check :as tc])
(require '[clojure.test.check.properties :as prop])
(require '[clojure.test.check.generators :as gen])

;; Generative properties for the JSON reader and writer. Each
;; property runs a bounded number of trials so the lane stays fast;
;; a failure here is a real codec bug and gets pinned as a
;; regression beside the property.
;;
;; This file runs in its own lane (task test-generative) with the
;; JIT disabled: the property loops push clojure.data.json hot
;; enough to trip a CPJIT defect (tracked as a known issue), and
;; the interpreter path is sound at the same seeds. Depth is capped
;; at 2 because trial size grows to ~60 and a third nesting level
;; makes generation combinatorial (minutes per property under the
;; interpreter).

(def trials 60)

(defn- one-of
  "Pick one generator from gens. Elements yields a generator as a
  plain value; bind then runs it."
  [gens]
  (gen/bind (gen/elements gens) identity))

;; JSON has no NaN or Infinity, and the writer correctly rejects them
;; the way clojure.data.json does, so a round-trip property must draw
;; only the finite doubles JSON can represent.
(def ^:private json-double-gen
  (gen/such-that (fn [d] (and (not (NaN? d)) (not (infinite? d)))) gen/double))

(defn- json-leaf-gen []
  (one-of [(gen/return nil)
           (gen/return true)
           (gen/return false)
           gen/int
           json-double-gen
           gen/string]))

(defn json-gen
  "Depth-bounded generator over the value shapes JSON round-trips:
  nil, booleans, integers, doubles, strings, vectors, and maps with
  string keys."
  [depth]
  (if (pos? depth)
    (one-of [(json-leaf-gen)
             (gen/fmap vec (gen/vector (json-gen (dec depth))))
             (gen/fmap #(into {} %)
                       (gen/vector (gen/tuple gen/string-alphanumeric
                                              (json-gen (dec depth)))))])
    (json-leaf-gen)))

(defn qc [prop-fn]
  (:result (tc/quick-check trials prop-fn)))

(deftest random-values-round-trip
  (is (qc (prop/for-all [v (json-gen 2)]
           (= v (json/read-str (json/write-str v)))))))

(deftest random-printable-strings-round-trip
  ;; gen/string spans the printable ASCII range, so quotes,
  ;; backslashes, and the control-adjacent escapes all appear.
  (is (qc (prop/for-all [s gen/string]
           (= s (json/read-str (json/write-str s)))))))

(def sample-doc
  (json/write-str {"users" [{"name" "alice" "age" 30 "tags" ["a" "b"]}
                            {"name" "bob" "scores" [1.5 -2 1e-05]}]
                   "count" 2 "ok" true "note" nil}))

(deftest truncations-throw-rather-than-hang
  ;; Cutting a valid document at any offset yields malformed input;
  ;; the reader must reject it as data, never return a silent value
  ;; or loop forever.
  (dotimes [i (count sample-doc)]
    (is (thrown? (json/read-str (subs sample-doc 0 i))))))

(deftest corruption-of-last-byte-throws
  (let [corrupt (str (subs sample-doc 0 (dec (count sample-doc))) "#")]
    (is (thrown? (json/read-str corrupt)))))

(deftest adversarial-strings-round-trip
  (dotimes [_ 40]
    (let [s (str "\"\\\n\r\t\b\f/ \u0001 \u001f \u007f"
                 (rand-nth ["\u00e9" "\u4e2d" ""])
                 (rand-nth ["quote\"" "back\\slash" ""]))]
      (is (= s (json/read-str (json/write-str s)))))))

(run-tests-and-exit)
