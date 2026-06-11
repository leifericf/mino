---
name: write-changelog
description: Recipe for changelog entries - when a change deserves one, the entry voice, the category-first form, and the release-cutting ritual. Invoked by editors and writers when authoring proposal lines, and at release time.
user-invocable: false
---

# write-changelog

The changelog is written for users of mino; the commit log is written
for maintainers. Same change, two audiences. Voice rules:
`.claude/skills/check-style/references/prose-style.md` ("Changelog
entries"). Mechanics: lines travel as proposal EDN and the spine
serializes them (ADR 05) — never edit CHANGELOG.md directly during
parallel work.

## Does it deserve an entry?

Yes: behavior a user or embedder can observe changed — new surface,
a fix to wrong output/crash/leak, performance a user would notice,
anything security-relevant (always), removed or changed surface.

No: refactors, comment/doc fixes, test additions, CI/toolchain work,
skill-system changes. Those live in commit messages. An empty
`:changelog []` in a proposal is normal and correct.

## The entry

- One line in the proposal, `Category: <new observable behavior>`,
  category matching your commit's category so the section groups
  (the spine clusters bullets by prefix).
- Lead with the effect: `(format "%200d" 5)` pads correctly instead
  of reading past the buffer — written as what NOW happens. Old
  behavior/cause follows only if it earns its space.
- 1–5 lines once wrapped; mechanism detail beyond that belongs in the
  commit body or an ADR.
- The changelog talks only about the changes themselves: never
  reference plan or cycle names, round/phase numbers, finding or task
  IDs, or `.local/` working files. (A tool's documented output path
  is feature behavior and is fine.) That provenance lives in commits,
  run dirs, and ADRs.

## Cutting a release

At release time (maintainer-driven):

```
./mino tools/merge_proposals.clj cut-release --version vX.Y.Z --title "Short Theme"
```

- The title names the release's theme the way the existing sections
  do (`Security Fixes`, `Windows Timing and Bigint Fixes`) — derive
  it from the dominant category of the Unreleased bullets.
- The command refuses an empty Unreleased; the next merge starts a
  fresh one above the cut section.
- Read the section once after cutting: bullets are grouped by
  category; if one bullet reads as a maintainer note rather than a
  user effect, fix it now — this is the last cheap moment.
