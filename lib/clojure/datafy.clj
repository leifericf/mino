;; clojure.datafy - canonical entry points for the Datafiable / Navigable
;; protocols. Both delegate to clojure.core.protocols (which re-binds the
;; clojure.core protocol vars), so extending Datafiable or Navigable in
;; either namespace updates the same dispatch table that datafy and nav
;; consult.

(ns clojure.datafy
  (:require clojure.core.protocols))

(def datafy clojure.core/datafy)
(def nav    clojure.core/nav)
