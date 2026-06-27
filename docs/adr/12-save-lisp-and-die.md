# ADR 12: Save-lisp-and-die — value-serialization image with identity table

Date: 2026-06-27

## Context

Traditional Lisp implementations (SBCL, ECL, Clozure CL) offer
"save-lisp-and-die" (SLAD): dump the entire in-memory state to a file,
terminate, and later restart from that file with the exact same state —
all functions, variables, loaded libraries, and data structures intact.

mino needs this for two use cases:

1. **Per-player runtime persistence.** A game host gives each player a
   `mino_state` for their AI scripts, inventory, quest state, and
   custom-defined behaviors. On logout, the host saves the image; on
   login, it loads the image into a fresh state. The store (ADR 10)
   handles the *data* layer; SLAD handles the *code* layer — runtime-
   defined functions, closures, atoms, namespace structure.

2. **REPL session persistence.** A developer builds up a working state
   interactively (defines functions, transacts into stores, loads
   libraries) and wants to quit without losing it.

A codebase exploration (two parallel explore-agent sweeps) revealed
that mino's runtime state is **substantially serializable**: all
GC-managed heap values are reachable from a small set of roots
(`gc_mark_roots` in `src/gc/roots.c` enumerates them), bytecode is
self-contained (`uint32_t[]` + const pool), and namespaces live in
flat arrays. The JIT cache problem is already solved by
`mino_jit_invalidate`. The hard parts are: identity preservation,
C-function-pointer re-resolution, and the quiesce protocol.

## Decision

Implement SLAD as **value-serialization with an identity table**,
not a heap dump.

### Architecture

**Serialize** (C, ~500 lines):

1. **Quiesce gate.** Verify the runtime is at a top-level rest state:
   no active try/catch frames, no in-flight futures or agent actions,
   no pending STM transactions, no active channel operations.
2. **JIT flush.** Walk every reachable `MINO_FN` and call
   `mino_jit_invalidate`. Native code is reproducible from bytecode;
   serializing it would embed process-specific machine code.
3. **Root walk + ID assignment.** Walk all GC roots
   (`ns_env_table`, `var_registry`, `record_types`, `intern tables`,
   `module_cache`, `meta_table`). Assign each reachable `mino_val*` a
   stable integer ID via a hash table (pointer → ID). Shared references
   get one ID; cycles are handled naturally.
4. **Emit.** Write each value as a line: `<id> <type-tag> <payload>`.
   References to other values are written as `#<id>`. Emit side tables
   (namespaces, vars, record types, aliases) after the value pool.
5. **CRC trailer.** Append a CRC32 checksum for corruption detection.

**Deserialize** (C, ~600 lines):

1. **Bootstrap.** The host creates a fresh `mino_state` via
   `mino_state_new()` and runs `mino_install_all(S, env)` to register
   all primitives, core library, and special forms. This re-creates
   the C-function-pointer layer.
2. **First pass — allocate.** Read each value line, allocate a
   `mino_val` of the right type, store it in an `ID → pointer` table.
   Reference fields are left as placeholder sentinels.
3. **Second pass — patch.** Walk every allocated value, replace
   sentinel references with real pointers from the ID table.
4. **Splice.** Replace the fresh state's namespace envs, var registry,
   record types, and aliases with the deserialized ones. Re-intern all
   symbols and keywords to preserve `identical?` semantics.
5. **Post-load fixup.** Repoint all `owning_state` back-pointers to
   the new `S`. Re-resolve `MINO_PRIM` values by name against the
   installed primitive table. Reopen `MINO_STORE` handles from their
   paths (or downgrade to in-memory).

### Image format

Line-delimited text, consistent with the store's WAL format (ADR 11).
Human-readable for debuggability — a developer can `head`, `grep`, or
`diff` an image file.

