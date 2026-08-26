(ns mino.zip
  "Zip container scripting over the native zip prims (ADR 29).

  (require '[mino.zip :as zip])
  (zip/write [{:name \"report.csv\" :data csv}    ; => archive bytes
              {:name \"plot.png\"  :data png}])
  (zip/entries archive)                  ; => [{:name ...} ...]
  (zip/read archive \"report.csv\")      ; => the bytes written

  Thin aliases, no second entry shape: write entries are
  {:name :data :mtime :level :method :comment} and read entries add
  :size, :compressed-size, :crc32, and :directory? (field names
  follow the JVM ZipEntry accessors and python ZipInfo attributes).
  Output is deterministic by contract: the same entries and opts
  give byte-identical archives (defaults {:method :deflate
  :level 6 :mtime 0}, with :mtime 0 mapped to the DOS minimum
  1980-01-01), and {:zip64 true} forces always-zip64 structures.

  This namespace never touches clojure.zip: that is the tree-zipper
  namespace, this is the archive one."
  )

(defn entries
  "Lists a zip archive's entries as maps of {:name :size
  :compressed-size :crc32 :method :mtime :directory? :comment}, in
  archive order, from the central directory. Names decode as UTF-8
  when the entry sets the language-encoding flag (bit 11), else
  CP437: the python zipfile behavior, so listing and lookup agree.
  :mtime is epoch seconds, nil at the DOS minimum 1980-01-01;
  :method is :deflate, :store, or an unknown method's integer code;
  duplicate names all appear (read resolves them to the first)."
  ([archive]
   (zip-entries archive)))

(defn read
  "Returns the bytes of the archive entry named name, the first
  central-directory match in archive order. :max-bytes (default
  64 MiB) is checked against the central-directory declared size
  BEFORE any inflation (a zip bomb throws :codec/limit without
  allocating), and CRC32 is verified on the extracted bytes. Throws
  :codec/missing when no entry has that name. Nothing is written to
  any filesystem: entry names like \"../x\" are data, inert here by
  construction."
  ([archive name]
   (zip-read archive name))
  ([archive name opts]
   (zip-read archive name opts)))

(defn write
  "Builds a zip archive from an entry vector, in vector order:
  [{:name \"report.csv\" :data csv} {:name \"plot.png\" :data
  png}]. Each entry takes {:name (required string, forward slashes,
  no leading slash) :data (required bytes or string, UTF-8; empty
  with a trailing slash name writes a directory entry)
  :mtime (epoch seconds; 0 and nil clamp to 1980-01-01)
  :level (0-9, archive default 6) :method (:deflate or :store)
  :comment (central-directory string)}. Output is deterministic:
  same entries and opts give byte-identical bytes. opts
  {:zip64 true} forces zip64 structures; below the 4 GiB and
  65535-entry thresholds the automatic switch leaves them out."
  ([entry-maps]
   (zip-write entry-maps))
  ([entry-maps opts]
   (zip-write entry-maps opts)))
