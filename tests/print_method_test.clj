(require "tests/test")

;; Extensible printer via print-method multimethod.

;; --- the multimethod itself exists and has a :default method ---

(deftest print-method-exists
  ;; print-method is a multimethod, so (type) picks up the :multimethod
  ;; metadata tag attached by create-multimethod.
  (is (= :multimethod (type print-method)))
  (is (some? (get-method print-method :default))))

;; --- built-in round-trip via pr-str / read-string stays correct ---

(deftest print-method-builtin-roundtrip
  (doseq [x [1
             1.5
             "hello world"
             :key
             'sym
             true
             false
             nil
             [1 2 3]
             {:a 1 :b 2}
             #{1 2 3}
             '(1 2 3)]]
    (is (= x (read-string (pr-str x))))))

;; --- user extension via defmethod print-method ---

(deftest print-method-user-extension
  (defmethod print-method :my-print-test-type
    [v]
    (print "<mpt:")
    (pr-builtin (:payload v))
    (print ">"))
  ;; (type x) checks :type metadata first, so this dispatches to :my-print-test-type.
  (let [obj (with-meta {:payload 42} {:type :my-print-test-type})]
    (is (= :my-print-test-type (type obj)))
    ;; Route through prn; the user method writes "<mpt:42>" followed by \n.
    ;; Here we just assert the method is now registered and dispatchable.
    (is (some? (get-method print-method :my-print-test-type)))))

;; --- pr-str routes through the print-method hook ---

(deftest print-method-routes-pr-str
  (defmethod print-method :pr-str-probe
    [v]
    (print "<probe:")
    (pr-builtin (:payload v))
    (print ">"))
  (let [obj (with-meta {:payload 7} {:type :pr-str-probe})]
    ;; pr routes through the hook: the pinned baseline.
    (is (= "<probe:7>" (with-out-str (pr obj))))
    ;; pr-str must match the routed pr output, not the builtin form.
    (is (= "<probe:7>" (pr-str obj)))
    (is (= (with-out-str (pr obj)) (pr-str obj)))
    ;; Multiple args keep the space separator between routed forms.
    (is (= "<probe:7> <probe:7>" (pr-str obj obj)))
    ;; prn-str (a with-out-str wrapper over prn) is routed the same.
    (is (= "<probe:7>\n" (prn-str obj)))
    ;; str stays unrouted: it is not readable printing.
    (is (= "{:payload 7}" (str obj)))
    ;; Binding *print-readably* to false sends pr-str down the
    ;; print/println path, past the hook, exactly like pr.
    (is (= "{:payload 7}"
           (binding [*print-readably* false] (pr-str obj))))
    (is (= (binding [*print-readably* false] (with-out-str (pr obj)))
           (binding [*print-readably* false] (pr-str obj))))))

;; --- removing a user method falls back to :default ---

(deftest print-method-remove-falls-back
  (defmethod print-method :phase-b-removable [v] (print "X"))
  (is (some? (get-method print-method :phase-b-removable)))
  (remove-method print-method :phase-b-removable)
  (is (nil? (get-method print-method :phase-b-removable))))

;; --- set-print-method! argument types are checked ---

(deftest print-method-hook-type-check
  (is (thrown? (set-print-method! 42)))
  (is (thrown? (set-print-method! "not-a-fn"))))

;; --- pr-builtin is arity-checked ---

(deftest pr-builtin-arity
  (is (thrown? (pr-builtin)))
  (is (thrown? (pr-builtin 1 2))))

;; (run-tests) -- called by tests/run.clj