```
MINO-IMAGE/1                              ; magic + version
# created: 1719480000123                  ; wall-clock at save time
# flags: 0                                ; reserved

VALUES
0 N                                       ; nil
1 T                                       ; true
2 I 42                                    ; int
3 S "Alice"                               ; string (EDN-escaped)
4 K user/email                            ; keyword (ns/name)
5 M 4 2 7 3                               ; map: [key-id val-id]*
6 C 5 0                                   ; cons: car-id cdr-id
7 V 2 3 1                                 ; vector: [elem-id]*
8 A 2                                     ; atom: val-id
9 F 10 11 12 user 0 ...                   ; fn: params-id body-id env-id ns shape bc...
10 ...                                    ; (params vector)
11 ...                                    ; (body forms)
12 ...                                    ; (captured env)

ROOTS
NS user 12                                ; namespace: name env-id
NS clojure.core skip                      ; skip = reinstalled by bootstrap
VAR user/foo 9                            ; var: qualified-name val-id
VAR user/counter 8
TYPE user/Position :x :y                  ; record type: ns/Name fields
ALIAS user str clojure.string             ; alias: owner alias target
CURSOR user                               ; current namespace

CRC32 a1b2c3d4
```

The format is versioned. Future versions may add new type tags or
payload shapes. The reader checks the version and rejects incompatible
images. Version 1 covers all current `MINO_*` types.

### What is saved

| Category | Types | Notes |
|---|---|---|
| **Scalars** | nil, bool, int, char, float, float32, bigint, ratio, bigdec | Trivial |
| **Strings/syms/kws** | string, symbol, keyword | Re-interned on load to preserve identity |
| **Collections** | cons, vector, map, set, sorted-map, sorted-set, queue, map-entry | Recursive; sorted comparator is dropped (revert to default) |
| **Code** | fn, macro | params, body, env, defining_ns, shape. Bytecode is NOT saved; it is recompiled from body on first call after load (see What is dropped) |
| **Mutable refs** | atom, volatile, tx-ref, agent | val, watches, validator, err, identity counters |
| **Stores** | store | db value (serializable map); path; handle is reopened |
| **Records** | record, type | type registry spliced before instances are patched |
| **Namespaces** | ns_env, var, alias | Full side tables |
| **Other** | uuid, regex, bytes, lazy (if realized) | Lazy unrealized C-thunks are forced or dropped |

### What is dropped

| Category | Why |
|---|---|
| **Function bytecode** (`mino_bc_fn`) | Reproducible from the fn's params/body/env; `fn.bc` is set to NULL on load and recompiled by `mino_bc_compile_fn` on first call. Saves space and avoids serializing the const pool / clauses twice. |
| **JIT native code** | Reproducible from bytecode; recompiles on warmup |
| **Inline caches** | Reproducible; re-resolve on first call |
| **GC heap layout** | New heap; identity preserved via ID table |
| **Host handles** (`MINO_HANDLE`) | Opaque host pointers + C finalizers; cannot serialize |
| **Futures / promises** | Embed pthread mutexes + condvars + thread state |
| **Channels** (`MINO_CHAN`) | Embed pthread mutexes + pending queues |
| **Dynamic binding stack** | Per-thread; must be empty at quiesce |
| **Try/catch frames** | Per-thread; must be empty at quiesce |
| **Agent action queues** | Must be drained at quiesce |
| **STM transaction state** | Must be committed at quiesce |
| **Module resolver** | C function pointer; re-registered by host |
| **Trampoline sentinels** | Recreated by `mino_state_new` |
| **Small-int cache** | Repopulated by `mino_state_new` |

### What is patched

| Field | Original | After restore |
|---|---|---|
| `owning_state` (atom, ref, agent, store) | old `mino_state*` | new `mino_state*` |
| `MINO_PRIM.fn` / `.fn2` | old C function pointer | re-resolved by name from the install table |
| `MINO_STORE.handle` | old malloc'd handle | reopened via `mino_store_open(path)` or NULL (in-memory) |
| `fn.bc` | compiled `mino_bc_fn` | NULL; recompiled from body on first call |
| `bc->native` / `hot_counter` | JIT state | NULL / 0 (rewarms lazily) |
| `bc->ic_slots[].cached*` | IC resolutions | zeroed (re-resolves on first call) |
| `MINO_LAZY.c_thunk` | C function pointer | NULL (force-on-save or drop thunk; body+env survive) |

### Quiesce protocol

