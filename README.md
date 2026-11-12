# xbemu: Win64 loader and D3D11 host for Linux (no Wine)

xbemu runs ordinary Win64 (x64 PE32+) programs on Linux without Wine. It loads the
program and its DLLs, provides the Windows APIs they use as host shims, and runs
DXVK's `d3d11.dll` and `dxgi.dll` on the host's Vulkan driver.

Status: a D3D11 draw (shaders, a vertex buffer, and a draw call) reaches a window
and is checked on screen. C++ exceptions unwind across guest frames. Ordinary C and
C++ programs run, with their normal C runtime. Keyboard and mouse input reach window
procedures. Nothing here runs an Xbox One game yet.

## What works

- **Loader** (`src/pe.c`): PE32+ x86-64 images. Maps sections, applies relocations,
  binds imports across modules (PE to PE, host shim, data export, or logging stub),
  reads export tables, calls `DllMain`, and enforces W^X section protections.
- **Win64 <-> SysV ABI** (`src/w64abi.c`): per-import stubs and a thunk that saves the
  Win64 argument registers (including stack args), xmm0-3 (floats), and the guest's
  callee-saved state for unwinding.
- **Threads** (`src/teb.c`): a TEB per thread with the GS base set. Import stubs live in one
  memory region with a writable view for creating stubs and a separate executable view, so
  creating a stub never changes what other threads are executing.
- **Exceptions** (`src/host/seh.c`): `RaiseException`, `RtlUnwindEx`, `RtlVirtualUnwind`,
  and `RtlLookupFunctionEntry` implement the x64 unwind model. Language handlers and
  cleanup handlers run, and landing pads are entered with the right registers. The C++
  runtime's forced-unwind protocol is handled. An exception thrown in a window procedure
  reaches the guest `catch` that called `DispatchMessageW`, passing through the host shim.
  Tested with MinGW's C++ runtime.
- **kernel32** (`src/host/kernel32*.c`): process, time, TLS, errors, code pages,
  critical sections, SRW locks, condition variables, events, semaphores, threads, waits,
  module loading and `GetProcAddress`, file I/O (`CreateFile`, `ReadFile`, `WriteFile`,
  seeking, sizes, directories, attributes), `VirtualQuery`.
- **msvcrt** (`src/host/msvcrt.c`): startup (`__getmainargs`, `atexit`, `exit`), memory,
  strings, 16-bit wide strings, stdio and `printf` family (integers, strings, pointers,
  floats), locale, `qsort`, `rand`, `time`.
- **user32** (`src/host/user32.c`): window classes and windows backed by X11, the message
  queue (paint, size, close, keyboard, mouse, wheel, focus), `TranslateMessage` (WM_CHAR),
  key state (including caps, num, and scroll lock), cursor position, raw mouse motion
  (`src/host/rawinput.c`, XInput2: motion, left/middle/right buttons, and keys), monitors and display modes from RandR
  (`src/host/display.c`). IME is not implemented: `imm32.dll` reports no input context.
- **Audio** (`src/host/xaudio2.c`, `src/host/audio_out.c`): `xaudio2_9.dll` and
  `xaudio2_8.dll`, with engines, mastering voices, source voices, and submix voices. Each
  engine mixes on its own host thread at 48 kHz stereo, through ALSA "default". PCM
  8/16/32-bit and float sources are accepted, with nearest-neighbour resampling. Per-channel
  volumes apply. Buffer start and end callbacks reach the guest. Without a usable ALSA
  device, a silent sink paces playback in real time.
- **Vulkan float arguments** (`src/host/vulkan.c`, `vulkan_floats.inc`): functions with float
  parameters take them from xmm registers, as the Win64 ABI requires.
- **advapi32** (`src/host/advapi32.c`): LUIDs, and a registry with the standard root keys.
  Values persist to the file named by `XBEMU_REGISTRY`, if it is set. Without it, the
  registry lasts for one process.
- **Graphics**: DXVK (`d3d8`, `d3d9`, `d3d11`, `dxgi`) runs on the host's Vulkan driver.
  The Win32 surface maps to an xcb surface on the window (`src/host/vulkan.c`).
- **Paths** (`src/host/paths.c`): guest `Z:\...` paths map to host `/...` paths.

## Win32 WSI

DXVK asks for `VK_KHR_win32_surface`, which the Linux loader doesn't provide. The
Vulkan shim replaces it with `VK_KHR_xcb_surface` at instance creation, and maps the
Win32 surface and presentation-support entry points to their xcb equivalents. The surface
uses its own X connection, because Mesa's Present code reads events from the surface
connection.

Presentation needs DRI3 on the X server. RADV reports "No DRI3 support detected" on
Xvfb (which lacks DRI3), so the presentation tests use the software driver (llvmpipe),
which presents on Xvfb.

FIFO (vsync) presentation needs a display that produces vblank events. An X server with
no connected outputs never sends them, so FIFO presentation hangs there. Immediate mode
does not wait and works either way. The fix for a headless machine is an NVIDIA X server
that starts without a display: `tests/dxvk/xorg-headless.conf` sets
`AllowEmptyInitialConfiguration`, `UseDisplayDevice none`, and `AllowEmptyInput`. Start it
as root, with DRI3 then present on the RTX 5060:

```
sudo Xorg :97 -config tests/dxvk/xorg-headless.conf -nolisten tcp -noreset -ac
DISPLAY=:97 VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json tests/dxvk/run_dxvk_probe.sh
```

