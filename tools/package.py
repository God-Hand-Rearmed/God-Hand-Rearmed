"""Packs a build of God Hand: Rearmed into the files a release attaches: the Windows zip, the Linux tar.gz and AppImage.
One packer for both systems, so the two releases ship by the same rules (what goes in, the names, the checks).

    python -I tools/package.py --bin build/port/bin [--out dist] [--formats ...] [--allow-stub]
                               [--appimage-runtime FILE] [--allow-nonportable]
    python -I build.py package [the same options]           (--bin build/port/bin given)

The system is the build's: GHR.exe in --bin is a Windows build, an ELF GHR a Linux one (a zip is packed on Windows or
Linux alike: it needs Python only).

  dist/GHR-v<version>-Windows-x64.zip    "God Hand Rearmed/": GHR.exe, its DLLs, resources/, recomplauncher.json,
                                             README.txt, LICENSE and docs/licenses/. RecompLauncher picks a release's
                                             file by its system and CPU words; the launcher's updater takes the .zip.
  dist/GHR-v<version>.pdb                GHR.exe's symbols (the PDB whose GUID GHR.exe names), for reading a
                                             crash report (not for players).
  dist/GHR-v<version>-Linux-x64.tar.gz   "God Hand Rearmed/": GHR, its libraries, resources/, README.txt, LICENSE,
                                             licenses/, the icon and install-desktop-entry.sh. Unpack it anywhere you
                                             can write to; it is a play folder exactly like the Windows zip (settings
                                             and saves inside it).
  dist/GHR-v<version>-Linux-x64.AppImage One file. Settings and saves go to ~/.local/share/GodHandRearmed, or to a
                                             folder named GHR-data next to the AppImage when there is one (AppRun).
  dist/GHR-v<version>.debug              GHR's (Linux) debug info, linked by GNU debuglink (not for players).

None carries GHR.ini or controls.ini: GHR writes its defaults at the first start, and a folder updated in place keeps
the player's. Nor saves, logs, caches, mods/, texture packs, developer tools, symbols or dumps: what is shipped is
chosen by name (program_files), not by leaving out what a run may have left in the bin folder.

The names are what the launcher's updater looks for (src/kamite/update_archive.cpp, pickAsset): Windows takes the .zip,
an AppImage the newest "-Linux-x64.AppImage", a Linux folder install the "-Linux-x64.tar.gz".

Checks before anything is written (the release is refused when one fails):
- the build is not one on the stand-in game code (STUB-GAME-CODE.txt from tools/linux/stub_game_code.py), unless
  --allow-stub, which also puts "-stub" in the names;
- Windows: every shipped .exe / .dll is a 64-bit (x64) program, and none needs the debug C runtime (a Debug build:
  ucrtbased.dll, vcruntime140d.dll, msvcp140d.dll are on no player's PC). The DLLs needed from Windows itself and from
  the Visual C++ Redistributable are printed;
- Linux: every ELF file shipped needs only libraries shipped beside it or the base system's (glibc's own libraries),
  and no glibc symbol newer than 2.31 (the Steam Runtime's sniper, Debian 11, Ubuntu 20.04): the portable toolchain
  (cmake/toolchains/linux-portable.cmake) gives that, a distro compiler does not; --allow-nonportable skips it for a
  build only meant for the machine it was made on.

Debug info (Linux): the shipped binaries are stripped of it (objcopy --strip-debug; the dynamic symbols backtraces
print stay) and GHR's goes to the .debug file. Windows keeps it in the .pdb files, none of which is shipped.

Reproducible: file times are SOURCE_DATE_EPOCH (default: the last commit's time), owners are root, entries sorted;
the zip's entries are deflated at level 9 with MS-DOS attributes, whatever system packs it. The same input files and
the same zlib give the same bytes (a Python built with zlib-ng deflates differently; a Windows checkout with autocrlf
has CRLF texts in resources/, so it is other input).
The AppImage is the type2 runtime (pinned below, checked by SHA-256) followed by a squashfs of the AppDir (mksquashfs
from squashfs-tools; zstd). No appimagetool and no FUSE are needed to build one.
"""
import argparse
import gzip
import hashlib
import io
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# The AppImage runtime (github.com/AppImage/type2-runtime, release 20251108). flake.nix fetches the same file with the
# same hash (tools/linux/test_package.py checks the two agree).
RUNTIME_URL = "https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64"
RUNTIME_SHA256 = "2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d"

APP_ID = "godhandrearmed"
APP_NAME = "God Hand: Rearmed"
FOLDER_NAME = "God Hand Rearmed"
GLIBC_FLOOR = (2, 31)
# Libraries every glibc system has: the only ones a shipped binary may need from outside the package.
SYSTEM_LIBRARIES = {"libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1", "ld-linux-x86-64.so.2"}
ICON_SIZES = (16, 24, 32, 48, 64, 128, 256)
# Libraries next to GHR that it opens with dlopen (so no DT_NEEDED names them): shipped when the build made them.
# (VKShaderCache.cpp opens shaderc by its versioned name, GetVersionedFilename("shaderc_shared", 1).)
DLOPENED = ("libpcsx2gs.so", "libshaderc_shared.so.1")