`mino_save_image` refuses to save if the runtime is not at rest. The
checked conditions (heap/state that cannot be safely captured mid-flight):

1. **No in-flight futures** — `S->async.run_head == NULL`
2. **No pending agent actions** — `S->agent.pool[i].run_head == NULL` for all i
3. **No active STM transaction** — `S->tc->current_tx == NULL`

If any check fails, `mino_save_image` returns -1 with a diagnostic
identifying which check failed. The host resolves the issue (drain
queues, commit transactions) and retries.

Execution state is intentionally NOT gated: an active try/catch frame
(`S->tc->try_stack_top`), dynamic binding stack (`S->tc->dyn_stack`),
and module load stack (`S->module.load_stack_len`) are per-thread
execution state, not heap state. The image captures the current
binding/namespace state as-is; these transient frames do not survive
into the restored image (try/catch frames and the dyn stack are dropped
on load, per What is dropped). A host that wants a fully-clean snapshot
should ensure it calls `mino_save_image` from a top-level rest state;
the runtime does not refuse on their account.

### Trust model

An image file is a **trusted artifact**: the host writes it and loads it
back, much like a core dump. The CRC32 trailer guards against accidental
corruption (truncated writes, bit rot); it is not a cryptographic
authenticator and a missing trailer is tolerated for v0 compatibility, so
it must not be treated as an integrity boundary for an attacker-supplied
file. Loading an image from an untrusted source is unsafe: the value pool
is parsed defensively (bounded lengths, checked allocations) but the
ROOTS section restores arbitrary namespace/var/alias graphs, and a
serialized `MINO_STORE` carries its on-disk path, which is reopened on
load via `mino_store_open` — so a hostile image can direct the loader at
arbitrary snapshot/WAL paths. Only load images the host authored.

### Identity guarantees

| Level | Guaranteed? | How |
|---|---|---|
| **Value equality** (`=`) | Yes | All data is serialized and restored faithfully |
| **Pointer identity** (`identical?`) | Yes | Identity table ensures shared values get one allocation |
| **Interned identity** (symbols, keywords) | Yes | Re-interned into the new state's intern tables |
| **Record type identity** (`instance?`) | Yes | Type registry spliced before instances are patched |
| **Var identity** (`var?`, `resolve`) | Yes | Var registry spliced; vars get new pointers but same ns/name |
| **Behavioral identity** (calling a fn) | Yes (eventually) | Same bytecode → same results; JIT rewarms |
| **Clock identity** (`nanoTime`) | No | New wall-clock on restart; expected and correct |
| **Thread identity** | No | All threads gone; async state lost |
| **Resource identity** (file handles) | No | Must be re-opened by host |

### Public API

```c
/* Save the full runtime state to an image file. Returns 0 on success,
 * -1 on failure (with diagnostic). Refuses to save if the runtime is
 * not quiesced (see quiesce protocol). */
int mino_save_image(mino_state *S, const char *path);

/* Load an image into a pre-initialized state. The host must have
 * called mino_state_new + mino_install_all first. The image's
 * namespace envs, vars, record types, and values replace the
 * baseline state's. Returns 0 on success, -1 on failure. */
int mino_load_image_into(mino_state *S, const char *path);
```

Typical host usage (per-player save/restore):

```c
/* Logout */
mino_save_image(player_state, "players/alice.img");
mino_state_free(player_state);

/* Login */
mino_state *S = mino_state_new();
mino_env *env = mino_env_new(S);
mino_install_all(S, env);
mino_load_image_into(S, "players/alice.img");
/* Alice's fns, atoms, stores, namespaces are back */
```

### Bootstrap sequence (load)

The load path relies on a layered bootstrap:

1. `mino_state_new()` — allocates the runtime, GC, intern tables,
   singletons. State is empty but valid.
2. `mino_install_all(S, env)` — registers all C primitives, core
   library, special forms. This populates `clojure.core` and the
   primitive registry. The C-function-pointer layer is now live.
3. `mino_load_image_into(S, path)` — reads the image file, allocates
   all values, patches references, splices namespace envs and var
   registry into the state.

