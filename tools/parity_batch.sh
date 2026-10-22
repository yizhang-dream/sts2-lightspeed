#!/bin/bash
# T1 parity batch: replay every corpus run, report per-run verdict.
cd "$(dirname "$0")/.."
pass=0; fail=0; abort=0
for d in parity_data/corpus/*/; do
    r="$d/result.json"; rt="$d/route.json"
    [ -f "$r" ] && [ -f "$rt" ] || continue
    name=$(basename "$d")
    BIN=""
for cand in ./build/parity ./build/parity.exe ./build/Release/parity ./build/Release/parity.exe; do
  if [ -f "$cand" ]; then BIN="$cand"; break; fi
done
[ -n "$BIN" ] || { echo "parity binary not found (build first)"; exit 1; }; out=$($BIN replay "$r" "$rt" 2>&1)
    diffs=$(echo "$out" | grep "TOTAL FIELD DIFFS" | grep -oE "[0-9]+$")
    if echo "$out" | grep -q "NOT IN HAND\|not implemented\|parse failed\|expected 9 rng"; then
        echo "ABORT  $name :: $(echo "$out" | grep -E 'NOT IN HAND|not implemented|parse failed|expected 9' | head -1)"
        abort=$((abort+1))
    elif [ "$diffs" = "0" ]; then
        pass=$((pass+1))
    else
        echo "DIFF   $name :: $diffs field diffs :: $(echo "$out" | grep 'DIFF ' | awk '{print $3}' | sort -u | tr '\n' ' ')"
        fail=$((fail+1))
    fi
done
echo "== pass=$pass fail=$fail abort=$abort =="
