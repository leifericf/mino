# ADR 47: Defer markdown; no reader in the standard distribution

Date: 2026-09-03

## Context

Markdown is the lingua franca of docs and readme files, and doc or
site tooling written in mino would use a parser immediately. A few
scripting runtimes have recently bundled one. Parsing it well is
medium-sized work: the format has a long specification with nested
container blocks, lazy continuation lines, emphasis-delimiter
resolution, and raw HTML passthrough, and markdown files are commonly
third-party input, which in this codebase makes the reader a native
prim (the ADRs 23 to 28 rule). The structured-text readers mino
already ships (json, csv, toml, yaml, xml/html) are data formats a
script must parse to function; markdown is a document format whose
main consumers are site generators and doc pipelines, a narrower
niche the runtime does not otherwise serve.

## Decision

No markdown reader ships in the standard distribution now. The
question is deferred with its shape fixed: when a reader lands, it is
a native prim parsing to plain data in the hickory-style shape the
html reader established (ADR 28), specification-conformant, behind
the existing reader capability conventions, and it earns its place
through a concrete doc-tooling workload in this ecosystem rather than
through the observation that other runtimes bundled one. Until such a
workload exists, mino scripts that need markdown shell out to any of
the ubiquitous converters or parse the subset they need.

## Consequences

- Doc and site tooling in mino needs an external converter for the
  general case; the html/xml reader covers pipelines whose input is
  already rendered.
- The distribution avoids a medium-cost native parser, its fuzz and
  property obligations, and a specification-tracking burden, for a
  format no current workload consumes.
- The recorded shape means a future implementation starts from a
  settled design (native, plain data, hickory-style tree) instead of
  reopening the reader-placement debate.
- The deferral is revisitable on evidence; it is not the permanent
  exclusion list, where dying protocols and media codecs live.

## Alternatives

- **Ship the native reader now.** Real fit with doc tooling and a
  visible trend among bundled runtimes. Rejected: medium cost against
  a niche the runtime does not yet serve; the trend is weeks old and
  demand here is hypothetical.
- **A pure-mino subset parser.** Cheap, covers readmes. Rejected:
  markdown is third-party input with pathological nesting, exactly
  the reader class this codebase sends native; a subset also invites
  silent divergence from the specification, the worst failure class.
- **Emit-only support.** Writers are trusted-data string building and
  could ship cheaply. Rejected for now with the same evidence bar:
  no workload asks for it, and emit-only markdown is trivial enough
  in user code that bundling it adds little.
