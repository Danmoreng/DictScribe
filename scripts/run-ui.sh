#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
APP="$PROJECT_ROOT/build/core/dictscribe"
ASR_WORKER="$PROJECT_ROOT/build/asr-worker/dictscribe-asr-worker"
REWRITE_WORKER="$PROJECT_ROOT/build/rewrite-worker/bin/dictscribe-rewrite-worker"

if [[ ! -x "$APP" ]]; then
  echo "DictScribe UI is not built. Run ./scripts/build.sh first." >&2
  exit 1
fi
if [[ ! -x "$ASR_WORKER" || ! -x "$REWRITE_WORKER" ]]; then
  echo "DictScribe workers are not built. Run ./scripts/build.sh first." >&2
  exit 1
fi

exec "$APP" "$@"
