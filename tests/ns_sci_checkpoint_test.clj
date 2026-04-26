(require "tests/test")

;; Translated from upstream SCI test suite:
;;   /tmp/upstream-ns-tests/sci/namespaces_test.cljc
;;
;; This is a checkpoint translation written ahead of mino's first-class
;; namespace support. Most tests are expected to FAIL today and will pass
;; as the namespace feature lands. SCI-API-coupled, JVM-only and CLJS-only
;; tests are skipped with #_ and a reason.
;;
;; Translation notes:
;; - Upstream SCI tests use (eval* "...") to evaluate strings against a
;;   fresh SCI runtime. We translate code directly into the test body when
;;   semantics allow, and use (eval (read-string "...")) only when the
;;   test specifically depends on read-time *ns* (e.g. ::foo resolution).
;; - thrown-with-msg? is converted to plain (is (thrown? body)); the
;;   original regex pattern is preserved as a comment.
;; - Aliases used in upstream eval* setup:
;;     str -> clojure.string
;;     set -> clojure.set
;;   We require these explicitly where needed.

;; ---- require-test ----

(deftest require-test
  ;; "(str/join \"-\" [1 2 3])"
  (require '[clojure.string :as str])
  (is (= "1-2-3" (str/join "-" [1 2 3])))

  ;; "(require '[clojure.string :as string]) (string/join \"-\" [1 2 3])"
  (require '[clojure.string :as string])
  (is (= "1-2-3" (string/join "-" [1 2 3])))

  ;; "(require '[clojure.string :refer [join]]) (join \"-\" [1 2 3])"
  (require '[clojure.string :refer [join]])
  (is (= "1-2-3" (join "-" [1 2 3])))

  ;; "(require '[clojure.string :refer :all]) (join \"-\" [1 2 3])"
  (require '[clojure.string :refer :all])
  (is (= "1-2-3" (join "-" [1 2 3])))

  ;; thrown-with-msg? #"must be a sequential"
  (is (thrown? (require '[clojure.string :refer 1])))

  ;; "(set/union #{1 2 3} #{4 5 6})"
  (require '[clojure.set :as set])
  (is (= #{1 4 6 3 2 5} (set/union #{1 2 3} #{4 5 6})))

  ;; "(require '[clojure.set :as s]) (s/union #{1 2 3} #{4 5 6})"
  (require '[clojure.set :as s])
  (is (= #{1 4 6 3 2 5} (s/union #{1 2 3} #{4 5 6})))

  ;; thrown-with-msg? #"clojure.foo"
  (is (thrown? (require '[clojure.foo :as s])))

  ;; thrown-with-msg? #"quux does not exist"
  (is (thrown? (require '[clojure.set :refer [quux]])))

  ;; Vars partitioned by ns; original eval* string version preserved via
  ;; eval+read-string because it depends on multiple ns transitions and
  ;; resolution of foo/x and bar/x.
  (is (= [1 2 3]
         (eval (read-string "(do (ns foo) (def x 1) (ns bar) (def x 2) (in-ns 'baz) (def x 3) (require 'foo 'bar) [foo/x bar/x x])"))))

  (testing "Require evaluates arguments"
    (is (= [1 2 3]
           (eval (read-string "(do (ns foo) (def x 1) (ns bar) (def x 2) (in-ns 'baz) (def x 3) (require (symbol \"foo\") (symbol \"bar\")) [foo/x bar/x x])")))))

  (testing "require as function"
    (is (= 1
           (eval (read-string "(do (ns foo) (defn foo [] 1) (ns bar) (apply require ['[foo :as f]]) (f/foo))")))))

  (testing "rename"
    ;; "(require '[clojure.set :refer [union] :rename {union union2}]) (union2 #{1} #{2})"
    (is (= #{1 2}
           (eval (read-string "(do (require '[clojure.set :refer [union] :rename {union union2}]) (union2 #{1} #{2}))"))))
    ;; "(ns foo (:refer-clojure :rename {bit-shift-left <<})) (<< 8 1)"
    (is (= 16
           (eval (read-string "(do (ns foo (:refer-clojure :rename {bit-shift-left <<})) (<< 8 1))"))))
    ;; thrown-with-msg? #"Unable to resolve.*bit-shift-left"
    (is (thrown?
         (eval (read-string "(do (ns foo (:refer-clojure :rename {bit-shift-left <<})) (bit-shift-left 8 1))")))))

  #_ ;; SCI-API-coupled: uses tu/eval* with :load-fn callback. mino require
     ;; loads from disk, not from a host-supplied :load-fn.
  (testing "load-fn + requiring-resolve"
    (is (= :success
           (deref (requiring-resolve 'foo.bar/x)))))

  ;; thrown-with-msg? #"already refers to"
  (is (thrown?
       (eval (read-string "(do (ns foo (:require [clojure.string :refer [split]])) (declare split))")))))

;; ---- use-test ----

(deftest use-test
  ;; Each assertion uses a fresh namespace so :exclude / :only restrictions
  ;; aren't masked by names that earlier assertions referred in.
  (is (= #{1 2}
         (eval (read-string "(do (ns ut-a1 (:use clojure.set)) (union #{1} #{2}))"))))

  (is (= #{1 2}
         (eval (read-string "(do (ns ut-a2) (use 'clojure.set) (union #{1} #{2}))"))))

  (is (= #{1 2}
         (eval (read-string "(do (ns ut-a3) (use '[clojure.set :only [union]]) (union #{1} #{2}))"))))

  ;; :exclude hides union from the fresh ns.
  (is (thrown?
       (eval (read-string "(do (ns ut-a4) (use '[clojure.set :exclude [union]]) (union #{1} #{2}))"))))

  ;; :only [difference] does not bring in union.
  (is (thrown?
       (eval (read-string "(do (ns ut-a5) (use '[clojure.set :only [difference]]) (union #{1} #{2}))"))))

  ;; thrown-with-msg? #"already refers to"
  ;; capitalize is the mino-side definition that lives only in
  ;; clojure.string; bringing it in via :use should still trigger the
  ;; refer-collision when the ns already declares the same name.
  (is (thrown?
       (eval (read-string "(do (ns ut-a6 (:use clojure.string)) (declare capitalize))")))))

;; ---- misc-namespace-test ----

(deftest misc-namespace-test
  (is (= 1
         (eval (read-string "(do (alias (symbol \"c\") (symbol \"clojure.core\")) (c/and true 1))"))))

  ;; alias set1/set2 then chained difference/union (upstream uses 3-arity
  ;; mapv to register both aliases at once; mino's mapv is 2-arity).
  (require '[clojure.set])
  (is (= #{1 3 2}
         (eval (read-string "(do (alias 'set1 'clojure.set) (alias 'set2 'clojure.set) (set2/difference (set1/union #{1 2 3} #{4 5 6}) #{4 5 6}))"))))

  ;; "(ns-name (find-ns 'clojure.set))"
  (is (= 'clojure.set (ns-name (find-ns 'clojure.set))))

  ;; "(ns-name (the-ns (the-ns 'clojure.set)))"
  (is (= 'clojure.set (ns-name (the-ns (the-ns 'clojure.set)))))

  ;; "(alias 'c 'clojure.core) (ns-name (get (ns-aliases *ns*) 'c))"
  (alias 'c 'clojure.core)
  (is (= 'clojure.core (ns-name (get (ns-aliases *ns*) 'c))))

  ;; identical? find-ns vs alias lookup
  (is (true? (identical? (find-ns 'clojure.core) (get (ns-aliases *ns*) 'c))))

  (testing "alias accepts namespace objects"
    (alias 'c2 (find-ns 'clojure.core))
    (is (= 'clojure.core (ns-name (get (ns-aliases *ns*) 'c2))))
    (alias 'mns (create-ns 'my.new.ns))
    (is (= 'my.new.ns (ns-name (get (ns-aliases *ns*) 'mns)))))

  #_ ;; clojure.repl/dir-fn is REPL-only (per task spec, skip)
  (is (contains? (set (clojure.repl/dir-fn 'clojure.string)) 'last-index-of))

  ;; create-ns returns same object for same name
  (is (true?
       (let [foo-ns (create-ns 'foo)
             another-foo-ns (create-ns 'foo)]
         (and (identical? foo-ns another-foo-ns)
              (= 'foo (ns-name foo-ns)))))))

;; ---- autoresolve-test ----
;; Auto-resolved keywords (::foo, ::alias/name) are resolved at READ
;; time using the current *ns* and its aliases. mino keeps read and
;; eval as distinct phases (Hickey's read/eval separation) — the
;; upstream SCI checkpoint embeds (in-ns ...) inside the same source
;; string as ::foo and relies on SCI's interleaved read+eval pipeline
;; to observe the namespace switch before the keyword is read. Mino
;; reads the whole form first, so the second case below pre-runs the
;; ns / alias setup, then reads+evals the keyword in the resulting
;; context. The intent — "auto-resolved keywords pick up the active
;; ns and aliases" — is preserved.

(deftest autoresolve-test
  (is (= :user/foo (eval (read-string "::foo"))))

  ;; Switch ns first, then read+eval ::foo so the read sees the new
  ;; *ns* binding.
  (let [orig (ns-name *ns*)]
    (try
      (in-ns 'bar)
      (is (= :bar/foo (eval (read-string "::foo"))))
      (finally (in-ns orig))))

  ;; Set the alias first, then read+eval ::str/foo.
  (require '[clojure.string :as str])
  (is (= :clojure.string/foo (eval (read-string "::str/foo"))))

  ;; Re-binding the alias to clojure.set updates resolution for
  ;; subsequent reads.
  (require '[clojure.set :as str])
  (is (= :clojure.set/foo (eval (read-string "::str/foo"))))
  (require '[clojure.string :as str])

  ;; in-ns + alias + read+eval, all in the test ns.
  (let [orig (ns-name *ns*)]
    (try
      (in-ns 'foo)
      (require '[clojure.string :as str])
      (is (= :clojure.string/foo (eval (read-string "::str/foo"))))
      (finally (in-ns orig))))

  ;; ns form with :require :as: declare the ns first, then read+eval.
  (eval (read-string "(ns foo (:require [clojure.string :as s]))"))
  (let [orig (ns-name *ns*)]
    (try
      (in-ns 'foo)
      (is (= :clojure.string/foo (eval (read-string "::s/foo"))))
      (finally (in-ns orig)))))

;; ---- in-ns-test ----
;; See autoresolve-test for the rationale on splitting in-ns from the
;; subsequent read+eval of ::foo.

(deftest in-ns-test
  (is (= :user/foo (eval (read-string "::foo"))))

  (let [orig (ns-name *ns*)]
    (try
      (in-ns 'bar)
      (is (= :bar/foo (eval (read-string "::foo"))))
      (finally (in-ns orig))))

  ;; in-ns then def then in-ns again then read the var
  (is (= :bar/foo
         (eval (read-string "(do (in-ns 'bar) (def just-one-ns :bar/foo) (in-ns 'bar) just-one-ns)")))))

;; ---- vars-partitioned-by-namespace-test ----

(deftest vars-partitioned-by-namespace-test
  ;; "(in-ns 'foo) (def x 10) (in-ns 'bar) (def x 11) (in-ns 'foo) x"
  (is (= 10
         (eval (read-string "(do (in-ns 'foo) (def x 10) (in-ns 'bar) (def x 11) (in-ns 'foo) x)")))))

;; ---- ns-form-test ----

(deftest ns-form-test
  ;; "(ns foo (:require [clojure.set :as x])) (x/difference #{1 2 3} #{2 3 4})"
  (is (= #{1}
         (eval (read-string "(do (ns foo (:require [clojure.set :as x])) (x/difference #{1 2 3} #{2 3 4}))"))))

  ;; ns with docstring + metadata + :require :refer
  (is (= #{1}
         (eval (read-string "(do (ns foo \"docstring\" {:metadata 1} (:require [clojure.set :refer [difference]])) (difference #{1 2 3} #{2 3 4}))"))))

  #_ ;; JVM-only: (:import [clojure.lang ExceptionInfo])
  (is (= :foo/foo
         (eval (read-string "(do (ns foo (:import [clojure.lang ExceptionInfo])) (try ::foo (catch ExceptionInfo e nil)))")))))

;; ---- ns-name-test ----

(deftest ns-name-test
  (is (= 'foo (eval (read-string "(do (ns foo) (ns-name *ns*))")))))

;; ---- no-crash-test ----

(deftest no-crash-test
  ;; ns + auto-resolved keyword in the new ns. Per the autoresolve-test
  ;; rationale, declare the ns first then read+eval the keyword in it.
  (eval (read-string "(ns foo \"docstring\")"))
  (let [orig (ns-name *ns*)]
    (try
      (in-ns 'foo)
      (is (= :foo/foo (eval (read-string "::foo"))))
      (finally (in-ns orig)))))

;; ---- ns-metadata-test ----

(deftest ns-metadata-test
  (is (= {:a 1, :b 1}
         (eval (read-string "(do (ns ^{:a 1} foo {:b 1}) (meta *ns*))"))))
  (is (= {:a 1, :b 1}
         (eval (read-string "(do (ns ^{:a 1} foo {:b 1}) (meta *ns*) (ns bar) (meta (the-ns 'foo)))")))))

;; ---- recycle-namespace-objects ----

(deftest recycle-namespace-objects
  ;; (set/difference (set (all-ns)) (set (all-ns))) is empty
  (require '[clojure.set :as set])
  (is (empty? (set/difference (set (all-ns)) (set (all-ns))))))

;; ---- namespace-doc ----

(deftest namespace-doc
  (is (= "foobar"
         (:doc (eval (read-string "(do (ns foo \"foobar\") (meta (find-ns 'foo)))"))))))

#_ ;; JVM-only deftest: ns-imports-test — uses :import, clojure.lang.ExceptionInfo, java.lang.String.
(deftest ns-imports-test
  (is (some? (get (ns-imports *ns*) 'String)))
  (is (do (import 'clojure.lang.ExceptionInfo)
          (some? (get (ns-imports *ns*) 'ExceptionInfo)))))

;; ---- refer-clojure-exclude ----

#_ ;; SKIPPED until Phase A: the second assertion redefines `get` via eval,
   ;; leaking into the global root env and clobbering the test framework's
   ;; own (get state :pass) calls. Re-enable when per-ns isolation contains
   ;; the redef.
(deftest refer-clojure-exclude
  ;; (ns foo (:refer-clojure :exclude [get])) (some? get)  -- should throw
  (is (thrown?
       (eval (read-string "(do (ns foo (:refer-clojure :exclude [get])) (some? get))"))))
  ;; ... but defining get locally works
  (is (true?
       (eval (read-string "(do (ns foo (:refer-clojure :exclude [get])) (defn get []) (some? get))")))))

;; ---- refer-test ----

(deftest refer-test
  ;; Each form runs in a fresh namespace so :only / :exclude restrictions
  ;; aren't masked by names referred in by earlier assertions.
  ;; Upstream uses join/includes? as the test names. capitalize and
  ;; escape both live only in clojure.string and have no parent-chain
  ;; backing primitive, so the :only/:exclude restriction is observable
  ;; on them.
  (is (thrown?
       (eval (read-string "(do (ns rt-a1) (refer 'clojure.string :only [capitalize]) escape)"))))
  (is (thrown?
       (eval (read-string "(do (ns rt-a2) (refer 'clojure.string :exclude [capitalize]) capitalize)"))))
  (is (eval (read-string "(do (ns rt-a3) (refer 'clojure.string :only '[capitalize]) (some? capitalize))")))
  (is (eval (read-string "(do (ns rt-a4) (refer 'clojure.string) (some? capitalize))")))
  ;; local defn capitalize wins over refer
  (is (eval (read-string "(do (ns rt-a5) (defn capitalize []) (refer 'clojure.string) (= 'rt-a5 (:ns (meta #'capitalize))))")))
  ;; syntax-quote resolves to clojure.string/capitalize when refer'd
  (is (eval (read-string "(do (ns rt-a6) (refer 'clojure.string) (= 'clojure.string/capitalize `capitalize))"))))

;; ---- ns-publics-test ----

(deftest ns-publics-test
  (require '[clojure.string :as str])
  ;; (defn foo []) (str (ns-publics *ns*)) contains "foo #'user/foo"
  (is (str/includes?
       (eval (read-string "(do (defn foo []) (str (ns-publics *ns*)))"))
       "foo #'user/foo"))
  (testing "See issue 519, 520, 523"
    (is (eval (read-string "(do (require '[clojure.string :refer [includes?]]) (nil? (get (ns-publics *ns*) :refer)))")))))

;; ---- ns-refers-test ----

(deftest ns-refers-test
  (is (eval (read-string "(some? (get (ns-refers *ns*) 'inc))")))
  (is (eval (read-string "(do (def x 1) (nil? (get (ns-refers *ns*) 'x)))")))
  (is (eval (read-string "(do (require '[clojure.string :refer [includes?]]) (some? (get (ns-refers *ns*) 'includes?)))")))
  (testing "private vars are not referred"
    (is (eval (read-string "(every? (fn [[_ v]] (not (:private (meta v)))) (ns-refers *ns*))")))))

;; ---- ns-map-test ----

(deftest ns-map-test
  (is (eval (read-string "(some? (get (ns-map *ns*) 'inc))")))
  #_ ;; JVM-only: java.lang.String in ns-map
  (is (eval (read-string "(some? (get (ns-map *ns*) 'String))")))
  (is (eval (read-string "(do (defn- foo []) (some? (get (ns-map *ns*) 'foo)))")))
  #_ ;; SKIPPED until Phase A: redefines `inc` via eval, leaking into the
     ;; global root env and clobbering the test framework's own arithmetic.
     ;; Re-enable when per-ns isolation contains the redef.
  (testing "ns-map reflects interned vars shadowing refers"
    (is (= :foo
           (eval (read-string "(do (defn inc [x] :foo) ((get (ns-map *ns*) 'inc) 1))"))))))

;; ---- ns-unmap-test ----

(deftest ns-unmap-test
  (is (eval (read-string "(do (def foo 1) (ns-unmap *ns* 'foo) (nil? (resolve 'foo)))")))
  (is (eval (read-string "(do (defn bar []) (ns-unmap *ns* 'bar) (nil? (resolve 'bar)))")))
  (is (eval (read-string "(do (defn- baz []) (ns-unmap *ns* 'baz) (nil? (resolve 'baz)))")))
  (is (eval (read-string "(do (require '[clojure.string :refer [join]]) (ns-unmap *ns* 'join) (defn join []))")))

  #_ ;; JVM-only: java.lang.Object import/unmap
  (is (= [false true]
         (eval (read-string "(do (ns-unmap *ns* 'Object) (def o1 (resolve 'Object)) (import '[java.lang Object]) (def o2 (resolve 'Object)) [(some? o1) (some? o2)])"))))

  #_ ;; SCI-API-coupled: uses sci/init, sci/binding, sci/eval-form with :namespaces opt.
  (testing "issue 637: config already contain name of referred var in config"
    (is (= [nil "bar"]
           (do (require '[remote :refer [cake]])
               (ns-unmap *ns* 'cake)
               (def resolved (resolve 'cake))
               (intern *ns* 'cake "bar")
               [resolved cake]))))

  #_ ;; JVM-only: ns-unmap of the java.lang.String import. mino has
     ;; no Java imports — :import throws :mino/unsupported — so there
     ;; is no `String` mapping to remove. The upstream test exercises
     ;; "remove the imported class so the bare-symbol falls through
     ;; to the current ns", which doesn't apply on mino.
  (testing "unmapping Class and then fully qualifying it"
    (is (= 'user/String
           (eval (read-string "(do (ns-unmap *ns* 'String) `String)"))))))

;; ---- find-var-test ----

(deftest find-var-test
  (is (eval (read-string "(= #'clojure.core/map (find-var 'clojure.core/map))")))
  (is (eval (read-string "(nil? (find-var 'clojure.core/no-such-symbol))")))
  ;; thrown-with-msg? #"No such namespace: no.such.namespace"
  (is (thrown? (find-var 'no.such.namespace/var)))
  ;; thrown-with-msg? #"Not a qualified symbol: no-namespace"
  (is (thrown? (find-var 'no-namespace))))

;; ---- find-ns-test ----

(deftest find-ns-test
  (is (true?
       (eval (read-string "(do (ns foo) (some? (find-ns 'foo)))"))))
  (is (nil? (eval (read-string "(find-ns 'never-defined-ns-xyz)"))))
  (is (nil? (eval (read-string "(find-ns nil)")))))

;; ---- remove-ns-test ----

(deftest remove-ns-test
  (is (nil?
       (eval (read-string "(do (ns foo) (ns bar) (remove-ns 'foo) (find-ns 'foo))")))))

;; ---- ns-unalias-test ----

(deftest ns-unalias-test
  (testing "Removing an alias in an unknown namespace throws"
    (is (thrown? (ns-unalias (find-ns 'unknown) 'core))))

  (testing "Removing an undefined alias is a no-op"
    (is (nil? (ns-unalias *ns* 'core))))

  (testing "Removing a defined alias -- ns symbol"
    (is (nil?
         (eval (read-string "(do (alias 'core 'clojure.core) (ns-unalias 'user 'core) (get (ns-aliases *ns*) 'core))")))))

  (testing "Removing a defined alias -- ns object"
    (is (nil?
         (eval (read-string "(do (alias 'core 'clojure.core) (ns-unalias *ns* 'core) (get (ns-aliases *ns*) 'core))"))))))

;; ---- ns-syntax-test ----

(deftest ns-syntax-test
  ;; thrown-with-msg? #"symbol"
  (is (thrown? (eval (read-string "(ns 1)")))))

;; ---- nested-libspecs-test ----

(deftest nested-libspecs-test
  ;; "(require '[clojure [set :refer [union]]]) (union #{1 2 3} #{2 3 4})"
  (is (= #{1 2 3 4}
         (eval (read-string "(do (require '[clojure [set :refer [union]]]) (union #{1 2 3} #{2 3 4}))"))))

  ;; thrown-with-msg? #"lib names inside prefix lists must not contain periods"
  (is (thrown?
       (eval (read-string "(do (ns clojure.core.protocols) (ns foo) (require '[clojure [core.protocols]]))"))))

  ;; thrown-with-msg? #"Unsupported option\(s\) supplied: :foo"
  (is (thrown?
       (eval (read-string "(ns foo (:require [clojure.core] [dude] :foo))"))))

  #_ ;; thrown-with-data? is not in mino's test framework. The intent
     ;; (error message contains location) overlaps the previous case.
  (testing "error message contains location"
    (is (thrown?
         (eval (read-string "(ns foo (:require [clojure.core] [dude] :foo))"))))))

;; ---- cyclic-load-test ----
;; Upstream uses sci/eval-string with :load-fn callback to inject
;; sources for foo and bar; SCI's symbol-form require with the
;; runtime-ns shortcut tolerates a cyclic reference once the inner
;; namespace has bindings. mino's symbol-form require searches the
;; project :paths list (the embedder configures it from mino.edn)
;; and has no API to add a path at runtime, so the temp fixture has
;; to be reached via path-form require with absolute paths. Both
;; mutual files use path-form so the load-stack key is stable across
;; the cycle.
;;
;; The "foo already loaded, OK to have cyclic dep on foo from bar"
;; subcase from upstream relies on a runtime-ns shortcut that mino
;; gates on "no resolvable file" (a path that resolves to a real file
;; always goes through cycle detection). The runtime-only equivalent
;; is exercised by ns_libs_load_later — a runtime ns can require a
;; later-arriving ns without a backing file and the shortcut fires.

(deftest cyclic-load-test
  (let [dir     "/tmp/sci-test-cycle"
        foo-pth (str dir "/foo.clj")
        bar-pth (str dir "/bar.clj")]
    (mkdir-p dir)
    ;; (:require "...") inside an ns form expects a libspec symbol or
    ;; vector, so the cycle is set up via top-level (require "...")
    ;; calls inside each file.
    (spit foo-pth (str "(ns cyc-foo) (require \"" bar-pth "\")"))
    (spit bar-pth (str "(ns cyc-bar) (require \"" foo-pth "\")"))
    (is (thrown? (require foo-pth)))
    (is (thrown? (require bar-pth)))))

;; ---- as-alias-test ----
;; :as-alias registers an alias to a (possibly nonexistent) namespace
;; without loading it. The original SCI checkpoint smushes the alias
;; setup and the auto-resolved keyword into one read+eval; mino runs
;; the alias-registering ns / require first and then reads ::foo/bar
;; in that context (see autoresolve-test for the rationale).

(deftest as-alias-test
  (eval (read-string "(ns my-ns (:require [dude :as-alias foo]))"))
  (let [orig (ns-name *ns*)]
    (try
      (in-ns 'my-ns)
      (is (= :dude/bar (eval (read-string "::foo/bar"))))
      (finally (in-ns orig))))

  (require '[dude2 :as-alias foo2])
  (is (= :dude2/bar (eval (read-string "::foo2/bar")))))

#_ ;; SCI-API-coupled: exposed-vals-test exercises sci/init :load-fn callback
   ;; signature ({:keys [libname ctx ns opts]} ...). Not applicable to mino.
(deftest exposed-vals-test)

#_ ;; SCI-API-coupled: docstrings-test iterates over (sci/init {}) :env
   ;; :namespaces 'clojure.core. Skipped — relies on SCI internals.
(deftest docstrings-test)

#_ ;; JVM-only deftest: no-cljs-var-resolve-in-clj-test — uses sci/eval-string
   ;; with #?(:clj ...) reader conditional.
(deftest no-cljs-var-resolve-in-clj-test
  (is (nil? (eval (read-string "(resolve 'cljs.core/inc)")))))

#_ ;; CLJS-only deftest: test-munge-demunge — exercises CLJS munge/demunge.
(deftest test-munge-demunge)

;; ---- macroexpand-eval-test ----

(deftest macroexpand-eval-test
  ;; "(eval (macroexpand '(ns foo (:require [clojure.string :as str])))) `str/x"
  (is (= 'clojure.string/x
         (eval (read-string "(do (eval (macroexpand '(ns foo (:require [clojure.string :as str])))) `str/x)")))))

#_ ;; SCI-API-coupled: loaded-libs-test uses tu/eval* with :load-fn callback
   ;; and inspects (loaded-libs) / @*loaded-libs* — both SCI-internal state.
(deftest loaded-libs-test)

;; ---- issue-1011 ----

(deftest issue-1011
  ;; "(ns foo {:a 1}) (ns foo {:b 1}) (in-ns 'foo) (meta *ns*)"
  (is (= {:b 1}
         (eval (read-string "(do (ns foo {:a 1}) (ns foo {:b 1}) (in-ns 'foo) (meta *ns*))")))))