Step 2 is critical: `MINO_PRIM` values in the image carry names, not
function pointers. During patching, each `MINO_PRIM` is re-resolved by
looking up its name in the freshly installed primitive registry. If a
primitive is not found (e.g., the host installed a different capability
set), the var binding is left as a broken reference — the first call
will throw "unbound."

### Interaction with the store (ADR 10/11)

Stores are values (`MINO_STORE`) and serialize naturally:

- The db value (`as.store.val`) is a persistent map — fully serializable.
- The path (`handle->path`) is a string — serializable.
- The clock function pointer (`handle->clock`) — dropped; defaults to
  wall-clock on reopen.
- The WAL file on disk is untouched by SLAD. On restore, the store is
  reopened from its path, which replays the WAL (see ADR 11). If the
  image was saved after the last checkpoint, the store's in-memory db
  is newer than the snapshot. The reopened store starts from the
  snapshot + WAL replay, which matches the saved in-memory db.

One subtlety: if the host saves the image, then the process crashes
before checkpointing the store, then the host loads the image into a
new process — the new process's store is reopened from snapshot + WAL.
The WAL has all transactions since the last checkpoint, so the
reopened db matches the saved db. Consistent.

## Consequences

- **~1500–2000 lines of C** for the serializer, deserializer, quiesce
  checks, and API. Plus ~300 lines of tests.
- **Image files are proportional to live heap size**, not max heap.
  A player with 100 entities, 50 functions, 20 atoms ≈ 30 KB.
- **Save is O(live values)**; load is O(live values). Both are single-
  pass walks, no quadratic behavior.
- **The quiesce protocol is the host's responsibility.** mino detects
  non-quiesced state and refuses to save, but the host must drain
  queues and commit transactions before calling `mino_save_image`.
- **Host handles and async resources are lost.** The host must re-open
  files, re-connect sockets, and re-establish channels after load. This
  is the same contract as process restart — SLAD is not transparent
  for OS resources.
- **Bytecode + JIT warm-up cost on every restore.** Function bytecode
  is not serialized; each loaded fn starts with `bc == NULL`,
  recompiles from its body on first call, and then rewarms the JIT on
  the hot-path threshold. For a player image with 50 functions, the
  warm-up window is typically < 100ms.
- **The image format is versioned.** The magic string embeds the
  version (`MINO-IMAGE/1`); the reader rejects images whose magic does
  not match. Adding a new `MINO_*` type tag bumps the embedded version.
- **SLAD is orthogonal to the store.** The store persists application
  data (EAVT facts); SLAD persists the runtime (functions, namespaces,
  all mutable refs). A player's store survives via its own snapshot +
  WAL; a player's custom-defined AI function survives via SLAD. Both
  mechanisms coexist without interference.

## Alternatives

- **Heap dump (SBCL-style `mmap`).** Dump the raw GC heap to a file;
  map it back on restart. Rejected: mino's copying GC moves objects
  on every collection, so the heap layout is not stable. The JIT's
  native code is mmap'd executable memory that can't be serialized.
  A heap dump would be platform-specific (endianness, pointer width,
  page size) and would require fixup logic nearly as complex as value
  serialization.

- **Session replay.** Record every form evaluated in the REPL and
  re-evaluate on startup. Rejected: non-deterministic (side effects,
  timestamps, random numbers), slow (recompiles everything), doesn't
  capture runtime-only state (atoms created by functions, stores
  transacted programmatically). Useful as a development convenience
  but not a correct SLAD.

- **EDN-only serialization (no C).** Walk namespaces from Clojure,
  print everything as EDN, reload with the reader. Rejected: the
  reader requires a running state (chicken-and-egg), functions can't
  be printed as EDN (they're `#<fn>` opaque), and Clojure-level code
  can't allocate `MINO_FN` objects directly. The C serializer is
  necessary to walk bytecode, environments, and intern tables.

- **FASL per namespace (incremental save).** Instead of one monolithic
  image, compile each namespace to a FASL file. Rejected: doesn't
  capture mutable state (atoms, stores, agent values). FASL is code-
  only; SLAD needs code + data. Could be a future optimization for
  code-heavy images.
