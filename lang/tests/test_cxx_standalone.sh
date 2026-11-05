#!/usr/bin/env bash
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
driver="$root/lang/cxx/bin/brat-cxx"

"$driver" bundle "$tmp/release"
bundled="$tmp/release/lang/cxx/bin/brat-cxx"
printf 'fn main() -> i64 {\n  print(42)\n  return 0\n}\n' > "$tmp/hello.brat"
"$bundled" check "$tmp/hello.brat"
"$bundled" build "$tmp/hello.brat" -o "$tmp/hello-a"
"$bundled" build "$tmp/hello.brat" -o "$tmp/hello-b"
"$tmp/hello-a" > "$tmp/output"
test "$(cat "$tmp/output")" = "42"
cmp "$tmp/hello-a" "$tmp/hello-b"
printf 'C++ standalone bundle: ok\n'
