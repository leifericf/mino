(require "tests/test")

;; Special float tokens

(deftest special-float-inf
  (is (= ##Inf ##Inf))
  (is (= ##-Inf ##-Inf))
  (is (infinite? ##Inf))
  (is (infinite? ##-Inf))
  (is (not (infinite? 1.0)))
  (is (not (infinite? 0)))
  (is (= ##Inf (+ 1 ##Inf)))
  (is (= ##-Inf (- ##-Inf 1))))

(deftest special-float-nan
  (is (NaN? ##NaN))
  (is (not (NaN? 1.0)))
  (is (not (NaN? 0)))
  (is (not (= ##NaN ##NaN)))
  (is (NaN? (- ##Inf ##Inf))))

(deftest special-float-print
  (is (= "##Inf" (pr-str ##Inf)))
  (is (= "##-Inf" (pr-str ##-Inf)))
  (is (= "##NaN" (pr-str ##NaN))))

(deftest scientific-notation-uses-uppercase-e
  ;; JVM Double.toString prints scientific notation with uppercase E.
  ;; Portable Clojure code (and the ClojureDocs diff probe) expects the
  ;; same form. mino used to print lowercase e from snprintf's %e
  ;; conversion, which is a silent canon divergence.
  (is (= "1.0E-15" (pr-str 1.0e-15)))
  (is (= "1.0E20"  (pr-str 1e20)))
  (is (= "-1.4210854715202004E-14"
         (pr-str (- 100.0 100.00000000000001)))))

(deftest float32-print-uses-32-bit-precision
  ;; float32 (mino :float32) values must print with the shortest
  ;; representation that round-trips through Float.parseFloat -- not
  ;; with the full double precision their backing storage happens to
  ;; have after the widen-for-arith path. JVM Float.toString does
  ;; exactly this.
  (is (= "1.5"       (pr-str (float 1.5))))
  (is (= "0.1"       (pr-str (float 0.1))))
  (is (= "1.1111112" (pr-str (float 1.111111111111111M))))
  ;; Double-precision values keep their 17-digit form.
  (is (= "0.1"                (pr-str 0.1)))
  (is (= "1.1111111111111112" (pr-str 1.1111111111111112))))

(deftest special-float-read-string
  (is (= ##Inf (read-string "##Inf")))
  (is (= ##-Inf (read-string "##-Inf")))
  (is (NaN? (read-string "##NaN"))))

;; Float division by zero

(deftest float-div-by-zero
  (is (= ##Inf (/ 1.0 0)))
  (is (= ##Inf (/ 1.0 0.0)))
  (is (= ##-Inf (/ -1.0 0)))
  (is (= ##-Inf (/ -1.0 0.0)))
  (is (NaN? (/ 0.0 0.0)))
  (is (thrown? (/ 1 0)))
  (is (thrown? (/ 0 0))))

;; Character literals

(deftest char-literal-named
  (is (= \space \space))
  (is (= \newline \newline))
  (is (= \tab \tab))
  (is (= \return \return))
  (is (= " " (str \space)))
  (is (= "\n" (str \newline)))
  (is (= "\t" (str \tab)))
  (is (= "\r" (str \return))))

(deftest char-literal-single
  (is (= \A \A))
  (is (char? \A))
  (is (char? \z))
  (is (char? \0))
  (is (char? \9))
  (is (char? \!))
  (is (= 65 (int \A)))
  (is (= "A" (str \A))))

(deftest char-literal-is-char
  (is (= :char (type \A)))
  (is (= :char (type \space)))
  (is (char? \A))
  (is (not (char? "A")))
  (is (not (= \A "A"))))

;; Hex integer literals

(deftest hex-integers
  (is (= 255 0xFF))
  (is (= 0 0x0))
  (is (= 16 0x10))
  (is (= -1 -0x1))
  (is (= 4095 0xFFF))
  (is (= :int (type 0xFF))))

;; Ratio literals

(deftest ratio-literals
  ;; Ratio literals produce real ratios (Clojure dialect); use `==` to
  ;; compare numerically across the tower or coerce explicitly with double.
  (is (== 0.5 1/2))
  (is (= 1/2 1/2))
  (is (= 2 6/3))
  (is (= -3/4 -3/4))
  (is (== -0.75 -3/4))
  (is (= 1 5/5))
  (is (= -1 -10/10))
  (is (= :int (type 6/3)))
  (is (= :ratio (type 1/3)))
  (is (= 0.5 (double 1/2))))

;; Bigint N and bigdec M suffixes

(deftest bigint-suffix
  (is (= 42 42N))
  (is (= 0 0N))
  (is (= -1 -1N))
  (is (= :bigint (type 42N)))
  (is (bigint? 42N))
  (is (not (bigint? 42)))
  ;; The arbitrary-precision nature of 1N: magnitudes beyond long long
  ;; are preserved exactly rather than overflowing.
  (is (bigint? 99999999999999999999999N))
  (is (= 99999999999999999999999N
         (read-string (pr-str 99999999999999999999999N)))))

(deftest bigdec-suffix
  ;; M-suffixed literals are real bigdecs (Clojure dialect); use `==`
  ;; for cross-tier numeric equality with floats.
  (is (== 1.5 1.5M))
  (is (= 1.5M 1.5M))
  (is (== 0.0 0.0M))
  (is (== -2.5 -2.5M))
  (is (= :bigdec (type 1.5M)))
  (is (decimal? 1.5M)))

(deftest slash-keyword-literal
  ;; `:/` is the slash keyword (its name is "/"), a real Clojure
  ;; literal -- used e.g. by HoneySQL to represent the SQL / operator
  ;; alongside the other arithmetic-op keywords. The reader used to
  ;; reject it because the generic trailing-slash check fired without
  ;; verifying there was actual content before the slash.
  (is (keyword? :/))
  (is (= "/" (name :/)))
  (is (nil? (namespace :/)))
  (is (= (keyword "/") :/))
  (is (= :/ (read-string ":/")))
  (is (= :/ (read-string (pr-str :/)))))

(deftest trailing-slash-keyword-still-rejected
  ;; `:foo/` is genuinely malformed: name part is empty after the
  ;; slash. The reader must still reject it.
  (is (thrown? (read-string ":foo/"))))

(deftest non-empty-map-literal-uses-local-bindings
  ;; A defn body whose value is a literal map with non-self-evaluating
  ;; values (here: a symbol bound by the surrounding let) must build a
  ;; fresh map at each invocation, with the locals resolved to their
  ;; current values. The pattern shows up in macro bodies that emit
  ;; per-call result maps -- e.g., `defprotocol`'s method-meta builds
  ;; `{:mname mname :params params :dsym ...}` per method.
  (defn build-record__mt [n]
    (let [doubled (* n 2)]
      {:val n :doubled doubled :inc (+ n 1)}))
  (is (= {:val 3 :doubled 6 :inc 4} (build-record__mt 3)))
  (is (= {:val 7 :doubled 14 :inc 8} (build-record__mt 7)))
  ;; Each call returns a distinct map -- not a shared const-pool entry.
  (is (not (identical? (build-record__mt 1) (build-record__mt 1)))))

(deftest non-empty-set-literal-uses-local-bindings
  ;; Same as the map case but for set literals.
  (defn build-set__mt [a b]
    #{a b (+ a b)})
  (is (= #{1 2 3} (build-set__mt 1 2)))
  (is (= #{10 20 30} (build-set__mt 10 20)))
  (is (not (identical? (build-set__mt 1 2) (build-set__mt 1 2)))))

(deftest nested-literals-inside-macro-template
  ;; The original regression: a defmacro whose body uses a literal map
  ;; with symbol values from a let in the macro body. `defprotocol`
  ;; broke this way during construction-lane experiments. Verifies the
  ;; map / vec / set literal lowerings keep let scopes intact.
  (defmacro mk-pair__mt [a b]
    (let [tag :pair
          payload `(vector ~a ~b)]
      `(hash-map :tag ~tag :payload ~payload)))
  (is (= {:tag :pair :payload [1 2]} (mk-pair__mt 1 2)))
  (is (= {:tag :pair :payload [:x :y]} (mk-pair__mt :x :y))))
