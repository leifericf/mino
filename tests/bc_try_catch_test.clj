(require "tests/test")

;; Try/catch/throw exercised inside a defn body so the bytecode compiler
;; takes the form (top-level try forms run on the tree-walker and miss
;; the bc lane regardless). The point is to verify PUSHCATCH /
;; POPCATCH / THROW handle every reasonable nesting and unwind shape;
;; tree-walker parity is the contract, not new semantics.

(deftest bc-try-catch-simple
  (testing "throw in body, caught in same fn"
    (defn t1 [] (try (throw "boom") (catch e (ex-data e))))
    (is (= "boom" (t1))))
  (testing "throw of a map, catch reads :type"
    (defn t2 [] (try (throw {:type :err :v 42}) (catch e (:type (ex-data e)))))
    (is (= :err (t2))))
  (testing "no-throw path returns body value"
    (defn t3 [] (try 99 (catch e :no)))
    (is (= 99 (t3))))
  (testing "body computes through several ops, no throw"
    (defn t4 [] (try (let [a 10 b 20] (+ a b)) (catch e :no)))
    (is (= 30 (t4)))))

(deftest bc-try-catch-deep-throw
  (testing "throw from a callee unwinds into caller's catch"
    (defn deep []  (throw "deep"))
    (defn outer [] (try (deep) (catch e (ex-data e))))
    (is (= "deep" (outer))))
  (testing "throw from nested fn call chain"
    (defn lvl3 [] (throw {:level 3}))
    (defn lvl2 [] (lvl3))
    (defn lvl1 [] (lvl2))
    (defn top  [] (try (lvl1) (catch e (:level (ex-data e)))))
    (is (= 3 (top)))))

(deftest bc-try-catch-nested
  (testing "inner catch and re-throw to outer"
    (defn t1 [] (try (try (throw :inner)
                          (catch e (throw {:from :inner})))
                     (catch e (:from (ex-data e)))))
    (is (= :inner (t1))))
  (testing "inner handles, outer doesn't run"
    (defn t2 [] (try (try (throw :x)
                          (catch e :inner-caught))
                     (catch e :outer-caught)))
    (is (= :inner-caught (t2)))))

(deftest bc-try-finally
  (testing "finally runs on normal completion"
    (let [a (atom 0)]
      (defn t [] (try 7 (finally (reset! a 42))))
      (is (= 7 (t)))
      (is (= 42 @a))))
  (testing "finally runs on uncaught throw"
    (let [a (atom 0)]
      (defn t [] (try (throw "boom")
                      (finally (reset! a 99))))
      (try (t) (catch _ nil))
      (is (= 99 @a))))
  (testing "finally runs on caught throw"
    (let [a (atom 0)]
      (defn t [] (try (throw "x")
                      (catch e :caught)
                      (finally (reset! a 1))))
      (is (= :caught (t)))
      (is (= 1 @a))))
  (testing "finally runs on re-throw from handler"
    (let [a (atom 0)]
      (defn t [] (try (try (throw "inner")
                           (catch e (throw "rethrown"))
                           (finally (swap! a inc)))
                      (catch e (ex-data e))))
      (is (= "rethrown" (t)))
      (is (= 1 @a)))))

(deftest bc-try-error-path
  (testing "thrown? macro works on BC-compiled throws"
    (defn boom-int []  (inc 9223372036854775807))
    (is (thrown? (boom-int))))
  (testing "catch sees normalized diagnostic"
    (defn t [] (try (throw :raw) (catch e (error? e))))
    (is (true? (t))))
  (testing "try value escapes through let/do without losing exception data"
    (defn t [] (let [r (try (do (throw {:k :v}) :unreached)
                            (catch e (ex-data e)))]
                 r))
    (is (= {:k :v} (t)))))

(deftest bc-try-depth-cap-reports
  ;; A recursive (try ...) that hits MAX_TRY_DEPTH must surface a
  ;; throwable diagnostic so the nearest live catch gets a message,
  ;; instead of NULL propagating silently up the whole stack.
  (testing "deep recursive try is caught by next live catch"
    (defn rec__td [n]
      (if (zero? n) :done
          (try (rec__td (dec n)) (catch e :err))))
    ;; well under the cap completes normally
    (is (= :done (rec__td 60)))
    ;; deep call hits the cap; the next live try's catch fires (with
    ;; my fix, set_eval_diag throws into the surrounding try); before
    ;; the fix this returned NULL silently
    (is (= :err (rec__td 200))))
  (testing "MLM002 message visible when no inner catch swallows it"
    (defn rec__td2 [n]
      (if (zero? n) :done
          (try (rec__td2 (dec n))
               (catch e
                 ;; Re-throw so the message stays observable.
                 (throw e)))))
    (is (= "try nesting too deep"
           (try (rec__td2 200) (catch e (ex-message e)))))))

