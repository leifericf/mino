(require "tests/test")

;; Macros: defmacro, quasiquote, gensym, macroexpand, quote.

(deftest quote-shorthand
  (is (= 'foo 'foo))
  (is (= '(a (b c) d) '(a (b c) d))))

(deftest comments
  ;; This test just confirms that comments don't break evaluation.
  (is (= 2 (+ 1 1))))

(deftest defmacro-basic
  (defmacro twice__mt [x] `(do ~x ~x))
  (def mt-n (atom 0))
  (twice__mt (swap! mt-n inc))
  (is (= 2 @mt-n)))

(deftest quasiquote-splice
  (defmacro my-list__mt [& xs] `(list ~@xs))
  (is (= '(1 2 3) (my-list__mt 1 2 3))))

(deftest macroexpand-1-fn
  (defmacro unless__mt [c t f] (list 'if c f t))
  (is (= '(if x 2 1) (macroexpand-1 '(unless__mt x 1 2)))))

(deftest gensym-fresh
  (is (not= (gensym) (gensym))))

(deftest eval-fn
  (is (= 3 (eval '(+ 1 2))))
  (is (= 12 (eval (read-string "(* 3 4)"))))
  (is (= 30 (eval (list '+ 10 20)))))

(deftest multi-line-form
  (is (= '(1 2 3) (cons 1
                     (cons 2
                       (cons 3 nil))))))
