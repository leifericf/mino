# The generation-split range index

The GC range index maps payload address spans to owning headers for
the conservative stack scan and interior-pointer marking. Since ADR 30
it is split by generation: a young pair `ranges_y` + `ranges_y_pending`
that minor collections maintain alone, and an old pair `ranges_o` +
`ranges_o_pending`. The index lives in `src/gc/ranges.c`;
`src/gc/roots.c` keeps root enumeration and the conservative scans.

Why: the unified index compacted at O(live headers) every minor. Under
a big live tree (the json corpus parse shape, 290k+ entries) every
minor walked the whole index while the live young set stayed
nursery-sized: old-proportional minor pauses. The split makes the
per-minor merge and compact walk young data only; a promoted header's
entry rides one cycle in the young array and routes to the sorted old
pending buffer at the next compact, which folds into the old array at
the next index rebuild (or early, once it rivals the old array).

The `gc-stats` surface exposes the split: `:ranges-young-len`
(young array plus its pending buffer), `:ranges-old-len` (the folded
old array), `:ranges-old-pending-len` (promoted entries awaiting a
fold), and `:ranges-len` (the sum across all four buffers).
`:range-walk-entries` counts index entries examined by the per-minor
merge and compact; its per-collection delta is the young-bounded cost
signal `tests/gc_test.clj` pins.

## Measured effect

Measured 2026-08-27 on a quiet dev mac (arm64) and the Docker glibc
probe images. Before is the unified index at `65a13a43`; after is the
split at `490bcf6a`; tip-column numbers are at `838868ff` (23 later
non-GC commits sit between; A/B pairs isolate the split). Commands:
per side, a scratch checkout, `make`, then `time ./mino tests/run.clj`
(mac) or `docker run -v <probe-tree>:/src -w /src <image> sh -c
'MINO_TEST_SHARD=k/3 ./ops/rss_probe.sh ./mino tests/run.clj'`
(glibc VmHWM peak; force a clean container build per side, stale
binaries survive an rsync refresh).

Minor-collection index cost, json-corpus probe (10k-entry doc held
live, young churn, 4 minors):

| Signal | Unified | Split |
|--------|---------|-------|
| Walk entries per minor | 682,412 | 176,852 |
| Walk per minor / ranges-len | 2.36 | 0.61 |
| Walk per minor at a 30k doc | proportional (2.36x len) | 176,851 (unchanged from 10k) |

The 30k row is the point: tripling the live old tree leaves the
per-minor walk byte-identical. The residual walk is the young census
itself (churn allocations), nursery-bounded.

Full-suite wall time, mac, back-to-back quiet runs:

| Build | Wall | User |
|-------|------|------|
| Unified `65a13a43` | 343.0 s | 313.3 s |
| Split `490bcf6a` | 297.3 s | 261.4 s |
| Split at tip, 256 KiB nursery | 1739 s | 7 assertion fails, all ms-budget |

Peak RSS, Docker glibc VmHWM, shard peaks (N runs per side):

| Shard | Unified | Split |
|-------|---------|-------|
| 1/3 amd64 | 1.95 GiB | 1.92 GiB |
| 2/3 amd64, N=20 | median 2.94 GiB, max 3.35 | median 2.95 GiB, max 4.96 (one run; next 3.36) |
| 2/3 aarch64, N=3 | 2.93 GiB | 2.95 GiB |
| 3/3 amd64 | OOM-kill 7.15 GiB | OOM-kill 7.19 GiB |

The split does not move peak RSS. Shard 2 peaks cluster by generative
seed (about 1.3 / 2.0 / 2.9 / 3.4 GiB, one 4.96 GiB outlier) on both
sides: the drivers are workload allocations in the json and regex perf
files, not index size. The split's fixed cost is about 20 MiB (the
duplicated index buffers; visible in the aarch64 pair). The shard 3
ceiling is the zip tail working set, split-independent. A pre-split
3.49 GiB tracker figure for shard 2 does not reproduce in this
environment; the common cluster sits under the 3 GiB gate on both
sides and the gate is straddled by seed variance, not by the index.
RSS reduction work therefore belongs to the perf-file workloads, not
to the collector.
