(require "tests/test")
(require '[clojure.walk :as walk])

;; postwalk-demo / prewalk-demo: walk the form, printing
;; "Walked: <form>" (print + prn) for every visited node, and return
;; the form unchanged. Output order is the traversal order, so these
;; tests pin both the per-node format and the visit order.

(deftest walk-demo-postwalk-order-and-return
  (let [walk-demo-ret (atom nil)
        out (with-out-str
              (reset! walk-demo-ret (walk/postwalk-demo [1 [2]])))]
    (testing "postwalk-demo visits leaves before their composites"
      (is (= (str "Walked: 1\n"
                  "Walked: 2\n"
                  "Walked: [2]\n"
                  "Walked: [1 [2]]\n")
             out)))
    (testing "postwalk-demo returns the form"
      (is (= [1 [2]] @walk-demo-ret)))))

(deftest walk-demo-prewalk-order-and-return
  (let [walk-demo-ret (atom nil)
        out (with-out-str
              (reset! walk-demo-ret (walk/prewalk-demo [1 [2]])))]
    (testing "prewalk-demo visits composites before their leaves"
      (is (= (str "Walked: [1 [2]]\n"
                  "Walked: 1\n"
                  "Walked: [2]\n"
                  "Walked: 2\n")
             out)))
    (testing "prewalk-demo returns the form"
      (is (= [1 [2]] @walk-demo-ret)))))

(deftest walk-demo-scalar-and-empty
  (testing "a scalar form is its own single visit"
    (is (= "Walked: 7\n" (with-out-str (walk/postwalk-demo 7))))
    (is (= "Walked: 7\n" (with-out-str (walk/prewalk-demo 7)))))
  (testing "an empty collection is one visit, no children"
    (is (= "Walked: []\n" (with-out-str (walk/postwalk-demo []))))
    (is (= "Walked: []\n" (with-out-str (walk/prewalk-demo []))))))

(run-tests-and-exit)
