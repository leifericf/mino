(require "tests/test")

;; Odds-and-ends core vars: reader placeholders, var-level tests,
;; the Inst protocol, error data representations, printer tables,
;; reader defaults, line/queue seqs, STM sync, and Eduction.

;; --- unquote / unquote-splicing ---

(deftest unquote-vars-exist
  ;; Placeholder vars so ~ and ~@ forms resolve outside syntax-quote.
  (is (some? (resolve 'unquote)))
  (is (some? (resolve 'unquote-splicing))))

;; --- test: run the fn under :test in a var's metadata ---

(def ^{:test (fn [] (= 4 (+ 2 2)))} core-misc-tested-var 1)
(def core-misc-untested-var 2)
(def ^{:test (fn [] (throw (ex-info "test failed" {:why :spec})))}
  core-misc-failing-test-var 3)

(deftest test-runs-var-test-metadata
  (is (= :ok (test #'core-misc-tested-var)))
  (is (= :no-test (test #'core-misc-untested-var))))

(deftest test-propagates-test-fn-throw
  ;; The :test fn's own error flows out of (test v) unwrapped.
  (let [e (try (test #'core-misc-failing-test-var) (catch e e))]
    (is (= {:why :spec} (ex-data e)))
    (is (= "test failed" (ex-message e)))))

;; --- Inst protocol / inst-ms* ---

(deftest inst-protocol-satisfied-by-insts
  (is (true? (satisfies? Inst #inst "2026-05-21T00:00:00Z")))
  (is (false? (satisfies? Inst "2026-05-21T00:00:00Z"))))

(deftest inst-ms-star-epoch-millis
  (is (= 0 (inst-ms* #inst "1970-01-01T00:00:00Z")))
  (is (= 946684800000 (inst-ms* #inst "2000-01-01T00:00:00Z")))
  (let [v #inst "2026-05-21T12:00:00Z"]
    (is (= (inst-ms v) (inst-ms* v)))))

;; --- Throwable->map ---

(deftest throwable->map-shape
  (let [e (try (throw (ex-info "boom" {:k 1})) (catch e e))
        m (Throwable->map e)]
    (is (map? m))
    (is (= "boom" (:cause m)))
    (is (= {:k 1} (:data m)))
    (is (vector? (:via m)))
    (is (pos? (count (:via m))))
    (is (every? map? (:via m)))
    (is (= "boom" (:message (first (:via m)))))
    (is (vector? (:trace m)))))

(deftest throwable->map-without-data
  (let [e (try (throw (ex-info "plain" nil)) (catch e e))
        m (Throwable->map e)]
    (is (= "plain" (:cause m)))
    (is (nil? (:data m)))
    (is (= "plain" (:message (first (:via m)))))))

;; --- char-escape-string / char-name-string ---

(deftest char-escape-string-table
  (is (= "\\n" (char-escape-string \newline)))
  (is (= "\\t" (char-escape-string \tab)))
  (is (= "\\r" (char-escape-string \return)))
  (is (= "\\\"" (char-escape-string \")))
  (is (= "\\\\" (char-escape-string \\)))
  (is (= "\\f" (char-escape-string \formfeed)))
  (is (= "\\b" (char-escape-string \backspace)))
  (is (nil? (char-escape-string \a)))
  (is (nil? (char-escape-string \space))))

(deftest char-name-string-table
  (is (= "newline" (char-name-string \newline)))
  (is (= "tab" (char-name-string \tab)))
  (is (= "space" (char-name-string \space)))
  (is (= "backspace" (char-name-string \backspace)))
  (is (= "formfeed" (char-name-string \formfeed)))
  (is (= "return" (char-name-string \return)))
  (is (nil? (char-name-string \a))))

;; --- default-data-readers ---

(deftest default-data-readers-map
  ;; The reader supports #inst and #uuid literals, so both tags get
  ;; default readers.
  (is (map? default-data-readers))
  (is (contains? default-data-readers 'inst))
  (is (contains? default-data-readers 'uuid)))

;; --- *repl* ---

(deftest repl-var-default
  (is (some? (resolve '*repl*)))
  (is (not *repl*)))

;; --- read+string ---

(deftest read+string-returns-form-and-text
  (let [[form s] (read+string "(+ 1 2)")]
    (is (= '(+ 1 2) form))
    (is (= "(+ 1 2)" s))))

(deftest read+string-consumes-only-one-form
  (let [[form s] (read+string "[1 2] tail")]
    (is (= [1 2] form))
    (is (= "[1 2]" s))))

(deftest read+string-trims-surrounding-whitespace
  (let [[form s] (read+string "   :kw")]
    (is (= :kw form))
    (is (= ":kw" s))))

(deftest read+string-keeps-interior-text
  (let [[form s] (read+string "{:a 1, :b 2}")]
    (is (= {:a 1 :b 2} form))
    (is (= "{:a 1, :b 2}" s))))

;; --- line-seq ---
;;
;; mino's reader-like handle is the string-cursor atom that *in* /
;; with-in-str use; read-line consumes lines from it, so line-seq
;; takes the same handle.

(def core-misc-scratch-file "/tmp/mino-core-misc-scratch.txt")

(deftest line-seq-from-string-cursor
  (is (= ["a" "b" "c"] (into [] (line-seq (atom "a\nb\nc")))))
  ;; trailing newline does not add a phantom empty line
  (is (= ["one" "two"] (into [] (line-seq (atom "one\ntwo\n")))))
  ;; a lone separator is a single empty line
  (is (= [""] (into [] (line-seq (atom "\n")))))
  ;; exhausted source yields nil, not an empty seq object surprise
  (is (nil? (line-seq (atom "")))))

(deftest line-seq-is-seqable-lazily
  (let [ls (line-seq (atom "1\n2\n3"))]
    (is (= "1" (first ls)))
    (is (= ["1" "2" "3"] (into [] ls)))))

(deftest line-seq-composes-with-slurp
  (spit core-misc-scratch-file "l1\nl2\nl3\n")
  (is (= ["l1" "l2" "l3"]
         (into [] (line-seq (atom (slurp core-misc-scratch-file)))))))

;; --- seque ---

(deftest seque-preserves-elements-and-order
  (is (= [1 2 3 4 5] (into [] (seque [1 2 3 4 5]))))
  (is (= [0 1 2 3 4 5 6 7 8 9] (into [] (seque 3 (range 10)))))
  (is (= [2 3 4] (into [] (seque 2 (map inc [1 2 3])))))
  (is (nil? (seq (seque [])))))

(deftest seque-carries-nil-elements
  (is (= [nil 1 nil] (into [] (seque [nil 1 nil])))))

;; --- sync ---

(deftest sync-behaves-like-dosync
  (let [r (ref 0)]
    (sync nil (ref-set r 5))
    (is (= 5 (deref r))))
  (let [r (ref 10)]
    (is (= 16 (sync nil (alter r + 1 2 3))))
    (is (= 16 (deref r)))))

(deftest sync-coordinates-multiple-refs
  (let [a (ref 1)
        b (ref 2)]
    (sync nil
      (alter a + 10)
      (alter b + 20))
    (is (= 11 (deref a)))
    (is (= 22 (deref b)))))

;; --- xml-seq ---

(deftest xml-seq-traversal-order
  (let [leaf-d {:tag :d :content []}
        node-c {:tag :c :content [leaf-d]}
        node-b {:tag :b :content ["x"]}
        root   {:tag :a :content [node-b node-c]}]
    (is (= [root node-b "x" node-c leaf-d]
           (into [] (xml-seq root))))))

(deftest xml-seq-string-root-is-leaf
  (is (= ["just text"] (into [] (xml-seq "just text")))))

;; --- print-simple ---
;;
;; The writer argument is mino's string-collecting atom (the *out*
;; model). print-simple writes the plain string form, so strings
;; come out unquoted.

(deftest print-simple-writes-plainly
  (let [w (atom "")]
    (print-simple "hi" w)
    (is (= "hi" (deref w))))
  (let [w (atom "")]
    (print-simple :kw w)
    (is (= ":kw" (deref w))))
  (let [w (atom "")]
    (print-simple 42 w)
    (is (= "42" (deref w))))
  (let [w (atom "")]
    (print-simple [1 2 3] w)
    (is (= "[1 2 3]" (deref w)))))

;; --- ->Eduction ---

(deftest ->eduction-matches-eduction
  (is (= [2 3] (into [] (->Eduction (map inc) [1 2]))))
  (is (= (into [] (eduction (map inc) [1 2 3]))
         (into [] (->Eduction (map inc) [1 2 3]))))
  (is (= (seq (eduction (filter odd?) (range 6)))
         (seq (->Eduction (filter odd?) (range 6)))))
  (is (= (type (eduction (map inc) [1 2]))
         (type (->Eduction (map inc) [1 2])))))

(deftest ->eduction-is-reducible
  (is (= 9 (reduce + 0 (->Eduction (map inc) [1 2 3])))))

(run-tests-and-exit)