DESKTOP_ENTRY = """[Desktop Entry]
Type=Application
Name=God Hand: Rearmed
GenericName=Action game
Comment=Native PC port of God Hand (PS2). Plays your own disc image.
Exec={exec}
Icon={icon}
Terminal=false
Categories=Game;ActionGame;
Keywords=God Hand;Clover;PS2;
"""

APPRUN = """#!/bin/sh
# God Hand: Rearmed's AppImage entry point (tools/package.py). The AppImage is mounted read-only, so GHR's play
# folder (GHR.ini, controls.ini, memcards, states, logs, mods, texture packs, the game/ folder) lives elsewhere:
#   - a folder named GHR-data next to the AppImage, when there is one (portable: everything beside the file);
#   - else ${XDG_DATA_HOME:-~/.local/share}/GodHandRearmed.
# KAMITE_HOME set by hand wins over both. GHR reads it (src/kamite/paths.cpp); its resources stay inside the AppImage.
HERE="${APPDIR:-$(dirname "$(readlink -f "$0")")}"
if [ -z "${KAMITE_HOME:-}" ]; then
    if [ -n "${APPIMAGE:-}" ] && [ -d "$(dirname "$APPIMAGE")/GHR-data" ]; then
        KAMITE_HOME="$(dirname "$APPIMAGE")/GHR-data"
    else
        KAMITE_HOME="${XDG_DATA_HOME:-${HOME:-/tmp}/.local/share}/GodHandRearmed"
    fi
fi
mkdir -p "$KAMITE_HOME" || { echo "GHR: cannot create the play folder $KAMITE_HOME" >&2; exit 1; }
export KAMITE_HOME
cd "$KAMITE_HOME" || exit 1
exec "$HERE/usr/lib/ghr/GHR" "$@"
"""

INSTALL_DESKTOP_ENTRY = """#!/bin/sh
# Adds God Hand: Rearmed to your desktop's application menu (Steam Deck: Desktop Mode), pointing at this folder.
# Run it again after moving the folder. Remove with: rm ~/.local/share/applications/godhandrearmed.desktop
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
APPS="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
ICONS="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/256x256/apps"
mkdir -p "$APPS" "$ICONS"
cp "$HERE/godhandrearmed.png" "$ICONS/godhandrearmed.png"
{
    printf '[Desktop Entry]\\nType=Application\\nName=God Hand: Rearmed\\nGenericName=Action game\\n'
    printf 'Comment=Native PC port of God Hand (PS2). Plays your own disc image.\\n'
    printf 'Exec="%s/GHR"\\nPath=%s\\nIcon=godhandrearmed\\nTerminal=false\\nCategories=Game;ActionGame;\\n' "$HERE" "$HERE"
} > "$APPS/godhandrearmed.desktop"
echo "Added: $APPS/godhandrearmed.desktop"
"""


class PackageError(Exception):
    pass


# ---- facts about the checkout -------------------------------------------------------------------------------------

def read_version(root: Path = ROOT) -> str:
    text = (root / "src" / "kamite" / "version.h").read_text(encoding="utf-8")
    match = re.search(r'#define\s+KAMITE_VERSION\s+"([^"]+)"', text)
    if not match:
        raise PackageError("src/kamite/version.h has no KAMITE_VERSION")
    return match.group(1)


def source_date_epoch(root: Path = ROOT) -> int:
    if os.environ.get("SOURCE_DATE_EPOCH"):
        return int(os.environ["SOURCE_DATE_EPOCH"])
    try:
        out = subprocess.run(["git", "-C", str(root), "log", "-1", "--format=%ct"], capture_output=True, text=True)
        if out.returncode == 0 and out.stdout.strip():
            return int(out.stdout.strip())
    except OSError:
        pass
    return 0


def icon_pngs(ico: Path) -> dict[int, bytes]:
    """The PNG images inside src/kamite/ghr.ico by size (tools/icon/make_ico.py stores every size as a PNG)."""
    data = ico.read_bytes()
    _, kind, count = struct.unpack_from("<HHH", data, 0)
    if kind != 1:
        raise PackageError(f"{ico} is not an icon file")
    images = {}
    for i in range(count):
        width, _, _, _, _, _, size, offset = struct.unpack_from("<BBBBHHII", data, 6 + 16 * i)
        blob = data[offset:offset + size]
        if blob.startswith(b"\x89PNG\r\n\x1a\n"):
            images[width or 256] = blob
    if 256 not in images:
        raise PackageError(f"{ico} has no 256x256 PNG image")
    return images


# ---- ELF: what a binary needs ---------------------------------------------------------------------------------------

