(ns clojure.walk)

;; walk, postwalk, prewalk, postwalk-replace, prewalk-replace
;; are defined in core.mino and available in every namespace.

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
