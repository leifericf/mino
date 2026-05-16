# x86_64 Linux build image for end-to-end CI of the mino cpjit
# stencil headers on a native amd64 Linux toolchain.
#
# On an Apple Silicon dev host this runs via Rosetta 2 / qemu
# (~4-8x slower than native; functional). On a GitHub
# `ubuntu-24.04` runner it is native.
#
# The image only installs what `mino task release-gate` needs:
# gcc + make + pthread / libm headers. No shell helpers; the
# orchestration lives in `mino task ci-matrix`.
FROM amd64/ubuntu:24.04

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates \
        gcc \
        libc6-dev \
        make \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /mino

CMD ["/bin/sh", "-c", "make && ./mino task release-gate"]