def elf_requirements(path: Path):
    """(needed libraries, glibc versions) of a 64-bit little-endian ELF file, or None when it is not one. Read from the
    section table: .dynamic's DT_NEEDED entries and .gnu.version_r's GLIBC_x.y names, both as strings in .dynstr."""
    data = path.read_bytes()
    if data[:4] != b"\x7fELF":
        return None
    if data[4] != 2 or data[5] != 1:
        raise PackageError(f"{path}: not a 64-bit little-endian ELF file")
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    sections = []
    for i in range(shnum):
        name, kind, _, _, offset, size, link, info, _, _ = struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize)
        sections.append((name, kind, offset, size, link, info))

    def text_at(strtab, at):
        start = sections[strtab][2] + at
        return data[start:data.index(b"\0", start)].decode("utf-8", "replace")

    needed, versions = [], set()
    for name, kind, offset, size, link, info in sections:
        if kind == 6:  # SHT_DYNAMIC
            for at in range(offset, offset + size, 16):
                tag, value = struct.unpack_from("<qQ", data, at)
                if tag == 0:
                    break
                if tag == 1:  # DT_NEEDED
                    needed.append(text_at(link, value))
        elif kind == 0x6FFFFFFE:  # SHT_GNU_verneed
            at = offset
            for _ in range(info):
                _, count, _, aux, nxt = struct.unpack_from("<HHIII", data, at)
                aux_at = at + aux
                for _ in range(count):
                    _, _, _, vname, aux_next = struct.unpack_from("<IHHII", data, aux_at)
                    version = text_at(link, vname)
                    if version.startswith("GLIBC_"):
                        versions.add(version)
                    aux_at += aux_next
                at += nxt
    return needed, versions


def glibc_version(name: str) -> tuple:
    return tuple(int(part) for part in re.findall(r"\d+", name))


def check_portable(folder: Path) -> list[str]:
    """Problems that keep the folder's binaries from running on another Linux machine (empty = none)."""
    problems = []
    shipped = {p.name for p in folder.iterdir() if p.is_file()}
    for path in sorted(folder.iterdir()):
        if not path.is_file() or path.is_symlink():
            continue
        found = elf_requirements(path)
        if found is None:
            continue
        needed, versions = found
        for library in needed:
            if library not in shipped and library not in SYSTEM_LIBRARIES:
                problems.append(f"{path.name} needs {library}, which is neither shipped nor part of glibc")
        newest = max(versions, key=glibc_version, default=None)
        if newest and glibc_version(newest) > GLIBC_FLOOR:
            problems.append(f"{path.name} needs {newest} (the floor is GLIBC_{GLIBC_FLOOR[0]}.{GLIBC_FLOOR[1]})")
    return problems


# ---- the program folder -------------------------------------------------------------------------------------------

def program_files(bin_dir: Path) -> list[Path]:
    """What is shipped from the build's bin folder: GHR, its shared libraries and resources/. Nothing else that a run
    may have left there (GHR.ini, logs, memcards, caches)."""
    exe = bin_dir / "GHR"
    if not exe.is_file():
        raise PackageError(f"{exe} is missing (build the kamite target first)")
    if elf_requirements(exe) is None:
        raise PackageError(f"{exe} is not an ELF executable (a Windows build?)")
    if not (bin_dir / "resources").is_dir():
        raise PackageError(f"{bin_dir / 'resources'} is missing")
    files = [exe, bin_dir / "resources"]
    # The libraries shipped are the ones GHR loads, directly or through another shipped library (DT_NEEDED), under the
    # name it loads them by (libSDL3.so.0), as regular files: the build's symlink chain (libSDL3.so -> libSDL3.so.0 ->
    # libSDL3.so.0.4.10) is resolved at staging, because the launcher's updater (update_archive.cpp) writes regular
    # files only and would drop a symlink, leaving an updated install without the library. Libraries loaded at run
    # time with dlopen are not in DT_NEEDED: they go in DLOPENED.
    wanted, queue = set(), [exe]
    while queue:
        needed, _ = elf_requirements(queue.pop().resolve())
        for name in needed:
            path = bin_dir / name
            if name not in wanted and path.exists():
                wanted.add(name)
                queue.append(path)
    wanted |= {name for name in DLOPENED if (bin_dir / name).exists()}
    files += [bin_dir / name for name in sorted(wanted)]
    return files


def strip_debug(folder: Path, objcopy: str, debug_out: Path | None) -> None:
    """Debug info out of every ELF file in folder; GHR's kept in debug_out first (with a debuglink to it)."""
    for path in sorted(folder.iterdir()):
        if not path.is_file() or path.is_symlink() or elf_requirements(path) is None:
            continue
        if path.name == "GHR" and debug_out is not None:
            subprocess.run([objcopy, "--only-keep-debug", str(path), str(debug_out)], check=True)
            debug_out.chmod(0o644)
        subprocess.run([objcopy, "--strip-debug", str(path)], check=True)
        if path.name == "GHR" and debug_out is not None:
            # The link names the file next to the archives: copy it beside GHR (or into a debug folder) to use it.
            subprocess.run([objcopy, f"--add-gnu-debuglink={debug_out}", str(path)], check=True, cwd=debug_out.parent)


