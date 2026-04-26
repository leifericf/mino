(require "tests/test")

;; Tests for mino-level multimethod dialect semantics: cache invalidation
;; on hierarchy mutation, transitive prefers, prefers / remove-all-methods.

;; --- prefers function ---

(defmulti pf-mm identity)
(defmethod pf-mm :a [_] :a)
(defmethod pf-mm :b [_] :b)

(deftest prefers-empty
  (is (= {} (prefers pf-mm))))

(deftest prefers-after-prefer-method
  (prefer-method pf-mm :a :b)
  (let [pt (prefers pf-mm)]
    (is (contains? pt :a))
    (is (contains? (get pt :a) :b))))

;; --- remove-all-methods ---

(defmulti ram-mm identity)
(defmethod ram-mm :x [_] :x)
(defmethod ram-mm :y [_] :y)

(deftest remove-all-methods-clears
  (is (= 2 (count (methods ram-mm))))
  (remove-all-methods ram-mm)
  (is (= 0 (count (methods ram-mm))))
  (is (thrown? (ram-mm :x))))

;; --- Cache invalidation on derive after dispatch ---

(defmulti ci-mm identity)
(defmethod ci-mm :ci-parent [_] :parent-method)

(deftest hierarchy-invalidates-cache
  ;; Without :ci-child as parent of :ci-parent, no method matches.
  (is (thrown? (ci-mm :ci-child)))
  ;; Now derive — the cached "no method" result must be invalidated so
  ;; the new ancestor lookup runs.
  (derive :ci-child :ci-parent)
  (is (= :parent-method (ci-mm :ci-child)))
  ;; Underive must also invalidate the now-stale positive cache entry.
  (underive :ci-child :ci-parent)
  (is (thrown? (ci-mm :ci-child))))

;; --- Transitive prefers via parents ---
;;
;; In the prefer-table we register :A > :B. Then we derive :child-a from
;; :A and :child-b from :B. Dispatching on :triangle (which derives from
;; both child-a and child-b) yields two ancestor matches; transitive
;; prefers must follow parents to resolve :child-a as preferred even
;; though no direct child-a > child-b entry exists.

(defmulti tp-mm identity)

(deftest transitive-prefers
  (prefer-method tp-mm :tp-A :tp-B)
  (derive :tp-child-a :tp-A)
  (derive :tp-child-b :tp-B)
  (defmethod tp-mm :tp-child-a [_] :child-a-method)
  (defmethod tp-mm :tp-child-b [_] :child-b-method)
  (derive :tp-tri :tp-child-a)
  (derive :tp-tri :tp-child-b)
  ;; Both :tp-child-a and :tp-child-b match :tp-tri. The prefer-table
  ;; only has :tp-A > :tp-B; transitive prefers follow parents so
  ;; :tp-child-a wins via its parent :tp-A.
  (is (= :child-a-method (tp-mm :tp-tri))))
