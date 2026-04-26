(require "tests/test")

;; Source: /tmp/upstream-ns-tests/clojure/reader.clj
;; Subset: namespace-related reader tests only.
;;
;; Translated from upstream Clojure reader tests. We cherry-pick
;; assertions that exercise:
;;   - namespaced keywords        :foo/bar
;;   - auto-resolved keywords     ::foo, ::alias/foo
;;   - namespaced symbols         foo/bar  (read/print round-trip)
;;   - namespaced map literals    #:foo{...} and #::{...}
;;   - var quote on qualified     #'foo/bar
;;   - read-string + pr-str round-trip for the above
;;
;; Many of these will fail until ::foo / #:foo{...} / #::{...} are
;; implemented in the mino reader. That is intentional — failing
;; tests drive the work. See also reader_macros_test.clj for the
;; non-namespace dispatch reader features that already exist.
;;
;; Upstream deftests we drew from:
;;   Symbols, t-Keywords, reading-keywords, t-Var-quote (header only),
;;   namespaced-maps, namespaced-map-errors, namespaced-map-edn,
;;   test-read+string (the namespaced-keyword aspect).
;; Excluded: number/string/regex/char readers, reader-conditionals
;; (covered by reader_cond_test.clj), instants/UUIDs, line/column
;; metadata, #= reader-eval, Java-specific dispatch.

;; ------------------------------------------------------------------
;; Symbols  (subset of upstream `Symbols`) — namespaced-symbol shape.
;; ------------------------------------------------------------------