def stage_program(bin_dir: Path, dest: Path) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    for path in program_files(bin_dir):
        target = dest / path.name
        if path.is_dir():
            shutil.copytree(path, target, symlinks=True)
        elif path.is_symlink():
            shutil.copy2(path.resolve(), target)  # libSDL3.so.0's contents under that name (program_files)
        else:
            shutil.copy2(path, target)
    # Copies from a read-only place (a Nix store path) keep its modes: made writable so stripping can rewrite them.
    for path in [dest, *dest.rglob("*")]:
        if not path.is_symlink():
            path.chmod(path.stat().st_mode | stat.S_IWUSR)
    exe = dest / "GHR"
    exe.chmod(exe.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def stage_docs(dest: Path, readme: str, root: Path = ROOT) -> None:
    (dest / "README.txt").write_text(readme, encoding="utf-8")
    shutil.copy2(root / "LICENSE", dest / "LICENSE")
    shutil.copytree(root / "docs" / "licenses", dest / "licenses")


def readme_text(version: str, root: Path = ROOT) -> str:
    return (root / "tools" / "linux" / "README-linux.txt").read_text(encoding="utf-8").replace("{version}", version)


# ---- archives -----------------------------------------------------------------------------------------------------

def normalize(info: tarfile.TarInfo, epoch: int) -> tarfile.TarInfo:
    info.uid = info.gid = 0
    info.uname = info.gname = "root"
    info.mtime = epoch
    if info.isdir() or info.mode & stat.S_IXUSR:
        info.mode = 0o755
    elif not info.issym():
        info.mode = 0o644
    return info


def write_tar(folder: Path, top: str, out: Path, epoch: int) -> None:
    """A reproducible .tar.gz of folder under the name top/ (sorted entries, fixed owners, times and gzip header)."""
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.GNU_FORMAT) as tar:
        paths = [folder] + sorted(folder.rglob("*"), key=lambda p: p.relative_to(folder).as_posix())
        for path in paths:
            rel = path.relative_to(folder).as_posix()
            name = top if rel == "." else f"{top}/{rel}"
            info = normalize(tar.gettarinfo(str(path), arcname=name), epoch)
            if info.isreg():
                with open(path, "rb") as handle:
                    tar.addfile(info, handle)
            else:
                tar.addfile(info)
    with open(out, "wb") as handle, gzip.GzipFile(filename="", mode="wb", fileobj=handle, mtime=epoch, compresslevel=9) as gz:
        gz.write(raw.getvalue())


def appimage_runtime(path: str | None, cache: Path) -> bytes:
    if path:
        data = Path(path).read_bytes()
    else:
        cached = cache / "runtime-x86_64"
        if not cached.exists():
            cache.mkdir(parents=True, exist_ok=True)
            print(f"downloading the AppImage runtime: {RUNTIME_URL}")
            with urllib.request.urlopen(RUNTIME_URL, timeout=60) as response:
                cached.write_bytes(response.read())
        data = cached.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != RUNTIME_SHA256:
        raise PackageError(f"the AppImage runtime's SHA-256 is {digest}, expected {RUNTIME_SHA256}")
    return data


def write_appimage(appdir: Path, runtime: bytes, out: Path, epoch: int, mksquashfs: str) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        squash = Path(tmp) / "fs.squashfs"
        cmd = [mksquashfs, str(appdir), str(squash), "-root-owned", "-noappend", "-no-xattrs", "-comp", "zstd",
               "-Xcompression-level", "19", "-b", "1M", "-all-time", str(epoch), "-mkfs-time", str(epoch),
               "-processors", "4", "-quiet"]
        # The times are given on the command line; mksquashfs refuses SOURCE_DATE_EPOCH alongside them.
        env = {k: v for k, v in os.environ.items() if k != "SOURCE_DATE_EPOCH"}
        result = subprocess.run(cmd, capture_output=True, text=True, env=env)
        if result.returncode:
            raise PackageError(f"mksquashfs failed: {result.stderr.strip() or result.stdout.strip()}")
        with open(out, "wb") as handle:
            handle.write(runtime)
            with open(squash, "rb") as fs:
                shutil.copyfileobj(fs, handle)
    out.chmod(0o755)


# ---- the two forms ------------------------------------------------------------------------------------------------

def build_tar(bin_dir: Path, out_dir: Path, base: str, version: str, epoch: int, stub: bool, objcopy: str | None,
              root: Path = ROOT) -> Path:
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp) / FOLDER_NAME
        stage_program(bin_dir, folder)
        if objcopy:
            strip_debug(folder, objcopy, out_dir / f"{base}.debug")
        stage_docs(folder, readme_text(version, root), root)
        (folder / f"{APP_ID}.png").write_bytes(icon_pngs(root / "src" / "kamite" / "ghr.ico")[256])
        script = folder / "install-desktop-entry.sh"
        script.write_text(INSTALL_DESKTOP_ENTRY, encoding="utf-8")
        script.chmod(0o755)
        if stub:
            shutil.copy2(bin_dir / "STUB-GAME-CODE.txt", folder / "STUB-GAME-CODE.txt")
        out = out_dir / f"{base}-Linux-x64.tar.gz"
        write_tar(folder, FOLDER_NAME, out, epoch)
    return out


