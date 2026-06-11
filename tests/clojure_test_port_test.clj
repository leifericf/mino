(require "tests/test")

;; clojure.test surface tests: definition forms, assertion machinery,
;; report dispatch and counters, fixture composition, and the
;; namespace-level runners.

;; --- Helpers ---

(defn- tp-quietly
  "Calls thunk with test reporting output routed into a discarded
  string buffer; returns thunk's value."
  [thunk]
  (let [result (atom nil)]
    (with-out-str
      (binding [*test-out* *out*]
        (reset! result (thunk))))
    @result))

(defn- tp-counted
  "Runs thunk with a fresh counter map bound to *report-counters*,
  reporting output silenced. Returns the final counter map."
  [thunk]
  (tp-quietly
   (fn []
     (binding [*report-counters* (atom *initial-report-counters*)]
       (thunk)
       @*report-counters*))))

(defn- tp-reported
  "Runs thunk with report rebound to a collecting fn. Returns a map of
  the thunk's return value and the report events it produced, in
  order: {:value v :events [...]}."
  [thunk]
  (let [log (atom [])
        v   (binding [report (fn [m] (swap! log conj m))]
              (thunk))]
    {:value v :events @log}))

(defn- tp-fresh-ns
  "Removes any stale namespace named ns-sym, then recreates it empty."
  [ns-sym]
  (when (find-ns ns-sym) (remove-ns ns-sym))
  (create-ns ns-sym)
  ns-sym)

(defn- tp-test-var!
  "Interns vname in ns-sym with f as both the value and the :test
  metadata fn; returns the var."
  [ns-sym vname f]
  (let [v (intern ns-sym vname f)]
    (alter-meta! v assoc :test f)
    v))

;; --- Shared sample definitions ---

(deftest tp-canon-sample
  (is (= 1 1)))

(def tp-gpu-bound 41)
(declare tp-gpu-unbound)

(def tp-st-target :unchanged)
(alter-meta! (var tp-st-target) assoc :test (fn [] :original))

;; --- Assertions keep working ---

(deftest tp-is-still-passes
  (is (= 1 1))
  (is (= 4 (+ 2 2)) "documented message"))

;; --- Reporting globals ---

(deftest tp-initial-report-counters-starts-at-zero
  (is (= 0 (:test *initial-report-counters*)))
  (is (= 0 (:pass *initial-report-counters*)))
  (is (= 0 (:fail *initial-report-counters*)))
  (is (= 0 (:error *initial-report-counters*))))

(deftest tp-load-tests-defaults-true
  (is (true? *load-tests*)))

