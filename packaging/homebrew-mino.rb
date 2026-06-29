# Homebrew formula template for mino.
#
# To publish: create a GitHub release with a source tarball, compute the
# SHA256, fill in the url/sha256 below, and submit to a Homebrew tap.
# Consumers then run: brew install <tap>/mino
#
# mino builds with a C-only bootstrap (make + cc). No working mino binary
# is needed to build; the Makefile drives cc over the C sources directly.

class Mino < Formula
  desc "C runtime hosting a Clojure dialect with an EAVT store and SLAD images"
  homepage "https://github.com/leifericf/mino"
  # url "https://github.com/leifericf/mino/archive/refs/tags/v0.1.0.tar.gz"
  # sha256 "FILL_IN_RELEASE_SHA256"
  version "0.1.0"

  # Build-time dependencies: a C compiler and make are standard on macOS/Linux.
  # No runtime dependencies (mino is a single static binary).
  uses_from_macos "bc" => :build

  def install
    # Bootstrap build: regenerates bundled headers and compiles ./mino.
    # C-only; never requires a working mino to build.
    system "make"

    # Install the binary.
    bin.install "mino"
  end

  # Verification: mino can evaluate a simple expression.
  test do
    assert_equal "42\n", shell_output("#{bin}/mino -e '(prn 42)'")
  end
end
