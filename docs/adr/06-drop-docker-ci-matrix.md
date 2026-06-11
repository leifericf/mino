# ADR 06: Drop the local Docker CI mirror; zig cross-build + qemu covers it

Date: 2026-06-11

## Context

`./mino task ci-matrix` built two Ubuntu images (`docker/*.Dockerfile`,
linux/arm64 + linux/amd64) and ran `release-gate` inside each — a
local mirror of the CI matrix from before the zig toolchain lanes
existed. By 2026-06 the zig axis provided `cross-build` (Linux
amd64/arm64 + Windows artifacts from one host), `test-cross-qemu`
(executing the arm64 artifact + full suite under binfmt),
`check-binary-reproducible`, and CI gained a native arm64 runner. The
Docker path required a running daemon, multi-GB images, and
Rosetta/qemu-in-Docker emulation that was slower than qemu-user
directly.

## Decision

Delete `docker/` and the `ci-matrix` task. Local cross-target
verification is `cross-build` + `test-cross-qemu`; the GHA matrix
remains the authoritative gate. (CI's `docker/setup-qemu-action` is
an unrelated GitHub action and stays.)

## Consequences

- No Docker dependency anywhere in the maintainer workflow.
- Local arm64 verification executes the actual release artifact
  (static musl binary under qemu-user) instead of a container
  approximation of CI.
- Coverage difference accepted: ci-matrix exercised release-gate
  inside a clean Ubuntu userland; cross+qemu exercises the artifact
  itself. The clean-environment property is CI's job.

## Alternatives

- **Keep both** — two overlapping local mirrors to maintain, with
  the Docker one slower and heavier. Rejected.
- **Containerize the zig lanes instead** — adds the Docker
  dependency back for hermeticity zig already provides (pinned
  compiler, musl static linking). Rejected.