def build_appimage(bin_dir: Path, out_dir: Path, base: str, version: str, epoch: int, stub: bool, runtime: bytes,
                   mksquashfs: str, objcopy: str | None, root: Path = ROOT) -> Path:
    with tempfile.TemporaryDirectory() as tmp:
        appdir = Path(tmp) / "AppDir"
        program = appdir / "usr" / "lib" / "ghr"
        stage_program(bin_dir, program)
        if objcopy:
            strip_debug(program, objcopy, out_dir / f"{base}.debug")
        if stub:
            shutil.copy2(bin_dir / "STUB-GAME-CODE.txt", program / "STUB-GAME-CODE.txt")
        docs = appdir / "usr" / "share" / "doc" / "ghr"
        docs.mkdir(parents=True)
        stage_docs(docs, readme_text(version, root), root)
        icons = icon_pngs(root / "src" / "kamite" / "ghr.ico")
        for size in ICON_SIZES:
            if size in icons:
                target = appdir / "usr" / "share" / "icons" / "hicolor" / f"{size}x{size}" / "apps" / f"{APP_ID}.png"
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(icons[size])
        (appdir / f"{APP_ID}.png").write_bytes(icons[256])
        os.symlink(f"{APP_ID}.png", appdir / ".DirIcon")
        entry = DESKTOP_ENTRY.format(exec="GHR", icon=APP_ID) + f"X-AppImage-Version={version}\n"
        (appdir / f"{APP_ID}.desktop").write_text(entry, encoding="utf-8")
        applications = appdir / "usr" / "share" / "applications"
        applications.mkdir(parents=True)
        (applications / f"{APP_ID}.desktop").write_text(entry, encoding="utf-8")
        apprun = appdir / "AppRun"
        apprun.write_text(APPRUN, encoding="utf-8")
        apprun.chmod(0o755)
        for path in [appdir, *appdir.rglob("*")]:
            if not path.is_symlink():
                os.utime(path, (epoch, epoch))
        out = out_dir / f"{base}-Linux-x64.AppImage"
        write_appimage(appdir, runtime, out, epoch, mksquashfs)
    return out


# ---- Windows --------------------------------------------------------------------------------------------------------

# The debug C runtime: on no player's PC (it comes with Visual Studio only). A shipped binary needing one is a Debug build.
DEBUG_RUNTIMES = {"ucrtbased.dll", "vcruntime140d.dll", "vcruntime140_1d.dll", "msvcp140d.dll", "concrt140d.dll"}
# The Visual C++ Redistributable's DLLs (the release build links the C runtime as a DLL, /MD: CMake's default).
VC_REDIST = re.compile(r"^(vcruntime140(_1)?|msvcp140(_1|_2|_atomic_wait|_codecvt_ids)?|concrt140|vcomp140)\.dll$")


def pe_imports(path: Path):
    """The DLL names (lower case) a Windows x64 program or DLL imports, at load time or delayed, or None when the file
    is not a PE file. Read from the optional header's import (1) and delay-import (13) directories. A file cut short or
    otherwise damaged (a header, a section or a name past its end, an import outside the file's section data) is a
    PackageError: a broken DLL is never shipped as one with nothing to import."""
    data = path.read_bytes()
    if data[:2] != b"MZ" or len(data) < 0x40:
        return None
    try:
        return _pe_imports(path.name, data)
    except (struct.error, ValueError) as error:  # a read past the end, a name with no terminating NUL
        raise PackageError(f"{path.name}: a damaged or truncated PE file ({error})") from None


def _pe_imports(name: str, data: bytes):
    pe, = struct.unpack_from("<I", data, 0x3C)
    if data[pe:pe + 4] != b"PE\0\0":
        return None
    machine, sections_count = struct.unpack_from("<HH", data, pe + 4)
    optional_size, = struct.unpack_from("<H", data, pe + 20)
    optional = pe + 24
    magic, = struct.unpack_from("<H", data, optional)
    if machine != 0x8664 or magic != 0x20B:
        raise PackageError(f"{name}: not a 64-bit x64 Windows program (machine {machine:#x}, magic {magic:#x})")
    rva_count, = struct.unpack_from("<I", data, optional + 108)
    directories = [struct.unpack_from("<II", data, optional + 112 + 8 * i) for i in range(min(rva_count, 16))]
    sections = []
    for i in range(sections_count):
        vsize, va, raw_size, raw = struct.unpack_from("<IIII", data, optional + optional_size + 40 * i + 8)
        if raw_size and raw + raw_size > len(data):
            raise PackageError(f"{name}: truncated (a section's data ends at {raw + raw_size:#x}, the file at "
                               f"{len(data):#x})")
        sections.append((va, raw_size, raw))

    def offset(rva):
        # Only bytes the file holds: a section's tail past its raw data is zeros at load time, never an import table.
        for va, raw_size, raw in sections:
            if va <= rva < va + raw_size:
                return rva - va + raw
        raise PackageError(f"{name}: an import names an address outside its sections' data ({rva:#x})")

    def text_at(rva):
        start = offset(rva)
        return data[start:data.index(b"\0", start)].decode("ascii", "replace").lower()

    names = set()
    for directory, size, name_at in ((1, 20, 12), (13, 32, 4)):  # IMAGE_IMPORT_DESCRIPTOR / ImgDelayDescr: Name
        if directory >= len(directories) or not directories[directory][0]:
            continue
        at = offset(directories[directory][0])
        while True:
            entry = data[at:at + size]
            if len(entry) < size:
                raise PackageError(f"{name}: the import table runs past the end of the file")
            if not any(entry):
                break
            if directory == 13 and not entry[0] & 1:
                raise PackageError(f"{name}: an old-style (VA) delay-import descriptor, which no x64 linker writes")
            names.add(text_at(struct.unpack_from("<I", entry, name_at)[0]))
            at += size
    return names


