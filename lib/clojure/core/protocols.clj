;; clojure.core.protocols - extension points for reduce/reduce-kv/datafy/nav.
;;
;; The four protocols (CollReduce, IKVReduce, Datafiable, Navigable)
;; are interned at boot time in clojure.core. This namespace re-binds
;; the same vars under clojure.core.protocols so user code can write
;; (extend-protocol clojure.core.protocols/CollReduce SomeType ...)
;; matching canonical Clojure usage. Because every binding holds the
;; same atom value, swap! mutations made through this namespace are
;; visible to the boot-time reduce / reduce-kv / datafy / nav.

(ns clojure.core.protocols)

(def CollReduce clojure.core/CollReduce)
(def IKVReduce  clojure.core/IKVReduce)
(def Datafiable clojure.core/Datafiable)
(def Navigable  clojure.core/Navigable)

(def coll-reduce clojure.core/coll-reduce)
(def kv-reduce   clojure.core/kv-reduce)
(def datafy      clojure.core/datafy)
(def nav         clojure.core/nav)

(def CollReduce--coll-reduce clojure.core/CollReduce--coll-reduce)
(def IKVReduce--kv-reduce    clojure.core/IKVReduce--kv-reduce)
(def Datafiable--datafy      clojure.core/Datafiable--datafy)
(def Navigable--nav          clojure.core/Navigable--nav)

;; InternalReduce - the seq-side reduction extension point. It has no
;; boot-time twin in clojure.core (reduce's built-in seq path lives in
;; C), so the protocol itself is declared here rather than re-bound.
;; The :default implementation hands the seq to the built-in reduction,
;; so calling internal-reduce directly works on any reducible value.

(defprotocol InternalReduce
  "Protocol for seq types that can provide their own reduction
  strategy. The default walks the seq with the built-in reduction."
  (internal-reduce [s f start]))

(extend-type :default InternalReduce
  (internal-reduce [s f start]
    (clojure.core/internal-reduce f start s)))
