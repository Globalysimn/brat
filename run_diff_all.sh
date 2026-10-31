#!/bin/sh
# three-way differential over every example, one line each
fail=0
for f in lang/examples/*.brat bench/*.brat lang/servers/*.brat; do
  out=$(python3 diffprobe.py "$f" 2>&1)
  if echo "$out" | grep -q DIVERGENCE; then
    echo "$out"
    fail=$((fail+1))
  elif echo "$out" | grep -q "refused by the checker"; then
    echo "skip $f  (checker refuses it; --interp is not gated)"
  else
    echo "ok   $f"
  fi
done
echo "---- diverging files: $fail"
