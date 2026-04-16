(require "tests/test")

;; Reader macros: #(), #_, callable keywords, ex-info/ex-data.

;; --- #_ discard ---

(deftest discard-in-vector
  (is (= [1 3] [1 #_2 3])))

(deftest discard-in-list
  (is (= 5 (+ 1 #_(* 2 3) 4))))

(deftest discard-in-map
  (is (= {:a 1 :c 3} {:a 1 #_:b #_2 :c 3})))

(deftest discard-stacked
  (is (= 3 #_#_1 2 3)))

;; --- #() anonymous fn ---

(deftest shorthand-fn-bare-percent
  (is (= [2 3 4] (mapv #(inc %) [1 2 3]))))

(deftest shorthand-fn-numbered-args
  (is (= 7 (#(+ %1 %2) 3 4))))

(deftest shorthand-fn-with-map
  (is (= [1 4 9] (mapv #(* % %) [1 2 3]))))

(deftest shorthand-fn-with-str
  (is (= "hello world" (#(str "hello " %) "world"))))

(deftest shorthand-fn-rest-args
  (is (= 10 (#(apply + %&) 1 2 3 4))))

(deftest shorthand-fn-mixed-positional-and-rest
  (is (= [1 [2 3]] (#(vector %1 (vec %&)) 1 2 3))))

;; --- Callable keywords ---

(deftest keyword-as-fn-basic
  (is (= 1 (:a {:a 1 :b 2}))))

(deftest keyword-as-fn-missing
  (is (= nil (:z {:a 1}))))

(deftest keyword-as-fn-default
  (is (= :nope (:z {:a 1} :nope))))

(deftest keyword-as-fn-with-map
  (is (= ["a" "b" "c"] (mapv :name [{:name "a"} {:name "b"} {:name "c"}]))))

(deftest keyword-as-fn-non-map
  (is (= nil (:a 42))))

(deftest keyword-as-fn-nil-coll
  (is (= nil (:a nil))))

(deftest keyword-as-fn-nil-coll-default
  (is (= :default (:a nil :default))))

;; --- ex-info / ex-data / ex-message ---

(deftest ex-info-creates-map
  (let [e (ex-info "boom" {:code 500})]
    (is (= {:message "boom" :data {:code 500}} e))))

(deftest ex-data-extracts-data
  (is (= {:id 42} (ex-data (ex-info "not found" {:id 42})))))

(deftest ex-message-extracts-message
  (is (= "oops" (ex-message (ex-info "oops" {})))))

(deftest ex-info-with-throw-catch
  (is (= {:code 404}
         (try
           (throw (ex-info "not found" {:code 404}))
           (catch e (ex-data e))))))

(deftest ex-data-non-map
  (is (= nil (ex-data "not a map"))))

(deftest ex-message-non-map
  (is (= nil (ex-message 42))))
