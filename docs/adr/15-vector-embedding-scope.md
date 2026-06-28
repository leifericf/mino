# ADR 15: Vector embedding and similarity search are out of scope for mino.store

Date: 2026-06-28

## Context

Vector embedding storage and similarity search are increasingly
expected from data stores that back AI and retrieval workloads: embed a
corpus into high-dimensional float vectors, then answer nearest-neighbor
queries against them. Libraries and databases that specialize in this
(Chroma, Qdrant, LanceDB, pgvector, FAISS) ship approximate
nearest-neighbor (ANN) indexes such as HNSW, IVF, or ScaNN.

mino.store's indexes are exact hash-based lookups: AET, AVET, VAET for
entity-attribute navigation. There is no nearest-neighbor primitive, no
distance function, and no ranked result set in the query layer.

ANN is a specialized domain. Production-quality HNSW needs graph
maintenance on every write, tuned layer and efConstruction parameters,
memory proportional to graph fanout, and distance metrics chosen at
index-build time. None of this fits mino's embedded, single-threaded,
ANSI C99, KB-scale-per-store profile. The store is built for thousands
of small per-state stores (game NPCs, agent memory, plugin config), not
for one large vector corpus.

## Decision

Native vector search is out of scope for mino.store core.

For v1 and the foreseeable future, mino.store will not ship:

- a `:vector` value type distinct from existing scalar and collection
  types;
- an ANN index (HNSW, IVF, or otherwise);
- similarity-query primitives (k-nearest, cosine, distance operators);
- a ranked or score-ordered result set in the query layer.

The host application brokers external vector databases through the
existing `host/*` interop or by holding an external connection itself.
mino.store's job is the structured, exact, time-traveling fact store;
vector retrieval is a separate concern composed at the host layer.

Embeddings can still be stored. A vector that fits an existing type (a
vector of floats, a bytes blob, a string of packed floats) is just a
value. What mino will not do is index it for similarity or answer
similarity queries against it.

## Consequences

- Embedders building retrieval-augmented workflows compose mino.store
  with an external vector DB at the host layer. Entity IDs in the store
  serve as the join key to document IDs in the vector DB.
- The store's write path, memory budget, and C99 portability stay clean.
  No floating-point distance kernels, no graph index, no tuning surface.
- Adding vector support later is not precluded. The bar to revisit is
  measured embedder demand plus a clean implementation path that does
  not violate ADR 01 (ANSI C99) or ADR 10's per-state isolation.
- This decision narrows the v1 query surface. Any future vector work
  lands as a new ADR that supersedes this one, not as a quiet addition.

## Alternatives

- **Ship a minimal HNSW index in core.** Proven in standalone vector
  libraries, but the implementation complexity (graph maintenance, layer
  tuning, memory overhead, concurrency story) is disproportionate to
  the embedded profile and the small-store cardinality mino targets.
  Rejected for v1.
- **Add a `:vector` type with brute-force similarity (O(n) scan).** A
  distance function over stored floats is cheap to write, but an O(n)
  scan is the host's job, not the store's. Shipping it invites the
  expectation that mino answers similarity queries, which it cannot do
  at scale. Rejected: the boundary is cleaner if the store does no
  similarity work at all.
- **Wrap an external ANN library (FAISS, hnswlib) as a capability.**
  Adds a non-C99 dependency, a new capability bit, and a foreign-code
  maintenance burden for a feature most embedders will broker at the
  host layer anyway. Rejected.
