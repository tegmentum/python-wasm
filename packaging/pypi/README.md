# `python-wasm` on PyPI

The pip-installable distribution channel for python-wasm. Ships the
bundled `python.composed.wasm` interpreter, the stdlib tree, the
launcher script, and the rebuild source tree as Python package data.

## Install

```bash
pip install python-wasm
python-wasm -c "print('hi from wasi')"
```

You still need `wasmtime` on PATH; `python-wasm doctor` will point at
the installer if it's missing.

## Why a pip wheel

A `pip install` path:

- works on any platform that already has CPython
- integrates naturally with `python-wasm pip install <ext-wheel>` (the
  extension-wheel install path documented in `pyforge-wheel-spec.md`)
- gets free `pip install --upgrade` semantics

This is **not** the right channel for users who don't already have
host CPython — they should use `install.sh`, Homebrew, or the
standalone tarball instead.

## Build

```bash
# From the python-wasm repo root:
scripts/build-pypi-wheel.sh 0.1.0
ls dist/pypi/python_wasm-0.1.0-py3-none-any.whl
```

Internally `scripts/build-pypi-wheel.sh`:

1. Builds python.composed.wasm via `make python-composed`.
2. Stages the bundled artifacts into `packaging/pypi/src/python_wasm/data/`:
   - `data/bin/python-wasm` (the launcher)
   - `data/lib/python-wasm/*.sh` (subcommand helpers)
   - `data/python.composed.wasm`
   - `data/cpython/` (stdlib tree)
   - `data/cpython-ext-base/` (rebuild sources)
   - `data/VERSION`
3. Runs `python -m build --wheel` from `packaging/pypi/`.
4. Drops the wheel into `dist/pypi/`.

The build produces a single `py3-none-any` wheel — wasm is universal,
the launcher is POSIX sh, so one wheel covers every platform with
host Python.

## Test

```bash
python3 -m venv /tmp/pw-venv
/tmp/pw-venv/bin/pip install dist/pypi/python_wasm-0.1.0-py3-none-any.whl
/tmp/pw-venv/bin/python-wasm -c "print(1+1)"
```

## Publish

```bash
twine upload dist/pypi/*.whl
```

(One-time: register `python-wasm` on PyPI under the Tegmentum org.)
