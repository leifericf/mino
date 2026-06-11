# mino prose style — the writing standard

Applies to everything written for humans: decision records
(`docs/adr/`), the policy docs, changelog entries, commit messages,
skill bodies, docstrings, and code comments. The existing exemplars
are `docs/MAINTAINER_TOOLCHAIN.md` and the v0.423.x changelog
sections — match them.

## Voice

- Plain, direct, technical. No metaphors, no codenames, no cleverness
  in names or headings — a reader six months from now gets the plain
  meaning on first read.
- Active voice, present tense for current behavior ("the gate fails
  when..."), past tense for history ("the spike showed...").
- State constraints and effects, not narrative. "snprintf returns the
  would-be length, not what was written" beats "we discovered an
  interesting issue with snprintf".
- No marketing adjectives (powerful, robust, simple, blazing). If a
  property matters, state the measurement or the mechanism instead.
- Never "hand-written" or "hand-rolled" in any public-facing text.

## Succinctness

- Cut anything the reader can derive, keep everything they would
  otherwise have to rediscover. The test: would deleting this
  sentence cost a future reader a wrong decision or a re-derivation?
  If not, delete it.
- One idea per sentence; one topic per paragraph. Lists for
  enumerable facts, prose for reasoning — not the reverse.
- Concrete beats general: name the file, the flag, the number, the
  commit. "~90 sites, every one an intentional const-drop" carries
  more than "many sites were examined".
- Don't restate what an adjacent artifact already says — cite it by
  path and add only what's new here.

## Decision records (docs/adr/)

- Fields: Title, Date, Context, Decision, Consequences, Alternatives.
  No status field; a decision stands until a later record supersedes
  it by name.
- Title is the decision itself, imperative or declarative — readable
  in the index without opening the file ("Defer PGO until zig ships
  the profile runtime", not "PGO investigation").
- Context is neutral: the facts and constraints as they stood, no
  foreshadowing of the answer.
- Decision is one paragraph, what and how, present tense.
- Consequences include the costs — a record listing only upsides is
  advertising, not a record.
- Alternatives get their real strengths stated before the rejection;
  a strawman alternative documents nothing.
- Whole record fits in one screenful (~40-60 lines). If it can't,
  the decision is probably several decisions.

## Changelog entries

- Written for users of mino, not maintainers: state the new
  observable behavior, then the old behavior or cause if it earns
  its space. Lead with the user-visible effect.
- `Category: ...` prefix matching the commit category, so sections
  group and scan.
- A bullet is 1-5 lines; mechanism detail beyond that belongs in the
  commit body or an ADR.

## Commit messages

- The full form is in
  `.claude/skills/implement-change/references/worktree-model.md`
  (category first, imperative sentence-case subject, effect not
  diff, trailers at the bottom).
