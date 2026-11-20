# thirdparty/

| folder | what | from | license |
|---|---|---|---|
| `pcsx2/` | PCSX2's GS (hardware renderer: DX11 and Vulkan; no DX12), `common/` and the libraries they use (fmt, imgui, cpuinfo, Vulkan headers + VMA, xbyak, fast_float, WIL, xxhash) | [PCSX2/pcsx2](https://github.com/PCSX2/pcsx2) commit `2954ebe` (2026-10-11), plus its GS fixes up to `da6bae1` (2026-11-01: texture cache, shuffles, transfers, DS-as-RT feedback; not the FullscreenUI blur), `d1662db` (PSMT4 bounds) and the Vulkan renderer, its shaders and `3rdparty/vulkan` from `695e267` (2026-11-07). shaderc (the Vulkan shader compiler) is built from source at configure time: `pcsx2-gs/shaderc.cmake` | GPL-3.0 (`pcsx2/COPYING.GPLv3`); each library's license is in its folder |
| `pcsx2-gs/` | ours: builds `pcsx2/` into `pcsx2gs.dll` with a C interface (`include/pcsx2gs_api.h`), the host stubs, the shaders/font and the shader cache. See `docs/GS_PCSX2.md` | this project | GPL-3.0 |
| `directx-headers/` | `include/` of DirectX-Headers (`directx/d3d12.h` and friends, needed by PCSX2's DX12 code) | [microsoft/DirectX-Headers](https://github.com/microsoft/DirectX-Headers) commit `461ef51` | MIT (`directx-headers/LICENSE`) |

`pcsx2/` holds only the files the build compiles or includes: the sources listed in `pcsx2-gs/CMakeLists.txt` and
the headers from Ninja's dependency log of a full build (`ninja -t deps`), plus the license files and the whole
`3rdparty/cpuinfo` (it is built with its own CMakeLists). The folder layout is PCSX2's, so a newer PCSX2 can be
copied over the same paths. `pcsx2-gs/tools/make_loaders.py` and `extract_config.py` read a full PCSX2 checkout.

## Linux

On Linux `pcsx2-gs/` builds `libpcsx2gs.so` (Vulkan only, presenting into an X11 window: `VK_KHR_xlib_surface`) and
`libshaderc_shared.so.1` next to GHR, with `resources/`. These PCSX2 files were added for it, from the same commit
`2954ebe` (`https://raw.githubusercontent.com/PCSX2/pcsx2/2954ebe6965669219f1bbd256e958fbd8f83a85d/<path>`):

| file | changed |
|---|---|
| `pcsx2/common/Linux/LnxHostSys.cpp` | no |
| `pcsx2/common/Linux/LnxThreads.cpp` | no |
| `pcsx2/common/Linux/LnxMisc.cpp` | its D-Bus screensaver inhibition and Xlib mouse functions are stubs under `PCSX2GS_LIBRARY` (marked `pcsx2gs (Kamite)`), so the library links neither libdbus nor libX11 |

Every other PCSX2 file the Linux build compiles is the one the Windows build already has (their Linux code paths were
in the trimmed copy). The library exports only the C interface (`pcsx2-gs/compat/pcsx2gs.map`) and links no libX11:
GHR passes it the X11 connection (`pcsx2gs_set_display`) and an X11 child window of the game window. Both libraries
get `pcsx2-gs/compat/dso_finalize.c`, which runs their static destructors when they are unloaded: zig's toolchain (the
portable build) has no crtbegin to do it, and exit crashed once PCSX2 unloaded `libshaderc_shared.so.1`. The PNG decoder for texture replacements and the port's screen images is
[stb_image](https://github.com/nothings/stb) v2.30 (commit `f58f558`, public domain / MIT), downloaded at configure
time with its SHA-256 checked (`pcsx2-gs/compat/PngStb.inl`, where Windows has WIC in `PngWic.inl`).

With `KAMITE_BUILD_TESTS=ON` three tests are registered with CTest, all headless (Xvfb, Mesa's lavapipe Vulkan and
llvmpipe GL, no GPU):

- `pcsx2_gs_smoke` (`pcsx2-gs/test/SmokeTest.cpp`): loads `libpcsx2gs.so` as GHR does, opens Vulkan on an X11
  window with GHR's default image options and `api=dx11` (refused: Vulkan), renders a GIF packet, presents, resizes,
  and checks the picture in the GS's image and on the window, that `libshaderc_shared.so.1` compiled the shaders and
  that only the C interface is exported.
- `gs_pcsx2_backend_test` (`runtime/ps2xRuntime/test/gs_pcsx2_backend_test.cpp`): the runtime's side without a disc:
  a raylib game window, the X11 child window over it, resizes, hiding for a host UI, the Graphics API switch to DX11
  turned down, the shader cache under `KAMITE_HOME`, the picture on the child window.
- `gs_pcsx2_backend_no_x11`: without `$DISPLAY` the PCSX2 GS turns itself down (GHR then uses its built-in GS).

Build them with the game code or a stand-in (`-DKAMITE_GENERATED_DIR=... -DKAMITE_MODULES_DIR=...`), then run them
from the repository root; on NixOS the libraries GLFW and Vulkan open at run time are not on a global path, hence the
`LD_LIBRARY_PATH` (a regular distribution has them):

```
nix develop -c bash -c 'cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DKAMITE_BUILD_TESTS=ON && ninja -C build/linux -j4 kamite kamite_test_programs'
nix develop -c bash -c '
  p() { nix build --no-link --print-out-paths "nixpkgs#$1" | head -1; }
  mesa=$(p mesa)
  export VK_DRIVER_FILES=$mesa/share/vulkan/icd.d/lvp_icd.x86_64.json VK_ICD_FILENAMES=$mesa/share/vulkan/icd.d/lvp_icd.x86_64.json
  export PATH=$(p xvfb-run)/bin:$PATH __GLX_VENDOR_LIBRARY_NAME=mesa LD_LIBRARY_PATH=$(p vulkan-loader)/lib:$(p libglvnd)/lib:$mesa/lib
  for x in libx11 libxext libxrandr libxinerama libxcursor libxi libxrender libxfixes libxcb; do LD_LIBRARY_PATH+=:$(p $x)/lib; done
  KAMITE_SMOKE_XVFB=1 ctest --test-dir build/linux --output-on-failure'
```
