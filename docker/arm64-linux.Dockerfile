# ARM64 Linux build image for end-to-end CI of the mino cpjit
# stencil headers on a native AArch64 Linux toolchain.
#
# On an Apple Silicon dev host this runs natively via the
# macOS Virtualization framework (no qemu). On a GitHub
# `ubuntu-24.04-arm` runner it is also native.
#
# The image only installs what `mino task release-gate` needs:
# gcc + make + pthread / libm headers. No shell helpers; the
# orchestration lives in `mino task ci-matrix`.
FROM arm64v8/ubuntu:24.04

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates \
        gcc \
        libc6-dev \
        make \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /mino

# The repo is bind-mounted as a read-write volume by `mino task
# ci-matrix`. The default entrypoint runs the release-gate; the
# matrix driver overrides it for one-off probes.
CMD ["/bin/sh", "-c", "make && ./mino task release-gate"]