# DLLs GHR opens at run time (LoadLibrary), so no import table names them: shipped when the build made them.
RUNTIME_LOADED = ("pcsx2gs.dll", "shaderc_shared.dll")
# Windows' own DLLs that graphics and input overlays replace by sitting beside the program (ReShade, DXVK, Special K,
# input remappers). One in the bin folder is a developer's tool, never part of the game.
PROXY_DLLS = {"d3d8.dll", "d3d9.dll", "d3d10.dll", "d3d10core.dll", "d3d11.dll", "d3d12.dll", "dxgi.dll", "ddraw.dll",
              "opengl32.dll", "dinput.dll", "dinput8.dll", "xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll",
              "winmm.dll", "version.dll", "dsound.dll", "winhttp.dll", "dbghelp.dll"}


def program_files_windows(bin_dir: Path) -> list[Path]:
    """What the zip ships from the build's bin folder: GHR.exe, the DLLs beside it that it loads (its imports, theirs,
    and RUNTIME_LOADED), resources/ and recomplauncher.json. Nothing else a build or a run leaves there (.pdb, .ilk,
    GHR.ini, logs, memcards, caches, a developer's overlay DLL)."""
    exe = bin_dir / "GHR.exe"
    if not exe.is_file():
        raise PackageError(f"{exe} is missing (build the kamite target first)")
    if pe_imports(exe) is None:
        raise PackageError(f"{exe} is not a Windows program")
    for needed in ("resources", "recomplauncher.json"):
        if not (bin_dir / needed).exists():
            raise PackageError(f"{bin_dir / needed} is missing")
    beside = {p.name.lower(): p for p in bin_dir.iterdir() if p.is_file() and p.suffix.lower() == ".dll"}
    proxies = sorted(beside[n].name for n in beside if n in PROXY_DLLS)
    if proxies:
        raise PackageError(f"{bin_dir} holds {', '.join(proxies)}: Windows' own DLL name(s), there only as an overlay "
                           "or a tool's copy (ReShade, DXVK...). Remove them: GHR.exe would load them on every player's PC")
    wanted, queue = set(), [exe]
    queue += [beside[n] for n in RUNTIME_LOADED if n in beside]
    wanted |= {n for n in RUNTIME_LOADED if n in beside}
    while queue:
        for name in pe_imports(queue.pop()) or ():
            if name in beside and name not in wanted:
                wanted.add(name)
                queue.append(beside[name])
    dlls = sorted((beside[n] for n in wanted), key=lambda p: p.name.lower())
    return [exe, *dlls, bin_dir / "recomplauncher.json", bin_dir / "resources"]


def check_windows(files: list[Path]) -> tuple[list[str], list[str]]:
    """(problems that refuse the zip, the DLLs needed from outside it) for the shipped programs and DLLs."""
    shipped = {p.name.lower() for p in files if p.is_file()}
    problems, outside = [], set()
    for path in files:
        if not path.is_file() or path.suffix.lower() not in (".exe", ".dll"):
            continue
        imports = pe_imports(path)
        if imports is None:
            problems.append(f"{path.name} is not a Windows program or DLL")
            continue
        for name in sorted(imports & DEBUG_RUNTIMES):
            problems.append(f"{path.name} needs {name}, the debug C runtime (a Debug build): no player has it")
        outside |= imports - shipped - DEBUG_RUNTIMES
    return problems, sorted(outside)


def readme_windows(version: str, root: Path = ROOT) -> str:
    text = (root / "tools" / "win" / "README-windows.txt").read_text(encoding="utf-8").replace("{version}", version)
    return text.replace("\r\n", "\n").replace("\n", "\r\n")


def zip_time(epoch: int) -> tuple:
    import time
    return max(time.gmtime(epoch)[:6], (1980, 1, 1, 0, 0, 0))  # a zip can't say earlier than 1980


MAX_ZIP_ENTRIES = 0xFFFF  # the end record's 16-bit count; past it a zip needs zip64


def write_zip(folder: Path, top: str, out: Path, epoch: int) -> None:
    """A reproducible .zip of folder under the name top/: sorted entries, one fixed time, MS-DOS attributes, deflate at
    level 9. What the launcher's updater reads (launcher_update.cpp readZip): stored or deflated entries, no
    encryption, no zip64 (every file and the zip under 2 GB, under 65535 entries), one top folder that it strips.
    Same bytes for the same input files and the same zlib: Python's zlib decides the deflated bytes (one built with
    zlib-ng packs differently), and a Windows checkout with autocrlf has other bytes in resources/' texts."""
    when = zip_time(epoch)
    files = sorted((p for p in folder.rglob("*") if p.is_file()), key=lambda p: p.relative_to(folder).as_posix())
    if len(files) >= MAX_ZIP_ENTRIES:
        raise PackageError(f"{len(files)} files: more than a zip without zip64 holds (the updater reads no zip64)")
    partial = out.with_name(out.name + ".partial")  # a failed run leaves no zip that looks finished
    try:
        with zipfile.ZipFile(partial, "w", allowZip64=False) as archive:
            # Files only, no folder entries (the updater makes the folders a file needs), as the maintainer's zips.
            for path in files:
                info = zipfile.ZipInfo(f"{top}/{path.relative_to(folder).as_posix()}", date_time=when)
                info.create_system = 0  # MS-DOS, not the packing system's: no Unix modes in external_attr
                info.external_attr = 0x20  # FILE_ATTRIBUTE_ARCHIVE
                info.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(info, path.read_bytes(), compresslevel=9)
    except zipfile.LargeZipFile as error:
        partial.unlink(missing_ok=True)
        raise PackageError(f"too big for a zip without zip64, which the updater can't read ({error})") from None
    except BaseException:
        partial.unlink(missing_ok=True)
        raise
    partial.replace(out)