(deftest bc-classed-catch-dispatch
  ;; Classed clauses filter on the diagnostic's :mino/kind per the
  ;; shared class table; first-match-wins, bare still catches all.
  (testing "Exception catches and binds the diagnostic map"
    (defn cc1 [] (try (throw (ex-info "boom" {:a 1}))
                      (catch Exception e (:a (ex-data e)))))
    (is (= 1 (cc1))))
  (testing "out-of-class clause declines to outer"
    (defn cc2 [] (try (try (throw (ex-info "x" {})) (catch Error e :err))
                      (catch Exception e :outer)))
    (is (= :outer (cc2))))
  (testing "first-match-wins across classed clauses"
    (defn cc3 [] (try (throw (ex-info "x" {}))
                      (catch Error e :e)
                      (catch Exception e :x)))
    (is (= :x (cc3))))
  (testing "bare clause after classed still catches"
    (defn cc4 [] (try (throw (ex-info "x" {}))
                      (catch Error e :e)
                      (catch e :bare)))
    (is (= :bare (cc4))))
  (testing ":default keyword catch-all"
    (defn cc5 [] (try (throw (ex-info "x" {})) (catch :default e :d)))
    (is (= :d (cc5))))
  (testing "IndexOutOfBoundsException catches nth OOB"
    (defn cc6 [] (try (nth [1] 5) (catch IndexOutOfBoundsException e :oob)))
    (is (= :oob (cc6)))
    (defn cc7 [] (try (nth [1] 5) (catch Exception e :e)))
    (is (= :e (cc7))))
  (testing "IllegalArgumentException catches an arity error"
    (defn cc8 [] (try ((fn [a] a)) (catch IllegalArgumentException e :iae)))
    (is (= :iae (cc8))))
  (testing "qualified ExceptionInfo symbol matches by name tail"
    (defn cc9 [] (try (throw (ex-info "x" {}))
                      (catch clojure.lang.ExceptionInfo e :ei)))
    (is (= :ei (cc9)))))

(deftest bc-classed-catch-finally
  (testing "finally runs when no clause matches and the throw escapes"
    (let [a (atom 0)]
      (defn cf1 [] (try (throw (ex-info "x" {})) (catch Error e :e)
                        (finally (reset! a 1))))
      (is (thrown? (cf1)))
      (is (= 1 @a))))
  (testing "matched clause runs handler then finally"
    (let [a (atom 0)]
      (defn cf2 [] (try (throw (ex-info "x" {})) (catch Exception e :x)
                        (finally (reset! a 2))))
      (is (= :x (cf2)))
      (is (= 2 @a)))))

(deftest bc-classed-catch-unknown-class-errors
  (defn cu1 [] (try 1 (catch NotAClass e 2)))
  (let [msg (try (cu1) (catch e (ex-message e)))]
    (is (clojure.string/includes? msg "NotAClass"))))

(deftest bc-catch-releases-intermediate-frame-state
  ;; A throw from a deeply-nested fn unwinds through intermediate
  ;; bc_run frames whose bc_pop_window calls are skipped by the
  ;; longjmp. If the catching fn loops over many such catches, the
  ;; intermediate-frame register slots stay pinned as GC roots in
  ;; bc_regs[base..bc_top) across iterations and accumulate -- the
  ;; loop's next OP_CALL pushes its new window ABOVE the leaked
  ;; range. The catch landing must restore bc_top so loop iterations
  ;; reuse the same window range.
  (defn alloc-leak__catch [n]
    (let [big (vec (range 50000))]   ;; ~50k YOUNG cells pinned in a reg
      (throw {:msg "x", :big-size (count big)})))
  (defn middle__catch [n] (alloc-leak__catch n))
  ;; Catching fn that loops and observes peak bytes-live INSIDE its
  ;; own bc_run frame. The catching frame is long-lived (single
  ;; bc_run instance across all iterations), so leaked
  ;; intermediate-frame slots from earlier iterations stay in
  ;; bc_regs[base..bc_top) until this fn returns. The peak inside
  ;; this fn captures the in-flight retention; the value AFTER
  ;; return would not, because bc_pop_window on outermost return
  ;; flushes everything.
  (defn loop-catcher [iters]
    (let [peak (atom 0)]
      (loop [i 0]
        (when (< i iters)
          (try (middle__catch i) (catch e :caught))
          (gc!)
          (let [b (long (:bytes-live (gc-stats)))]
            (swap! peak max b))
          (recur (inc i))))
      @peak))
  (gc!)
  (let [baseline (long (:bytes-live (gc-stats)))
        peak     (loop-catcher 32)
        delta    (- peak baseline)]
    ;; 32 leaked intermediate-frame `big` vecs would each pin a
    ;; 50k-element vector (~400 KB of cells per vec, often 12+ MB
    ;; total). 2 MB is well above unrelated alloc noise and well
    ;; below leak magnitude.
    (is (< delta (* 2 1024 1024))
        (str "bc_top leak suspected: bytes-live peak grew "
             delta " bytes above baseline during 32 looped catches"))))