(deftest namespaced-symbol-construction
  (is (= 'abc/def (symbol "abc" "def")))
  (is (= 'abc.def/ghi (symbol "abc.def" "ghi")))
  (is (= 'abc/def.ghi (symbol "abc" "def.ghi"))))

(deftest namespaced-symbol-namespace-and-name
  (is (= "abc"     (namespace 'abc/def)))
  (is (= "def"     (name      'abc/def)))
  (is (= nil       (namespace 'abc)))
  (is (= "abc"     (name      'abc))))

(deftest namespaced-symbol-read-string
  (is (= 'foo/bar (read-string "foo/bar")))
  (is (= 'a.b.c/d (read-string "a.b.c/d"))))

(deftest namespaced-symbol-pr-str
  (is (= "foo/bar" (pr-str 'foo/bar)))
  (is (= "a.b.c/d" (pr-str 'a.b.c/d))))

(deftest namespaced-symbol-roundtrip
  (are [s] (= s (read-string (pr-str s)))
    'foo/bar
    'a.b.c/d
    'foo
    'with-dash/in-name))

;; ------------------------------------------------------------------
;; t-Keywords  (subset) — namespaced-keyword shape.
;; ------------------------------------------------------------------

(deftest namespaced-keyword-construction
  (is (= :abc/def     (keyword "abc" "def")))
  (is (= :abc/def     (keyword 'abc/def)))
  (is (= :abc.def/ghi (keyword "abc.def" "ghi")))
  (is (= :abc/def.ghi (keyword "abc" "def.ghi"))))

(deftest namespaced-keyword-namespace-and-name
  (is (= "abc" (namespace :abc/def)))
  (is (= "def" (name      :abc/def)))
  (is (= nil   (namespace :abc)))
  (is (= "abc" (name      :abc))))

(deftest namespaced-keyword-read-string
  (is (= :foo/bar (read-string ":foo/bar")))
  (is (= :a.b.c/d (read-string ":a.b.c/d"))))

(deftest namespaced-keyword-pr-str
  (is (= ":foo/bar" (pr-str :foo/bar)))
  (is (= ":foo"     (pr-str :foo))))

(deftest namespaced-keyword-roundtrip
  (are [k] (= k (read-string (pr-str k)))
    :foo/bar
    :a.b.c/d
    :foo))

;; ------------------------------------------------------------------
;; reading-keywords — auto-resolved ::foo and ::alias/foo.
;; Upstream binds *ns* to user; mino test runs in whatever ns is
;; current when this file is loaded. We assert current-ns resolution.
;; ------------------------------------------------------------------

(deftest read-plain-keyword-via-read-string
  (is (= :foo (read-string ":foo"))))

(deftest read-namespaced-keyword-via-read-string
  (is (= :foo/bar (read-string ":foo/bar"))))

(deftest read-auto-resolved-keyword-current-ns
  ;; ::foo at read-time resolves to the *current* ns.
  ;; The literal ::foo and the read form must agree.
  (is (= ::foo (read-string "::foo"))))

(deftest auto-resolved-keyword-has-namespace
  (is (not (nil? (namespace ::foo))))
  (is (= "foo" (name ::foo))))

(deftest auto-resolved-keyword-pr-str-is-fully-qualified
  ;; ::foo prints as :<current-ns>/foo, never as ::foo.
  (let [s (pr-str ::foo)]
    (is (= \: (first s)))
    (is (not (= \: (second s))))   ; no leading "::"
    (is (some? (re-find #"/foo$" s)))))

(deftest read-auto-resolved-alias-keyword-no-alias
  ;; ::does.not/exist must throw because the alias is unknown.
  (is (thrown? (read-string "::does.not/exist"))))

(deftest read-invalid-keyword-tokens
  (is (thrown? (read-string "foo:")))
  (is (thrown? (read-string ":bar/"))))

;; ------------------------------------------------------------------
;; t-Var-quote — #'foo/bar form. Upstream test body is empty; we add
;; the namespace-related read shape so the syntax is exercised.
;; ------------------------------------------------------------------

(deftest var-quote-reads-as-var-form
  ;; #'foo expands to (var foo). Same for qualified names.
  (is (= '(var foo)         (read-string "#'foo")))
  (is (= '(var foo/bar)     (read-string "#'foo/bar")))
  (is (= '(var a.b.c/d)     (read-string "#'a.b.c/d"))))

;; ------------------------------------------------------------------
;; namespaced-maps — #:foo{...} and #::{...}.
;; ------------------------------------------------------------------

(deftest namespaced-map-prefix-form
  ;; #:a{...} qualifies bare keys with ns "a"; preserves already-
  ;; namespaced keys; preserves non-keyword keys; :_/d becomes :d.
  (is (= {1 nil, :a/b nil, :b/c nil, :d nil}
         (read-string "#:a{1 nil, :b nil, :b/c nil, :_/d nil}")))
  (is (= {1 nil, :a/b nil, :b/c nil, :d nil}
         (read-string "#:a {1 nil, :b nil, :b/c nil, :_/d nil}"))))

(deftest namespaced-map-prefix-symbol-keys
  ;; Bare symbol keys also get qualified; already-qualified preserved.
  (is (= {'a/b 1, 'b/c 2}
         (read-string "#:a{b 1 b/c 2}"))))

(deftest namespaced-map-auto-current-ns
  ;; #::{...} uses the current namespace as prefix.
  (let [m (read-string "#::{:a 1 :b/c 2 :_/d 3}")]
    (is (= 1 (get m (keyword (str (ns-name *ns*)) "a"))))
    (is (= 2 (:b/c m)))
    (is (= 3 (:d m)))))

(deftest namespaced-map-auto-alias
  ;; #::s{...} uses the alias `s` (must be aliased to a real ns).
  ;; In stock mino there is no `s` alias, so this must throw.
  (is (thrown? (read-string "#::BOGUS{1 2}"))))

(deftest namespaced-map-pr-str
  ;; A map whose keys share a namespace may print in #:ns{...} form.
  ;; Whatever pr-str produces, it must round-trip via read-string.
  (let [m {:a/b 1 :a/c 2}]
    (is (= m (read-string (pr-str m))))))

;; ------------------------------------------------------------------
;; namespaced-map-errors — invalid forms must throw.
;; ------------------------------------------------------------------

(deftest namespaced-map-error-invalid-token
  (is (thrown? (read-string "#:::"))))

(deftest namespaced-map-error-odd-forms
  (is (thrown? (read-string "#:s{1}"))))

(deftest namespaced-map-error-slash-in-prefix
  (is (thrown? (read-string "#:s/t{1 2}"))))

(deftest namespaced-map-error-unknown-alias
  (is (thrown? (read-string "#::BOGUS{1 2}"))))

(deftest namespaced-map-error-space-after-hash-colon
  ;; "#: s{...}" — space between #: and the ns name is illegal.
  (is (thrown? (read-string "#: s{:a 1}"))))

(deftest namespaced-map-error-duplicate-keys
  (is (thrown? (read-string "#::{:a 1 :a 2}")))
  (is (thrown? (read-string "#::{a 1 a 2}"))))

;; ------------------------------------------------------------------
;; namespaced-map-edn — upstream uses clojure.edn/read-string. mino
;; has no separate edn reader; we assert read-string accepts the
;; same shape (no auto-resolve, just prefix qualification).
;; ------------------------------------------------------------------

(deftest namespaced-map-edn-shape
  (is (= {1 1, :a/b 2, :b/c 3, :d 4}
         (read-string "#:a{1 1, :b 2, :b/c 3, :_/d 4}")))
  (is (= {1 1, :a/b 2, :b/c 3, :d 4}
         (read-string "#:a {1 1, :b 2, :b/c 3, :_/d 4}"))))

;; ------------------------------------------------------------------
;; test-read+string — namespace-related slice. Upstream uses
;; LineNumberingPushbackReader (JVM-only); we keep the read-string
;; round-trip for namespaced forms only.
;; ------------------------------------------------------------------

(deftest read-string-namespaced-keyword-roundtrip
  (is (= :foo/bar (first (read-string "[:foo/bar 100]")))))

#_ ;; JVM-only: read+string returns [form source-text]; no equivalent in mino.
(deftest test-read+string-source-capture
  (let [[r s] (read+string (str->lnpr "[:foo  100]"))]
    (is (= [:foo 100] r))
    (is (= "[:foo  100]" s))))
