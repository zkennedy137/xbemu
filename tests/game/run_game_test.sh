#!/usr/bin/env bash
# Runs the homebrew test game (tests/game/game.c) for its fixed run time. It passes when the
# game renders frames and exits cleanly. Keyboard and sound need a focused window and an
# audio device, so they are not checked here.
#   DISPLAY                 X display (required; the test is skipped without one)
#   XBEMU_PATH              directories with DXVK's d3d11.dll and dxgi.dll (required)
#   XBEMU_PRESENT_ICD       Vulkan ICD JSON for the display (default: llvmpipe)
set -u
here="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$here"
[[ -n "${DISPLAY:-}" ]] || { echo "SKIP: game test needs DISPLAY"; exit 77; }
[[ -n "${XBEMU_PATH:-}" ]] || { echo "SKIP: game test needs XBEMU_PATH (DXVK build directories)"; exit 77; }
make -s xbemu tests/game/game.exe

icd="${XBEMU_PRESENT_ICD:-/usr/share/vulkan/icd.d/lvp_icd.json}"
if [[ "$icd" == */lvp_icd.json ]]; then export DXVK_FILTER_DEVICE_NAME=llvmpipe; fi
out=$(VK_ICD_FILENAMES="$icd" timeout 60 ./xbemu tests/game/game.exe 2>/dev/null)
rc=$?
frames=$(sed -n 's/^frames \([0-9]*\) beeps.*/\1/p' <<<"$out")
if [[ $rc -eq 0 && "$out" == *"ready"* && "$out" == *"done"* && "${frames:-0}" -gt 0 ]]; then
    echo "PASS: homebrew game ran ($frames frames)"
else
    echo "FAIL: homebrew game (rc=$rc)"; echo "$out"; exit 1
fi
