# `_kdf_cap` — Password-hash / KDF dispatcher (pbkdf2, scrypt, argon2) over the password-hash-multiplexer cap

Pattern A (cpython-ext static linkage) extension scaffolded by
`pylon pkg new _kdf_cap`. See `../../docs/pyforge-pkg-spec.md` for the
authoring contract and the six already-shipped extensions for working
examples.

## What you have

```
cpython-ext/_kdf_cap/
  _kdf_capmodule.c            (C extension stub)
  wit/world.wit             (kdf-cap-import)
  pyforge-pkg.toml          (Pattern A manifest)
  README.md                 (this file)
```

WIT capabilities declared:

  - `tegmentum:password-hash-multiplexer/password-dispatcher@0.1.0`

## What you need to do

### 1. Vendor capability WIT files

For each capability you import, copy its `.wit` files (and transitive
deps) into `wit/deps/<cap-name>/`. The existing extensions follow this
convention — see `cpython-ext/_compression/wit/deps/compression-multiplexer/`
for the layout.

Typical command (adjust the source path):
```sh
mkdir -p wit/deps/<cap>
cp ~/git/<cap-repo>/wit/*.wit wit/deps/<cap>/
```

### 2. Generate the wit-bindgen-c bindings

Once the deps are in place:
```sh
cd cpython-ext/_kdf_cap
wit-bindgen c --world kdf-cap-import --out-dir gen wit/
```

This produces:
- `gen/kdf_cap_import.c` — the canonical-ABI shims (C source)
- `gen/kdf_cap_import.h` — the header your `.c` includes
- `gen/kdf_cap_import_component_type.o` — pre-built component-type metadata (.o)

Commit all three. The build relies on the pre-generated artifacts;
wit-bindgen-c is not invoked at build time.

### 3. Implement real methods

Replace the `mod_hello` stub in `_kdf_capmodule.c` with methods that call
into the generated bindings. Pattern reference: the six existing
extensions, in order of increasing complexity:

- `_xxhash` — non-crypto hashing, no resources, lookup-table approach
- `_compression` — result<T,E> handling for compress/decompress
- `_crypto_hash` — WIT resources (hasher) + idempotent digest buffering
- `_ssl_capability` — large surface area, TLS state machines
- `_sqlite_capability` — SQL DB-API over a high-level WIT contract
- `_v86_posix` — direct OS primitives + stream resources

### 4. Wire the extension into the build

Edit `scripts/wire-cpython-ext.sh`'s `EXTS` array — add a line of shape:
```
"_kdf_cap|_kdf_cap|_kdf_capmodule.c|kdf_cap_import.c|kdf_cap_import_component_type.o"
```

`pylon pkg verify` cross-checks the EXTS entry against this extension's
pyforge-pkg.toml.

### 5. Add Python shim(s) if needed

Pattern A extensions can either:

- Be imported directly as `_kdf_cap` — set `[[provides]] module = "_kdf_cap"`
  with no shim/dest (direct-C form).
- Have a stdlib-shaped surface in `Lib/<name>.py` — drop a `<name>.py` next
  to `_kdf_capmodule.c`, set `shim = "<name>.py"` + `dest = "Lib/<name>.py"`,
  and add a `cp` line to `Makefile`'s `install-python-shims` target.

### 6. Verify

```sh
pylon pkg verify -r ~/git/python-wasm --forge <manifest>.toml
```

Should report zero errors for this package once steps 1–5 are done.

### 7. Build + smoke

```sh
make build && python3 -c 'import _kdf_cap; print(_kdf_cap.hello())'
```
