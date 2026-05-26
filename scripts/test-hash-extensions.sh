#!/usr/bin/env bash
# Componentize-python plan, Phase 2: end-to-end smoke test of the composed
# python.composed.wasm + _crypto_hash and _xxhash extensions.
#
# Verifies canonical "abc" vectors for all 9 crypto algorithms (md5, sha1,
# sha2/3 family, blake2 family) plus reference vectors for the non-crypto
# algorithms (xxh32/64, crc32/c, blake3).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
COMP="$PROJECT_DIR/build/python.composed.wasm"

[ -f "$COMP" ] || { echo "test-hash-extensions: $COMP not found — run scripts/compose-python-component.sh first." >&2; exit 1; }
command -v wasmtime >/dev/null 2>&1 || { echo "test-hash-extensions: 'wasmtime' is required on PATH." >&2; exit 1; }

LIBDIR="$(basename "$(ls -d "$PROJECT_DIR"/deps/cpython/cross-build/wasm32-wasip2/build/lib.wasi-wasm32-* | head -1)")"

wasmtime run --wasm max-wasm-stack=16777216 \
    --dir "$PROJECT_DIR/deps/cpython::/" \
    --env "PYTHONPATH=/cross-build/wasm32-wasip2/build/$LIBDIR" \
    "$COMP" -c "
import sys
import _crypto_hash, _xxhash

failures = 0

CRYPTO_ABC = {
    'md5':      '900150983cd24fb0d6963f7d28e17f72',
    'sha1':     'a9993e364706816aba3e25717850c26c9cd0d89d',
    'sha256':   'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad',
    'sha384':   'cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7',
    'sha512':   'ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f',
    'sha3_256': '3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532',
    'sha3_512': 'b751850b1a57168a5693cd924b6b096e08f621827444f70d884f5d0240d2712e10e116e9192af3c91a7ec57647e3934057340b4cf408d5a56592f8274eec53f0',
    'blake2b':  'ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d17d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923',
    'blake2s':  '508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982',
}

print('--- _crypto_hash: one-shot + streaming + idempotency ---')
for name, want in CRYPTO_ABC.items():
    got = _crypto_hash.digest(name, b'abc').hex()
    if got != want:
        print(f'{name:9} one-shot   : FAIL got={got}'); failures += 1; continue
    h = _crypto_hash.new(name)
    h.update(b'a'); h.update(b'bc')
    s1 = h.hexdigest(); s2 = h.hexdigest()
    if s1 != want or s2 != want:
        print(f'{name:9} streaming  : FAIL stream={s1} stream2={s2}'); failures += 1; continue
    print(f'{name:9} {got}  OK')

print('--- _xxhash: cross-verified against reference C xxhash ---')
DATA = b'Nobody inspects the spammish repetition'
NCRYPTO = {
    'xxh32':  (DATA,    0, 'e2293b2f'),
    'xxh64':  (DATA,    0, 'fbcea83c8a378bf1'),
    'crc32':  (b'abc',  0, '352441c2'),
    'crc32c': (b'abc',  0, '364b3fb7'),
    'blake3': (b'abc',  0, '6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85'),
}
for name, (data, seed, want) in NCRYPTO.items():
    got = _xxhash.digest(name, data, seed=seed).hex()
    if got != want:
        print(f'{name:10}: FAIL got={got} want={want}'); failures += 1
        continue
    print(f'{name:10}: {got}  OK')

# Algorithm enumeration
algos_c = _crypto_hash.algorithms()
algos_x = _xxhash.algorithms()
print('algorithms (crypto)   :', algos_c)
print('algorithms (non-crypto):', algos_x)
if len(algos_c) != 9 or len(algos_x) != 9:
    print('FAIL: expected 9 algorithms each'); failures += 1

sys.exit(failures)
" \
    && echo "OK: _crypto_hash + _xxhash all-vector smoke passed." \
    || { echo "FAIL: extension or composition broken." >&2; exit 1; }
