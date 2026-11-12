#!/usr/bin/env bash
# XAudio2 tone test: the program plays a 440 Hz tone; the emulator dumps what it sends to
# the audio output, and check_tone.py confirms the frequency and level.
# Plays through the default ALSA device when one exists; otherwise the silent sink is used.
set -u
here="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$here"
make -s xbemu tests/audio/tone.exe

out=$(mktemp -p "${TMPDIR:-$here}" xbemu-audio.XXXXXX)
dump="$out.raw"
trap 'rm -f "$out" "$dump"' EXIT
XBEMU_AUDIO_DUMP="$dump" timeout 30 ./xbemu tests/audio/tone.exe >"$out" 2>&1
rc=$?
cat "$out"
if [[ $rc -ne 0 ]] || ! grep -q "ended 4" "$out"; then
    echo "FAIL: audio program did not finish all buffers (rc=$rc)"; exit 1
fi
if python3 tests/audio/check_tone.py "$dump" 440 1; then
    echo "PASS: 440 Hz XAudio2 tone"
else
    echo "FAIL: tone frequency or level"; exit 1
fi
