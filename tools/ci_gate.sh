#!/bin/bash
# Run-level determinism + a small bench smoke. Works for single- and multi-config
# generators (build/run vs build/Release/run.exe).
set -e
cd "$(dirname "$0")/.."
BIN=""
for cand in build/run build/run.exe build/Release/run build/Release/run.exe; do
  if [ -f "$cand" ]; then BIN="$cand"; break; fi
done
[ -n "$BIN" ] || { echo "run binary not found (build first)"; exit 1; }
"$BIN" determinism 960000 500
"$BIN" bench 960000 5 300 | tail -1
