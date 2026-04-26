(require "tests/test")

;; Tests for core.clj conformance additions.

;; --- some-> ---

(deftest some->-basic
  (is (= 2 (some-> {:a 1} :a inc)))
  (is (nil? (some-> {:a 1} :b inc)))
  (is (nil? (some-> nil :a)))
  (is (= 1 (some-> 0 inc)))
  (is (= 3 (some-> 1 inc inc))))

(deftest some->-with-forms
  (is (= "1" (some-> 1 str)))
  (is (nil? (some-> nil str)))
  (is (= 2 (some-> {:a {:b 2}} :a :b)))
  (is (nil? (some-> {:a {:b 2}} :c :b))))

(deftest some->-no-forms
  (is (= 42 (some-> 42)))
  (is (nil? (some-> nil))))

;; --- some->> ---

(deftest some->>-basic
  (is (= '(2 3 4) (some->> [1 2 3] (map inc))))
  (is (nil? (some->> nil (map inc))))
  (is (= 6 (some->> [1 2 3] (reduce +)))))

(deftest some->>-chained
  (is (= '(2 4) (some->> [1 2 3] (map inc) (filter even?))))
  (is (nil? (some->> nil (map inc) (filter even?)))))

(deftest some->>-no-forms
  (is (= 42 (some->> 42)))
  (is (nil? (some->> nil))))

;; --- update-vals ---

(deftest update-vals-basic
  (is (= {:a 2 :b 4 :c 6} (update-vals {:a 1 :b 2 :c 3} (fn [v] (* v 2)))))
  (is (= {:a 2 :b 3 :c 4} (update-vals {:a 1 :b 2 :c 3} inc))))

(deftest update-vals-empty
  (is (= {} (update-vals {} inc))))

(deftest update-vals-identity
  (is (= {:a 1 :b 2} (update-vals {:a 1 :b 2} identity))))

;; --- update-keys ---

(deftest update-keys-basic
  (is (= {"a" 1 "b" 2} (update-keys {:a 1 :b 2} name)))
  (is (= {2 :a 4 :b} (update-keys {1 :a 2 :b} (fn [k] (* k 2))))))

(deftest update-keys-empty
  (is (= {} (update-keys {} inc))))

(deftest update-keys-identity
  (is (= {:a 1 :b 2} (update-keys {:a 1 :b 2} identity))))

;; --- min-key ---

(deftest min-key-basic
  (is (= "a" (min-key count "ab" "a" "abc")))
  (is (= {:x 1} (min-key :x {:x 3} {:x 1} {:x 2}))))

(deftest min-key-single
  (is (= 42 (min-key identity 42))))

(deftest min-key-tie-returns-last
  (is (= "de" (min-key count "ab" "cd" "de"))))

;; --- max-key ---

(deftest max-key-basic
  (is (= "abc" (max-key count "ab" "a" "abc")))
  (is (= {:x 3} (max-key :x {:x 3} {:x 1} {:x 2}))))

(deftest max-key-single
  (is (= 42 (max-key identity 42))))

(deftest max-key-tie-returns-last
  (is (= "de" (max-key count "ab" "cd" "de"))))

;; --- random-sample ---

(deftest random-sample-zero
  (is (= '() (random-sample 0 [1 2 3 4 5]))))

(deftest random-sample-one
  (is (= '(1 2 3 4 5) (random-sample 1 [1 2 3 4 5]))))

(deftest random-sample-returns-subset
  (let [result (random-sample 0.5 (range 1000))]
    (is (< (count result) 1000))
    (is (> (count result) 0))))

(deftest random-sample-transducer
  (is (= '() (into [] (random-sample 0) [1 2 3 4 5])))
  (is (= [1 2 3 4 5] (into [] (random-sample 1) [1 2 3 4 5]))))

;; --- halt-when ---

(deftest halt-when-basic
  (is (= 4 (transduce (halt-when even?) conj [1 3 5 4 7]))))

(deftest halt-when-with-retf
  (is (= {:stopped 4 :so-far [1 3 5]}
         (transduce (halt-when even?
                      (fn [r input] {:stopped input :so-far r}))
                    conj [1 3 5 4 7]))))

(deftest halt-when-never-triggers
  (is (= [1 3 5] (transduce (halt-when even?) conj [1 3 5]))))

(deftest halt-when-first-element
  (is (= 2 (transduce (halt-when even?) conj [2 3 4 5]))))

;; --- bounded-count ---

(deftest bounded-count-vector
  (is (= 5 (bounded-count 10 [1 2 3 4 5]))))

(deftest bounded-count-lazy
  (is (= 10 (bounded-count 10 (range))))
  (is (= 3 (bounded-count 10 (take 3 (range))))))

(deftest bounded-count-empty
  (is (= 0 (bounded-count 10 [])))
  (is (= 0 (bounded-count 10 nil))))

;; --- while ---

(deftest while-basic
  (let [a (atom 0)]
    (while (< @a 5)
      (swap! a inc))
    (is (= 5 @a))))

(deftest while-never-runs
  (let [a (atom 10)]
    (while (< @a 5)
      (swap! a inc))
    (is (= 10 @a))))

;; --- distinct? ---

(deftest distinct?-basic
  (is (distinct? 1 2 3))
  (is (not (distinct? 1 2 1)))
  (is (distinct? :a :b :c))
  (is (not (distinct? :a :b :a))))

(deftest distinct?-single
  (is (distinct? 1)))

(deftest distinct?-empty
  (is (distinct?)))

;; --- type predicates ---

(deftest sorted?-test
  (is (sorted? (sorted-map :a 1)))
  (is (sorted? (sorted-set 1 2)))
  (is (not (sorted? {:a 1})))
  (is (not (sorted? [1 2]))))

(deftest associative?-test
  (is (associative? {:a 1}))
  (is (associative? [1 2]))
  (is (associative? (sorted-map :a 1)))
  (is (not (associative? '(1 2))))
  (is (not (associative? #{1 2}))))

(deftest reversible?-test
  (is (reversible? [1 2 3]))
  (is (not (reversible? '(1 2))))
  (is (not (reversible? {:a 1}))))

(deftest any?-test
  (is (any? nil))
  (is (any? false))
  (is (any? 42))
  (is (any? "hello")))

;; --- ensure-reduced ---

(deftest ensure-reduced-basic
  (is (reduced? (ensure-reduced 42)))
  (is (= 42 @(ensure-reduced 42))))

(deftest ensure-reduced-already-reduced
  (let [r (reduced 42)]
    (is (identical? r (ensure-reduced r)))))

;; (run-tests) -- called by tests/run.clj
