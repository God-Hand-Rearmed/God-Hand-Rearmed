# Building and using God Hand: Rearmed

How to build God Hand: Rearmed (codename Kamite) from source, play it, and work on it. The [README](../README.md) has the overview.

## How to set it up

### What you need

1. **Windows 10 or 11** and a reasonably modern graphics card (Linux: see [Linux and Steam Deck](#linux-and-steam-deck)).
2. **Visual Studio 2022 Build Tools** with "Desktop development with C++" (free from Microsoft).
3. **Python 3.10 or newer**.
4. **Your own disc image of the game** (USA, `.iso`, serial SLUS-21503). It must be this exact version: check it against
   [DISC_HASHES.md](../DISC_HASHES.md). The port refuses any other disc.

### Build it

Open a terminal (PowerShell) and run these one at a time:

```powershell
git clone https://github.com/God-Hand-Rearmed/God-Hand-Rearmed.git
cd God-Hand-Rearmed
copy "D:\path\to\your-disc.iso" game\
python -I build.py generate
python -I tools\snr2\snr2.py --iso "game\your-disc.iso" extract
python -I tools\snr2\gen_modules.py
python -I build.py modules
python -I build.py vu
python -I build.py configure -DKAMITE_PCSX2_GS=ON -DKAMITE_SDL_INPUT=ON
python -I build.py build
python -I build.py characters
```

- `generate` and the SNR2 steps turn the code from your disc into C++ and take a while; `vu` compiles the PS2's VU1
  microcode ahead of time; `characters` builds the playable characters from your disc.
- `build` downloads a few libraries the first time and then compiles everything (expect 10+ minutes).

### Play

Double-click **`Start-GHR.cmd`**, or run `build\port\bin\GHR.exe`. The launcher opens first: point it at your
disc image once, then Play. The very first start can sit on a black screen for a couple of minutes while the graphics
are prepared for your card; later starts take seconds.

Your settings are in `build\port\bin\GHR.ini` (every launcher option is a line there, with a comment), your saves in
`build\port\bin\memcards\`, your controls in `build\port\bin\controls.ini`.

### Linux and Steam Deck

The same steps work on Linux (x86-64). The easiest toolchain is Nix: `nix develop` at the top of the checkout gives
CMake, Ninja, GCC, zig, Python, the X11 / Wayland / Vulkan headers and the packaging tools (`flake.nix`). Without Nix,
install CMake 3.21+, Ninja, a C++20 compiler (GCC 12+ or Clang 16+), Python 3.10+, `pkg-config` and the X11 headers
(`libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev`), plus `zig` 0.16 for the portable build below.

```sh
git clone https://github.com/God-Hand-Rearmed/God-Hand-Rearmed.git
cd God-Hand-Rearmed
nix develop                     # or have the tools above installed
cp /path/to/your-disc.iso game/
export KAMITE_ALLOW_DOWNLOADS=1 # the first configure downloads raylib, SDL3, imgui, shaderc...
python3 -I build.py generate
python3 -I tools/snr2/snr2.py --iso game/your-disc.iso extract
python3 -I tools/snr2/gen_modules.py
python3 -I build.py modules
python3 -I build.py vu
python3 -I build.py configure --toolchain cmake/toolchains/linux-portable.cmake
python3 -I build.py build
python3 -I build.py characters
build/port/bin/GHR              # or: python3 -I build.py run
```

- `--toolchain cmake/toolchains/linux-portable.cmake` builds with zig against glibc 2.31 and a static libc++: the
  result runs on any x86-64 Linux with glibc 2.31 or newer (SteamOS, Debian 11+, Ubuntu 20.04+), not only on the
  machine that built it. Leave it out to build with your system's compiler for that machine alone.
- The renderer on Linux is PCSX2's Vulkan renderer (`libpcsx2gs.so`), drawn into the game window through X11 (on a
  Wayland desktop through Xwayland; the Steam Deck's Game Mode is Xwayland too). Direct3D 11 does not exist there.
- Settings, saves and logs are in `build/port/bin`, like on Windows.

To make the files players download (what a release attaches next to the Windows zip):

```sh
python3 -I build.py configure --toolchain cmake/toolchains/linux-portable.cmake -DKAMITE_RELEASE=ON
python3 -I build.py build
python3 -I tools/linux/package.py --bin build/port/bin      # or: nix run .#package -- --bin build/port/bin
```

`dist/` then holds `GHR-v<version>-linux-x86_64.tar.gz`, `GHR-v<version>-x86_64.AppImage` and
`GHR-v<version>.debug` (GHR's debug info, for reading crash addresses; not for players). The packer refuses a build
that is not portable or that was made on the stand-in game code. What players see is `tools/linux/README-linux.txt`
(shipped as README.txt).

#### Linux CI

`.github/workflows/linux.yml` runs `nix flake check` on every push and pull request, then starts the packaged AppImage
and tar.gz on Debian 11, Ubuntu 20.04 and 24.04, Fedora, Arch and the Steam Runtime's sniper. CI has no disc, so
its builds use a stand-in for the recompiled game code (`tools/linux/stub_game_code.py`): everything else (runtime,
launcher, renderer, platform code) is compiled, linked and started, and the artifacts are named `-stub`. The checks:

| Check | What it proves |
|---|---|
| `native` | GHR builds with GCC on the stand-in code; the C++ unit tests (`tests/unit`, `KAMITE_BUILD_TESTS=ON`) pass |
| `portable-binary` | the release toolchain's GHR and libraries need glibc 2.31 at most and no library the package lacks |
| `linux-stub` | `tools/linux/package.py` makes the AppImage and the tar.gz from that build (reproducible: same input, same bytes) |
| `packaging-tests` | `tools/linux/test_*.py`: the packer, the AppRun play-folder rules, the desktop entry, the stand-in |
| `launcher-smoke` | the tar.gz's GHR opens its launcher on a 1280x800 virtual screen and draws it (`tools/linux/smoke_launcher.py`) |

Run them all locally with `nix flake check -L`, one with `nix build .#checks.x86_64-linux.<name> -L`.

### Options

| Where | What |
|---|---|
| Display | Game Window: display mode (fullscreen, maximized window or window) and window size, aspect ratio. Camera: Camera & FOV (presets; with "Show advanced settings": field of view, in fights, distance, tilt, enemies following the view), Steady Idle Camera, See Through. Interface: HUD layout (16:9 edges or 4:3), menu width (4:3 or stretched) |
| Graphics | Renderer: graphics API (Vulkan or Direct3D 11), internal resolution (1x–8x). Effects: post effects (PS2 resolution or upscaled), depth of field |
| Gameplay | 60 FPS; Click-Stick Dash, control style (classic / modern with mouse camera), Busta-Chan Jive; Moveset Presets, Buck Breaker; Cunt Punter, Sway Bar, Tension is Placable (and Off the Cuff), Wild Curry Funk; roulette timer, Target Select, Dry Spinning; Intro Skipper, Hard-On, Mute Music; Better Shop & Casino Poses |
| Controls | Menu controls, button prompts, every key, mouse button and controller button (also F1 in game), the port's hotkeys |
| Mods | Character: Gene or Xavier; HD texture pack and its folder |

In game: **F1** controls, **F2** the dev menu (display, graphics, gameplay, camera, controls, NPCs, debug; options tuned live), **F5** / **F9** save / load state, **F7** HUD layout, **F11** full screen, **F12** debugger. All but F1 are rebindable (GHR.ini [Hotkeys]; the launcher's and the F2 menu's Controls tabs).

---

## Mods

### Playable characters

A character is a folder `mods/characters/<id>/` next to the game with the disc files it replaces (`pl/pl00.dat` is
Gene's model) and, optionally, `textures/` with full-resolution replacements in PCSX2's format. The port writes them
over the game's files as the disc is read, so the game itself is never patched. Xavier is the example mod:
`assets/characters/xavier/` is its source; `tools/models/` has the model and texture writers for the game's formats and the Blender pipeline
that put Xavier onto Gene's skeleton (`tools/models/xavier/README.md`).

### HD textures

Any PCSX2 texture pack for the game (USA, SLUS-21503) works: set its folder in Graphics, or put it at `textures/SLUS-21503/`
next to `GHR.exe`.

---

## Troubleshooting

| Problem | What to do |
|---------|------------|
| "Wrong disc" message | Your image is a different version. Compare it with [DISC_HASHES.md](../DISC_HASHES.md). |
| Black screen on the very first start | Wait: the graphics are being prepared. Later starts are fast. |
| Short pauses the first time an effect appears | Its shaders are being prepared; they do not come back once seen. |
| Controls screen (F1) does not draw | With the PCSX2 renderer it cannot be drawn yet: use the launcher's Controls tab. |
| The game crashes or freezes | Please report it with the log (next section). |

## Reporting bugs

Open an issue and attach the log of your last session: `build\port\bin\logs\play_last.log` (the session before that is
kept as `logs\play_<time>.log`), and `logs\crash.txt` if there is one. Say where in the game it happened and which
options were on (60 FPS, aspect ratio, character).

---

## For developers

- **How it works:** the main program (`SLUS_215.03`) is recompiled to C++ with the recompiler in `runtime/ps2xRecomp`;
  the 81 code overlays the game loads (rooms, events, memory card) are relocated and recompiled by `tools/snr2`, and
  `src/kamite` installs the right one as the game loads it. The PS2 hardware (graphics, sound, I/O processor) is in
  `runtime/`; graphics go through PCSX2's renderer (`thirdparty/pcsx2-gs`). Every game-specific fix is commented where
  it is made.
- **Function names:** `recomp/kamite_functions.csv` carries the boundaries and names of the game's functions; the names
  come from the [decomp](https://github.com/LucasPicoli/god-hand-decomp) and are refreshed with
  `python -I tools/symbols/import_decomp_names.py <decomp checkout>`. The generated code, the crash log, the hang dump,
  the PC sampler and the call profiler all use them.
- **Switchable patches:** `[[patches.conditional]]` and `[[patches.hooks]]` in `recomp/kamite.toml` (and per module in
  `recomp/modules/`) are compiled in both forms and picked by a run-time flag; `src/kamite/frame_patches.cpp` sets the
  flags from `GHR.ini`.
- **Faster 3D:** `python -I build.py vu` compiles every VU1 microcode upload found on the disc. A few uploads are placed
  by the game itself: play with `PS2X_VU1_CAPTURE=<folder>` set, then `python -I build.py vu <folder>` and build again.
- **Diagnostics** (environment or `[Runtime]` in `GHR.ini`): `KAMITE_PC_SAMPLE` (`KAMITE_PC_SAMPLE_ALL=1` adds
  every thread of the process: busy share and hottest host functions, for a frame's bottleneck outside the EE),
  `KAMITE_CALL_PROFILE`, `KAMITE_FIND_COUNTERS`, `KAMITE_TRACE`; F8 in game dumps RAM and the VU1 trace; Shift+F8 writes a hang dump (guest threads, PCs, waits) to the log; F6 writes one
  frame's 2D primitives and every drawn layout's entry table to the log.
- **Watching upstream:** changes in LasVola's recomp and in the decomp are reviewed weekly and ported when they help.
- **The README:** the options list, the controls tables, the presets table and the preset tags on the feature lines are
  generated from the code (`src/kamite/settings_table.cpp`, `presets.cpp`, the default bindings in
  `runtime/ps2xRuntime/src/lib/ps2_host_input.cpp`): `python -I tools/readme/gen_readme.py` after changing any of them
  (`--check` tells you if it's stale). It needs a C++ compiler on PATH (c++, g++, clang++ or cl).

### Releases

- **Version:** `src/kamite/version.h` (`KAMITE_VERSION`, the tag without its "v"); raised in the commit a release is
  built from. The launcher compares it with the GitHub releases and offers newer ones (`src/kamite/launcher_update.cpp`:
  it downloads the release zip, swaps the program files in and restarts; settings, saves, states, cases, logs, mods and
  texture packs are never touched).
- **Release builds:** configure with `-DKAMITE_RELEASE=ON`. It compiles out the developer dump keys and profilers (F2,
  test cases and crash logs stay), gives the built-in default `GHR.ini` the players' values (a visible window, no time
  limit) and turns on the update check. It applies to GHR.exe's own code and the runtime only, so switching it doesn't
  rebuild the generated game code. Dev builds check for updates only with `[Launcher] UpdateTest=1`, and then treat any
  release as newer (to test the updater on a copy of a play folder).
- **The zip:** a `God Hand Rearmed` folder holding GHR.exe, its DLLs, `resources/`, a fresh `GHR.ini` (the players'
  values, no disc image set), `README.txt`, `LICENSE` and `docs/licenses/`; never saves, logs, caches, `mods/`, tools,
  symbols or dumps. Characters are built from the player's disc, so none ship. The maintainer's builder packs it and
  publishes it as a GitHub pre-release named after the tag, with the CHANGELOG lines since the last release as its
  notes and the zip as its one asset (the updater installs the newest release's `.zip`).
