#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

ctest --test-dir "$PROJECT_ROOT/build/core" --output-on-failure
"$PROJECT_ROOT/build/asr-worker/dictscribe-asr-worker" --version
"$PROJECT_ROOT/build/rewrite-worker/bin/dictscribe-rewrite-worker" --version

if [[ -n "${DICTSCRIBE_ASR_MODEL:-}" && -n "${DICTSCRIBE_REWRITE_MODEL:-}" ]]; then
  python3 "$SCRIPT_DIR/smoke-workers.py" \
    --asr-model "$DICTSCRIBE_ASR_MODEL" \
    --rewrite-model "$DICTSCRIBE_REWRITE_MODEL"
else
  echo "Model smoke test skipped. Set DICTSCRIBE_ASR_MODEL and DICTSCRIBE_REWRITE_MODEL to enable it."
fi
