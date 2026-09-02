# ADR 46: Defer password hashing primitives

Date: 2026-09-03

## Context

Password hashing (a memory-hard or cost-parameterized scheme such as
argon2 or bcrypt) is the correct answer wherever passwords are stored,
and some scripting runtimes now bundle one. mino's crypto surface is
deliberately narrow: hashes, HMAC, and a TLS client, plus OS entropy
via `secure-rand-bytes`. That boundary exists because a bundled crypto
suite turns every scheme weakness into a runtime CVE, the failure mode
that pushed other stdlibs to expel their servers and ciphers. mino's
target user writes scripts and embeds a small runtime; storing user
passwords means running an auth service, which is application
territory, not scripting territory. No demand from an actual mino
workload has asked for it.

## Decision

mino ships no password hashing primitive. The crypto boundary stays
hashes, HMAC, TLS client, and OS entropy. This is a deferral with a
named trigger, not a permanent exclusion: if auth-adjacent scripting
demonstrably enters scope (a real workload storing credentials with
mino, not a checklist comparison against other runtimes), a new record
weighs which scheme to vendor and how to gate it. Until that record
exists, the question is settled and is not reopened by the observation
that other runtimes bundle one.

## Consequences

- A script that must verify or store passwords shells out to a system
  tool or links the embedder's own crypto; mino does not pretend to
  cover auth.
- The runtime carries no cost-parameter defaults to keep current and
  no scheme implementation to audit; the vendored-crypto surface
  stays the TLS client it already is.
- The general-purpose hashes remain unsuitable for passwords, and
  their docs should keep saying so; the absence of a password prim is
  the honest signal, where a half-measure (fast hash plus salt
  helper) would be a trap.
- The trigger is recorded, so a future request meets a stated bar
  instead of restarting the debate.

## Alternatives

- **Vendor one scheme now.** Small C, precedent in other runtimes,
  and it removes a class of do-it-yourself mistakes for anyone who
  does store passwords. Rejected: it invites auth workloads onto a
  runtime that offers none of the rest of that stack, and it puts a
  security-critical, parameter-sensitive implementation on the audit
  surface for a use no current workload has.
- **Pure-mino implementation.** No new C. Rejected: memory-hard
  hashing is exactly the workload an interpreter mis-serves; slow
  hashing at the wrong layer weakens the cost parameter it exists
  for.
- **Permanent exclusion.** Cleaner than a deferral. Rejected: the
  embedding story could legitimately grow an auth-adjacent niche;
  a trigger costs nothing and keeps the record honest.
