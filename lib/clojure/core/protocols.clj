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