With that server, all four DXVK tests pass, including vsync (test 4). The headless server
does not pace FIFO presents: a vsync loop runs as fast as the CPU, so the test checks that
presentation completes, not its timing. RADV's FIFO has not been tested on a headless
server (its AMD server had disconnected outputs, which is the case that hangs).

## Homebrew test game

`tests/game/game.c` is an original Win64 test program. It draws a triangle with D3D11 and
shaders, moves it with the arrow keys, plays a beep on space through XAudio2, and quits on
Escape or after six seconds. It needs the same DXVK setup as the DXVK tests. Run it with:

```
DISPLAY=:97 XBEMU_PRESENT_ICD=/usr/share/vulkan/icd.d/nvidia_icd.json \
  XBEMU_PATH=<dxvk-build>/src/d3d11:<dxvk-build>/src/dxgi tests/game/run_game_test.sh
```

The test passes when the game renders frames and exits cleanly. The keyboard and sound parts
need a focused window and an audio device, so they are checked by hand.

## Audio device access

ALSA opens the sound card through `/dev/snd/controlC*`, which is owned by `root:audio`.
Without membership in the `audio` group, the emulator falls back to the silent sink and
prints a warning. To play sound, add the user to that group once (needs sudo), then log in
again:

```
sudo usermod -aG audio <your-username>
```

`tests/audio/run_audio_test.sh` checks the tone either way. `XBEMU_AUDIO=null` forces the
silent sink, and `XBEMU_AUDIO_DUMP=<file>` writes the output samples as raw S16LE stereo.

## Tests

`make test` runs `tests/run_tests.sh`: 15 checks with a display, and 13 checks plus 2 skips without one. The DXVK tests
are in `tests/dxvk/run_dxvk_probe.sh`. The threading, registry, and Vulkan float tests are
standalone programs in `tests/threads`, `tests/registry`, and `tests/vulkan`.

## Shader bytecode

The D3D11 draw test needs DXBC shaders. `tests/d3d11/make_shaders.py` generates them:
SM4.0 vertex and pixel shaders, with the DXBC container and the digest DXVK validates. The
digest algorithm was checked against a known-good blob from DXVK's own tests. No D3D
shader compiler is needed.

## Build and test

```
make                     # emulator and the Win64 test programs (needs mingw-w64)
make test                # emulator tests: loader, C++ exceptions, default C runtime,
                         #   keyboard and mouse (with DISPLAY)
DXVK_SRC=<dxvk_x checkout> DISPLAY=:99 tests/dxvk/run_dxvk_probe.sh
```

`DXVK_SRC` is a checkout of github.com/xwine1/dxvk_x with its submodules
(`git submodule update --init --recursive`). The DXVK script builds DXVK for Win64 with
meson, then runs three tests: device creation, a clear presented to a window, and a
triangle drawn with shaders. The last two need an X display and `xwd`. To try it locally:
`Xvfb :99 -screen 0 1280x1024x24 -nolisten tcp &`.

Set `DXVK_LOG_PATH=<dir>` to get DXVK's own log. `XBEMU_PATH` adds directories to search
for DLLs, and `-v` prints the load address.

## Known gaps

Not done, and why:

- **RADV FIFO presentation.** RADV's DRI3 path works on an AMD Xorg server with
  immediate presentation. FIFO hung on that server because its outputs were disconnected.
  The RTX 5060 passes FIFO on a headless server (see Presentation above). Attaching a
  monitor or a dummy plug to the AMD iGPU would show whether RADV FIFO works too.
- **IME.** Not implemented. `imm32.dll` reports no input context, so IME-aware programs
  use their direct-key path. Real composition needs an input-method server (XIM), and
  none is running here to test against.
- **Display mode changes.** `ChangeDisplaySettingsExW` refuses. Changing a real monitor's
  mode is not something to test blind. The Xvfb server here has only one mode.
- **Audio effects and filters.** Accepted but not applied. Submix voices, per-channel
  volumes, and multiple engines work and are tested.
- **Audio on your speakers.** The tone and routing tests verify the samples the emulator
  sends to the device. Whether they are audible needs speakers on the USB device.
- **D3DKMT.** Stubs that report `STATUS_NOT_SUPPORTED`. The real calls are the Windows
  display driver model, which Linux does not provide.
- **SetupAPI.** Stubs that find no devices. There is no Windows device database.
- **Vulkan floats.** Forwarded from a table generated from the Vulkan header
  (`tools/gen_vk_floats.py`). The header has five functions with float parameters, and
  they are all in the table. Extension headers that aren't in `vulkan_core.h` would need
  regenerating from their own headers.
- **Memory model.** Guest and host share one address space. No isolation. This is the
  architecture, not a bug.
- **Xbox content.** Xbox One titles are encrypted (XVC/XVD). Nothing here decrypts them,
  and no key handling or DRM bypass will be added. Only programs you have the right to run
  are in scope. The Xbox ERA system DLLs are not included.

## Layout

- `src/pe.c`: PE32+ loader and module registry
- `src/w64abi.c`/`.h`: thunks, stubs, dispatch, call-out into Win64 code
- `src/teb.c`: TEB/PEB and GS setup
- `src/host/`: host shims (`kernel32*.c`, `msvcrt.c`, `user32.c`, `seh.c`, `vulkan.c`, ...),
  the DLL registry (`hostlib.c`), and X11 connections (`xdisplay.c`)
- `src/main.c`: CLI (`xbemu [-v] program.exe [args]`)
- `tests/`: Win64 test programs (MinGW), the emulator test script, DXVK probes, the draw test
- `docs/`: DXVK import inventory
