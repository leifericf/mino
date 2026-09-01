(require "tests/test")

;; Media type lookup by file extension.
;;
;; (mino.mime/mime-type path) returns the MIME type string for the
;; extension of path, or application/octet-stream for unknowns.

(require '[mino.mime :as mime])

;;;; text/web types

(deftest mime-html-extensions
  (is (= "text/html" (mime/mime-type "index.html")))
  (is (= "text/html" (mime/mime-type "page.htm"))))

(deftest mime-css
  (is (= "text/css" (mime/mime-type "styles.css"))))

(deftest mime-javascript
  (is (= "text/javascript" (mime/mime-type "app.js")))
  (is (= "text/javascript" (mime/mime-type "module.mjs"))))

(deftest mime-json
  (is (= "application/json" (mime/mime-type "data.json"))))

(deftest mime-xml
  (is (= "application/xml" (mime/mime-type "feed.xml"))))

(deftest mime-wasm
  (is (= "application/wasm" (mime/mime-type "app.wasm"))))

(deftest mime-plain-text
  (is (= "text/plain" (mime/mime-type "readme.txt"))))

(deftest mime-markdown
  (is (= "text/markdown" (mime/mime-type "README.md"))))

(deftest mime-csv
  (is (= "text/csv" (mime/mime-type "data.csv"))))

;;;; image types

(deftest mime-png
  (is (= "image/png" (mime/mime-type "logo.png"))))

(deftest mime-jpeg
  (is (= "image/jpeg" (mime/mime-type "photo.jpg")))
  (is (= "image/jpeg" (mime/mime-type "photo.jpeg"))))

(deftest mime-gif
  (is (= "image/gif" (mime/mime-type "anim.gif"))))

(deftest mime-svg
  (is (= "image/svg+xml" (mime/mime-type "icon.svg"))))

(deftest mime-webp
  (is (= "image/webp" (mime/mime-type "hero.webp"))))

(deftest mime-ico
  (is (= "image/x-icon" (mime/mime-type "favicon.ico"))))

;;;; font types

(deftest mime-woff2
  (is (= "font/woff2" (mime/mime-type "Inter.woff2"))))

(deftest mime-woff
  (is (= "font/woff" (mime/mime-type "Inter.woff"))))

(deftest mime-ttf
  (is (= "font/ttf" (mime/mime-type "Arial.ttf"))))

;;;; audio/video types

(deftest mime-mp3
  (is (= "audio/mpeg" (mime/mime-type "track.mp3"))))

(deftest mime-mp4
  (is (= "video/mp4" (mime/mime-type "clip.mp4"))))

(deftest mime-webm
  (is (= "video/webm" (mime/mime-type "video.webm"))))

;;;; archive types

(deftest mime-zip
  (is (= "application/zip" (mime/mime-type "archive.zip"))))

(deftest mime-tar
  (is (= "application/x-tar" (mime/mime-type "bundle.tar"))))

(deftest mime-gz
  (is (= "application/gzip" (mime/mime-type "data.gz"))))

(deftest mime-tgz
  (is (= "application/gzip" (mime/mime-type "src.tgz"))))

(deftest mime-pdf
  (is (= "application/pdf" (mime/mime-type "manual.pdf"))))

;;;; structured data

(deftest mime-yaml
  (is (= "application/yaml" (mime/mime-type "config.yaml")))
  (is (= "application/yaml" (mime/mime-type "config.yml"))))

(deftest mime-toml
  (is (= "application/toml" (mime/mime-type "Cargo.toml"))))

(deftest mime-jsonl
  (is (= "application/x-ndjson" (mime/mime-type "stream.jsonl")))
  (is (= "application/x-ndjson" (mime/mime-type "stream.ndjson"))))

;;;; default for unknown extensions

(deftest mime-unknown-extension
  (is (= "application/octet-stream" (mime/mime-type "data.bin")))
  (is (= "application/octet-stream" (mime/mime-type "archive.xyzzy"))))

(deftest mime-no-extension
  (is (= "application/octet-stream" (mime/mime-type "Makefile"))))

(deftest mime-empty-string
  (is (= "application/octet-stream" (mime/mime-type ""))))

;;;; case-insensitive lookup

(deftest mime-case-insensitive-html
  (is (= "text/html" (mime/mime-type "index.HTML")))
  (is (= "text/html" (mime/mime-type "index.Html"))))

(deftest mime-case-insensitive-jpeg
  (is (= "image/jpeg" (mime/mime-type "photo.JPG")))
  (is (= "image/jpeg" (mime/mime-type "photo.JPEG"))))

(deftest mime-case-insensitive-js
  (is (= "text/javascript" (mime/mime-type "app.JS"))))

;;;; path with directories

(deftest mime-path-with-directories
  (is (= "text/html" (mime/mime-type "/var/www/index.html")))
  (is (= "text/css" (mime/mime-type "static/css/main.css"))))

;;;; http server static-file integration check
;;
;; Verify that mime-type is usable for serving static files: given
;; a path, a Content-Type header can be built from it.

(deftest mime-content-type-header-construction
  (let [path "/static/app.js"
        ct   (str (mime/mime-type path) "; charset=utf-8")]
    (is (clojure.string/starts-with? ct "text/javascript"))))

(run-tests-and-exit)