(deftest tp-load-tests-false-omits-tests
  ;; With *load-tests* bound to false, with-test still evaluates the
  ;; definition but attaches no :test metadata, and deftest defines
  ;; nothing at all.
  (binding [*load-tests* false]
    (eval '(with-test (def tp-lt-def 17) (is (= 1 2))))
    (eval '(deftest tp-lt-suppressed (is (= 1 2)))))
  (is (= 17 (var-get (resolve 'tp-lt-def))))
  (is (nil? (:test (meta (resolve 'tp-lt-def)))))
  (is (nil? (resolve 'tp-lt-suppressed))))

(deftest tp-stack-trace-depth-defaults-nil
  (is (nil? *stack-trace-depth*))
  (is (= 3 (binding [*stack-trace-depth* 3] *stack-trace-depth*))))

(deftest tp-with-test-out-binds-out
  (is (some? *test-out*))
  ;; with-test-out evaluates its body with *out* bound to the current
  ;; value of *test-out*.
  (is (= "routed" (with-out-str
                    (binding [*test-out* *out*]
                      (with-test-out (print "routed"))))))
  (is (= :sentinel (binding [*test-out* :sentinel]
                     (with-test-out *out*)))))

;; --- Test context strings ---

(deftest tp-testing-vars-defaults-empty
  (is (empty? *testing-vars*)))

(deftest tp-testing-vars-bound-during-test-var
  (let [seen (atom :unset)
        c    (try
               (tp-fresh-ns 'tp.port.tvars)
               (let [v (tp-test-var! 'tp.port.tvars 'tv-observe
                                     (fn []
                                       (reset! seen
                                               (mapv (fn [tv] (:name (meta tv)))
                                                     *testing-vars*))))]
                 (tp-counted (fn [] (test-var v))))
               (finally (remove-ns 'tp.port.tvars)))]
    (is (= ['tv-observe] @seen))
    (is (= 1 (:test c)))
    (is (= 0 (:fail c)))))

(deftest tp-testing-vars-str-names-the-var
  (let [s (try
            (tp-fresh-ns 'tp.port.tvstr)
            (let [v (tp-test-var! 'tp.port.tvstr 'tvs-named (fn [] nil))]
              (binding [*testing-vars* (list v)]
                (testing-vars-str {:file "port.clj" :line 12})))
            (finally (remove-ns 'tp.port.tvstr)))]
    (is (clojure.string/includes? s "tvs-named"))
    (is (clojure.string/includes? s "port.clj:12"))))

(deftest tp-testing-contexts-str-joins-outer-to-inner
  (testing "outer"
    (testing "inner"
      (is (= "outer inner" (testing-contexts-str)))))
  (is (= "" (testing-contexts-str))))

;; --- Counters and report dispatch ---

(deftest tp-inc-report-counter-counts
  (let [c (binding [*report-counters* (atom *initial-report-counters*)]
            (inc-report-counter :pass)
            (inc-report-counter :pass)
            (inc-report-counter :custom)
            @*report-counters*)]
    (is (= 2 (:pass c)))
    (is (= 1 (:custom c)))
    (is (= 0 (:fail c))))
  ;; nil counters disable counting rather than throwing
  (is (nil? (binding [*report-counters* nil] (inc-report-counter :pass)))))

(deftest tp-do-report-dispatches-through-report
  (let [r (tp-reported (fn [] (do-report {:type :pass :message "m"
                                          :expected '(= 1 1)})))]
    (is (= 1 (count (:events r))))
    (is (= :pass (:type (first (:events r)))))
    (is (= "m" (:message (first (:events r))))))
  ;; position keys supplied by the caller survive dispatch
  (let [r (tp-reported (fn [] (do-report {:type :fail :file "f.clj" :line 3
                                          :expected 1 :actual 2})))]
    (is (= :fail (:type (first (:events r)))))
    (is (= "f.clj" (:file (first (:events r)))))
    (is (= 3 (:line (first (:events r)))))))

(deftest tp-report-default-methods-count
  (let [c (tp-counted
           (fn []
             (do-report {:type :pass})
             (do-report {:type :fail :expected 1 :actual 2})
             (do-report {:type :error :expected 1 :actual "boom"})))]
    (is (= 1 (:pass c)))
    (is (= 1 (:fail c)))
    (is (= 1 (:error c)))))

;; --- Assertion utilities ---

(deftest tp-get-possibly-unbound-var-behavior
  (is (= 41 (get-possibly-unbound-var (var tp-gpu-bound))))
  (is (nil? (get-possibly-unbound-var (var tp-gpu-unbound)))))

(deftest tp-function?-recognizes-functions-only
  (is (true? (function? (fn [x] x))))
  (is (function? inc))
  (is (function? 'inc))
  (is (not (function? 'when)))
  (is (not (function? 'tp-no-such-symbol)))
  (is (not (function? 42)))
  (is (not (function? nil))))

(deftest tp-file-position-is-callable
  (is (do (file-position 1) true)))

(deftest tp-assert-predicate-reports-pass-and-fail
  (let [r (tp-reported (fn [] (eval (assert-predicate "msg" '(< 1 2)))))]
    (is (true? (:value r)))
    (is (= :pass (:type (first (:events r)))))
    (is (= "msg" (:message (first (:events r)))))
    (is (= '(< 1 2) (:expected (first (:events r))))))
  (let [r (tp-reported (fn [] (eval (assert-predicate nil '(< 2 1)))))]
    (is (false? (:value r)))
    (is (= :fail (:type (first (:events r)))))
    ;; a false predicate result reports its evaluated form inside (not ...)
    (is (= '(not (< 2 1)) (:actual (first (:events r)))))))

(deftest tp-assert-any-reports-pass-and-fail
  (let [r (tp-reported (fn [] (eval (assert-any "m" '(let [x 7] x)))))]
    (is (= 7 (:value r)))
    (is (= :pass (:type (first (:events r))))))
  (let [r (tp-reported (fn [] (eval (assert-any nil 'nil))))]
    (is (nil? (:value r)))
    (is (= :fail (:type (first (:events r)))))))

(deftest tp-assert-expr-routes-by-operator
  (let [r (tp-reported (fn [] (eval (assert-expr nil '(= 1 1)))))]
    (is (true? (:value r)))
    (is (= :pass (:type (first (:events r))))))
  ;; a nil test form always fails
  (let [r (tp-reported (fn [] (eval (assert-expr nil nil))))]
    (is (= :fail (:type (first (:events r)))))))

(deftest tp-assert-expr-accepts-new-operators
  ;; assert-expr dispatches on the operator of the test form, so new
  ;; assertion kinds can be plugged in per operator.
  (defmethod assert-expr 'tp-custom-check [msg form]
    (list 'do-report {:type :pass :message msg
                      :expected (list 'quote form) :actual :tp-custom}))
  (try
    (let [r (tp-reported (fn [] (eval (assert-expr "ext" '(tp-custom-check 1)))))]
      (is (= :pass (:type (first (:events r)))))
      (is (= "ext" (:message (first (:events r)))))
      (is (= :tp-custom (:actual (first (:events r))))))
    (finally (remove-method assert-expr 'tp-custom-check))))

(deftest tp-try-expr-reports-pass-and-error
  (let [r (tp-reported (fn [] (try-expr "te" (= 1 1))))]
    (is (= :pass (:type (first (:events r))))))
  ;; a throw out of the test form reports :error instead of escaping
  (let [r (tp-reported (fn [] (try-expr "boom" (throw (ex-info "boom" {})))))]
    (is (= :error (:type (first (:events r)))))
    (is (= "boom" (:message (first (:events r)))))))

;; --- Definition forms ---

(deftest tp-deftest-defines-a-test-var
  (let [v (resolve 'tp-canon-sample)]
    (is (some? v))
    (is (fn? (:test (meta v))))
    (is (fn? (var-get v))))
  ;; calling the defined var runs it as a test
  (let [c (tp-counted (fn [] (tp-canon-sample)))]
    (is (= 1 (:test c)))
    (is (= 1 (:pass c)))
    (is (= 0 (:fail c)))))

(deftest tp-deftest-dash-defines-private-test-var
  (deftest- tp-private-sample (is (= 1 1)))
  (let [v (resolve 'tp-private-sample)]
    (is (some? v))
    (is (true? (:private (meta v))))
    (is (fn? (:test (meta v))))))

(deftest tp-with-test-attaches-test-metadata
  (with-test
    (defn tp-wt-add [a b] (+ a b))
    (is (= 4 ((var-get (resolve 'tp-wt-add)) 2 2))))
  (let [v (resolve 'tp-wt-add)]
    (is (some? v))
    (is (= 5 ((var-get v) 2 3)))
    (is (fn? (:test (meta v)))))
  (let [c (tp-counted (fn [] (test-var (resolve 'tp-wt-add))))]
    (is (= 1 (:test c)))
    (is (= 1 (:pass c)))))

(deftest tp-set-test-replaces-test-metadata
  (is (= :original ((:test (meta (var tp-st-target))))))
  (let [log (atom [])]
    (set-test tp-st-target (swap! log conj :replaced))
    ;; the value is untouched; only the :test metadata changes
    (is (= :unchanged tp-st-target))
    (let [t (:test (meta (var tp-st-target)))]
      (is (fn? t))
      (t)
      (is (= [:replaced] @log)))))

;; --- Fixture composition ---

(deftest tp-compose-fixtures-nests-first-outermost
  (let [log      (atom [])
        f1       (fn [g] (swap! log conj :f1-in) (g) (swap! log conj :f1-out))
        f2       (fn [g] (swap! log conj :f2-in) (g) (swap! log conj :f2-out))
        composed (compose-fixtures f1 f2)]
    (composed (fn [] (swap! log conj :body)))
    (is (= [:f1-in :f2-in :body :f2-out :f1-out] @log))))

(deftest tp-join-fixtures-composes-in-order
  (let [log    (atom [])
        mk     (fn [k] (fn [g]
                         (swap! log conj [k :in])
                         (g)
                         (swap! log conj [k :out])))
        joined (join-fixtures [(mk :a) (mk :b) (mk :c)])]
    (joined (fn [] (swap! log conj :body)))
    (is (= [[:a :in] [:b :in] [:c :in] :body [:c :out] [:b :out] [:a :out]]
           @log)))
  ;; an empty collection still yields a usable fixture
  (let [ran (atom false)]
    ((join-fixtures []) (fn [] (reset! ran true)))
    (is (true? @ran))))

;; --- Low-level runners ---

(deftest tp-test-var-counts-each-outcome
  (let [results
        (try
          (tp-fresh-ns 'tp.port.tvar)
          (let [vp (tp-test-var! 'tp.port.tvar 'tv-pass (fn [] (is (= 1 1))))
                vf (tp-test-var! 'tp.port.tvar 'tv-fail (fn [] (is (= 1 2))))
                ve (tp-test-var! 'tp.port.tvar 'tv-err
                                 (fn [] (throw (ex-info "kaboom" {}))))
                vn (intern 'tp.port.tvar 'tv-plain 42)]
            {:pass-c (tp-counted (fn [] (test-var vp)))
             :fail-c (tp-counted (fn [] (test-var vf)))
             :err-c  (tp-counted (fn [] (test-var ve)))
             :none-c (tp-counted (fn [] (test-var vn)))})
          (finally (remove-ns 'tp.port.tvar)))]
    (let [c (:pass-c results)]
      (is (= 1 (:test c)))
      (is (= 1 (:pass c)))
      (is (= 0 (:fail c))))
    (let [c (:fail-c results)]
      (is (= 1 (:test c)))
      (is (= 1 (:fail c)))
      (is (= 0 (:pass c))))
    (let [c (:err-c results)]
      (is (= 1 (:test c)))
      (is (= 1 (:error c))))
    ;; a var without :test metadata is skipped entirely
    (let [c (:none-c results)]
      (is (= 0 (:test c)))
      (is (= 0 (:pass c))))))

(deftest tp-test-vars-runs-tests-in-order
  (let [log (atom [])
        c   (try
              (tp-fresh-ns 'tp.port.tvs)
              (let [v1 (tp-test-var! 'tp.port.tvs 'tvs-one
                                     (fn [] (swap! log conj :one)))
                    v2 (tp-test-var! 'tp.port.tvs 'tvs-two
                                     (fn [] (swap! log conj :two)))
                    v3 (intern 'tp.port.tvs 'tvs-plain 42)]
                (tp-counted (fn [] (test-vars [v1 v2 v3]))))
              (finally (remove-ns 'tp.port.tvs)))]
    (is (= [:one :two] @log))
    (is (= 2 (:test c)))))

(deftest tp-test-all-vars-covers-the-namespace
  (let [log (atom [])
        c   (try
              (tp-fresh-ns 'tp.port.allv)
              (tp-test-var! 'tp.port.allv 'av-one (fn [] (swap! log conj :av-one)))
              (tp-test-var! 'tp.port.allv 'av-two (fn [] (swap! log conj :av-two)))
              (intern 'tp.port.allv 'av-plain 42)
              (tp-counted (fn [] (test-all-vars 'tp.port.allv)))
              (finally (remove-ns 'tp.port.allv)))]
    (is (= #{:av-one :av-two} (set @log)))
    (is (= 2 (:test c)))))

(deftest tp-test-ns-returns-final-counters
  (let [c (try
            (tp-fresh-ns 'tp.port.nsrun)
            (tp-test-var! 'tp.port.nsrun 'ns-pass (fn [] (is (= 1 1))))
            (tp-test-var! 'tp.port.nsrun 'ns-fail (fn [] (is (= 1 2))))
            (tp-quietly (fn [] (test-ns 'tp.port.nsrun)))
            (finally (remove-ns 'tp.port.nsrun)))]
    (is (= 2 (:test c)))
    (is (= 1 (:pass c)))
    (is (= 1 (:fail c)))
    (is (= 0 (:error c)))))

;; --- High-level runners ---

(deftest tp-run-test-var-returns-summary
  (let [s (try
            (tp-fresh-ns 'tp.port.rtv)
            (let [v (tp-test-var! 'tp.port.rtv 'rtv-sample
                                  (fn [] (is (= 1 1)) (is (= 2 2))))]
              (tp-quietly (fn [] (run-test-var v))))
            (finally (remove-ns 'tp.port.rtv)))]
    (is (= :summary (:type s)))
    (is (= 1 (:test s)))
    (is (= 2 (:pass s)))
    (is (= 0 (:fail s)))
    (is (= 0 (:error s)))))

(deftest tp-run-test-runs-one-test-by-name
  (let [s (tp-quietly (fn [] (run-test tp-canon-sample)))]
    (is (= :summary (:type s)))
    (is (= 1 (:test s)))
    (is (= 1 (:pass s)))
    (is (= 0 (:fail s)))
    (is (true? (successful? s)))))

(deftest tp-run-all-tests-filters-namespaces-by-regex
  (let [s (try
            (tp-fresh-ns 'tp.port.runall)
            (tp-test-var! 'tp.port.runall 'ra-pass (fn [] (is (= 1 1))))
            (tp-quietly (fn [] (run-all-tests #"tp\.port\.runall")))
            (finally (remove-ns 'tp.port.runall)))]
    (is (= :summary (:type s)))
    (is (= 1 (:test s)))
    (is (= 1 (:pass s)))
    (is (= 0 (:fail s)))))

(deftest tp-successful?-checks-fail-and-error
  (is (true? (successful? {:test 5 :pass 9 :fail 0 :error 0})))
  (is (false? (successful? {:fail 1 :error 0})))
  (is (false? (successful? {:fail 0 :error 2})))
  ;; missing counters default to zero
  (is (true? (successful? {}))))

(run-tests-and-exit)
