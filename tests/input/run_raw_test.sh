#!/usr/bin/env bash
# Raw input: the window receives WM_INPUT for the motion, button, and key XTest sends.
# Needs DISPLAY. Exits 77 (skip) without one.
set -u
here="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$here"
[[ -n "${DISPLAY:-}" ]] || { echo "SKIP: raw input test needs DISPLAY"; exit 77; }
make -s xbemu tests/input/raw.exe tests/input/sendraw

out=$(mktemp -p "${TMPDIR:-$here}" xbemu-raw.XXXXXX)
trap 'rm -f "$out"' EXIT
timeout 30 ./xbemu tests/input/raw.exe >"$out" 2>&1 &
pid=$!
for _ in $(seq 1 100); do grep -q ready "$out" 2>/dev/null && break; sleep 0.1; done
./tests/input/sendraw || true
wait "$pid"
rc=$?
actual=$(cat "$out")
expected='ready
raw mouse 7 -3
raw button 1 down
raw button 1 up
raw key 65 down
raw key 65 up'
if [[ $rc -eq 0 && "$actual" == "$expected" ]]; then
    echo "PASS: raw mouse, button, and keyboard input"
else
    echo "FAIL: raw input (rc=$rc)"; echo "$actual"; exit 1
fi
