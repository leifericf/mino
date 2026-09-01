(ns mino.mime
  "Media type lookup by file extension.

  (require '[mino.mime :as mime])
  (mime/mime-type \"index.html\")   ; \"text/html\"
  (mime/mime-type \"logo.png\")     ; \"image/png\"
  (mime/mime-type \"data.bin\")     ; \"application/octet-stream\"
  (mime/mime-type \"/src/app.js\")  ; \"text/javascript\"

  mime-type accepts a path string and returns the media type string for
  the last extension component. The lookup is case-insensitive on the
  extension. Unknown extensions return application/octet-stream.

  The table covers the practical set: text, web, image, font, audio,
  video, archive, wasm, and structured-data types.")

(require '[mino.path :as path])

;;;; Media type table

(def ^:private types
  {;; Text
   ".txt"    "text/plain"
   ".md"     "text/markdown"
   ".rst"    "text/x-rst"
   ".csv"    "text/csv"
   ".tsv"    "text/tab-separated-values"
   ".ics"    "text/calendar"
   ".vcf"    "text/vcard"
   ".log"    "text/plain"
   ;; Web
   ".html"   "text/html"
   ".htm"    "text/html"
   ".css"    "text/css"
   ".js"     "text/javascript"
   ".mjs"    "text/javascript"
   ".jsx"    "text/javascript"
   ".ts"     "text/javascript"
   ".tsx"    "text/javascript"
   ".map"    "application/json"
   ".wasm"   "application/wasm"
   ".xml"    "application/xml"
   ".xhtml"  "application/xhtml+xml"
   ".xsl"    "application/xslt+xml"
   ".xslt"   "application/xslt+xml"
   ;; Data
   ".json"   "application/json"
   ".jsonl"  "application/x-ndjson"
   ".ndjson" "application/x-ndjson"
   ".yaml"   "application/yaml"
   ".yml"    "application/yaml"
   ".toml"   "application/toml"
   ".edn"    "application/edn"
   ".rdf"    "application/rdf+xml"
   ".atom"   "application/atom+xml"
   ".rss"    "application/rss+xml"
   ;; Scripts / source
   ".sh"     "application/x-sh"
   ".py"     "text/x-python"
   ".rb"     "application/x-ruby"
   ".pl"     "text/x-perl"
   ;; Images
   ".png"    "image/png"
   ".jpg"    "image/jpeg"
   ".jpeg"   "image/jpeg"
   ".gif"    "image/gif"
   ".webp"   "image/webp"
   ".avif"   "image/avif"
   ".svg"    "image/svg+xml"
   ".ico"    "image/x-icon"
   ".bmp"    "image/bmp"
   ".tiff"   "image/tiff"
   ".tif"    "image/tiff"
   ;; Fonts
   ".woff"   "font/woff"
   ".woff2"  "font/woff2"
   ".ttf"    "font/ttf"
   ".otf"    "font/otf"
   ".eot"    "application/vnd.ms-fontobject"
   ;; Audio
   ".mp3"    "audio/mpeg"
   ".ogg"    "audio/ogg"
   ".oga"    "audio/ogg"
   ".wav"    "audio/wav"
   ".flac"   "audio/flac"
   ".aac"    "audio/aac"
   ".m4a"    "audio/mp4"
   ".opus"   "audio/ogg"
   ;; Video
   ".mp4"    "video/mp4"
   ".m4v"    "video/mp4"
   ".webm"   "video/webm"
   ".ogv"    "video/ogg"
   ".mov"    "video/quicktime"
   ".avi"    "video/x-msvideo"
   ".mkv"    "video/x-matroska"
   ;; Archives / binary
   ".zip"    "application/zip"
   ".tar"    "application/x-tar"
   ".gz"     "application/gzip"
   ".tgz"    "application/gzip"
   ".bz2"    "application/x-bzip2"
   ".xz"     "application/x-xz"
   ".7z"     "application/x-7z-compressed"
   ".rar"    "application/vnd.rar"
   ".jar"    "application/java-archive"
   ".war"    "application/java-archive"
   ".ear"    "application/java-archive"
   ".deb"    "application/vnd.debian.binary-package"
   ".rpm"    "application/x-rpm"
   ;; Documents
   ".pdf"    "application/pdf"
   ".rtf"    "application/rtf"
   ".doc"    "application/msword"
   ".docx"   "application/vnd.openxmlformats-officedocument.wordprocessingml.document"
   ".xls"    "application/vnd.ms-excel"
   ".xlsx"   "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"
   ".ppt"    "application/vnd.ms-powerpoint"
   ".pptx"   "application/vnd.openxmlformats-officedocument.presentationml.presentation"
   ;; Data / binary formats
   ".sqlite" "application/vnd.sqlite3"
   ".db"     "application/vnd.sqlite3"
   ".proto"  "application/x-protobuf"
   ".pb"     "application/x-protobuf"
   ;; Certificates and crypto
   ".pem"    "application/x-pem-file"
   ".der"    "application/x-x509-ca-cert"
   ".p12"    "application/x-pkcs12"
   ".pfx"    "application/x-pkcs12"})

(def ^:private default-type "application/octet-stream")

;;;; Public API

(defn mime-type
  "Returns the media type string for the file extension of path.
  The extension is the last dot-delimited suffix of the basename;
  lookup is case-insensitive. Returns application/octet-stream for
  unknown or absent extensions."
  [path-str]
  (let [ext (path/extension path-str)]
    (if (or (nil? ext) (= ext ""))
      default-type
      (get types (clojure.string/lower-case ext) default-type))))
