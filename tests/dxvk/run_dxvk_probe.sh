#!/usr/bin/env bash
# Builds DXVK (Win64) from the xwine1/dxvk_x checkout and runs two tests:
#   1. D3D11CreateDevice succeeds.
#   2. A D3D11 clear is presented to an X11 window, and the screen shows the color.
#
#   DXVK_SRC    dxvk_x checkout with submodules initialized (required)
#   DXVK_BUILD  meson build directory (default: ./build-dxvk)
#   DISPLAY     X display for test 2 (required for test 2)
#   XBEMU_PRESENT_ICD  Vulkan ICD JSON for test 2 (default: lavapipe, see below)
#
# Test 2 defaults to the software Vulkan driver (llvmpipe). Mesa's RADV driver needs
# DRI3 for X11 presentation, and Xvfb does not provide it. On a real X server with DRI3
# (Xorg or Xwayland on a GPU), set XBEMU_PRESENT_ICD=<radeon/nvidia/intel icd json> and
# DXVK_FILTER_DEVICE_NAME to match the GPU, or leave the filter unset for auto selection.
set -eu
here="$(cd "$(dirname "$0")/../.." && pwd)"
: "${DXVK_SRC:?set DXVK_SRC to the dxvk_x checkout}"
DXVK_BUILD="${DXVK_BUILD:-$here/build-dxvk}"

if [[ ! -d "$DXVK_BUILD" ]]; then
    (cd "$DXVK_SRC" && meson setup "$DXVK_BUILD" --cross-file build-win64.txt --buildtype release)
fi
ninja -C "$DXVK_BUILD" >/dev/null

cd "$here"
make -s xbemu tests/dxvk/probe.exe tests/dxvk/present.exe tests/dxvk/present_vsync.exe
export XBEMU_PATH="$DXVK_BUILD/src/d3d11:$DXVK_BUILD/src/dxgi"
export DXVK_LOG_PATH="$DXVK_BUILD"
fail=0

# Test 1: device creation.
out=$(timeout 120 ./xbemu tests/dxvk/probe.exe 2>/dev/null) || true
echo "$out"
if grep -q "D3D11CreateDevice hr = 0x0" <<<"$out"; then
    echo "PASS: D3D11CreateDevice succeeded"
else
    echo "FAIL: D3D11CreateDevice did not succeed"; fail=1
fi

# Test 2: clear and present, verified with a screenshot.
if [[ -z "${DISPLAY:-}" ]] || ! command -v xwd >/dev/null; then
    echo "SKIP: present test needs DISPLAY and xwd"
else
    icd="${XBEMU_PRESENT_ICD:-/usr/share/vulkan/icd.d/lvp_icd.json}"
    shot="$DXVK_BUILD/present-screen.xwd"
    if [[ "$icd" == */lvp_icd.json ]]; then export DXVK_FILTER_DEVICE_NAME=llvmpipe; fi
    VK_ICD_FILENAMES="$icd" timeout 120 ./xbemu tests/dxvk/present.exe >"$DXVK_BUILD/present.out" 2>&1 &
    pid=$!
    sleep 2
    xwd -root -silent -out "$shot"
    if wait "$pid" && grep -q "^done" "$DXVK_BUILD/present.out" \
        && python3 tests/dxvk/check_clear.py "$shot" 51 102 204 300000; then
        echo "PASS: clear color presented to the X11 window"
    else
        echo "FAIL: present test (see $DXVK_BUILD/present.out)"; fail=1
    fi
fi
# Test 3: draw a triangle with shaders and a vertex buffer, present, check the red area.
# The triangle covers 320x240 / 2 = 38400 pixels of a 640x480 window.
if [[ -z "${DISPLAY:-}" ]] || ! command -v xwd >/dev/null; then
    echo "SKIP: draw test needs DISPLAY and xwd"
else
    icd="${XBEMU_PRESENT_ICD:-/usr/share/vulkan/icd.d/lvp_icd.json}"
    shot="$DXVK_BUILD/triangle-screen.xwd"
    if [[ "$icd" == */lvp_icd.json ]]; then export DXVK_FILTER_DEVICE_NAME=llvmpipe; fi
    make -s tests/d3d11/triangle.exe
    VK_ICD_FILENAMES="$icd" timeout 120 ./xbemu tests/d3d11/triangle.exe >"$DXVK_BUILD/triangle.out" 2>&1 &
    pid=$!
    sleep 2
    xwd -root -silent -out "$shot"
    if wait "$pid" && grep -q "^done" "$DXVK_BUILD/triangle.out"         && python3 tests/dxvk/check_clear.py "$shot" 255 0 0 30000; then
        echo "PASS: triangle drawn with D3D11 shaders"
    else
        echo "FAIL: triangle test (see $DXVK_BUILD/triangle.out)"; fail=1
    fi
fi
# Test 4: vblank-synchronized presentation (FIFO). This hung on X servers whose outputs are
# disconnected, since no vblank arrives; it needs a server with a connected or headless
# output (see tests/dxvk/xorg-headless.conf).
if [[ -z "${DISPLAY:-}" ]]; then
    echo "SKIP: vsync present test needs DISPLAY"
else
    icd="${XBEMU_PRESENT_ICD:-/usr/share/vulkan/icd.d/lvp_icd.json}"
    if [[ "$icd" == */lvp_icd.json ]]; then export DXVK_FILTER_DEVICE_NAME=llvmpipe; fi
    if VK_ICD_FILENAMES="$icd" timeout 60 ./xbemu tests/dxvk/present_vsync.exe >"$DXVK_BUILD/present_vsync.out" 2>&1 \
        && grep -q "^presented" "$DXVK_BUILD/present_vsync.out"; then
        echo "PASS: vsync (FIFO) presentation completed"
    else
        echo "FAIL: vsync present (see $DXVK_BUILD/present_vsync.out)"; fail=1
    fi
fi
exit $fail
