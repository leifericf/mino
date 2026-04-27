;; clojure.core.specs.alpha -- specs for core macro forms (defn, let,
;; binding, fn, ...).  These are not enforced automatically; tools that
;; want to validate forms call (s/conform :clojure.core.specs.alpha/...).

(ns clojure.core.specs.alpha
  (:require [clojure.spec.alpha :as s]))

;;; ---------------------------------------------------------------------------
;;; Destructuring binding forms.
;;; ---------------------------------------------------------------------------

(s/def ::local-name (s/and simple-symbol? #(not= '& %)))

(s/def ::binding-form
  (s/or :sym ::local-name
        :seq ::seq-binding-form
        :map ::map-binding-form))

(s/def ::seq-binding-form
  (s/and vector?
         (s/cat :forms   (s/* ::binding-form)
                :rest    (s/? (s/cat :amp #{'&} :form ::binding-form))
                :as-form (s/? (s/cat :as #{:as} :as-sym ::local-name)))))

(s/def ::keys     (s/coll-of ident? :kind vector?))
(s/def ::syms     (s/coll-of symbol? :kind vector?))
(s/def ::strs     (s/coll-of simple-symbol? :kind vector?))
(s/def ::or       (s/map-of simple-symbol? any?))
(s/def ::as       ::local-name)

(s/def ::map-special-binding
  (s/keys :opt-un [::as ::or ::keys ::syms ::strs]))

(s/def ::map-binding (s/tuple ::binding-form any?))

(s/def ::ns-keys
  (s/tuple
    (s/and qualified-keyword? #(-> % name #{"keys" "syms"}))
    (s/coll-of simple-symbol? :kind vector?)))

(s/def ::map-bindings
  (s/coll-of (s/or :map-binding ::map-binding
                   :qualified-keys-or-syms ::ns-keys
                   :special-binding (s/tuple #{:as :or :keys :syms :strs} any?))
             :kind map?))

;; Canonical clojure.core.specs.alpha defines map-binding-form as
;; (s/merge ::map-bindings ::map-special-binding).  mino's spec port
;; doesn't ship s/merge yet; users who need the combined check can call
;; both specs via s/and at the use site.
(s/def ::map-binding-form (s/and ::map-bindings ::map-special-binding))

;;; ---------------------------------------------------------------------------
;;; defn / fn arglist forms.
;;; ---------------------------------------------------------------------------

(s/def ::param-list ::seq-binding-form)
(s/def ::params+body
  (s/cat :params ::param-list
         :body   (s/* any?)))

(s/def ::defn-args
  (s/cat :name        simple-symbol?
         :docstring   (s/? string?)
         :meta        (s/? map?)
         :fn-tail     (s/alt :arity-1 ::params+body
                             :arity-n (s/cat :bodies (s/+ (s/spec ::params+body))
                                             :attr-map (s/? map?)))))

;;; ---------------------------------------------------------------------------
;;; binding-style let / loop forms.
;;; ---------------------------------------------------------------------------

(s/def ::binding (s/cat :form ::binding-form :init-expr any?))
(s/def ::bindings (s/and vector? (s/* ::binding)))
