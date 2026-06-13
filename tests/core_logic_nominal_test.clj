(require "tests/test")
(require '[clojure.core.logic :as l :refer [run*]])
(require '[clojure.core.logic.nominal :as nom])

;; clojure.core.logic.nominal -- nominal logic.  The surface (nom/fresh,
;; nom?, tie, hash) matches upstream; noms and ties participate in
;; core.logic unification through the ITerm hook, so ties unify up to
;; alpha-equivalence and the freshness goal constrains where a name may
;; occur.

(deftest alpha-equivalence
  ;; [a]a and [b]b are the same abstraction up to renaming.
  (is (= '(:ok)
         (run* [q] (nom/fresh [a b]
                     (l/== (nom/tie a a) (nom/tie b b))
                     (l/== q :ok)))))
  ;; [a]a and [a]b differ (a distinct b under the same binder).
  (is (= '()
         (run* [q] (nom/fresh [a b]
                     (l/== (nom/tie a a) (nom/tie a b))
                     (l/== q :ok))))))

(deftest nested-alpha-equivalence
  ;; [a][b](a b) ~ [c][d](c d)
  (is (= '(:ok)
         (run* [q] (nom/fresh [a b c d]
                     (l/== (nom/tie a (nom/tie b [a b]))
                         (nom/tie c (nom/tie d [c d])))
                     (l/== q :ok))))))

(deftest freshness
  ;; Distinct noms are fresh for one another.
  (is (= '(:ok) (run* [q] (nom/fresh [a b] (nom/hash a b) (l/== q :ok)))))
  ;; A nom is never fresh for itself.
  (is (= '() (run* [q] (nom/fresh [a] (nom/hash a a) (l/== q :ok)))))
  ;; A nom is fresh for an abstraction that rebinds it.
  (is (= '(:ok) (run* [q] (nom/fresh [a] (nom/hash a (nom/tie a a)) (l/== q :ok)))))
  ;; A nom is not fresh for a term in which it occurs free.
  (is (= '() (run* [q] (nom/fresh [a b] (nom/hash a (nom/tie b a)) (l/== q :ok))))))

(deftest nom-predicate
  (is (true? (nom/nom? (nom/nom))))
  (is (false? (nom/nom? 5)))
  (is (true? (nom/tie? (nom/tie (nom/nom) 1)))))

(deftest reify-tie
  ;; A reified tie carries reification names, with the binder and its
  ;; bound occurrence sharing the same name.
  (let [r (first (run* [q] (nom/fresh [a] (l/== q (nom/tie a a)))))]
    (is (nom/tie? r))
    (is (= (:binding r) (:term r)))))

(run-tests-and-exit)
