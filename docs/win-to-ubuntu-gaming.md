# Win to Ubuntu gaming: project summary

## What xbemu is

xbemu runs Windows 64-bit (PE32+) programs directly on Linux, without Wine. It loads the
program and its DLLs, provides the Windows APIs they call as host shims, and runs DXVK's
Direct3D 8, 9, 11, and DXGI on the host's Vulkan driver.

It is not a console emulator. It does not include the Xbox One operating system, and it
does not decrypt or run Xbox One content.

## What is tested and working

- Loader: PE32+ x86-64 images, imports across modules, exports, DllMain, W^X sections.
- Win64 to SysV ABI bridge, with stubs in a dual-mapped region (fixes a race when stubs
  are created while other threads run).
- C and C++ programs, including C++ exceptions across guest and host frames.
- kernel32, msvcrt, advapi32 (registry, persisted with `XBEMU_REGISTRY`), file I/O, threads,
  and synchronization.
- user32: windows on X11, the message queue, keyboard, mouse, and raw input (XInput2).
- XAudio2 shims with a silent fallback when ALSA is not available.
- D3D11 through DXVK: device creation, shaders, vertex buffers, draw calls, and presentation.
- Vsync (FIFO) presentation completes on a headless NVIDIA X server (`:97`).
- Test suite: 16/16 with a display, 13 passed and 2 skipped without one.
- Homebrew test game (`tests/game/game.c`): runs for six seconds and exits cleanly.

## Known gaps

- **D3D12:** not started. Many modern games need it.
- **Game coverage:** most commercial games use APIs not yet implemented. No commercial game
  has been run end to end.
- **Store and DRM:** no store client support. Store DRM will block most programs.
- **Vsync pacing:** the headless server does not pace FIFO presents.
- **RADV FIFO:** not tested with a connected output on the AMD iGPU.
- **Manual checks:** the homebrew game's keyboard movement and beep need a focused window
  and audible output, so they are checked by hand.
- **IME:** not implemented. `imm32.dll` reports no input context.
- **Display modes:** `ChangeDisplaySettingsExW` refuses.
- **Audio effects:** accepted but not applied.
- **D3DKMT and SetupAPI:** stubs only.
- **Memory:** guest and host share one address space, with no isolation.

## Out of scope

- Xbox One and Xbox 360 content. Xbox One software and games are encrypted, and running them
  would require key handling or decryption. This project does not add that, for any source.
- The Xbox system software is not included and is not part of this project.

## Next steps (not started)

1. D3D12 support (for example, vkd3d-proton on the host).
2. Grow API coverage by checking the imports of legally obtained Windows programs.
3. Test RADV FIFO presentation with a dummy plug or connected output on the AMD iGPU.

## How to run the tests

```
make test                       # emulator tests (add DISPLAY for the display tests)
DISPLAY=:97 tests/dxvk/run_dxvk_probe.sh
DISPLAY=:97 XBEMU_PRESENT_ICD=/usr/share/vulkan/icd.d/nvidia_icd.json \
  XBEMU_PATH=<dxvk-build>/src/d3d11:<dxvk-build>/src/dxgi tests/game/run_game_test.sh
```

The headless server is started with `sudo Xorg :97 -config tests/dxvk/xorg-headless.conf
-nolisten tcp -noreset -ac`. See README.md for details.
