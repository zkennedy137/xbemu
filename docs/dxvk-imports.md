# DXVK import inventory (Win64 build of xwine1/dxvk_x)

Built with: `meson setup <build> --cross-file build-win64.txt --buildtype release`
after `git submodule update --init --recursive` in the dxvk_x checkout.
Source commit: xwine1/dxvk_x `43df29b` ("Xbox changes").

Raw lists (`dll function`, one per line, from `x86_64-w64-mingw32-objdump -p`):
- `imports-d3d11.txt` (201 entries)
- `imports-dxgi.txt` (199 entries)

Per-DLL breakdown of d3d11.dll:
| DLL | Count | Notes |
|---|---|---|
| KERNEL32 | 76 | threads, critical sections, SRW locks, events, files, TLS, time |
| msvcrt | 75 | C runtime: malloc/printf/string/stdio; mostly thin host forwards |
| GDI32 | 20 | `D3DKMT*` WDDM kernel-graphics calls (adapter, device, resources). Xbox equivalent is VdEra. |
| USER32 | 20 | display/monitor enumeration, window geometry |
| ADVAPI32 | 5 | registry reads for DXVK config |
| SETUPAPI | 4 | device-interface enumeration |
| dxgi.dll | (link) | DXVK-internal dependency, resolved as a loaded module |

Implications for xbemu:
- Imports are static; a list says what the binary can call, not what it calls at runtime.
  The real kernel scope comes from running the device-creation path and fixing what is hit.
- `D3DKMT*` and `SetupDi*` are only reached during adapter enumeration. They need shims
  that report one fake adapter.
- Full DirectX 11 device creation also needs a `vulkan-1.dll` forwarder to the host Vulkan loader.
