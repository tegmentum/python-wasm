# Homebrew packaging

`python-wasm.rb` is the Homebrew formula for python-wasm. It depends on
`wasmtime` and unpacks the standard release tarball into `prefix/`.

## Setting up the tap (one-time, admin)

Homebrew installs from "taps" — git repos under `Homebrew/Formula/`.
The recommended layout for python-wasm is a sibling `homebrew-tap`
repository under the same org:

```
github.com/tegmentum/python-wasm           # this repo
github.com/tegmentum/homebrew-tap          # the tap
└── Formula/
    └── python-wasm.rb                     # copy from packaging/homebrew/
```

Users then install with:

```bash
brew tap tegmentum/tap
brew install python-wasm
```

(`tegmentum/tap` resolves to `github.com/tegmentum/homebrew-tap` by
the standard brew convention.)

## Release process

1. `make python-composed PROFILE=default` — fresh build.
2. `scripts/package-release.sh <version> darwin-arm64` — and the other
   three platforms; ideally on per-platform runners in CI.
3. Push tag `v<version>`; GitHub Actions publishes the four tarballs +
   `checksums.txt` under
   `https://github.com/tegmentum/python-wasm/releases/download/v<version>/`.
4. Update `packaging/homebrew/python-wasm.rb`:
   - bump `version`
   - replace the four `REPLACE_ME_*_SHA256` with the values from
     `checksums.txt`
5. Copy the updated `python-wasm.rb` into
   `tegmentum/homebrew-tap/Formula/` and push.

## Local testing

Verify the formula installs cleanly without publishing:

```bash
# From a python-wasm checkout:
brew install --build-from-source ./packaging/homebrew/python-wasm.rb
brew test python-wasm
brew uninstall python-wasm
```

`--build-from-source` skips bottle resolution; for an URL-based test,
host the tarball locally and override `--HEAD` or set `HOMEBREW_NO_INSTALL_FROM_API=1`.

## Pushing to homebrew-core

After the tap has been stable for a release or two, a PR to
`homebrew/homebrew-core` is the canonical end state. The formula above
is already shaped to that spec — no changes needed beyond the URL and
`HOMEBREW_CORE_GIT_REMOTE` workflow steps.
