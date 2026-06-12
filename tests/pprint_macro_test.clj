(require "tests/test")
(require '[clojure.pprint :as pp])

;; Spec-first tests for the formatter / formatter-out macro conversion.
;;
;; The macro-flag assertions (ppmt-formatter-is-macro,
;; ppmt-formatter-out-is-macro) are RED today: both vars are currently
;; plain defns.  They must turn GREEN once the impl agent converts them
;; to macros backed by private -impl fns.
;;
;; The behavior-regression assertions are GREEN today and must stay
;; GREEN through and after the conversion.

;; ---------------------------------------------------------------------------
;; Canonical macro-flag probe (establishes the :macro assertion pattern)
;; ---------------------------------------------------------------------------

(deftest ppmt-known-macro-carries-flag
  ;; clojure.core/when is a macro: its var must have :macro true.
  ;; This test acts as a sanity check that the :macro metadata path works
  ;; before asserting on the vars under conversion.
  (is (true? (:macro (meta (resolve 'clojure.core/when)))))
  ;; clojure.test/deftest is a lib macro — also carries the flag.
  (is (true? (:macro (meta (resolve 'clojure.test/deftest))))))

;; ---------------------------------------------------------------------------
;; Macro-flag assertions — RED until the impl lands
;; ---------------------------------------------------------------------------

(deftest ppmt-formatter-is-macro
  ;; After conversion: (formatter ...) must be a macro, not a function.
  ;; The var's metadata must carry :macro true.
  (is (true? (:macro (meta (resolve 'clojure.pprint/formatter))))
      "clojure.pprint/formatter must be a macro (currently a defn — expected failure)"))

(deftest ppmt-formatter-out-is-macro
  ;; After conversion: (formatter-out ...) must also be a macro.
  (is (true? (:macro (meta (resolve 'clojure.pprint/formatter-out))))
      "clojure.pprint/formatter-out must be a macro (currently a defn — expected failure)"))

;; ---------------------------------------------------------------------------
;; Behavior regressions — GREEN today, must stay GREEN after conversion
;; ---------------------------------------------------------------------------

(deftest ppmt-formatter-nil-stream-returns-string
  ;; The returned fn, called with nil as the writer, must return a string.
  (let [f (pp/formatter "~a")]
    (is (fn? f))
    (is (= "42" (f nil 42)))))

(deftest ppmt-formatter-nil-stream-multi-arg
  ;; Multiple args work correctly with nil stream.
  (let [f (pp/formatter "~a ~a")]
    (is (= "x y" (f nil "x" "y")))))

(deftest ppmt-formatter-nil-42-shape
  ;; The usage shape from the spec: ((formatter "~a") nil 42)
  (is (= "42" ((pp/formatter "~a") nil 42))))

(deftest ppmt-formatter-true-stream-writes-to-out
  ;; With true as the writer, the returned fn writes to *out* and returns nil.
  (let [f   (pp/formatter "~a")
        ret (atom ::unset)
        out (with-out-str (reset! ret (f true 99)))]
    (is (= "99" out))
    (is (nil? @ret))))

(deftest ppmt-formatter-various-directives
  ;; Directives beyond ~a must survive the macro conversion.
  (is (= "ff"  ((pp/formatter "~x") nil 255)))
  (is (= "377" ((pp/formatter "~o") nil 255)))
  (is (= "+7"  ((pp/formatter "~@d") nil 7))))

(deftest ppmt-formatter-out-writes-to-dynamic-out
  ;; formatter-out's returned fn always writes to *out*, returns nil.
  (let [f   (pp/formatter-out "~a")
        ret (atom ::unset)
        out (with-out-str (reset! ret (f 42)))]
    (is (= "42" out))
    (is (nil? @ret))))

(deftest ppmt-formatter-out-multi-arg
  ;; formatter-out with multiple args.
  (let [f   (pp/formatter-out "~a+~a")
        out (with-out-str (f 1 2))]
    (is (= "1+2" out))))

(deftest ppmt-formatter-out-respects-bound-out
  ;; formatter-out must write to the dynamically bound *out*,
  ;; not a captured writer from definition time.
  (let [f    (pp/formatter-out "~a")
        out1 (with-out-str (f "first"))
        out2 (with-out-str (f "second"))]
    (is (= "first"  out1))
    (is (= "second" out2))))

(run-tests-and-exit)
