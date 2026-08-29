(require "tests/test")
(require '[mino.env :as env])
(require '[clojure.string :as str])

;; dotenv support: parse-dotenv is pure text -> plain string map over
;; the common dotenv conventions (docker-compose / ruby-dotenv shape:
;; KEY=VALUE, export prefix, # comments, quotes with double-quote-only
;; escapes). load-env reads a file via slurp and installs an overlay
;; that mino.env/getenv reads before the process environment; the
;; process env itself is never mutated.
;;
;; Golden fixtures live in tests/fixtures/env/ and are never named
;; .env or .env.* (secret-deny policy). basic.env and quoted.env use
;; LF; edge.env carries a UTF-8 BOM and CRLF endings.

(def ^:private fx "tests/fixtures/env/")

(def ^:private basic-golden
  {"HOST" "localhost"
   "PORT" "8080"
   "DATABASE_URL" "postgres://localhost:5432/mydb"
   "EMPTY" ""
   "GREETING" "hello world"
   "HASHED" "value#not-a-comment"
   "SPACED" "padded value"
   "URL" "https://example.com/x?a=1&b=2"})

(def ^:private quoted-golden
  {"DQ" "line1\nline2\tend"
   "DQ_ESC" "back\\slash \"quoted\" and \rcr"
   "SQ" "literal \\n two words # hash"
   "SQ_EMPTY" ""
   "DQ_EMPTY" ""
   "SQ_IN_DQ" "it's fine"
   "DQ_IN_SQ" "say \"hi\""
   "DQ_TRAIL" "quoted value"
   "DQ_PUNCT" "a=b and # inside"})

(def ^:private edge-golden
  {"EXPORTED" "yes"
   "UNKNOWN_ESC" "keep \\q backslash"
   "INDENTED" "spaced"
   "LAST" "no-trailing-newline"})

(defn- parse-error
  "ex-data of parse-dotenv on s, or :no-throw when it succeeds."
  [s]
  (try (env/parse-dotenv s) :no-throw
       (catch e (ex-data e))))

(defn- parse-kind
  "The :mino/kind on the diagnostic parse-dotenv throws for s, or
  :no-throw when it succeeds."
  [s]
  (try (env/parse-dotenv s) :no-throw
       (catch e (:mino/kind e))))

(deftest parse-dotenv-basic-fixture
  (is (= basic-golden (env/parse-dotenv (slurp (str fx "basic.env"))))))

(deftest parse-dotenv-quoted-fixture
  (is (= quoted-golden (env/parse-dotenv (slurp (str fx "quoted.env"))))))

(deftest parse-dotenv-bom-crlf-export-fixture
  (is (= edge-golden (env/parse-dotenv (slurp (str fx "edge.env"))))))

(deftest parse-dotenv-inline-cases
  (is (= {} (env/parse-dotenv "")))
  (is (= {} (env/parse-dotenv "\n\n   \n# only\n  # indented comment\n")))
  (is (= {"A" "1"} (env/parse-dotenv "A=1")))
  (is (= {"A" "2"} (env/parse-dotenv "A=1\nA=2"))
      "a later duplicate key wins")
  (is (= {"EQ" "a=b=c"} (env/parse-dotenv "EQ=a=b=c"))
      "only the first = splits")
  (is (= {"T" "tabbed"} (env/parse-dotenv "export\tT=tabbed"))
      "export with a tab separator")
  (is (= {"LW" "indented"} (env/parse-dotenv "  LW=indented")))
  (is (= {"TR" "cr at eof"} (env/parse-dotenv "TR=cr at eof\r")))
  (is (= {"C1" ""} (env/parse-dotenv "C1= # only a comment")))
  (is (= {"C2" ""} (env/parse-dotenv "C2=   ")))
  (is (= {"EXPORTS" "1"} (env/parse-dotenv "EXPORTS=1"))
      "export must be followed by whitespace to be a prefix")
  (is (= {"D" "d"} (env/parse-dotenv "export D=d"))))

(deftest parse-dotenv-throws-on-malformed-lines
  (doseq [[s line] [["NOT_AN_ASSIGNMENT" 1]
                    ["A=1\nBROKEN" 2]
                    ["=x" 1]
                    ["9BAD=x" 1]
                    ["BAD KEY=x" 1]
                    ["OPEN=\"unterminated" 1]
                    ["SQ='open" 1]
                    ["A=\"closed\" junk" 1]
                    ["A='closed' junk" 1]]]
    (let [e (parse-error s)]
      (is (map? e) (str "throws a diagnostic for " (pr-str s)))
      (is (= :env/parse (parse-kind s)))
      (is (= line (:line e)))
      (is (string? (:text e))))))

(deftest parse-dotenv-classifies-with-mino-kind
  ;; ADR 37: the thrown diagnostic carries :mino/kind so a classed
  ;; catch dispatches on it, and ex-data still reads the detail.
  (is (= :env/parse (parse-kind "NOT_AN_ASSIGNMENT"))
      "a malformed KEY=VALUE line classifies as :env/parse")
  (is (= :env/parse (parse-kind "OPEN=\"unterminated"))
      "an unterminated quoted value classifies as :env/parse")
  (is (= :caught
         (try (env/parse-dotenv "BAD KEY=x")
              (catch :env/parse _ :caught)))
      "a classed catch on :env/parse dispatches")
  (is (= :env/opts
         (try (env/parse-dotenv :not-a-string) (catch e (:mino/kind e))))
      "a bad argument classifies as :env/opts")
  (let [d (try (env/parse-dotenv "A=1\nBROKEN") (catch e (ex-data e)))]
    (is (= 2 (:line d)) "ex-data still reads the 1-based line")
    (is (string? (:text d)))))

(deftest parse-dotenv-requires-a-string
  (let [e (parse-error :not-a-string)]
    (is (map? e))
    (is (= :env/opts (parse-kind :not-a-string)))))

(deftest parse-dotenv-is-pure
  (let [s "P=one\nQ=two\n"]
    (is (= (env/parse-dotenv s) (env/parse-dotenv s)))))

(deftest load-env-installs-the-getenv-overlay
  ;; Parsing alone never touches the overlay.
  (is (nil? (getenv "MINO_ENV_TEST_UNSET_9Q7"))
      "guard: sentinel absent from the process env")
  (is (= {"GREETING" "parsed only"}
         (env/parse-dotenv "GREETING=parsed only\n")))
  (is (nil? (env/getenv "MINO_ENV_TEST_UNSET_9Q7")))

  ;; A plain load replaces the overlay with the file's map.
  (is (= basic-golden (env/load-env (str fx "basic.env"))))
  (is (= "hello world" (env/getenv "GREETING")))
  (is (= "8080" (env/getenv "PORT")))

  ;; getenv falls back to the process environment.
  (let [sys (getenv "PATH")]
    (is (some? sys) "guard: PATH exists in the process env")
    (is (= sys (env/getenv "PATH"))))

  ;; A file that overrides PATH: the overlay wins, and the process
  ;; environment itself is untouched.
  (let [tmp   (str (or (getenv "TMPDIR") "/tmp")
                   "/mino_env_override.env")
        sys   (getenv "PATH")]
    (spit tmp "PATH=/mino-env-overlay-wins\nGREETING=replaced\nEXTRA=1\n")
    (is (= {"PATH" "/mino-env-overlay-wins"
            "GREETING" "replaced"
            "EXTRA" "1"}
           (env/load-env tmp)))
    (is (= "/mino-env-overlay-wins" (env/getenv "PATH")))
    (is (= sys (getenv "PATH")) "the process env is never mutated")
    (is (= "replaced" (env/getenv "GREETING")))
    (is (nil? (env/getenv "SPACED"))
        "a fresh load replaces, not merges, the overlay"))

  ;; {:merge true} merges over the loaded overlay; the new file wins
  ;; on conflicts, earlier keys survive.
  (is (= basic-golden (env/load-env (str fx "basic.env"))))
  (let [tmp (str (or (getenv "TMPDIR") "/tmp")
                 "/mino_env_override.env")
        merged (env/load-env tmp {:merge true})]
    (is (= "replaced" (env/getenv "GREETING")) "merged file wins")
    (is (= "localhost" (env/getenv "HOST")) "prior overlay survives")
    (is (= "1" (env/getenv "EXTRA")))
    (is (= "localhost" (get merged "HOST")))
    (is (contains? merged "EXTRA"))
    (is (not (contains? merged "MIXED"))
        "the return is the merged overlay, not just the file")))

(deftest load-env-and-getenv-validate-arguments
  (let [k (try (env/getenv :KEY) (catch e (:mino/kind e)))]
    (is (= :env/opts k)))
  (let [k (try (env/load-env (str fx "basic.env") :not-a-map)
               (catch e (:mino/kind e)))]
    (is (= :env/opts k)))
  (let [k (try (env/load-env 42) (catch e (:mino/kind e)))]
    (is (= :env/opts k))))

(deftest load-env-propagates-slurp-errors
  (is (thrown-with-msg? #"slurp"
                        (env/load-env "tests/fixtures/env/no-such-file.env"))))

(defn- env-scale-doc
  "Deterministic ~75KB document: 2000 lines (a large-by-dotenv-
  standards file), every 500th quoted with escapes so both value
  paths run at scale. Transient accumulation; no lazy chains at
  this size."
  []
  (let [acc (transient [])]
    (dotimes [i 2000]
      (conj! acc
             (if (zero? (rem i 500))
               (str "K_" i "=\"quoted \\\\ value " i "\\n\"\n")
               (str "K_" i "=plain value " i " with#hash " i "\n"))))
    (str/join (persistent! acc))))

(def ^:private env-doc (env-scale-doc))
(def ^:private env-doc-size (count env-doc))

(deftest parse-dotenv-scales-within-budget
  ;; parse-dotenv must stay linear in document size (Drive-1 lesson:
  ;; the reader's scaling gate lands with the reader, as an absolute
  ;; budget over a realistic large fixture, never a wall-clock
  ;; ratio). 2000 lines is large by dotenv standards; the hash in
  ;; the plain values is deliberately NOT preceded by whitespace so
  ;; it must survive as part of the value. Bundled-namespace code
  ;; runs on the interpreter floor, so the budget carries headroom
  ;; over the measured ~250ms rather than chasing parity.
  (let [t0   (nano-time)
        m    (env/parse-dotenv env-doc)
        ms   (quot (- (nano-time) t0) 1000000)]
    (is (> env-doc-size 60000) "document must be substantial")
    (is (= 2000 (count m)))
    ;; Spot-check structure so a fast-but-wrong parser cannot pass.
    (is (= "plain value 1 with#hash 1" (get m "K_1")))
    (is (= "quoted \\ value 1500\n" (get m "K_1500")))
    (is (= "plain value 1999 with#hash 1999" (get m "K_1999")))
    (is (< ms 1500) (str "75KB parse took " ms "ms"))))

(run-tests-and-exit)
