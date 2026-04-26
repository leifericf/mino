(ns clojure.test
  (:require [clojure.string :refer [join]]))

;; --- State ---

(def *tests* (atom []))
(def *test-state* (atom {:pass 0 :fail 0 :error 0 :failures []}))
(def *testing-context* (atom ()))
(def *current-test* (atom nil))

;; --- Internal helpers ---

(defn assert-pass! []
  (swap! *test-state* update :pass inc))

(defn assert-fail! [form msg detail]
  (let [ctx   (reverse @*testing-context*)
        tname @*current-test*
        entry {:test tname
               :form form
               :message msg
               :detail detail
               :context ctx}]
    (swap! *test-state* update :fail inc)
    (swap! *test-state* update :failures conj entry)))

;; --- Public API ---

(defn thrown?-form? [sym]
  (and (symbol? sym) (= "thrown?" (name sym))))

(defmacro is [& args]
  (let [expr (first args)
        msg  (second args)]
    (if (and (cons? expr) (thrown?-form? (first expr)))
      ;; (is (thrown? body...)) or (is (p/thrown? body...))
      `(try
         (do ~@(rest expr)
             (assert-fail! (pr-str '~expr) ~msg "expected exception but none thrown"))
         (catch __e (assert-pass!)))
      (if (and (cons? expr) (= (first expr) '=))
        ;; (is (= expected actual)) — equality with diff
        (let [gs-exp (gensym) gs-act (gensym)]
          `(let [~gs-exp ~(first (rest expr))
                 ~gs-act ~(first (rest (rest expr)))]
             (if (= ~gs-exp ~gs-act)
               (assert-pass!)
               (assert-fail! (pr-str '~expr) ~msg
                 (str "expected: " (pr-str ~gs-exp) "\n    actual: " (pr-str ~gs-act))))))
        ;; (is expr) — truthy assertion
        (let [gs (gensym)]
          `(let [~gs ~expr]
             (if ~gs
               (assert-pass!)
               (assert-fail! (pr-str '~expr) ~msg
                 (str "expected truthy, got: " (pr-str ~gs))))))))))

(defmacro deftest [tname & body]
  `(swap! *tests* conj
     {:name (name '~tname)
      :fn   (fn [] ~@body)}))

(defmacro testing [desc & body]
  `(do
     (reset! *testing-context* (cons ~desc @*testing-context*))
     (let [__r (do ~@body)]
       (reset! *testing-context* (rest @*testing-context*))
       __r)))

(defmacro are [bindings expr & args]
  (let [n     (count bindings)
        rows  (partition n args)
        tests (apply list
                (map (fn [row]
                       (let [smap (zipmap bindings row)]
                         `(is ~(postwalk-replace smap expr))))
                     rows))]
    `(do ~@tests)))

;; --- Runner ---

(defn run-tests []
  (let [tests @*tests*
        n     (count tests)]
    (reset! *test-state* {:pass 0 :fail 0 :error 0 :failures []})
    (loop [i 0]
      (when (< i n)
        (let [t     (nth tests i)
              tname (get t :name)
              tfn   (get t :fn)]
          (reset! *current-test* tname)
          (reset! *testing-context* ())
          (try
            (tfn)
            (catch e
              (do
                (swap! *test-state* update :error inc)
                (swap! *test-state* update :failures conj
                  {:test tname :error (str e)})))))
        (recur (+ i 1))))
    ;; Report
    (let [state    @*test-state*
          passes   (get state :pass)
          fails    (get state :fail)
          errors   (get state :error)
          total    (+ passes fails errors)
          failures (get state :failures)]
      ;; Print failures
      (when (seq failures)
        (println "")
        (println "Failures:")
        (loop [fs failures]
          (when (seq fs)
            (let [f (first fs)]
              (when (get f :test)
                (println (str "  in " (get f :test))))
              (when (seq (get f :context))
                (println (str "    " (join " > " (get f :context)))))
              (when (get f :form)
                (println (str "    " (get f :form))))
              (when (get f :message)
                (println (str "    " (get f :message))))
              (when (get f :detail)
                (println (str "    " (get f :detail))))
              (when (get f :error)
                (println (str "    ERROR: " (get f :error))))
              (println ""))
            (recur (rest fs)))))
      ;; Summary
      (println (str n " tests, " total " assertions: "
                    passes " passed, " fails " failed, " errors " errors"))
      (exit (if (and (= fails 0) (= errors 0)) 0 1)))))
