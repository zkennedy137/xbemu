#!/usr/bin/env bash
# Keyboard and mouse messages: a window receives events sent through the X server.
# Needs DISPLAY. Exits 77 (skip) without one.
set -u
here="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$here"
[[ -n "${DISPLAY:-}" ]] || { echo "SKIP: input test needs DISPLAY"; exit 77; }
make -s xbemu tests/input/input.exe tests/input/sendevents

out=$(mktemp -p "${TMPDIR:-$here}" xbemu-input.XXXXXX)
trap 'rm -f "$out"' EXIT
timeout 30 ./xbemu tests/input/input.exe >"$out" 2>&1 &
pid=$!
for _ in $(seq 1 100); do grep -q ready "$out" 2>/dev/null && break; sleep 0.1; done
DISPLAY="$DISPLAY" ./tests/input/sendevents "xbemu input test" || true
wait "$pid" || true

expected='ready
KEYDOWN 65
CHAR 97
KEYUP 65
KEYDOWN 20
KEYUP 20
KEYDOWN 65
CHAR 65
KEYUP 65
LDOWN 10,20
MOVE 30,40
LUP
WHEEL 120
KEYDOWN 27
CHAR 27
done'
actual=$(cat "$out")
if [[ "$actual" == "$expected" ]]; then
    echo "PASS: keyboard and mouse messages"
else
    echo "FAIL: keyboard and mouse messages"; echo "$actual"; exit 1
fi
