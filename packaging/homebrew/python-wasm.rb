class PythonWasm < Formula
  desc "CPython 3.14 interpreter compiled to WebAssembly (WASI Preview 2)"
  homepage "https://github.com/tegmentum/python-wasm"
  version "0.1.0"
  license "PSF-2.0"

  on_macos do
    on_arm do
      url "https://github.com/tegmentum/python-wasm/releases/download/v#{version}/python-wasm-#{version}-darwin-arm64.tar.gz"
      sha256 "REPLACE_ME_DARWIN_ARM64_SHA256"
    end
    on_intel do
      url "https://github.com/tegmentum/python-wasm/releases/download/v#{version}/python-wasm-#{version}-darwin-x86_64.tar.gz"
      sha256 "REPLACE_ME_DARWIN_X86_64_SHA256"
    end
  end

  on_linux do
    on_arm do
      url "https://github.com/tegmentum/python-wasm/releases/download/v#{version}/python-wasm-#{version}-linux-arm64.tar.gz"
      sha256 "REPLACE_ME_LINUX_ARM64_SHA256"
    end
    on_intel do
      url "https://github.com/tegmentum/python-wasm/releases/download/v#{version}/python-wasm-#{version}-linux-x86_64.tar.gz"
      sha256 "REPLACE_ME_LINUX_X86_64_SHA256"
    end
  end

  depends_on "wasmtime"

  def install
    # The tarball expands to python-wasm-<version>-<platform>/{bin,lib,share,VERSION}.
    # Strip the leading directory by globbing the inner content.
    bin.install   "bin/python-wasm"
    lib.install   Dir["lib/*"]
    share.install Dir["share/*"]
    (prefix/"VERSION").write(version.to_s)
  end

  def caveats
    <<~EOS
      python-wasm runs CPython on top of wasmtime. To install extensions:

        python-wasm pip install <pkg>
        python-wasm rebuild            # if the wheel ships a bridge

      User state (rebuild output, installed wheels) lives in
      ~/.python-wasm/. Override with PYTHON_WASM_HOME.

      See `python-wasm --help` for the full subcommand list.
    EOS
  end

  test do
    assert_match version.to_s, shell_output("#{bin}/python-wasm version")
    assert_match "Python 3.14", shell_output("#{bin}/python-wasm -c 'import sys; print(sys.version)' 2>&1")
  end
end
