(require "tests/test")

;; *print-length* / *print-level* exposed in v0.403.0. Resolved once
;; per top-level pr / print / pr-str call from the dynamic binding
;; stack, cached on state, consulted by every collection printer.

(deftest print-length-vector
  (is (= "[1 2 3]"      (binding [*print-length* 5] (pr-str [1 2 3]))))
  (is (= "[1 2 3 ...]"  (binding [*print-length* 3] (pr-str [1 2 3 4 5]))))
  (is (= "[...]"        (binding [*print-length* 0] (pr-str [1 2 3]))))
  (is (= "[1 2 3 4 5]"  (pr-str [1 2 3 4 5]))))

(deftest print-length-list
  (is (= "(1 2 3 ...)" (binding [*print-length* 3] (pr-str '(1 2 3 4 5)))))
  (is (= "(0 1 2 ...)" (binding [*print-length* 3] (pr-str (range 10))))))

(deftest print-length-map
  ;; Map entries are unordered so we can only check the truncation marker
  ;; and the total truncated count. Use sorted-map for a deterministic
  ;; key order in this test.
  (let [printed (binding [*print-length* 2] (pr-str {:a 1 :b 2 :c 3 :d 4}))]
    (is (some? (re-find #", \.\.\." printed)))))

(deftest print-length-set
  (let [printed (binding [*print-length* 1] (pr-str #{1 2 3 4 5}))]
    (is (some? (re-find #" \.\.\." printed)))))

(deftest print-level-collapse
  (is (= "[# #]"       (binding [*print-level* 1] (pr-str [[1] [2]]))))
  (is (= "[[#] [#]]"   (binding [*print-level* 2] (pr-str [[[1]] [[2]]]))))
  (is (= "#"           (binding [*print-level* 0] (pr-str [1 2 3]))))
  (is (= "[[1] [2]]"   (pr-str [[1] [2]]))))

(deftest print-length-and-level-combined
  (let [printed (binding [*print-length* 1 *print-level* 2]
                  (pr-str {:a [[1 2]] :b [[3 4]] :c [[5 6]]}))]
    (is (some? (re-find #"\.\.\." printed)))))

(deftest print-dynvars-restore-after
  ;; The state's cached limit must be restored after a top-level pr,
  ;; so that subsequent prints outside the binding see no limit.
  (binding [*print-length* 2] (pr-str [1 2 3]))
  (is (= "[1 2 3 4 5]" (pr-str [1 2 3 4 5]))))

(deftest print-method-extension
  ;; Late-binding hook for user-extensible printing: lock in that
  ;; defmethod print-method <type> routes pr through the custom impl.
  (defmethod print-method :shouty [v] (print "<SHOUTY>"))
  (is (= "<SHOUTY>" (with-out-str (pr ^{:type :shouty} {:s "hi"}))))
  ;; Outer prn newline wraps the user-written body
  (is (= "<SHOUTY>\n" (with-out-str (prn ^{:type :shouty} {:s "hi"})))))

;; *print-readably*: when false, strings and chars print their raw
;; bytes through pr/prn (i.e. pr/prn behaves like print/println).
;; Nested string/char values inside collections honor the binding too.
(deftest print-readably-strings
  (is (= "\"x\""   (pr-str "x")))
  (is (= "x"       (binding [*print-readably* false] (pr-str "x"))))
  (is (= "[x y]"   (binding [*print-readably* false] (pr-str ["x" "y"]))))
  (is (= "\\a"     (pr-str \a)))
  (is (= "a"       (binding [*print-readably* false] (pr-str \a))))
  ;; bound to nil = falsy
  (is (= "x"       (binding [*print-readably* nil] (pr-str "x")))))

;; *print-meta*: emit ^meta prefix before any value carrying non-nil
;; meta. The flag is silent when the value has no meta.
(deftest print-meta-prefix
  (is (= "{:x 1}" (pr-str {:x 1})))
  (is (= "{:x 1}" (binding [*print-meta* true] (pr-str {:x 1}))))
  (let [m  (with-meta {:x 1} {:tag :hint})
        printed (binding [*print-meta* true] (pr-str m))]
    (is (some? (re-find #"\^" printed)))
    (is (some? (re-find #":tag :hint" printed))))
  ;; Default still false: no ^meta in the printed form.
  (let [m       (with-meta {:x 1} {:tag :hint})
        printed (pr-str m)]
    (is (nil? (re-find #"\^" printed)))))

;; *print-dup*: defined as a dynvar, may be bound without error;
;; mino's existing pr forms for built-in types are already
;; reader-roundtrip-compatible so the flag is an information channel
;; for user code rather than a content switch on the C side.
(deftest print-dup-binding-honored
  (is (false? (boolean *print-dup*)))
  (is (true?  (binding [*print-dup* true] (true? *print-dup*))))
  ;; pr still works under the binding.
  (is (= "[1 2 3]" (binding [*print-dup* true] (pr-str [1 2 3])))))

;; *print-namespace-maps*: a map whose keys all share one namespace
;; prints as #:ns{...} with the prefix stripped from each key.
(deftest print-namespace-maps-collapse
  (is (= "{:a/x 1, :a/y 2}"
         (pr-str (sorted-map :a/x 1 :a/y 2))))
  (is (= "#:a{:x 1, :y 2}"
         (binding [*print-namespace-maps* true]
           (pr-str (sorted-map :a/x 1 :a/y 2)))))
  ;; Mixed-ns maps fall through to the long form even when the flag is on.
  (is (= "{:a/x 1, :b/y 2}"
         (binding [*print-namespace-maps* true]
           (pr-str (sorted-map :a/x 1 :b/y 2)))))
  ;; Maps with un-namespaced keys also fall through.
  (is (= "{:x 1, :y 2}"
         (binding [*print-namespace-maps* true]
           (pr-str (sorted-map :x 1 :y 2))))))

;; *flush-on-newline*: bindable; the print/prn path observes it.
;; We can't directly observe flush vs no-flush without a custom sink,
;; but we lock in that the dynvar is readable and the printer doesn't
;; throw with it set either way.
(deftest flush-on-newline-binding-no-throw
  (is (true? (boolean *flush-on-newline*)))
  (is (= nil  (binding [*flush-on-newline* false] (println "a"))))
  (is (= nil  (binding [*flush-on-newline* true]  (println "b")))))
