(require "tests/test")

;; --- host/new ---

(deftest host-new-counter
  (let [c (host/new :Counter)]
    (is (= :handle (type c)))))

;; --- host/call ---

(deftest host-call-inc
  (let [c (host/new :Counter)]
    (host/call c :inc)
    (is (= 1 (host/call c :get)))))

(deftest host-call-add
  (let [c (host/new :Counter)]
    (host/call c :add 5)
    (is (= 5 (host/call c :get)))))

(deftest host-call-chain
  (let [c (host/new :Counter)]
    (host/call c :inc)
    (host/call c :inc)
    (host/call c :inc)
    (host/call c :add 10)
    (is (= 13 (host/call c :get)))))

;; --- host/get ---

(deftest host-get-value
  (let [c (host/new :Counter)]
    (host/call c :add 42)
    (is (= 42 (host/get c :value)))))

;; --- host/static-call ---

(deftest host-static-add
  (is (= 7 (host/static-call :Math :add 3 4))))

(deftest host-static-pi
  (is (< 3.14 (host/static-call :Math :pi)))
  (is (> 3.15 (host/static-call :Math :pi))))

;; --- error handling ---

(deftest host-unknown-type
  (is (thrown? (host/new :Bogus))))

(deftest host-unknown-method
  (let [c (host/new :Counter)]
    (is (thrown? (host/call c :bogus)))))

(deftest host-unknown-getter
  (let [c (host/new :Counter)]
    (is (thrown? (host/get c :bogus)))))

(deftest host-unknown-static
  (is (thrown? (host/static-call :Math :bogus))))

(deftest host-bad-target
  (is (thrown? (host/call 42 :inc))))

(deftest host-arity-mismatch
  (is (thrown? (host/new :Counter 1 2 3))))

(deftest host-error-messages
  (is (= "unknown type: :Nope"
         (try (host/new :Nope) (catch e e))))
  (let [c (host/new :Counter)]
    (is (= "member not found: :Counter/:nope"
           (try (host/call c :nope) (catch e e))))
    (is (= "member not found: :Counter/:nope"
           (try (host/get c :nope) (catch e e)))))
  (is (= "member not found: :Math/:nope"
         (try (host/static-call :Math :nope) (catch e e))))
  (is (= "target is not a host handle"
         (try (host/call "string" :method) (catch e e)))))

;; --- syntax: .method ---

(deftest syntax-dot-method
  (let [c (new Counter)]
    (.inc c)
    (is (= 1 (.get c)))))

(deftest syntax-dot-method-with-args
  (let [c (new Counter)]
    (.add c 10)
    (is (= 10 (.get c)))))

;; --- syntax: .-field ---

(deftest syntax-dash-field
  (let [c (new Counter)]
    (.add c 99)
    (is (= 99 (.-value c)))))

;; --- syntax: new ---

(deftest syntax-new
  (let [c (new Counter)]
    (is (= :handle (type c)))))

;; --- syntax: Type/static ---

(deftest syntax-static-call
  (is (= 7 (Math/add 3 4))))

(deftest syntax-static-no-args
  (is (< 3.14 (Math/pi)))
  (is (> 3.15 (Math/pi))))

;; --- syntax parity with explicit forms ---

(deftest syntax-parity
  (let [c1 (host/new :Counter)
        c2 (new Counter)]
    (host/call c1 :add 5)
    (.add c2 5)
    (is (= (host/call c1 :get) (.get c2)))
    (is (= (host/get c1 :value) (.-value c2)))
    (is (= (host/static-call :Math :add 1 2) (Math/add 1 2)))))

;; --- syntax error handling ---

(deftest syntax-dot-unknown-method
  (let [c (new Counter)]
    (is (thrown? (.bogus c)))))

(deftest syntax-static-unknown
  (is (thrown? (Math/bogus 1))))

(run-tests-and-exit)
