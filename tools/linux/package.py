"""Packs a Linux build of God Hand: Rearmed into the two release forms: a tar.gz folder and an AppImage.

    python -I tools/linux/package.py --bin build/linux/bin [--out dist] [--formats tar,appimage]
                                     [--appimage-runtime FILE] [--allow-stub] [--allow-nonportable]

  dist/GHR-v<version>-linux-x86_64.tar.gz   "God Hand Rearmed/": GHR, its libraries, resources/, a fresh GHR.ini
                                             (the players' values), README.txt, LICENSE, licenses/, the icon and
                                             install-desktop-entry.sh. Unpack it anywhere you can write to; it is a
                                             play folder exactly like the Windows zip (settings and saves inside it).
  dist/GHR-v<version>-x86_64.AppImage        One file. Settings and saves go to ~/.local/share/GodHandRearmed, or to a
                                             folder named GHR-data next to the AppImage when there is one (AppRun).

The names are what the launcher's updater looks for (src/kamite/launcher_update.cpp): the AppImage replaces itself with
the newest "-x86_64.AppImage", a folder install takes the "-linux-x86_64.tar.gz".

Checks before anything is written (the release is refused when one fails):
- the build is not one on the stand-in game code (STUB-GAME-CODE.txt from tools/linux/stub_game_code.py), unless
  --allow-stub, which also puts "-stub" in the names;
- every ELF file shipped needs only libraries shipped beside it or the base system's (glibc's own libraries), and no
  glibc symbol newer than 2.31 (the Steam Runtime's sniper, Debian 11, Ubuntu 20.04): the portable toolchain
  (cmake/toolchains/linux-portable.cmake) gives that, a distro compiler does not; --allow-nonportable skips it for a
  build only meant for the machine it was made on.

Debug info: the shipped binaries are stripped of it (objcopy --strip-debug; the dynamic symbols backtraces print stay)
and GHR's goes to dist/GHR-v<version>.debug, linked by GNU debuglink, for reading a crash report's addresses.

Reproducible: file times are SOURCE_DATE_EPOCH (default: the last commit's time), owners are root, entries sorted.
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
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

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
DLOPENED = ("libpcsx2gs.so", "libshaderc_shared.so", "libshaderc_shared.so.1")

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
# God Hand: Rearmed's AppImage entry point (tools/linux/package.py). The AppImage is mounted read-only, so GHR's play
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


def release_pairs(root: Path = ROOT) -> list[str]:
    """The KEY=VALUE pairs a release's GHR.ini takes (src/CMakeLists.txt, the KAMITE_RELEASE foreach): one list, read
    from there, so the two never drift."""
    text = (root / "src" / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"foreach\(KAMITE_INI_PAIR((?:\s+\"[^\"]*\")+)\)", text)
    if not match:
        raise PackageError("src/CMakeLists.txt: the KAMITE_INI_PAIR list is missing")
    return re.findall(r'"([^"]*)"', match.group(1))


def release_ini(template: str, pairs: list[str]) -> str:
    """GHR.ini with each pair's key set to its value, as CMake does it: "\\nKEY=..." lines replaced (every section)."""
    for pair in pairs:
        key, value = pair.split("=", 1)
        template, count = re.subn(rf"\n{re.escape(key)}=[^\n]*", lambda _m: f"\n{key}={value}", template)
        if count == 0:
            raise PackageError(f"GHR.ini has no {key}= line to set")
    return template


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
        ini = release_ini((root / "GHR.ini").read_text(encoding="utf-8"), release_pairs(root))
        (folder / "GHR.ini").write_text(ini, encoding="utf-8")
        stage_docs(folder, readme_text(version, root), root)
        (folder / f"{APP_ID}.png").write_bytes(icon_pngs(root / "src" / "kamite" / "ghr.ico")[256])
        script = folder / "install-desktop-entry.sh"
        script.write_text(INSTALL_DESKTOP_ENTRY, encoding="utf-8")
        script.chmod(0o755)
        if stub:
            shutil.copy2(bin_dir / "STUB-GAME-CODE.txt", folder / "STUB-GAME-CODE.txt")
        out = out_dir / f"{base}-linux-x86_64.tar.gz"
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
        out = out_dir / f"{base}-x86_64.AppImage"
        write_appimage(appdir, runtime, out, epoch, mksquashfs)
    return out


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--bin", required=True, type=Path, help="the build's bin folder (GHR, libraries, resources/)")
    parser.add_argument("--out", type=Path, default=ROOT / "dist")
    parser.add_argument("--formats", default="tar,appimage", help="comma-separated: tar, appimage")
    parser.add_argument("--version", help="default: src/kamite/version.h")
    parser.add_argument("--appimage-runtime", default=os.environ.get("KAMITE_APPIMAGE_RUNTIME"),
                        help="the type2 runtime file (default: $KAMITE_APPIMAGE_RUNTIME from nix develop, else "
                             "downloaded once into build/cache)")
    parser.add_argument("--mksquashfs", default=shutil.which("mksquashfs") or "mksquashfs")
    parser.add_argument("--objcopy", default=shutil.which("objcopy"), help="for stripping debug info (binutils)")
    parser.add_argument("--keep-debug", action="store_true", help="ship the binaries with their debug info")
    parser.add_argument("--allow-stub", action="store_true", help="package a stand-in game code build (names: -stub)")
    parser.add_argument("--allow-nonportable", action="store_true", help="skip the portability check")
    args = parser.parse_args(argv)

    try:
        formats = {f.strip() for f in args.formats.split(",") if f.strip()}
        if not formats or formats - {"tar", "appimage"}:
            raise PackageError(f"--formats: tar and/or appimage, not {args.formats}")
        bin_dir = args.bin.resolve()
        stub = (bin_dir / "STUB-GAME-CODE.txt").exists()
        if stub and not args.allow_stub:
            raise PackageError(f"{bin_dir} is a build on the stand-in game code (STUB-GAME-CODE.txt): it cannot play "
                               "the game and is never a release (--allow-stub packs it as a test, named -stub)")
        program_files(bin_dir)  # the folder's shape, before anything is checked or written
        problems = check_portable(bin_dir)
        if problems and not args.allow_nonportable:
            raise PackageError("not portable (build with --toolchain cmake/toolchains/linux-portable.cmake):\n  "
                               + "\n  ".join(problems))
        for problem in problems:
            print(f"warning: {problem}")
        version = args.version or read_version()
        base = f"GHR-v{version}" + ("-stub" if stub else "")
        epoch = source_date_epoch()
        args.out.mkdir(parents=True, exist_ok=True)
        objcopy = None if args.keep_debug else args.objcopy
        if not args.keep_debug and not objcopy:
            raise PackageError("objcopy (binutils) not found: install it, or pass --keep-debug")
        made = []
        if "tar" in formats:
            made.append(build_tar(bin_dir, args.out, base, version, epoch, stub, objcopy))
        if "appimage" in formats:
            runtime = appimage_runtime(args.appimage_runtime, ROOT / "build" / "cache")
            made.append(build_appimage(bin_dir, args.out, base, version, epoch, stub, runtime, args.mksquashfs, objcopy))
    except PackageError as error:
        print(f"package: {error}", file=sys.stderr)
        return 1
    debug = args.out / f"{base}.debug"
    if objcopy and debug.exists():
        made.append(debug)
    for path in made:
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        print(f"{path}  {path.stat().st_size} bytes  sha256 {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
