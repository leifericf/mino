(require "tests/test")
(require '[clojure.string :as str])
(require '[mino.yaml :as yaml])

;; YAML reading must stay well inside the absolute budget at document
;; scale. The reader is the native single-pass yaml-parse prim (ADR
;; 26); the mino-side Clojure reader it replaced was linear but 41x
;; over budget (82 s for 1.05 MB through interpreter dispatch and
;; per-token regex compiles), which is why the reader went native.
;; The budget is absolute, never a wall-clock ratio (CI-runner
;; lesson), and carries in-suite headroom for resident-set GC
;; pressure (the p7 toml lesson, measured ~3x). This file joins the
;; nightly fuzz/stress exclusions.

(defn- yaml-service-block
  [i]
  (let [r (rem i 9)]
    (str
      "# service block " i "\n"
      "services.svc" i ":\n"
      "  name: resizer-" i "\n"
      "  version: \"2." r "." (rem i 20) "\"\n"
      "  enabled: " (if (even? i) "true" "false") "\n"
      "  replicas: " (inc r) "\n"
      "  timeout_s: " r "." (rem i 10) "\n"
      "  image: registry.internal/resizer:2." r "\n"
      "  command: [\"resizer\", \"--workers\", \"" (+ 2 r) "\", \"--queue\", \"resize\"]\n"
      "  env:\n"
      "    BACKEND: gpu\n"
      "    MAX_PIXELS: " (+ 4096 r) "\n"
      "    TRACE: ~\n"
      "  resources:\n"
      "    requests: {cpu: 0.5, memory: \"512Mi\"}\n"
      "    limits:\n"
      "      cpu: " (+ 2 r) "\n"
      "      memory: 2Gi\n"
      "  buckets:\n"
      "    - name: avatar\n"
      "      max: " (+ 512 r) "\n"
      "      strip_meta: true\n"
      "      formats: [png, webp]\n"
      "    - name: hero\n"
      "      max: 2048\n"
      "      strip_meta: false\n"
      "      formats:\n"
      "        - jpeg\n"
      "        - webp\n"
      "  banner: |\n"
      "    Care and feeding\n"
      "    of service " i ".\n"
      "  notes: >\n"
      "    Folded note " i "\n"
      "    spanning two lines.\n"
      "  health: /healthz\n"
      "  ports:\n"
      "    - {port: " (+ 8000 r) ", name: http}\n"
      "    - port: " (+ 9000 r) "\n"
      "      name: metrics\n"
      "\n")))

(defn- yaml-build-doc
  "Deterministic megabyte-scale config-shaped document: ~1350
  service blocks covering scalars, flow collections, nesting,
  sequences of mappings, literal and folded blocks, comments, and
  core-schema resolution. Transient accumulation; no lazy chains at
  this size."
  []
  (let [acc (transient [])]
    (loop [i 0]
      (if (= i 1350)
        (str/join (persistent! acc))
        (do
          (conj! acc (yaml-service-block i))
          (recur (inc i)))))))

(def ^:private yaml-perf-doc (yaml-build-doc))
(def ^:private yaml-doc-size (count yaml-perf-doc))

(deftest yaml-read-one-megabyte-config-shape-within-budget
  (let [t0 (nano-time)
        m (yaml/parse-string yaml-perf-doc)
        ms (quot (- (nano-time) t0) 1000000)
        svc0 (get m (keyword "services.svc0"))
        svc1349 (get m (keyword "services.svc1349"))]
    (is (> yaml-doc-size 1000000) "document must be megabyte scale")
    (is (= 1350 (count m)))
    ;; Spot-check structure so a fast-but-wrong reader cannot pass.
    (is (= "resizer-0" (:name svc0)))
    (is (= "resizer-1349" (:name svc1349)))
    (is (= true (:enabled svc0)))
    (is (= false (:enabled svc1349)))
    (is (= 9 (:replicas svc1349)))
    (is (= ["resizer" "--workers" "2" "--queue" "resize"]
           (:command svc0)))
    (is (= nil (get-in svc0 [:env :TRACE])))
    (is (= 4096 (get-in svc0 [:env :MAX_PIXELS])))
    (is (= 0.5 (get-in svc0 [:resources :requests :cpu])))
    (is (= 10 (get-in svc1349 [:resources :limits :cpu])))
    (is (= "avatar" (get-in svc0 [:buckets 0 :name])))
    (is (= ["jpeg" "webp"] (get-in svc1349 [:buckets 1 :formats])))
    (is (= "Care and feeding\nof service 0.\n" (:banner svc0)))
    (is (= "Folded note 0 spanning two lines.\n" (:notes svc0)))
    (is (= 8000 (get-in svc0 [:ports 0 :port])))
    (is (= "metrics" (get-in svc1349 [:ports 1 :name])))
    (println (str "  [perf] megabyte parse took " ms "ms"))))

(deftest yaml-parse-is-deterministic-at-scale
  (let [m (yaml/parse-string yaml-perf-doc)
        m2 (yaml/parse-string yaml-perf-doc)]
    (is (= m m2))
    (is (= 1350 (count m2)))))

(deftest yaml-mixed-quotes-and-flow-roundtrip-at-scale
  (let [m (yaml/parse-string yaml-perf-doc {:keywords false})]
    (is (map? m))
    (is (string? (get (get m "services.svc7") "name")))
    (is (= "resizer-7" (get (get m "services.svc7") "name")))))

;;; Real-world shape: a committed k8s manifest stream fixture

(def ^:private k8s-fx "tests/fixtures/yaml/k8s.yaml")

(deftest yaml-k8s-manifest-fixture
  (let [docs (yaml/parse-string-all (slurp k8s-fx))]
    (is (= 3 (count docs)))
    (is (= "apps/v1" (get-in docs [0 :apiVersion])))
    (is (= "Deployment" (get-in docs [0 :kind])))
    (is (= "image-resizer" (get-in docs [0 :metadata :name])))
    (is (= "media" (get-in docs [0 :metadata :namespace])))
    (is (= "resizer" (get-in docs
                             [0 :spec :template :spec :containers 0 :name])))
    (is (= ["--workers" "4" "--queue" "resize"]
           (get-in docs
                   [0 :spec :template :spec :containers 0 :args])))
    (is (= nil (get-in docs [0 :spec :template :spec :containers 0
                             :env 2 :value])))
    (is (= "4096" (get-in docs [0 :spec :template :spec :containers 0
                                :env 1 :value])))
    (is (= "512Mi" (get-in docs [0 :spec :template :spec :containers 0
                                 :resources :requests :memory])))
    (is (= 10 (get-in docs [0 :spec :template :spec :containers 0
                            :livenessProbe :periodSeconds])))
    (is (= "Service" (get-in docs [1 :kind])))
    (is (= "http" (get-in docs [1 :spec :ports 0 :name])))
    (is (= "CronJob" (get-in docs [2 :kind])))
    (is (= "0 */6 * * *" (get-in docs [2 :spec :schedule])))
    (is (= "14" (get-in docs [2 :spec :jobTemplate :spec :template
                              :spec :containers 0 :env 0 :value])))
    ;; parsing the fixture twice must agree
    (is (= docs (yaml/parse-string-all (slurp k8s-fx))))))

(run-tests-and-exit)
