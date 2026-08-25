(require "tests/test")
(require '[clojure.string :as str])
(require '[mino.toml :as toml])

;; TOML reading must stay well inside the absolute budget at document
;; scale. The reader is the native single-pass toml-parse prim (ADR
;; 25); the mino-side Clojure reader it replaced was linear but 35x
;; over budget (74 s for 835 KB through interpreter dispatch and
;; per-call regex compiles), which is why the reader went native.
;; The budget is absolute, never a wall-clock ratio (CI-runner
;; lesson), and this file joins the nightly fuzz/stress exclusions.

(defn- toml-package-block
  [i]
  (str
    "[package.pkg-" i "]\n"
    "name = \"pkg-" i "\"\n"
    "version = \"1." (rem i 40) "." (rem i 9) "\"\n"
    "description = \"\"\"\nA package for scaling gates with an é and\\ttab,\nspanning lines.\"\"\"\n"
    "requires-python = \">=3.11\"\n"
    "published = 2024-06-01T08:00:0" (rem i 10) "Z\n"
    "keywords = [\"sample\", \"cli-" i "\", \"testing\"]\n"
    "rating = " (+ 1.0 (* 0.25 (rem i 20))) "\n"
    "downloads = " (* i 12345) "\n"
    "verified = " (if (even? i) "true" "false") "\n"
    "[package.pkg-" i ".scripts]\n"
    "run = \"pkg_" i ":main\"\n"
    "[package.pkg-" i ".metadata]\n"
    "author = \"Jane Doe " i "\"\n"
    "tags = [\"a\", \"b\", \"c\"]\n"))

(defn- toml-build-doc
  "Deterministic megabyte-scale pyproject-shaped document: 2400
  package tables, each with scalar, string, multiline-string, date,
  array, and subtable coverage. Transient accumulation; no lazy
  chains at this size."
  []
  (let [acc (transient [])]
    (dotimes [i 2400]
      (conj! acc (toml-package-block i)))
    (str/join (persistent! acc))))

(def ^:private toml-perf-doc (toml-build-doc))
(def ^:private toml-doc-size (count toml-perf-doc))

(deftest read-one-megabyte-pyproject-shape-within-budget
  (let [t0 (nano-time)
        m  (toml/parse-string toml-perf-doc)
        ms (quot (- (nano-time) t0) 1000000)
        pkgs (get m :package)]
    (is (> toml-doc-size 1000000) "document must be megabyte scale")
    (is (= 2400 (count pkgs)))
    ;; Spot-check structure so a fast-but-wrong reader cannot pass.
    (is (= "pkg-0" (get-in pkgs [(keyword "pkg-0") :name])))
    (is (= "pkg-2399" (get-in pkgs [(keyword "pkg-2399") :name])))
    (is (= (* 2399 12345)
           (get-in pkgs [(keyword "pkg-2399") :downloads])))
    (is (= "2024-06-01T08:00:09Z"
           (get-in pkgs [(keyword "pkg-2399") :published])))
    (is (str/starts-with?
          (get-in pkgs [(keyword "pkg-2399") :description])
          "A package for scaling gates"))
    (is (= ["a" "b" "c"]
           (get-in pkgs [(keyword "pkg-2399") :metadata :tags])))
    (is (= "pkg_2399:main"
           (get-in pkgs [(keyword "pkg-2399") :scripts :run])))
    (is (< ms 12000) (str "megabyte parse took " ms "ms"))))

(deftest parse-is-deterministic-at-scale
  (let [m  (toml/parse-string toml-perf-doc)
        m2 (toml/parse-string toml-perf-doc)]
    (is (= m m2))
    (is (= 2400 (count (get m2 :package))))))

;;; Real-world shape: a committed pyproject.toml fixture

(def ^:private pyproject-fx "tests/fixtures/toml/pyproject.toml")

(deftest pyproject-fixture-round-trip
  (let [text (slurp pyproject-fx)
        m    (toml/parse-string text)
        m2   (toml/parse-string (slurp pyproject-fx))]
    (is (= m m2) "parsing the fixture twice must agree")
    (is (= "sample-project" (get-in m [:project :name])))
    (is (= "0.9.2" (get-in m [:project :version])))
    (is (= ["click>=8.1" "httpx>=0.27" "rich>=13.7"]
           (get-in m [:project :dependencies])))
    (is (= {:sample "sample_project.cli:main"
            (keyword "sample-worker") "sample_project.worker:main"}
           (get-in m [:project :scripts])))
    (is (= {:text "MIT" :spdx "MIT"}
           (get-in m [:project :license])))
    (is (= 2 (count (get-in m [:project :authors]))))
    (is (= "jane@example.com"
           (get-in m [:project :authors 0 :email])))
    (is (= ["--strict" "--fast"] (get-in m [:tool :sample :run :args])))
    (is (= "2024-06-01" (get-in m [:tool :sample :released])))
    (is (= 3735928559 (get-in m [:tool :sample :mask])))
    (is (= false (get-in m [:tool :sample :cache
                            (keyword "cold.start") :enabled])))
    (is (= 2 (count (get-in m [:tool :sample :mirrors]))))
    (is (= "https://us.example.com/mirror"
           (get-in m [:tool :sample :mirrors 1 :url])))
    (is (= [{:name "pytest" :version "8.0"} {:name "ruff"}]
           (get-in m [:dependency-groups :dev])))
    (is (= "A sample project exercising every TOML shape a real pyproject uses: tables, dotted keys, inline tables, arrays of tables, multiline strings, dates, and radix ints."
           (get-in m [:project :description])))
    (is (= {"author" "Jane Doe"}
           {"author" (get-in m [:project :entry-points
                                (keyword "package.metadata") :author])}))))

(run-tests-and-exit)