MSF_MAGIC = b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0"


def pe_pdb_guid(path: Path):
    """The GUID of the PDB a Windows program was linked with (its debug directory's CodeView "RSDS" record), or None."""
    data = path.read_bytes()
    try:
        pe, = struct.unpack_from("<I", data, 0x3C)
        sections_count, = struct.unpack_from("<H", data, pe + 6)
        optional_size, = struct.unpack_from("<H", data, pe + 20)
        optional = pe + 24
        rva_count, = struct.unpack_from("<I", data, optional + 108)
        if rva_count <= 6:
            return None
        debug_rva, debug_size = struct.unpack_from("<II", data, optional + 112 + 8 * 6)
        for i in range(sections_count):
            vsize, va, raw_size, raw = struct.unpack_from("<IIII", data, optional + optional_size + 40 * i + 8)
            if va <= debug_rva < va + raw_size:
                at = debug_rva - va + raw
                break
        else:
            return None
        for entry in range(at, at + debug_size, 28):  # IMAGE_DEBUG_DIRECTORY
            kind, = struct.unpack_from("<I", data, entry + 12)
            pointer, = struct.unpack_from("<I", data, entry + 24)
            if kind == 2 and data[pointer:pointer + 4] == b"RSDS":  # IMAGE_DEBUG_TYPE_CODEVIEW
                guid = data[pointer + 4:pointer + 20]
                return guid if len(guid) == 16 else None
    except struct.error:
        return None
    return None


