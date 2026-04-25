class Mino < Formula
  desc "Tiny, embeddable Lisp in pure ANSI C"
  homepage "https://mino-lang.org"
  version "__VERSION_NUMBER__"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/leifericf/mino/releases/download/v__VERSION_NUMBER__/mino_darwin_arm64_v__VERSION_NUMBER__.tar.gz"
      sha256 "__SHA_DARWIN_ARM64__"
    end
    on_intel do
      url "https://github.com/leifericf/mino/releases/download/v__VERSION_NUMBER__/mino_darwin_amd64_v__VERSION_NUMBER__.tar.gz"
      sha256 "__SHA_DARWIN_AMD64__"
    end
  end

  on_linux do
    on_arm do
      url "https://github.com/leifericf/mino/releases/download/v__VERSION_NUMBER__/mino_linux_arm64_v__VERSION_NUMBER__.tar.gz"
      sha256 "__SHA_LINUX_ARM64__"
    end
    on_intel do
      url "https://github.com/leifericf/mino/releases/download/v__VERSION_NUMBER__/mino_linux_amd64_v__VERSION_NUMBER__.tar.gz"
      sha256 "__SHA_LINUX_AMD64__"
    end
  end

  def install
    bin.install "mino"
  end

  test do
    assert_match version.to_s, shell_output("#{bin}/mino --version")
    assert_equal "3", shell_output("#{bin}/mino -e '(+ 1 2)'").strip
  end
end
