;; clojure.core.logic.nominal -- nominal logic for core.logic.  Nominal
;; logic adds names (noms) and name-binding abstractions (ties) so that
;; terms with binders -- lambda terms, quantified formulas -- can be
;; unified up to alpha-equivalence.  The surface matches upstream: the
;; nom/fresh binder, nom? , tie, and the freshness goal hash.
;;
;; A Nom is an atom of identity; a Tie [a]t is an abstraction binding the
;; nom a in the term t.  Two ties unify up to a permutation of their bound
;; names (a name swap), which is what makes [a]a and [b]b unify.  The
;; freshness goal (hash a t) holds when the nom a does not occur free in
;; t.  Nom and Tie participate in core.logic's unifier through the ITerm
;; extension hook.

(ns clojure.core.logic.nominal
  (:refer-clojure :exclude [hash])
  (:require [clojure.core.logic :as l]))

;; --------------------------------------------------------------------
;; Noms and ties
;; --------------------------------------------------------------------

(defrecord Nom [id name])
(defrecord Tie [binding term])

(def ^:private nom-counter (atom 0))

(defn nom
  "Create a fresh nom (a name) with an advisory display name."
  ([] (nom 'a))
  ([name] (->Nom (swap! nom-counter inc) name)))

(defn nom?
  "True when x is a nom."
  [x]
  (instance? Nom x))

(defn tie
  "Abstraction [a]t: bind the nom a in the term t."
  [a t]
  (->Tie a t))

(defn tie?
  "True when x is a tie (a name abstraction)."
  [x]
  (instance? Tie x))

(defmacro fresh
  "Introduce fresh noms bound for the conjunction of goals."
  [noms & goals]
  (let [a (gensym "a")]
    `(fn [~a]
       (let [~@(mapcat (fn [n] [n `(nom '~n)]) noms)]
         ((l/all ~@goals) ~a)))))

;; --------------------------------------------------------------------
;; Name swapping (the permutation (n1 n2) applied through a term)
;; --------------------------------------------------------------------

(defn- swap-term
  "Apply the name swap (n1 n2) through a walked term, exchanging the two
  noms everywhere, including inside tie binders (the permutation is
  uniform)."
  [n1 n2 t]
  (cond
    (nom? t) (cond (= t n1) n2 (= t n2) n1 :else t)
    (tie? t) (->Tie (swap-term n1 n2 (:binding t)) (swap-term n1 n2 (:term t)))
    (vector? t) (mapv #(swap-term n1 n2 %) t)
    (map? t) (into {} (map (fn [[k v]] [(swap-term n1 n2 k) (swap-term n1 n2 v)]) t))
    (sequential? t) (map #(swap-term n1 n2 %) t)
    :else t))

;; --------------------------------------------------------------------
;; Freshness (a # t): a does not occur free in t
;; --------------------------------------------------------------------

(defn- freshness-status
  "Whether nom a is fresh for the walked term t: :fresh, :not-fresh (a
  occurs free), or :unknown (t still contains an unbound variable)."
  [a t]
  (cond
    (nom? t) (if (= t a) :not-fresh :fresh)
    (l/lvar? t) :unknown
    (tie? t) (if (= (:binding t) a)
               :fresh                       ;; a is rebound: not free below
               (freshness-status a (:term t)))
    (vector? t) (reduce (fn [acc x]
                          (let [s (freshness-status a x)]
                            (cond (= s :not-fresh) (reduced :not-fresh)
                                  (= s :unknown) :unknown
                                  :else acc)))
                        :fresh t)
    (sequential? t) (reduce (fn [acc x]
                              (let [s (freshness-status a x)]
                                (cond (= s :not-fresh) (reduced :not-fresh)
                                      (= s :unknown) :unknown
                                      :else acc)))
                            :fresh t)
    (map? t) (reduce (fn [acc [k v]]
                       (let [sk (freshness-status a k)
                             sv (freshness-status a v)]
                         (cond (or (= sk :not-fresh) (= sv :not-fresh)) (reduced :not-fresh)
                               (or (= sk :unknown) (= sv :unknown)) :unknown
                               :else acc)))
                     :fresh t)
    :else :fresh))

(defn- fresh-constraint
  "A constraint that holds while nom a is fresh for term t; fails once a
  is shown to occur free, and stays pending while t is not fully ground."
  [a t]
  (fn [pkg]
    (let [t' (l/walk* t (:s pkg))]
      (case (freshness-status a t')
        :not-fresh nil
        pkg))))

(defn hash
  "Freshness goal: succeed while nom a does not occur free in term t."
  [a t]
  (fn [pkg]
    (let [pkg' (l/run-constraints (update pkg :fc conj (fresh-constraint a t)))]
      (if pkg' (l/unit pkg') l/mzero))))

;; --------------------------------------------------------------------
;; ITerm: how noms and ties unify, walk, and reify
;; --------------------------------------------------------------------

(extend-protocol l/ITerm
  Nom
  (-unify-term [u v s]
    (if (and (nom? v) (= u v)) s nil))
  ;; Look u up in s: a no-op against a substitution (noms are never keys
  ;; there), but during reification s is the name map and this replaces
  ;; the nom with its reification name.
  (-walk-term [u s] (get s u u))
  (-reify-term [u names]
    (if (contains? names u)
      names
      (assoc names u (symbol (str (:name u) "_" (count names))))))

  Tie
  (-unify-term [u v s]
    (when (tie? v)
      (let [a1 (:binding u) a2 (:binding v)]
        (if (= a1 a2)
          (l/unify (:term u) (:term v) s)
          ;; alpha-equivalence: unify this body against the other body with
          ;; the two binders swapped.
          (l/unify (:term u)
                   (swap-term a1 a2 (l/walk* (:term v) s))
                   s)))))
  (-walk-term [u s]
    (->Tie (l/walk* (:binding u) s) (l/walk* (:term u) s)))
  (-reify-term [u names]
    (l/reify-names (:term u) (l/reify-names (:binding u) names))))
