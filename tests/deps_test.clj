(require "tests/test")
(require '[mino.deps :as deps])

;; Dependency management: manifest loading, validation, path resolution.

(def test-dir "/tmp/mino-deps-test")

(deftest load-manifest-basic
  (let [path (str test-dir "/test-mino.edn")]
    (mkdir-p test-dir)
    (spit path "{:paths [\"src\"] :deps {:foo {:path \"../foo\"}}}")
    (let [m (deps/load-manifest path)]
      (is (= ["src"] (:paths m)))
      (is (= {:foo {:path "../foo"}} (:deps m))))
    (rm-rf test-dir)))

(deftest load-manifest-unknown-keys-ignored
  (let [path (str test-dir "/test-mino.edn")]
    (mkdir-p test-dir)
    (spit path "{:paths [\"src\"] :deps {} :tasks {} :main \"app\"}")
    (let [m (deps/load-manifest path)]
      (is (= ["src"] (:paths m)))
      (is (map? (:deps m)))
      (is (= "app" (:main m))))
    (rm-rf test-dir)))

(deftest load-manifest-validation
  (let [path (str test-dir "/bad.edn")]
    (mkdir-p test-dir)
    ;; :paths must be a vector
    (spit path "{:paths \"src\"}")
    (is (thrown? (deps/load-manifest path)))
    ;; :deps must be a map
    (spit path "{:deps [1 2 3]}")
    (is (thrown? (deps/load-manifest path)))
    ;; root must be a map
    (spit path "[1 2 3]")
    (is (thrown? (deps/load-manifest path)))
    (rm-rf test-dir)))

(deftest validate-dep-spec-errors
  ;; Missing :rev for git dep
  (is (thrown? (deps/validate-dep-spec :foo {:git "https://example.com"})))
  ;; Not a map
  (is (thrown? (deps/validate-dep-spec :foo "not a map")))
  ;; No recognized source type
  (is (thrown? (deps/validate-dep-spec :foo {:unknown true}))))

(deftest validate-dep-spec-ok
  (is (nil? (deps/validate-dep-spec :foo {:path "../foo"})))
  (is (nil? (deps/validate-dep-spec :bar {:git "https://example.com" :rev "abc"}))))

(deftest validate-dep-spec-rejects-option-like-git-args
  ;; A :git url or :rev beginning with '-' would be read by git as an
  ;; option (e.g. --upload-pack=...); reject it as data before it reaches
  ;; the git argv.
  (is (thrown? (deps/validate-dep-spec :x {:git "--upload-pack=touch /tmp/pwn"
                                           :rev "main"})))
  (is (thrown? (deps/validate-dep-spec :x {:git "https://h/r.git" :rev "--foo"})))
  (is (thrown? (deps/validate-dep-spec :x {:git 42 :rev "main"})))
  (is (= :deps/spec
         (try (deps/validate-dep-spec :x {:git "-bad" :rev "main"})
              (catch e (:mino/kind e))))))

(deftest resolve-paths-with-deps
  (let [m {:paths ["src" "lib"]
           :deps {:foo {:path "../foo"}
                  :bar {:git "https://example.com" :rev "abc"}}}]
    (is (= ["src" "lib" "../foo" ".mino/deps/bar/src"]
           (deps/resolve-paths m)))))

(deftest resolve-paths-custom-root
  (let [m {:deps {:mylib {:git "https://example.com" :rev "abc"
                          :deps/root ["lib" "src"]}}}]
    (is (= ["src" "lib" ".mino/deps/mylib/lib" ".mino/deps/mylib/src"]
           (deps/resolve-paths m)))))

(deftest resolve-paths-defaults
  ;; When :paths is absent, defaults to ["src" "lib"]
  (let [m {:deps {:foo {:path "../x"}}}]
    (is (= ["src" "lib" "../x"]
           (deps/resolve-paths m))))

  ;; Git deps default to src/ subdirectory
  (let [m {:deps {:bar {:git "https://x.com" :rev "abc"}}}]
    (is (= ["src" "lib" ".mino/deps/bar/src"]
           (deps/resolve-paths m)))))

(deftest resolve-paths-no-deps
  (let [m {:paths ["mylib"]}]
    (is (= ["mylib"] (deps/resolve-paths m)))))

(run-tests-and-exit)
