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

(run-tests-and-exit)
