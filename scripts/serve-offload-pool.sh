#!/usr/bin/env bash
# Host-side worker pool for the py-offload boundary (Phase 6).
#
# Spawns N native python processes, each running serve_mailbox over its own
# mailbox-<i>/ subdirectory under MAILBOX_ROOT. The guest's OffloadPool
# distributes tasks across the workers and waits for responses. Workers
# run concurrently because they're separate OS processes — the wall-clock
# of an N-task `Pool(N).map(...)` is roughly the time of one task.
#
# Usage:
#   ./scripts/serve-offload-pool.sh <root-dir> <num-workers>
#
# Lifecycle:
#   * each worker is backgrounded; pids written to <root-dir>/worker-<i>.pid
#   * this script blocks via `wait` so it can be killed cleanly with SIGTERM
#   * on signal, all workers are killed and pid files cleaned up
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REF="$PROJECT_DIR/reference-worker"

ROOT="${1:-}"
N="${2:-}"
if [ -z "$ROOT" ] || [ -z "$N" ]; then
    echo "usage: $0 <root-dir> <num-workers>" >&2
    exit 2
fi

[ -d "$REF" ] || { echo "serve-offload-pool: reference-worker/ missing at $REF" >&2; exit 1; }

mkdir -p "$ROOT"

PIDS=()
cleanup() {
    for pid in "${PIDS[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
    rm -f "$ROOT"/worker-*.pid 2>/dev/null || true
}
trap cleanup EXIT INT TERM

for ((i = 0; i < N; i++)); do
    MBOX="$ROOT/mailbox-$i"
    mkdir -p "$MBOX"
    PYTHONPATH="$REF" python3 -c "
from py_offload.mailbox import serve_mailbox
serve_mailbox('$MBOX')
" &
    pid=$!
    PIDS+=("$pid")
    echo "$pid" > "$ROOT/worker-$i.pid"
done

# Let workers enter their poll loops.
sleep 0.2

echo "serve-offload-pool: $N workers under $ROOT (pids: ${PIDS[*]})"
wait
