#!/usr/bin/env bash
# XAudio2 routing: a submix with channel volumes, and a direct source. The output dump is
# checked for which tones reach which channel.
set -u
here="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$here"
make -s xbemu tests/audio/routing.exe

out=$(mktemp -p "${TMPDIR:-$here}" xbemu-routing.XXXXXX)
dump="$out.raw"
trap 'rm -f "$out" "$dump"' EXIT
XBEMU_AUDIO_DUMP="$dump" timeout 30 ./xbemu tests/audio/routing.exe >"$out" 2>&1
rc=$?
cat "$out"
if [[ $rc -ne 0 ]]; then echo "FAIL: routing program (rc=$rc)"; exit 1; fi
if python3 tests/audio/check_routing.py "$dump" 1; then
    echo "PASS: submix routing and channel volumes"
else
    echo "FAIL: routing output"; exit 1
fi
