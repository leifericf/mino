(require "tests/test")
(require '[clojure.string :as str])

;; SLAD image save tests. The load round-trip is tested by
;; examples/embed_slad.c (cross-state save->free->new->install->load).

(def image-test-dir "/tmp/mino-image-test")

(deftest image-save-creates-file
  (rm-rf image-test-dir)
  (mkdir-p image-test-dir)
  (let [path (str image-test-dir "/simple.img")]
    (save-image path)
    (is (file-exists? path) "image file created")
    (is (pos? (count (slurp path))) "image non-empty"))
  (rm-rf image-test-dir))

(deftest image-save-has-magic-header
  (rm-rf image-test-dir)
  (mkdir-p image-test-dir)
  (let [path (str image-test-dir "/magic.img")]
    (save-image path)
    (let [content (slurp path)]
      (is (= "MINO-IMAGE/1" (subs content 0 12))
          "image starts with magic header")))
  (rm-rf image-test-dir))

(deftest image-save-has-crc32
  (rm-rf image-test-dir)
  (mkdir-p image-test-dir)
  (let [path (str image-test-dir "/crc.img")]
    (save-image path)
    (let [content (slurp path)]
      (is (str/includes? content "CRC32 ")
          "image has CRC32 trailer")))
  (rm-rf image-test-dir))

(deftest image-save-contains-user-vars
  (rm-rf image-test-dir)
  (mkdir-p image-test-dir)
  (let [path (str image-test-dir "/vars.img")]
    (eval "(def img-unique-var 12345)")
    (save-image path)
    (let [content (slurp path)]
      (is (str/includes? content "img-unique-var")
          "image contains user var name")))
  (rm-rf image-test-dir))

(deftest image-save-refuses-with-in-flight-future
  ;; Quiesce protocol: a pending future means the runtime is not at
  ;; rest. save-image must refuse rather than silently capturing a
  ;; half-mutated heap. Future is given a long enough body that the
  ;; save attempt lands while it is still in flight; we deref it
  ;; afterwards to clean up.
  (rm-rf image-test-dir)
  (mkdir-p image-test-dir)
  (let [path (str image-test-dir "/fut.img")
        f (future (Thread/sleep 3000) 42)]
    (is (thrown? (save-image path))
        "save-image refuses while a future is in flight")
    (is (= 42 @f) "future still resolves after the refused save"))
  (rm-rf image-test-dir))

(deftest image-load-rejects-truncated-v1-image
  ;; A v1 image (matching MINO-IMAGE/1 magic) whose CRC32 trailer is
  ;; missing -- e.g. a torn write that lost the tail -- must be
  ;; rejected. The v0-compat escape hatch is narrow enough that it
  ;; can't swallow a v1 file with a missing trailer.
  (rm-rf image-test-dir)
  (mkdir-p image-test-dir)
  (let [full   (str image-test-dir "/full.img")
        trunc  (str image-test-dir "/trunc.img")]
    (save-image full)
    (let [content (slurp full)
          cut     (quot (count content) 2)
          half    (subs content 0 (max 20 cut))]
      (spit trunc half)
      (is (thrown? (load-image-into trunc))
          "truncated v1 image without CRC trailer is rejected")))
  (rm-rf image-test-dir))

(run-tests-and-exit)