def pdb_guid(path: Path):
    """The GUID in a PDB's info stream (stream 1 of its MSF 7.0 container), or None when it is not a readable PDB."""
    data = path.read_bytes()
    if not data.startswith(MSF_MAGIC):
        return None
    try:
        block, _, _, directory_bytes, _, map_block = struct.unpack_from("<IIIIII", data, 32)
        if block not in (512, 1024, 2048, 4096):
            return None
        blocks = -(-directory_bytes // block)
        directory = b"".join(data[b * block:(b + 1) * block]
                             for b in struct.unpack_from(f"<{blocks}I", data, map_block * block))[:directory_bytes]
        streams, = struct.unpack_from("<I", directory, 0)
        if streams < 2:
            return None
        sizes = struct.unpack_from(f"<{streams}I", directory, 4)
        first = 4 + 4 * streams + 4 * (-(-sizes[0] // block) if sizes[0] != 0xFFFFFFFF else 0)
        info_block, = struct.unpack_from("<I", directory, first)
        guid = data[info_block * block + 12:info_block * block + 28]  # version, signature, age, then the GUID
        return guid if len(guid) == 16 else None
    except struct.error:
        return None


def crlf(data: bytes) -> bytes:
    return data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")


def build_zip(bin_dir: Path, out_dir: Path, base: str, version: str, epoch: int, stub: bool, files: list[Path],
              root: Path = ROOT) -> list[Path]:
    """The zip, and GHR.exe's symbols beside it when the build has them."""
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp) / FOLDER_NAME
        folder.mkdir()
        for path in files:
            if path.is_dir():
                shutil.copytree(path, folder / path.name)
            else:
                shutil.copy2(path, folder / path.name)
        # The texts a player opens, with Windows line ends (Notepad), as the maintainer's zips have them.
        (folder / "README.txt").write_bytes(readme_windows(version, root).encode("utf-8"))
        (folder / "LICENSE").write_bytes(crlf((root / "LICENSE").read_bytes()))
        licenses = folder / "docs" / "licenses"
        licenses.mkdir(parents=True)
        for path in sorted((root / "docs" / "licenses").iterdir()):
            if path.is_file():
                (licenses / path.name).write_bytes(crlf(path.read_bytes()))
        if stub:
            shutil.copy2(bin_dir / "STUB-GAME-CODE.txt", folder / "STUB-GAME-CODE.txt")
        out = out_dir / f"{base}-Windows-x64.zip"
        write_zip(folder, FOLDER_NAME, out, epoch)
    made = [out]
    pdb = bin_dir / "GHR.pdb"
    # Only GHR.exe's own symbols: the PDB whose GUID GHR.exe names (an earlier build's would read its crashes wrong).
    if pdb.is_file():
        wanted, found = pe_pdb_guid(bin_dir / "GHR.exe"), pdb_guid(pdb)
        if wanted is not None and wanted == found:
            made.append(out_dir / f"{base}.pdb")
            shutil.copy2(pdb, made[-1])
        else:
            print(f"warning: {pdb} is not GHR.exe's own (another build's, or not a PDB): not copied")
    return made


def detect_system(bin_dir: Path) -> str:
    windows, linux = (bin_dir / "GHR.exe").is_file(), (bin_dir / "GHR").is_file()
    if windows and linux:
        raise PackageError(f"{bin_dir} holds both GHR.exe and GHR: one build per bin folder")
    if windows:
        return "windows"
    if linux:
        return "linux"
    raise PackageError(f"{bin_dir} holds neither GHR.exe nor GHR (build the kamite target first)")


FORMATS = {"windows": {"zip"}, "linux": {"tar", "appimage"}}


def package_windows(args, bin_dir: Path, formats: set, base: str, version: str, epoch: int, stub: bool) -> list[Path]:
    files = program_files_windows(bin_dir)
    problems, outside = check_windows(files)
    if problems:
        raise PackageError("not a release build:\n  " + "\n  ".join(problems))
    redist = [name for name in outside if VC_REDIST.match(name)]
    print(f"needs from outside the zip (Windows, or loaded only where present): {', '.join(n for n in outside if n not in redist) or 'nothing'}")
    print(f"needs the Visual C++ Redistributable (x64): {', '.join(redist) or 'no'}")
    args.out.mkdir(parents=True, exist_ok=True)
    return build_zip(bin_dir, args.out, base, version, epoch, stub, files)


def package_linux(args, bin_dir: Path, formats: set, base: str, version: str, epoch: int, stub: bool) -> list[Path]:
    program_files(bin_dir)  # the folder's shape, before anything is checked or written
    problems = check_portable(bin_dir)
    if problems and not args.allow_nonportable:
        raise PackageError("not portable (build with --toolchain cmake/toolchains/linux-portable.cmake):\n  "
                           + "\n  ".join(problems))
    for problem in problems:
        print(f"warning: {problem}")
    objcopy = None if args.keep_debug else args.objcopy
    if not args.keep_debug and not objcopy:
        raise PackageError("objcopy (binutils) not found: install it, or pass --keep-debug")
    args.out.mkdir(parents=True, exist_ok=True)
    made = []
    if "tar" in formats:
        made.append(build_tar(bin_dir, args.out, base, version, epoch, stub, objcopy))
    if "appimage" in formats:
        runtime = appimage_runtime(args.appimage_runtime, ROOT / "build" / "cache")
        made.append(build_appimage(bin_dir, args.out, base, version, epoch, stub, runtime, args.mksquashfs, objcopy))
    debug = args.out / f"{base}.debug"
    if objcopy and debug.exists():
        made.append(debug)
    return made


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--bin", required=True, type=Path, help="the build's bin folder (GHR.exe or GHR, resources/)")
    parser.add_argument("--out", type=Path, default=ROOT / "dist")
    parser.add_argument("--formats", help="comma-separated; Windows: zip; Linux: tar, appimage (default: all of the "
                                          "build's system)")
    parser.add_argument("--version", help="default: src/kamite/version.h")
    parser.add_argument("--appimage-runtime", default=os.environ.get("KAMITE_APPIMAGE_RUNTIME"),
                        help="the type2 runtime file (default: $KAMITE_APPIMAGE_RUNTIME from nix develop, else "
                             "downloaded once into build/cache)")
    parser.add_argument("--mksquashfs", default=shutil.which("mksquashfs") or "mksquashfs")
    parser.add_argument("--objcopy", default=shutil.which("objcopy"), help="for stripping debug info (binutils)")
    parser.add_argument("--keep-debug", action="store_true", help="Linux: ship the binaries with their debug info")
    parser.add_argument("--allow-stub", action="store_true", help="package a stand-in game code build (names: -stub)")
    parser.add_argument("--allow-nonportable", action="store_true", help="Linux: skip the portability check")
    args = parser.parse_args(argv)

    try:
        bin_dir = args.bin.resolve()
        system = detect_system(bin_dir)
        formats = FORMATS[system] if not args.formats else {f.strip() for f in args.formats.split(",") if f.strip()}
        if not formats or formats - FORMATS[system]:
            raise PackageError(f"--formats: a {system} build packs as {', '.join(sorted(FORMATS[system]))}, "
                               f"not {args.formats}")
        stub = (bin_dir / "STUB-GAME-CODE.txt").exists()
        if stub and not args.allow_stub:
            raise PackageError(f"{bin_dir} is a build on the stand-in game code (STUB-GAME-CODE.txt): it cannot play "
                               "the game and is never a release (--allow-stub packs it as a test, named -stub)")
        version = args.version or read_version()
        base = f"GHR-v{version}" + ("-stub" if stub else "")
        epoch = source_date_epoch()
        pack = package_windows if system == "windows" else package_linux
        made = pack(args, bin_dir, formats, base, version, epoch, stub)
    except PackageError as error:
        print(f"package: {error}", file=sys.stderr)
        return 1
    for path in made:
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        print(f"{path}  {path.stat().st_size} bytes  sha256 {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
