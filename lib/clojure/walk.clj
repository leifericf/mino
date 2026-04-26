(ns clojure.walk)

;; The traversal kernel (walk, postwalk, prewalk, postwalk-replace,
;; prewalk-replace) lives in core.clj because its primitives are
;; needed across the standard library. Re-export the canonical
;; clojure.walk surface here so portable Clojure code that calls
;; (clojure.walk/postwalk ...) resolves the same vars without
;; depending on the bundled-core organization.

(def walk             clojure.core/walk)
(def postwalk         clojure.core/postwalk)
(def prewalk          clojure.core/prewalk)
(def postwalk-replace clojure.core/postwalk-replace)
(def prewalk-replace  clojure.core/prewalk-replace)

(defn keywordize-keys [m]
  (postwalk (fn [x]
    (if (map? x)
      (into {} (map (fn [[k v]] [(if (string? k) (keyword k) k) v]) x))
      x)) m))

(defn stringify-keys [m]
  (postwalk (fn [x]
    (if (map? x)
      (into {} (map (fn [[k v]] [(if (keyword? k) (name k) k) v]) x))
      x)) m))

(defn macroexpand-all [form]
  (prewalk (fn [x] (if (seq? x) (macroexpand x) x)) form))
