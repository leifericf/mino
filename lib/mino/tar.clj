(ns mino.tar
  "Tar container scripting over the native tar prims (ADR 29's
  container pattern), with .tar.gz composed from the existing gzip
  stream at this level.

  (require '[mino.tar :as tar])
  (tar/create [{:name \"report.csv\" :data csv}      ; => archive bytes
               {:name \"logs/\" :type :dir}])
  (tar/create \"some/dir\")                  ; => archive of a tree
  (tar/create entries {:gzip true})          ; => a .tar.gz member
  (tar/entries archive)                  ; => [{:name ...} ...]
  (tar/read archive \"report.csv\")      ; => the entry bytes
  (tar/extract archive \"dest/dir\")     ; => [name ...], hardened

  Thin aliases over tar-entries / tar-read / tar-extract, plus a
  create that builds from an entry-map vector or a directory tree.
  Read entry maps carry {:name :size :mode :mtime :type :linkname}
  (:mtime epoch seconds, nil when the stored field is zero; :type one
  of :file :dir :symlink :hardlink; :linkname the target for a link
  entry, nil otherwise). create defaults an omitted :type to :file,
  :mode to 0644 (file) / 0755 (dir) / 0777 (symlink), and an omitted
  :mtime to the zero field (which lists back as nil).

  Output is deterministic by contract: the same entries give
  byte-identical archives, plain or gzipped. The read-side fns
  transparently gunzip a gzip input (magic 0x1f 0x8b); the bare
  tar-* prims stay strict and read a gzip stream as :codec/magic.

  This namespace never touches clojure.zip: that is the tree-zipper
  namespace, this is a tar archive one."
  )

(require '[mino.path :as path])

(defn- gzip?
  "True when the bytes start with the gzip magic 0x1f 0x8b."
  [b]
  (and (bytes? b)
       (>= (count b) 2)
       (= 0x1f (bit-and (aget b 0) 0xff))
       (= 0x8b (bit-and (aget b 1) 0xff))))

(defn- degzip
  "Return the plain archive bytes: gunzip a gzip input, else pass
  through unchanged. The transparent-decompression ergonomic lives in
  the facade so the bare prim never guesses a format."
  [archive]
  (if (gzip? archive) (gzip-decompress archive) archive))

(defn entries
  "Lists a tar archive's members as maps of {:name :size :mode :mtime
  :type :linkname}, in archive order. :mtime is epoch seconds, nil
  when the stored field is zero; :type is :file, :dir, :symlink,
  :hardlink, or another header type's keyword; :linkname is the link
  target for a link entry, nil otherwise. A gzip input is gunzipped
  transparently. The archive is bytes or a string."
  [archive]
  (tar-entries (degzip archive)))

(defn read
  "Returns the bytes of the member named name, the first match in
  archive order. Throws :codec/missing when no member has that name.
  A gzip input is gunzipped transparently. Nothing is written to any
  filesystem: a member named \"../x\" is inert data here."
  [archive name]
  (tar-read (degzip archive) name))

(defn extract
  "Materializes a tar archive under the destination directory and
  returns the vector of extracted member names in archive order.
  Hardened: an absolute member name, a name with a \"..\" component,
  or a link entry whose target escapes the destination throws
  :codec/unsafe before anything lands, and no write descends through
  a symlink an earlier member planted. Symlink entries are recreated
  as links, never followed. Modes and mtimes are restored. A gzip
  input is gunzipped transparently."
  ([archive dest] (tar-extract (degzip archive) dest))
  ([archive dest opts] (tar-extract (degzip archive) dest opts)))

(defn- dir-tree-entries
  "Walk the directory tree rooted at dir into a vector of tar entry
  maps: a sorted single-level glob per directory, names relative to
  dir, directories with a trailing slash, symlinks stored as link
  entries (the copy-tree link policy; never followed). Modes and
  mtimes come from the p1 stat surface; stat's :mtime is milliseconds
  and tar stores seconds, so it floors here."
  [dir]
  (letfn [(sec [ms] (when ms (quot ms 1000)))
          (walk [abs rel acc]
            (reduce
             (fn [acc child-abs]
               ;; glob answers absolute child paths; derive the leaf.
               (let [child (path/basename child-abs)
                     child-rel (if (empty? rel) child (str rel "/" child))
                     st (stat child-abs)]
                 (case (:type st)
                   :dir
                   (let [acc (conj acc {:name (str child-rel "/")
                                        :type :dir
                                        :mode (:mode st)
                                        :mtime (sec (:mtime st))})]
                     (walk child-abs child-rel acc))
                   :symlink
                   (conj acc {:name child-rel
                              :type :symlink
                              :linkname (read-symlink child-abs)
                              :mode (:mode st)
                              :mtime (sec (:mtime st))})
                   :file
                   ;; slurp reads "rb" into a mino byte-string, so the
                   ;; file's raw bytes become the entry :data.
                   (conj acc {:name child-rel
                              :data (slurp child-abs)
                              :mode (:mode st)
                              :mtime (sec (:mtime st))})
                   ;; other node types (fifo, device) are skipped
                   acc)))
             acc
             (path/glob "*" abs {:match-dot true})))]
    (walk dir "" [])))

(defn create
  "Builds a tar archive and returns the bytes. The first argument is
  either a vector of entry maps or a directory path (a string that
  names an existing directory).

  Entry maps: {:name (required string) :type :file (default) / :dir /
  :symlink / :hardlink :data (bytes or string; file entries) :linkname
  (string; link entries) :mode (0..07777; default 0644 file, 0755 dir,
  0777 symlink) :mtime (epoch seconds, or nil for the zero field)}.

  A directory path is walked into entry maps (sorted, relative names,
  trailing slash on directories, symlinks stored as links), modes and
  mtimes taken from stat.

  opts {:gzip true} wraps the archive in one gzip member (a .tar.gz).
  Output is deterministic: same entries and opts give byte-identical
  bytes."
  ([source] (create source nil))
  ([source opts]
   (let [entry-maps (if (string? source) (dir-tree-entries source) source)
         archive (tar-create (vec entry-maps))]
     (if (:gzip opts)
       (gzip-compress archive)
       archive))))
