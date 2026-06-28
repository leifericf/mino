# Deliberately out of scope

This appendix lists what mino.store will not do for v1 and the
foreseeable future, and why. It is a reference, not a single decision:
each item ties back to the ADR that settled it. The list exists so that
"why doesn't mino do X" has a citable answer without re-deriving the
argument each time. An item leaves this list only when a new ADR says
so.

- **Bitemporal valid-time.** mino.store is single-axis: transaction
  time, monotonic per store. Valid-time (when a fact was true in the
  world, independent of when the store learned it) is XTDB's niche and
  doubles the query and indexing surface for a use case the embedded
  profile does not need. Ref ADR 10.

- **Distributed cluster, Kafka, or any cross-process log.** A store
  belongs to one `mino_state`; there is no transactor process, no
  peer-to-peer protocol, no log replication. Multi-process shared
  mutable state is a host responsibility, brokered via `host/*` interop
  or an external database. Ref ADR 10.

- **Rules in Datalog.** Query supports patterns, predicates, negation
  (`not`/`not-join`), and disjunction (`or`). Rules (named query
  fragments with recursion) are out: they add a solver, a stratification
  check, and a meaningfully larger query compiler for power most embedded
  users do not draw on.

- **SQL query interface.** SQL presumes a server-process planner,
  cursor state, and a relational algebra layer that does not match an
  embedded Lispy runtime. The host that wants SQL points a real SQL
  engine at its own data; mino does not impersonate one.

- **Schema-as-data (`:db.install/attribute`).** Datomic-style schema
  installed by transacting meta-facts into the store itself is a rich
  mechanism mino does not need. Schema-as-map, supplied at open time, is
  simpler to read, simpler to validate, and sufficient for the indexed
  lookups the store actually performs.

- **Transactor and peer-client split.** The transactor/peer
  architecture is how Datomic separates writers from readers across
  processes. mino has one state per store, no network, and no
  transactor process; the split solves a problem mino does not have.
  Ref ADR 10.

- **MVCC and snapshot isolation across processes.** Within a state the
  persistent-tree db value already gives cheap snapshot reads. Across
  processes there is no shared state to isolate; cross-runtime transfer
  goes through `mino_clone`, not a lock manager. Ref ADR 10.

- **Native vector search and ANN indexes.** Embeddings and similarity
  queries belong to specialized vector databases (Chroma, Qdrant,
  LanceDB, pgvector). mino.store's indexes are exact hash-based
  lookups; similarity retrieval is brokered by the host against an
  external engine. Ref ADR 15.

- **Binary snapshot format for v1.** EDN text is the stable on-disk
  format. A binary encoding stays reserved (`0x01`) but unshipped until
  profiling data or embedder demand justifies the encoder, the dual read
  path, and the migration tooling. Ref ADR 16.

- **Full-text search.** Inverted indexes, tokenizers, stemming, and
  ranked retrieval are a separate domain. The host brokers an external
  search engine (Lucene, Tantivy, SQLite FTS, an external Postgres) and
  joins on entity IDs, the same composition pattern as vector search.

- **GraphQL or REST API.** mino is an embedded runtime, not a server.
  There is no HTTP listener, no request router, no serialization layer
  over the wire. An embedder that wants a network facade builds it in
  the host and calls into mino over the C ABI.

- **Automatic schema inference from data.** Schema is explicit at open
  time: the host declares attributes, types, and cardinality before
  transacting. Inferring schema from incoming facts would make
  validation order-dependent and silently lock in types that the first
  transaction happened to carry.
