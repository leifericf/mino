(ns clojure.zip)

;; Functional zipper implementation following Huet's algorithm.
;; A zipper loc is a vector [node {:l left :r right :pnodes pnodes :ppath ppath :changed? changed?}]

(defn zipper [branch? children make-node root]
  (with-meta [root nil]
    {:zip/branch? branch?
     :zip/children children
     :zip/make-node make-node}))

(defn node [loc] (first loc))

(defn branch? [loc]
  ((:zip/branch? (meta loc)) (node loc)))

(defn children [loc]
  (if (branch? loc)
    ((:zip/children (meta loc)) (node loc))
    (throw "called children on a leaf node")))

(defn make-node [loc node children]
  ((:zip/make-node (meta loc)) node children))

(defn path [loc]
  (:pnodes (second loc)))

(defn lefts [loc]
  (seq (:l (second loc))))

(defn rights [loc]
  (seq (:r (second loc))))

(defn down [loc]
  (when (branch? loc)
    (let [cs (children loc)]
      (when (seq cs)
        (let [node (first loc)
              p    (second loc)]
          (with-meta [(first cs)
                      {:l []
                       :r (rest cs)
                       :pnodes (if p (conj (:pnodes p) node) [node])
                       :ppath p
                       :changed? false}]
            (meta loc)))))))

(defn up [loc]
  (let [p (second loc)]
    (when p
      (let [node (first loc)
            changed? (:changed? p)]
        (if changed?
          (with-meta [(make-node loc (peek (:pnodes p))
                                 (concat (:l p) (cons node (:r p))))
                      (if-let [pp (:ppath p)]
                        (assoc pp :changed? true)
                        nil)]
            (meta loc))
          (with-meta [(peek (:pnodes p))
                      (:ppath p)]
            (meta loc)))))))

(defn root [loc]
  (if (= :end (second loc))
    (node loc)
    (let [p (up loc)]
      (if p (recur p) (node loc)))))

(defn right [loc]
  (let [p (second loc)]
    (when (and p (seq (:r p)))
      (with-meta [(first (:r p))
                  (assoc p
                    :l (conj (:l p) (first loc))
                    :r (rest (:r p)))]
        (meta loc)))))

(defn left [loc]
  (let [p (second loc)]
    (when (and p (seq (:l p)))
      (with-meta [(peek (:l p))
                  (assoc p
                    :l (pop (:l p))
                    :r (cons (first loc) (:r p)))]
        (meta loc)))))

(defn insert-left [loc item]
  (let [p (second loc)]
    (if (nil? p)
      (throw "insert at top")
      (with-meta [(first loc)
                  (assoc p
                    :l (conj (:l p) item)
                    :changed? true)]
        (meta loc)))))

(defn insert-right [loc item]
  (let [p (second loc)]
    (if (nil? p)
      (throw "insert at top")
      (with-meta [(first loc)
                  (assoc p
                    :r (cons item (:r p))
                    :changed? true)]
        (meta loc)))))

(defn replace [loc node]
  (with-meta [node (if-let [p (second loc)]
                     (assoc p :changed? true)
                     nil)]
    (meta loc)))

(defn edit [loc f & args]
  (replace loc (apply f (node loc) args)))

(defn insert-child [loc item]
  (replace loc (make-node loc (node loc) (cons item (children loc)))))

(defn append-child [loc item]
  (replace loc (make-node loc (node loc) (concat (children loc) [item]))))

(defn end? [loc]
  (= :end (second loc)))

(defn next [loc]
  (if (= :end (second loc))
    loc
    (or (and (branch? loc) (down loc))
        (right loc)
        (loop [p (up loc)]
          (when p
            (or (right p) (recur (up p)))))
        (with-meta [(node loc) :end] (meta loc)))))

(defn prev [loc]
  (if-let [lloc (left loc)]
    (loop [loc lloc]
      (if-let [child (and (branch? loc) (down loc))]
        (recur (loop [r child]
                 (if-let [nr (right r)] (recur nr) r)))
        loc))
    (up loc)))

(defn remove [loc]
  (let [p (second loc)]
    (if (nil? p)
      (throw "remove at top")
      (if (seq (:l p))
        (loop [loc (with-meta [(peek (:l p))
                               (assoc p
                                 :l (pop (:l p))
                                 :changed? true)]
                     (meta loc))]
          (if-let [child (and (branch? loc) (down loc))]
            (recur (loop [r child]
                     (if-let [nr (right r)] (recur nr) r)))
            loc))
        (with-meta [(make-node loc (peek (:pnodes p)) (:r p))
                    (if-let [pp (:ppath p)]
                      (assoc pp :changed? true)
                      nil)]
          (meta loc))))))

;; Convenience constructors

(defn seq-zip [root]
  (zipper seq? identity (fn [node children] (with-meta children (meta node))) root))

(defn vector-zip [root]
  (zipper vector? seq (fn [node children] (with-meta (vec children) (meta node))) root))

(defn xml-zip [root]
  (zipper (comp seq :content) :content
          (fn [node children] (assoc node :content (and children (apply vector children))))
          root))
