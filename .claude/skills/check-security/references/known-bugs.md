# Known bug patterns — mino's real defect history

Every entry below shipped and was fixed. Reviewers (`check-security`,
`check-memory`): treat these as the highest-prior-probability defect
classes — look for the *pattern*, not just the fixed site. Grows via
the `incorporate-feedback` skill; cite the commit when adding one.

## GC-unsafe allocation windows

**Pattern:** a raw C buffer or freshly built value is held across a
`gc_alloc_typed`/`alloc_val` call without `gc_pin` or a `gc_depth`
guard. The conservative stack scanner usually saves you — until the
optimizer keeps the pointer only in a register.

- `mino_string_n` held the data buffer across the `dup_n`→`alloc_val`
  pair; fixed with a `gc_depth` guard matching
  `intern_lookup_or_create` (commit b99fa6a).
- Repro idiom: `MINO_GC_STRESS=1 ./mino -e '...'` makes every
  allocation collect; any unpinned temporary dies immediately.

**Check:** every allocation between obtaining and storing a GC-visible
pointer; every helper that allocates twice.

## snprintf return value misused as a length

**Pattern:** `snprintf` returns the *would-be* length, not what was
written. Passing it to `memcpy`/`memmove` as the source size reads
past the stack buffer when the formatted value exceeds the buffer.

- `(format "%200d" 5)` read past `prim_format`'s 64-byte int buffer;
  the float branch had the same bug with its 128-byte buffer (commits
  978dbfb, 4344cea).

**Check:** every `snprintf`/`vsnprintf` whose return value flows into
a copy, allocation size, or index. Clamp to the buffer size.

## Symlink traversal in filesystem walks (CWE-59)

**Pattern:** classifying directory entries with `stat()` follows
symlinks, so a symlink-to-directory inside a tree being removed or
walked drags the *target* into the operation.

- `rm-rf` descended through a symlink and deleted the link target's
  contents; `file-seq` had the same flaw (commit 7a345f9). Fixed with
  `lstat()` — under `_WIN32` guards, since Windows has no `lstat`
  (commit ec84331).

**Check:** every `stat` in a recursive walk; every delete/copy that
can encounter attacker-placed links.

## realloc and error-path leaks

**Pattern:** `p = realloc(p, n)` leaks the original block on failure;
early-error returns leak partially built state.

- String builders leaked on realloc failure (f8038b4); `sh`/popen
  buffer growth routed through a temp (f530fe8); a batch of
  realloc/error-path leaks surfaced by the static analyzer (6af5669);
  worker `ctx->last_diag` leaked on throw (8b0a934).

**Check:** every `realloc` (must use a temp); every error path out of
a function that allocated — what owns each allocation at that line?

## Integer overflow in size arithmetic

**Pattern:** size-doubling growth (`cap * 2`, `n + m`) overflows
`size_t`/`int` and wraps to a small allocation followed by a large
write. Also: parser repetition counts and literal values overflowing
into UB instead of saturating or promoting.

- Buffer-growth guards added across io.c format buffers (90713d7,
  00159c4), read.c element arrays (94c960f), generic doubling paths
  (aab5b2d, 06fa42a); regex `{n,m}` counts now saturate (af2f046);
  vm.c arithmetic fallbacks had signed-overflow UB (95badfd); Windows
  QPC time conversion overflowed int64 after ~15 min uptime (2b8601d);
  overflowing integer literals now promote to bigint (20d3074,
  3a6aaee).

**Check:** every multiplication or addition that feeds
`malloc`/`realloc`/`memcpy`; every parser counter with user-controlled
input; signed arithmetic on values from user data.

## Verifying a finding

A memory/security finding is strongest with a repro:

    ./mino -e '<expression that triggers it>'
    MINO_GC_STRESS=1 ./mino -e '...'     # GC-window bugs
    ./mino task build-asan && MINO_TEST_BIN=./mino_asan ...  # ASan proof

Reviewers don't run these (read-only) — put the candidate repro in the
finding's `:suggestion` so the editor/verifier can confirm it.
