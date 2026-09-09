# ADR 62: Embedding consumers consume the amalgamation, built via mino tasks

Date: 2026-09-09

## Context

mino ships a single-file embedding distribution through `./mino task
amalgamate`: `dist/mino.c` (one translation unit inlining the whole
runtime and its vendored code), `dist/mino.h` (the sole public header),
plus the README and third-party license files. It is the embedding
boundary mino chose; `dist/` is gitignored and regenerated on demand,
bit-identical to the source tree at its pin.

Despite that boundary, the four embedding consumers (mino-lsp,
mino-nrepl, mino-examples, mino-bench) compiled mino from its private
`src/**` tree, each duplicating mino's source globs, the full
`-Imino/src/...` include set, the link flags, and the
`bundled.list`-to-`src/generated` header generation. Every mino C-tree
reorganization broke all four at once; it just did, at the
2026.09.09-alpha1 release. All four pin mino as a git submodule at the
same tag and already build mino from source. mino itself dogfoods its
build: a small bootstrap Makefile turns a clean checkout into `./mino`,
and every other build, test, and release task lives in the mino task
runner. The consumers had drifted from that pattern into hand-maintained
Makefiles that re-encoded mino's private layout.

## Decision

Every embedding consumer consumes the amalgamation and drives its build
with mino tasks. A consumer keeps no Makefile of its own. Bootstrap is
`cd mino && make` (mino's own bootstrap Makefile), then
`./mino/mino task build` runs a Clojure task (`mino.edn` plus
`lib/<consumer>/tasks.clj`) that materializes `dist/` from the pinned
submodule once per pin, compiles `dist/mino.c` to `dist/mino.o`, and
links the consumer's own sources against it with `-Imino/dist` and
`-lm -lpthread`, including `dist/mino.h` as the only mino header. The
regeneration is keyed to the submodule's checked-out commit via a
`dist/mino.o` pin sentinel, so a pin bump forces a rebuild and an
unchanged pin does not. Consumers hold zero knowledge of mino's private
layout.

## Consequences

A future mino C-tree reorganization is a no-op for every consumer build
once the pin is bumped; the amalgam absorbs it. Consumers dogfood the
task runner the way mino itself does, and no consumer Makefile survives
to drift. First cold build per pin pays the bootstrap plus amalgamate
cost once, bounded by the sentinel; incremental builds skip it. Each
consumer needs a working mino build toolchain at build time, which it
already had. The `dist/THIRD_PARTY_LICENSES.md` manifest now travels
with every consumer. The amalgam machinery (the pin sentinel and its
`ensure-dist!` helper) is currently copied into each consumer's
`tasks.clj`; if that duplication is measured to hurt, a later cycle
ships it once as a mino task namespace the consumers require through the
submodule.

## Alternatives

Publish `dist/` as a downloadable release asset. Decouples consumers from
the mino build toolchain and speeds cold builds, but adds
release-pipeline surface and a way for a consumer's `dist/` to drift from
its pin. Rejected: generate-on-demand needs no new infrastructure and
cannot drift from the pin.

Commit the amalgam into each consumer. Fastest build, no toolchain
needed, but commits an ~11.5 MB generated artifact per repo, invites
hand-edits, and drifts silently. Rejected.

Keep the Makefiles but point them at `dist/`. Removes the src-glob
coupling but leaves four hand-maintained build files that still drift on
the recipe, and abandons mino's dogfood pattern. Rejected: the task
runner is how mino builds everything above bootstrap.

A vendored `libmino.a` or shared library with pkg-config. A conventional
C distribution, but it competes with mino's existing single-file amalgam
choice and re-introduces an export surface for internals. Rejected: mino
already chose the amalgam as its embedding boundary.
