(require "tests/test")

;; pcalls / pvalues / alt! shipped in v0.404.0.

(deftest pcalls-basic
  (is (= '(1 2 3)
         (pcalls (fn [] 1) (fn [] 2) (fn [] 3)))))

(deftest pcalls-empty
  ;; (pcalls) with no fns returns the empty seq.
  (is (= () (pcalls))))

(deftest pcalls-side-effects-may-interleave
  ;; pcalls is parallel — the per-fn results land in input order but
  ;; the bodies may have run concurrently. Lock in that the result seq
  ;; preserves order.
  (let [result (pcalls (fn [] :a) (fn [] :b) (fn [] :c) (fn [] :d))]
    (is (= [:a :b :c :d] (vec result)))))

(deftest pvalues-basic
  (is (= [1 5 :x] (vec (pvalues 1 (+ 2 3) :x)))))

(deftest pvalues-deferred-eval
  ;; pvalues returns a lazy seq; realizing it runs each form once.
  (let [n (atom 0)]
    (doall (pvalues (swap! n inc) (swap! n inc)))
    (is (= 2 @n))))

(require '[clojure.core.async :as a])

(deftest alt!-single-take
  (let [ch (a/chan)]
    (a/put! ch :hi)
    (is (= "got :hi" (a/alt! ch ([v] (str "got " v)))))))

(deftest alt!-multi-take-second-ready
  (let [c1 (a/chan)
        c2 (a/chan)]
    (a/put! c2 :two)
    (is (= "from 2: :two"
           (a/alt! c1 ([v] (str "from 1: " v))
                   c2 ([v] (str "from 2: " v)))))))

(deftest alt!-put-form
  (let [ch (a/chan 1)]
    (is (= :put-ok (a/alt! [ch :payload] ([_] :put-ok))))))

(deftest alt!-default-fires
  (let [ch (a/chan)]
    (is (= :nothing (a/alt! ch ([v] (str "got " v))
                            :default :nothing)))))

(deftest alt!-binding-two-args
  (let [ch (a/chan)]
    (a/put! ch :hi)
    (let [[v c?] (a/alt! ch ([v c] [v (some? c)]))]
      (is (= :hi v))
      (is (true? c?)))))

(deftest alt!-plain-expr-clause
  ;; A clause without the binding-vector shape is just an expression.
  (let [ch (a/chan)]
    (a/put! ch :hi)
    (is (= :picked (a/alt! ch :picked)))))

(deftest locking-monitor
  (testing "body value is returned and nesting on the same monitor reenters"
    (let [m (atom :monitor)]
      (is (= :inner (locking m (locking m :inner))))))
  (testing "a throw releases the monitor for the next holder"
    (let [m (atom :monitor)]
      (is (= :caught (try (locking m (throw (ex-info "x" {})))
                       (catch e :caught))))
      (is (= :again (locking m :again)))))
  (testing "contending futures serialize their critical sections"
    (let [m       (atom :monitor)
          active  (atom 0)
          overlap (atom false)
          worker  (fn []
                    (locking m
                      (swap! active inc)
                      (when (> @active 1) (reset! overlap true))
                      (Thread/sleep 20)
                      (swap! active dec))
                    true)
          fs (mapv (fn [_] (future (worker))) (range 3))]
      (doseq [f fs] (deref f 4000 :timeout))
      (is (false? @overlap)))))
