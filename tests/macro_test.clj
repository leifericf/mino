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

(deftest auto-gensym-distinct-per-syntax-quote
  ;; Each syntax-quote read resolves its own x#; the same name in two
  ;; separate syntax-quote forms must not collide.
  (is (not= `x# `x#))
  ;; Within one syntax-quote, every x# is the same symbol.
  (is (let [[a b] `[x# x#]] (= a b)))
  ;; The resolved symbol carries the canonical __auto__ shape.
  (is (clojure.string/includes? (str `x#) "__auto__"))
  (is (clojure.string/starts-with? (str `x#) "x__")))

(deftest auto-gensym-prevents-cross-macro-capture
  (defmacro hygiene-inner [y] `(let [v# 2] (+ ~y v#)))
  (defmacro hygiene-outer [] `(let [v# 1] (hygiene-inner v#)))
  (is (= 3 (hygiene-outer))))

(deftest auto-gensym-stable-within-macro
  ;; The defmacro body is read once, so the baked gensym is the same
  ;; on every expansion.
  (defmacro hygiene-stable [] `(quote q#))
  (is (= (hygiene-stable) (hygiene-stable))))

(deftest auto-gensym-untouched-in-unquote
  ;; An unquoted position escapes gensym resolution entirely.
  (def gensym-passthrough 'kept#)
  (is (= 'kept# (let [r `(~gensym-passthrough)] (first r)))))
