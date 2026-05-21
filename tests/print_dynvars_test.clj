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
