#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CPYTHON_DIR="$PROJECT_DIR/deps/cpython"
RUN="$PROJECT_DIR/scripts/run-python.sh"

PASS=0
FAIL=0

run_test() {
    local name="$1"
    shift
    if "$@" >/dev/null 2>&1; then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name"
        FAIL=$((FAIL + 1))
    fi
}

run_test_output() {
    local name="$1"
    local expected="$2"
    shift 2
    local output
    output=$("$@" 2>&1) || true
    if echo "$output" | grep -q "$expected"; then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name (expected '$expected', got '$output')"
        FAIL=$((FAIL + 1))
    fi
}

echo "Running WASI CLI smoke tests..."
echo

# 1. Binary runs and reports version
run_test_output "python --version" "Python 3.14" \
    "$RUN" --version

# 2. Inline code execution
run_test_output "print hello" "hello" \
    "$RUN" -c "print('hello')"

# 3. Platform is wasi
run_test "sys.platform == wasi" \
    "$RUN" -c "import sys; assert sys.platform == 'wasi', sys.platform"

# 4. C extension module (math)
run_test_output "math.pi" "3.14159" \
    "$RUN" -c "import math; print(math.pi)"

# 5. Pure Python stdlib (json)
run_test_output "json round-trip" "hello" \
    "$RUN" -c "import json; print(json.loads(json.dumps({'key': 'hello'}))['key'])"

# 6. Environment access via wasi:cli
run_test "os.environ accessible" \
    "$RUN" -c "import os; os.environ"

# 7. File execution via filesystem preopens
# Copy smoke_test.py into CPython tree so it's visible at /smoke_test.py in the guest
cp "$SCRIPT_DIR/smoke_test.py" "$CPYTHON_DIR/smoke_test.py"
run_test_output "smoke_test.py" "smoke tests passed" \
    "$RUN" "/smoke_test.py"
rm -f "$CPYTHON_DIR/smoke_test.py"

echo
echo "Results: $PASS passed, $FAIL failed"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
