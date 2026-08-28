;; clojure.datafy - canonical entry points for the Datafiable / Navigable
;; protocols. Both delegate to clojure.core.protocols (which re-binds the
;; clojure.core protocol vars), so extending Datafiable or Navigable in
;; either namespace updates the same dispatch table that datafy and nav
;; consult.

(ns clojure.datafy
  (:require clojure.core.protocols))

;; Both re-exports carry their census-oracle arglists
;; (tools/gen_arglists.clj, lib-alias class) on def metadata: the init
;; values are vars, not fn forms.
(def ^{:arglists '([x])} datafy clojure.core/datafy)
(def ^{:arglists '([coll k v])} nav clojure.core/nav)
