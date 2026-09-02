# Decision records

Lightweight records of architecture decisions: a text file with
context, decision, consequences, and the alternatives weighed, written
when the decision is made, while the context is still cheap to state.
No status machinery; a decision stands until a later record supersedes
it by saying so. Recording ritual: the `record-decision` skill
(`.claude/skills/record-decision/`).

| # | Decision |
|---|----------|
| [01](01-c99-only-no-extensions.md) | Any ANSI C compiler + make builds mino: C99, no extensions |
| [02](02-zig-is-a-toolchain-not-a-source-language.md) | Zig is a toolchain, never a source language |
| [03](03-defer-pgo.md) | Defer PGO until zig ships the profile runtime and tooling |
| [04](04-curated-lint-set.md) | The lint lane gates a curated warning set, not -Weverything |
| [05](05-changelog-via-proposal.md) | Changelog lines travel as proposal EDN, serialized at land time |
| [06](06-drop-docker-ci-matrix.md) | Drop the local Docker CI mirror; zig cross-build + qemu covers it |
| [07](07-no-aot-compilation.md) | No AOT compilation: the tiers stay interpreter, bytecode VM, and runtime JIT |
| [08](08-one-options-entry-point.md) | One options entry point for per-state config knobs |
| [09](09-prims-are-static-tables-are-the-registry.md) | Primitives are static; the registration tables are the registry |
| [10](10-eavt-fact-store.md) | EAVT fact store, per-state isolated, no cross-runtime shared state |
| [11](11-store-on-disk-format.md) | Store on-disk format: EDN text with version header, line-delimited WAL |
| [12](12-save-lisp-and-die.md) | Save-lisp-and-die: value-serialization image with identity table |
| [13](13-references-and-reverse-indexes.md) | References and reverse indexes: `:ref` type, lazy reverse index, nil-on-retraction |
| [14](14-schema-migration.md) | Schema migration via `store/migrate`, validation-gated publish with `:coerce` and `:force` |
| [15](15-vector-embedding-scope.md) | Vector embedding and similarity search are out of scope for mino.store |
| [16](16-binary-snapshot-format.md) | EDN text is the stable on-disk format for mino.store v1 |
| [17](17-slad-forward-compatibility.md) | SLAD forward compatibility via skip-and-warn, plus an offline migration tool |
| [18](18-analyze-zig-baseline-is-ci-specific.md) | The analyze-zig baseline is CI-specific (platform-conditional analyzer output) |
| [19](19-census-as-source-of-truth.md) | clojure-census is the source of truth for the Clojure surface and mino divergences |
| [20](20-one-http-client-plain-data-vendored-tls.md) | One HTTP client, plain data in/out, vendored TLS |
| [21](21-time-date-own-core-epoch-ms.md) | Time and date: own C99 core, epoch-ms instants, plain maps |
| [22](22-path-lib-strings-and-glob.md) | Path library: paths are strings, unix-shell names, one glob walker |
| [23](23-json-reader-in-c-writer-stays-clojure.md) | The JSON reader is native C; the writer stays Clojure |
| [24](24-csv-reader-in-c-writer-stays-clojure.md) | The CSV reader is native C; the writer stays Clojure |
| [25](25-toml-reader-in-c.md) | The TOML reader is a native prim over byte indices |
| [26](26-yaml-subset-reader-first.md) | The YAML reader is a native subset prim (block/flow core, no anchors) |
| [27](27-timezones-over-embedded-tzdata.md) | Timezones over embedded compact tzdata generated at build time |
| [28](28-html-xml-native-tokenizer-two-modes.md) | HTML and XML readers: one native tokenizer, two modes, hickory/JVM shapes |
| [29](29-compression-zip-native-core.md) | Compression write side and zip container: native end to end over vendored miniz |
| [30](30-young-only-range-index-for-minors.md) | Minor collections touch a young-only range index |
| [31](31-unicode-case-tables.md) | Unicode case mappings from vendored tables generated at build time |
| [32](32-classed-catch-kind-dispatch.md) | Classed catch clauses dispatch on diagnostic kind |
| [33](33-var-based-namespace-env.md) | Namespace environments bind vars uniformly |
| [34](34-c-prim-arglists.md) | C prims carry oracle arglists as var metadata |
| [35](35-store-backend-seam.md) | Store backends are plain maps of fns behind a five-op seam |
| [36](36-http-server-ring-maps-over-net-prims.md) | The HTTP server is Ring maps over the net prims |
| [37](37-keyword-catch-classes-match-kind.md) | Keyword catch classes match diagnostic kind by equality |
| [38](38-errors-as-values-and-message-quality.md) | Errors as values, and best-in-class messages |
| [39](39-conformance-model-routing.md) | Conformance generation and triage inherit the session model |
| [40](40-conformance-probe-lane-ownership.md) | Conformance probe stays in the tests repo; lanes reach it via MINO_BIN |
| [41](41-websocket-native-codec-over-net-prims.md) | One websocket surface, mino.ws over the net prims, codec native |
| [42](42-sqlite-behind-the-store-seam.md) | mino.store is the in-box database; sqlite waits behind the seam |
| [43](43-file-watching-polls-stat.md) | File watching is a polling fn over stat, no native watchers |
| [44](44-unicode-normalization-generated-tables.md) | Unicode normalization and case folding via generated tables |
