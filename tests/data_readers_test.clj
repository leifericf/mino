(require "tests/test")

(require '[clojure.instant])

;; The reader resolves #tag forms at read time. Resolution order:
;;   1. (get *data-readers* 'tag) -- per-tag reader fn
;;   2. *default-data-reader-fn*  -- catch-all
;;   3. tagged-literal record fallback

(deftest reader-default-emits-tagged-literal-record
  (let [t (read-string "#foo bar")]
    (is (tagged-literal? t))
    (is (= 'foo (:tag t)))
    (is (= 'bar (:form t)))))

(deftest reader-default-tag-is-symbol-not-keyword
  ;; Canonical Clojure stores tags as symbols, not keywords. This
  ;; locks in the canon-parity decision made in v0.82.
  (let [t (read-string "#instant 42")]
    (is (symbol? (:tag t)))
    (is (= 'instant (:tag t)))))

(deftest reader-uses-data-readers-binding
  (is (= "bar"
         (binding [*data-readers* {'foo str}]
           (read-string "#foo bar"))))
  (is (= [:wrap 42]
         (binding [*data-readers* {'wrap (fn [v] [:wrap v])}]
           (read-string "#wrap 42")))))

(deftest reader-falls-through-to-default-data-reader-fn
  (is (= [:default 'whatever 42]
         (binding [*default-data-reader-fn* (fn [t f] [:default t f])]
           (read-string "#whatever 42")))))

(deftest reader-prefers-data-readers-over-default-fn
  (is (= "specific-bar"
         (binding [*data-readers*           {'tag (fn [v] (str "specific-" v))}
                   *default-data-reader-fn* (fn [t f] [:fallback t f])]
           (read-string "#tag bar")))))

(deftest reader-multiple-tags-mixed
  (let [readers {'a inc 'b dec}]
    (is (= [3 1]
           (binding [*data-readers* readers]
             [(read-string "#a 2") (read-string "#b 2")])))))

(deftest reader-time-resolution-not-eval-time
  ;; The binding visible at read-string call time decides the reader
  ;; fn. A subsequent rebind doesn't retroactively change the value.
  (let [v (binding [*data-readers* {'foo str}]
            (read-string "#foo abc"))]
    (binding [*data-readers* {'foo (fn [_] :other)}]
      (is (= "abc" v)))))

(deftest reader-default-fallback-when-no-binding
  ;; Outside any binding, *data-readers* is empty by default.
  (let [t (read-string "#unknown 99")]
    (is (tagged-literal? t))
    (is (= 'unknown (:tag t)))
    (is (= 99 (:form t)))))

(deftest inst-reader-via-clojure-instant
  ;; clojure.instant ships with read-instant-date returning a
  ;; component map (mino divergence: no host Date type). Wired
  ;; through *data-readers*, #inst returns the same shape.
  (let [m (binding [*data-readers* {'inst clojure.instant/read-instant-date}]
            (read-string "#inst \"2026-04-27\""))]
    (is (= 2026 (:years m)))
    (is (= 4    (:months m)))
    (is (= 27   (:days m)))))

(deftest tagged-literal-constructor-unchanged
  ;; Direct (tagged-literal ...) calls still work with any tag value;
  ;; only the reader was tightened to emit symbols.
  (let [t (tagged-literal 'sym 1)]
    (is (tagged-literal? t))
    (is (= 'sym (:tag t))))
  (let [t (tagged-literal :kw 1)]
    (is (tagged-literal? t))
    (is (= :kw (:tag t)))))
