(require "tests/test")
(require '[mino.template :as tpl])

;; mino.template: the Selmer-shaped renderer. Interpolation, dotted
;; lookup, each/if blocks, the upper/lower/join filter set, custom
;; delimiter opts, and compile-to-data plus render-many. Unknown
;; variables render as the empty string (Selmer's missing-value
;; semantics); if treats nil, false, the empty string, and empty
;; collections as falsy (the pinned Selmer choice, named in the
;; tests below); interpolation does not escape (Selmer does not
;; escape by default).

(defn- tpl-err
  "ex-data of calling f, or :no-throw when it succeeds."
  [f]
  (try (f) :no-throw
       (catch e (ex-data e))))

(defn- tpl-parse-err-at
  "render on t must throw :template/parse with reason and the
  1-based line/col of the offending tag."
  [t reason line col]
  (let [d (tpl-err #(tpl/render t {}))]
    (and (map? d)
         (= :template/parse (:kind d))
         (= reason (:reason d))
         (= line (get-in d [:location :line]))
         (= col (get-in d [:location :col])))))

;;; Interpolation and lookup

(deftest tpl-basic-interpolation
  (is (= "Hello World" (tpl/render "Hello {{name}}" {:name "World"})))
  (is (= "Hi Alice" (tpl/render "Hi {{ user.name }}" {:user {:name "Alice"}})))
  (is (= "no tags" (tpl/render "no tags" {:name "World"})))
  (is (= "" (tpl/render "" {:name "World"}))))

(deftest tpl-nested-lookup
  (is (= "deep" (tpl/render "{{a.b.c}}" {:a {:b {:c "deep"}}})))
  (is (= "Bob" (tpl/render "{{user.name}}" {:user {"name" "Bob"}})))
  (is (= "kw" (tpl/render "{{a}}" {:a "kw" "a" "str"}))
      "keyword keys are tried before string keys")
  (is (= "ab" (tpl/render "a{{x.y}}b" {:x {}}))
      "a missing path segment renders nothing"))

(deftest tpl-unknown-var-is-empty-string
  ;; Selmer renders missing values as the empty string.
  (is (= "ab" (tpl/render "a{{missing}}b" {:name "World"})))
  (is (= "" (tpl/render "{{x}}" {:x nil})))
  (is (= "[]" (tpl/render "[{{missing}}]" nil))))

(deftest tpl-non-string-values
  (is (= "n=42 ok=true k=:foo f=1.5"
         (tpl/render "n={{n}} ok={{ok}} k={{k}} f={{f}}"
                     {:n 42 :ok true :k :foo :f 1.5}))))

;;; each

(deftest tpl-each-over-vector-of-maps
  (is (= "A;B;" (tpl/render "{{#each users}}{{name}};{{/each}}"
                            {:users [{:name "A"} {:name "B"}]}))))

(deftest tpl-each-current-element
  (is (= "[1][2][3]" (tpl/render "{{#each xs}}[{{.}}]{{/each}}" {:xs [1 2 3]})))
  (is (= "(A)(B)" (tpl/render "{{#each xs}}({{.|upper}}){{/each}}"
                              {:xs ["a" "b"]}))))

(deftest tpl-each-over-seq-and-set
  (is (= "12" (tpl/render "{{#each xs}}{{.}}{{/each}}" {:xs (seq [1 2])})))
  (is (= "12" (apply str (sort (tpl/render "{{#each xs}}{{.}}{{/each}}"
                                           {:xs #{1 2}}))))
      "set iteration order is not pinned, only membership"))

(deftest tpl-each-over-map-iterates-vals
  (is (= "(v)" (tpl/render "{{#each m}}({{.}}){{/each}}" {:m {:only "v"}}))))

(deftest tpl-each-nested
  (is (= "12|3|" (tpl/render "{{#each rows}}{{#each cells}}{{.}}{{/each}}|{{/each}}"
                             {:rows [{:cells [1 2]} {:cells [3]}]}))))

(deftest tpl-each-scope-fallthrough
  (is (= "<a:acme><b:acme>"
         (tpl/render "{{#each items}}<{{name}}:{{org}}>{{/each}}"
                     {:org "acme" :items [{:name "a"} {:name "b"}]})))
  (is (= "x" (tpl/render "{{#each items}}{{name}}{{/each}}"
                         {:name "outer" :items [{:name "x"}]}))
      "the element shadows the outer context")
  (is (= "no" (tpl/render "{{#each items}}{{#if on}}yes{{else}}no{{/if}}{{/each}}"
                          {:on true :items [{:on false}]}))
      "a found false does not fall through to the outer scope, only nil does"))

(deftest tpl-each-empty-and-missing-render-nothing
  (is (= "" (tpl/render "{{#each xs}}X{{/each}}" {:xs []})))
  (is (= "ab" (tpl/render "a{{#each xs}}X{{/each}}b" {:other 1}))))

(deftest tpl-each-rejects-non-collections
  (is (= :template/each (:kind (tpl-err #(tpl/render "{{#each xs}}X{{/each}}" {:xs 5})))))
  (is (= :template/each (:kind (tpl-err #(tpl/render "{{#each xs}}X{{/each}}" {:xs "ab"}))))))

;;; if

(deftest tpl-if-branches
  (is (= "yes" (tpl/render "{{#if on}}yes{{else}}no{{/if}}" {:on true})))
  (is (= "no" (tpl/render "{{#if on}}yes{{else}}no{{/if}}" {:on false})))
  (is (= "no" (tpl/render "{{#if on}}yes{{else}}no{{/if}}" {})))
  (is (= "" (tpl/render "{{#if on}}yes{{/if}}" {:on nil})))
  (is (= "root" (tpl/render "{{#if user.admin}}root{{else}}guest{{/if}}"
                            {:user {:admin true}})))
  (is (= "nested" (tpl/render "{{#if a}}{{#if b}}both{{else}}nested{{/if}}{{/if}}"
                              {:a 1 :b nil}))))

(deftest tpl-if-empty-collection-renders-else-pinned
  ;; Selmer's if treats an empty collection as falsy, so the else
  ;; branch renders; this subset pins that choice here.
  (is (= "none" (tpl/render "{{#if xs}}some{{else}}none{{/if}}" {:xs []})))
  (is (= "none" (tpl/render "{{#if m}}some{{else}}none{{/if}}" {:m {}})))
  (is (= "some" (tpl/render "{{#if xs}}some{{else}}none{{/if}}" {:xs [1]}))))

(deftest tpl-if-truthiness-corners
  (is (= "yes" (tpl/render "{{#if n}}yes{{else}}no{{/if}}" {:n 0}))
      "0 is a value, so it is truthy")
  (is (= "no" (tpl/render "{{#if s}}yes{{else}}no{{/if}}" {:s ""}))
      "the empty string is falsy, matching the empty-collection pin")
  (is (= "yes" (tpl/render "{{#if k}}yes{{else}}no{{/if}}" {:k :x}))))

;;; Filters

(deftest tpl-filters-upper-lower
  (is (= "ABC" (tpl/render "{{x|upper}}" {:x "abc"})))
  (is (= "abc" (tpl/render "{{x|lower}}" {:x "ABC"})))
  (is (= "42" (tpl/render "{{n|upper}}" {:n 42}))
      "filters stringify before applying"))

(deftest tpl-filter-chains
  (is (= "abc" (tpl/render "{{x|upper|lower}}" {:x "AbC"})))
  (is (= "AB" (tpl/render "{{ user.name | upper }}" {:user {:name "ab"}}))
      "spaces around the filter tag are trimmed"))

(deftest tpl-filter-join
  (is (= "123" (tpl/render "{{xs|join}}" {:xs [1 2 3]}))
      "no argument joins with the empty string, clojure.string/join's default")
  (is (= "1, 2, 3" (tpl/render "{{xs|join:\", \"}}" {:xs [1 2 3]})))
  (is (= "1-2-3" (tpl/render "{{xs|join:-}}" {:xs [1 2 3]}))
      "an unquoted argument is the separator verbatim")
  (is (= "a" (tpl/render "{{xs|join:\",\"}}" {:xs ["a"]})))
  (is (= "a:b" (tpl/render "{{xs|join:\":\"}}" {:xs ["a" "b"]}))
      "only the first colon splits the filter from its argument")
  (is (= "1-\"2" (tpl/render "{{xs|join:-\"}}" {:xs [1 2]}))
      "an unbalanced quote in an argument stays verbatim")
  (is (= "" (tpl/render "{{missing|join:\",\"}}" {:xs [1]}))
      "join over a missing value renders nothing"))

(deftest tpl-filter-join-rejects-scalars
  (let [d (tpl-err #(tpl/render "{{x|join:\",\"}}" {:x 5}))]
    (is (and (map? d) (= :template/filter (:kind d)) (= :not-a-collection (:reason d))))))

(deftest tpl-unknown-filter-throws
  (let [d (tpl-err #(tpl/render "{{x|title}}" {:x "ab"}))]
    (is (and (map? d) (= :template/filter (:kind d)) (= "title" (:filter d)))))
  (is (= :template/filter (:kind (tpl-err #(tpl/compile "{{x|}}"))))))

(deftest tpl-filter-arity-errors
  (is (= :template/filter
         (:kind (tpl-err #(tpl/render "{{x|upper:\",\"}}" {:x "ab"})))))
  (is (= :template/filter
         (:kind (tpl-err #(tpl/render "{{x|lower:\",\"}}" {:x "ab"}))))))

;;; Delimiter opts

(deftest tpl-custom-delimiters
  (is (= "Hello W" (tpl/render "Hello ((name))" {:name "W"}
                               {:tag-open \( :tag-close \)}))
      "a single-character delimiter still doubles, Selmer's tag model")
  (is (= "W" (tpl/render "<<name>>" {:name "W"} {:tag-open \< :tag-close \>})))
  (is (= "yes" (tpl/render "<<#if on>>yes<<else>>no<</if>>" {:on 1}
                           {:tag-open \< :tag-close \>}))))

(deftest tpl-custom-filter-tag
  (is (= "ABC" (tpl/render "{{x#upper}}" {:x "abc"} {:filter-tag \#}))))

(deftest tpl-opts-total-and-defaulted
  (is (= "AB" (tpl/render "{{x|upper}}" {:x "ab"} {}))
      "an empty opts map keeps every default")
  (is (= "1{{x}}" (tpl/render "((n)){{x}}" {:n 1} {:tag-open \( :tag-close \)}))
      "the default delimiters are literal text once overridden"))

;;; compile and render-many

(deftest tpl-compile-is-plain-data
  (let [c (tpl/compile "a{{#each xs}}<{{.}}>{{/each}}b")]
    (is (vector? c))
    (is (every? (complement fn?) (tree-seq coll? seq c)))
    (is (some string? c) "literal text stays plain strings in the AST")))

(deftest tpl-render-many-reuses-the-compiled-ast
  (let [c (tpl/compile "n={{n}}")]
    (is (= "n=1" (tpl/render-many c {:n 1})))
    (is (= "n=2" (tpl/render-many c {:n 2})))))

(deftest tpl-render-composes-compile-and-render-many
  (is (= (tpl/render "{{#each xs}}[{{.}}]{{/each}}" {:xs [1 2]})
         (tpl/render-many (tpl/compile "{{#each xs}}[{{.}}]{{/each}}" nil) {:xs [1 2]})))
  (is (= "x" (tpl/render-many (tpl/compile "((a))" {:tag-open \( :tag-close \)})
                              {"a" "x"}))
      "compile bakes the delimiter opts into the AST"))

(deftest tpl-render-many-validates-input
  (is (= :template/opts (:kind (tpl-err #(tpl/render-many "not compiled" {}))))))

;;; Parse errors

(deftest tpl-parse-error-locations
  (is (tpl-parse-err-at "{{x" :unterminated-tag 1 1))
  (is (tpl-parse-err-at "a\n{{x" :unterminated-tag 2 1))
  (is (tpl-parse-err-at "{{}}" :empty-tag 1 1))
  (is (tpl-parse-err-at "{{a b}}" :bad-path 1 1))
  (is (tpl-parse-err-at "{{{x}}}" :bad-path 1 1))
  (is (tpl-parse-err-at "{{a.}}" :bad-path 1 1))
  (is (tpl-parse-err-at "{{#for x}}a{{/for}}" :unknown-block 1 1))
  (is (tpl-parse-err-at "abc{{/if}}" :unmatched-close 1 4))
  (is (tpl-parse-err-at "{{#if x}}a{{/each}}" :mismatched-close 1 11))
  (is (tpl-parse-err-at "{{#if x}}abc" :unclosed-block 1 1))
  (is (tpl-parse-err-at "x{{else}}" :else-outside-if 1 2))
  (is (tpl-parse-err-at "{{#each xs}}{{else}}{{/each}}" :else-not-allowed 1 13))
  (is (tpl-parse-err-at "{{#if x}}1{{else}}2{{else}}3{{/if}}" :duplicate-else 1 20))
  (is (tpl-parse-err-at "{{#if}}x{{/if}}" :bad-block 1 1))
  (is (tpl-parse-err-at "{{#if a b}}x{{/if}}" :bad-block 1 1))
  (is (tpl-parse-err-at "{{#each}}x{{/each}}" :bad-block 1 1)))

(deftest tpl-unknown-filter-also-from-compile
  (is (= :template/filter (:kind (tpl-err #(tpl/compile "{{x|nope}}"))))))

;;; Argument and opts validation

(deftest tpl-argument-validation
  (is (= :template/opts (:kind (tpl-err #(tpl/render 5 {})))))
  (is (= :template/opts (:kind (tpl-err #(tpl/render "x" 5)))))
  (is (= :template/opts (:kind (tpl-err #(tpl/render "x" {} {:tag-open "ab"})))))
  (is (= :template/opts (:kind (tpl-err #(tpl/render "x" {} {:tag-open 5})))))
  (is (= :template/opts (:kind (tpl-err #(tpl/render "x" {} {:filter-tag "||"})))))
  (is (= :template/opts (:kind (tpl-err #(tpl/compile 5)))))
  (is (= "ok" (tpl/render "{{a}}" {"a" "ok"} nil))
      "a nil opts map is accepted"))

;;; render-file

(deftest tpl-render-file
  (let [tmp (str (or (getenv "TMPDIR") "/tmp") "/mino_template_greet.txt")]
    (spit tmp "Hello {{name}}!\n")
    (is (= "Hello World!\n" (tpl/render-file tmp {:name "World"})))
    (is (= "Hello A!\n" (tpl/render-file tmp {:name "A"} {})))
    (is (thrown-with-msg? #"slurp"
                          (tpl/render-file "/mino/no/such_template_file.txt" {}))
        "a missing file propagates the slurp error")))

(run-tests-and-exit)
