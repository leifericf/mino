(require "tests/test")

;; Deep interpreter recursion drives the gc_save pin stack past its
;; ceiling: every eval_args frame pins its in-flight argument list.
;; The pin macro drops stores past GC_SAVE_MAX but still increments
;; the count, and manual slot writers that mirror the macro used to
;; store without the bound check, stamping pinned values into the
;; ctx fields that follow gc_save (parked_sp first). Under the ASan
;; lane the loud pin assert pins this; the fix keeps every manual
;; store in bounds and raises the ceiling past interpreter depth.

(def deep-form
  (reduce (fn [acc _] (list 'inc acc)) 0 (range 120)))

(def deep-pairs-form
  (reduce (fn [acc _] (list 'conj acc (list 'inc (quote 0)))) [0] (range 120)))

(deftest deep-interpreted-eval-survives-pin-pressure
  (is (= 120 (eval deep-form)))
  (is (= 121 (count (eval deep-pairs-form))))
  ;; Force a collection after the deep run so a corrupted ctx field
  ;; would be walked by the root scanner.
  (vec (map inc (range 20000)))
  (is (= 120 (eval deep-form))))

(run-tests-and-exit)
