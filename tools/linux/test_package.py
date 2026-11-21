"""Gate tests of the Linux packaging (tools/package.py's tar.gz and AppImage, stub_game_code.py, the AppRun contract;
tools/win/test_package_zip.py tests the same packer's Windows zip).

    python -I -m unittest discover -s tools/linux -p "test_*.py" -v

Deterministic, no network, a few seconds. Tests that need a C compiler (a real ELF to read) or mksquashfs skip
without them; the Nix checks (flake.nix) provide both, so CI always runs everything.
"""
import gzip
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))  # tools/package.py

import package  # noqa: E402
import stub_game_code  # noqa: E402

ROOT = package.ROOT
CC = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")


def compile_elf(folder: Path, name: str, source: str = "int main(void){return 0;}\n", extra=()) -> Path:
    src = folder / f"{name}.c"
    src.write_text(source)
    out = folder / name
    subprocess.run([CC, "-o", str(out), str(src), *extra], check=True, capture_output=True)
    return out


def fake_bin(folder: Path, exe: bytes | None = None) -> Path:
    """A build's bin folder: GHR (an ELF copied from the given bytes, else a compiled one), resources/, a leftover
    GHR.ini and logs/ that must never be shipped."""
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "resources" / "fonts").mkdir(parents=True)
    (folder / "resources" / "fonts" / "a.ttf").write_bytes(b"font")
    (folder / "GHR.ini").write_text("[Run]\nHeadless=1\n")  # a developer's run settings: not shipped
    (folder / "logs").mkdir()
    (folder / "logs" / "kamite.log").write_text("log")
    if exe is None:
        compile_elf(folder, "GHR")
    else:
        (folder / "GHR").write_bytes(exe)
        (folder / "GHR").chmod(0o755)
    return folder


class Facts(unittest.TestCase):
    def test_version_matches_header(self):
        self.assertRegex(package.read_version(), r"^\d+(\.\d+)+$")

    def test_icon_has_every_size(self):
        icons = package.icon_pngs(ROOT / "src" / "kamite" / "ghr.ico")
        for size in package.ICON_SIZES:
            self.assertIn(size, icons)
            self.assertTrue(icons[size].startswith(b"\x89PNG"))

    def test_runtime_pin_matches_flake(self):
        flake = (ROOT / "flake.nix").read_text()
        self.assertIn(package.RUNTIME_URL, flake)
        # flake.nix carries the same SHA-256 (as hex, so the two are compared without converting).
        self.assertIn(package.RUNTIME_SHA256, flake)


@unittest.skipUnless(CC, "needs a C compiler")
class Elf(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_reads_needed_and_glibc_versions(self):
        exe = compile_elf(self.tmp, "a", extra=("-lm",), source="#include <math.h>\n#include <stdio.h>\n"
                          "int main(int c,char**v){printf(\"%f\",cos(c));return 0;}\n")
        needed, versions = package.elf_requirements(exe)
        self.assertIn("libc.so.6", needed)
        self.assertTrue(any(v.startswith("GLIBC_2.") for v in versions))

    def test_not_elf_is_none(self):
        text = self.tmp / "t.txt"
        text.write_text("hello")
        self.assertIsNone(package.elf_requirements(text))

    def test_portability_flags_unshipped_library(self):
        lib = compile_elf(self.tmp, "libfoo.so", source="int foo(void){return 1;}\n",
                          extra=("-shared", "-fPIC", "-Wl,-soname,libfoo.so"))
        folder = self.tmp / "bin"
        folder.mkdir()
        compile_elf(folder, "GHR", source="int foo(void);int main(void){return foo();}\n",
                    extra=(str(lib),))
        problems = package.check_portable(folder)
        self.assertTrue(any("needs libfoo.so," in p for p in problems), problems)
        shutil.copy2(lib, folder / "libfoo.so")  # shipped beside it: fine
        self.assertFalse(any("needs libfoo.so," in p for p in package.check_portable(folder)))

    def test_glibc_floor(self):
        self.assertGreater(package.glibc_version("GLIBC_2.34"), package.GLIBC_FLOOR)
        self.assertLessEqual(package.glibc_version("GLIBC_2.31"), package.GLIBC_FLOOR)
        self.assertLess(package.glibc_version("GLIBC_2.2.5"), package.GLIBC_FLOOR)


@unittest.skipUnless(CC, "needs a C compiler")
class Program(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_only_program_files_are_shipped(self):
        bin_dir = self.tmp / "bin"
        fake_bin(bin_dir)
        # A library chain as CMake leaves it: libSDL3.so -> libSDL3.so.0 -> libSDL3.so.0.4.10 (soname libSDL3.so.0),
        # GHR linked against it, plus an unrelated library nothing loads.
        compile_elf(bin_dir, "libSDL3.so.0.4.10", source="int sdl(void){return 3;}\n",
                    extra=("-shared", "-fPIC", "-Wl,-soname,libSDL3.so.0"))
        os.symlink("libSDL3.so.0.4.10", bin_dir / "libSDL3.so.0")
        os.symlink("libSDL3.so.0", bin_dir / "libSDL3.so")
        compile_elf(bin_dir, "libunused.so", source="int u(void){return 0;}\n", extra=("-shared", "-fPIC"))
        compile_elf(bin_dir, "GHR", source="int sdl(void);int main(void){return sdl();}\n",
                    extra=(f"-L{bin_dir}", "-lSDL3"))
        names = [p.name for p in package.program_files(bin_dir)]
        # Only the name GHR loads ships, as a regular file with the library's bytes.
        self.assertEqual(names, ["GHR", "resources", "libSDL3.so.0"])
        dest = self.tmp / "out"
        package.stage_program(bin_dir, dest)
        self.assertFalse((dest / "libSDL3.so.0").is_symlink())
        self.assertEqual((dest / "libSDL3.so.0").read_bytes(), (bin_dir / "libSDL3.so.0.4.10").read_bytes())
        for name in ("libSDL3.so.0.4.10", "libSDL3.so", "libunused.so", "GHR.ini", "logs"):
            self.assertFalse((dest / name).exists(), name)

    def test_dlopened_libraries_ship(self):
        bin_dir = fake_bin(self.tmp / "bin")
        compile_elf(bin_dir, package.DLOPENED[0], source="int gs(void){return 0;}\n", extra=("-shared", "-fPIC"))
        self.assertIn(package.DLOPENED[0], [p.name for p in package.program_files(bin_dir)])

    def test_read_only_build_folder_is_staged_writable(self):
        bin_dir = fake_bin(self.tmp / "ro")
        for path in [bin_dir, *bin_dir.rglob("*")]:
            path.chmod(path.stat().st_mode & ~0o222)  # as a Nix store path is
        try:
            dest = self.tmp / "out"
            package.stage_program(bin_dir, dest)
            self.assertTrue(os.access(dest / "GHR", os.W_OK))
            self.assertTrue(os.access(dest / "resources" / "fonts" / "a.ttf", os.W_OK))
        finally:
            for path in [bin_dir, *bin_dir.rglob("*")]:
                path.chmod(path.stat().st_mode | 0o200)

    def test_missing_or_windows_exe_is_refused(self):
        with self.assertRaises(package.PackageError):
            package.program_files(self.tmp)
        bin_dir = fake_bin(self.tmp / "win", exe=b"MZ\x90\x00not an elf")
        with self.assertRaises(package.PackageError):
            package.program_files(bin_dir)

    def test_tar_is_reproducible_and_laid_out(self):
        bin_dir = fake_bin(self.tmp / "bin")
        outs = []
        for name in ("a", "b"):
            out_dir = self.tmp / name
            out_dir.mkdir()
            outs.append(package.build_tar(bin_dir, out_dir, "GHR-v1", "1", 1700000000, False, None))
        self.assertEqual(hashlib.sha256(outs[0].read_bytes()).digest(), hashlib.sha256(outs[1].read_bytes()).digest())
        self.assertEqual(outs[0].name, "GHR-v1-Linux-x64.tar.gz")
        with tarfile.open(outs[0]) as tar:
            members = {m.name: m for m in tar.getmembers()}
        top = package.FOLDER_NAME
        for name in ("GHR", "README.txt", "LICENSE", "licenses", "resources/fonts/a.ttf",
                     "godhandrearmed.png", "install-desktop-entry.sh"):
            self.assertIn(f"{top}/{name}", members)
        self.assertEqual(members[f"{top}/GHR"].mode, 0o755)
        self.assertEqual(members[f"{top}/install-desktop-entry.sh"].mode, 0o755)
        self.assertEqual(members[f"{top}/README.txt"].mode, 0o644)
        self.assertTrue(all(m.uid == 0 and m.mtime == 1700000000 for m in members.values()))
        self.assertNotIn(f"{top}/logs", members)
        # No settings files: GHR writes them at the first start; an update must never replace the player's.
        self.assertNotIn(f"{top}/GHR.ini", members)
        self.assertNotIn(f"{top}/controls.ini", members)
        readme = (ROOT / "tools" / "linux" / "README-linux.txt").read_text().replace("{version}", "1")
        with tarfile.open(outs[0]) as tar:
            self.assertEqual(tar.extractfile(f"{top}/README.txt").read().decode(), readme)

    def test_stub_build_is_refused_without_the_flag(self):
        bin_dir = fake_bin(self.tmp / "bin")
        (bin_dir / "STUB-GAME-CODE.txt").write_text("stub")
        code = package.main(["--bin", str(bin_dir), "--out", str(self.tmp / "dist"), "--formats", "tar",
                             "--keep-debug", "--allow-nonportable"])
        self.assertEqual(code, 1)
        self.assertFalse((self.tmp / "dist").exists() and any((self.tmp / "dist").iterdir()))
        code = package.main(["--bin", str(bin_dir), "--out", str(self.tmp / "dist"), "--formats", "tar",
                             "--keep-debug", "--allow-nonportable", "--allow-stub"])
        self.assertEqual(code, 0)
        made = list((self.tmp / "dist").iterdir())
        self.assertEqual(len(made), 1)
        self.assertIn("-stub-Linux-x64.tar.gz", made[0].name)

    def test_nonportable_build_is_refused(self):
        # A binary compiled here needs this machine's glibc (2.34+ for any current one) or more: not portable.
        bin_dir = fake_bin(self.tmp / "bin")
        needed, versions = package.elf_requirements(bin_dir / "GHR")
        if max(map(package.glibc_version, versions)) <= package.GLIBC_FLOOR:
            self.skipTest("this machine's glibc is old enough to be portable")
        code = package.main(["--bin", str(bin_dir), "--out", str(self.tmp / "dist"), "--formats", "tar", "--keep-debug"])
        self.assertEqual(code, 1)

    @unittest.skipUnless(shutil.which("objcopy"), "needs objcopy")
    def test_strip_keeps_debug_beside(self):
        bin_dir = fake_bin(self.tmp / "bin")
        compile_elf(bin_dir, "GHR", extra=("-g",))
        before = (bin_dir / "GHR").stat().st_size
        out_dir = self.tmp / "dist"
        out_dir.mkdir()
        package.build_tar(bin_dir, out_dir, "GHR-v1", "1", 0, False, shutil.which("objcopy"))
        self.assertTrue((out_dir / "GHR-v1.debug").exists())
        with tarfile.open(out_dir / "GHR-v1-Linux-x64.tar.gz") as tar:
            shipped = tar.extractfile(f"{package.FOLDER_NAME}/GHR").read()
        self.assertLess(len(shipped), before)
        self.assertIn(b".gnu_debuglink", shipped)

    @unittest.skipUnless(shutil.which("mksquashfs") and shutil.which("unsquashfs"), "needs squashfs-tools")
    def test_appimage_layout(self):
        bin_dir = fake_bin(self.tmp / "bin")
        runtime = b"\x7fELF" + b"\0" * 60  # a stand-in: only the layout is checked here, not that it runs
        out_dir = self.tmp / "dist"
        out_dir.mkdir()
        out = package.build_appimage(bin_dir, out_dir, "GHR-v1", "1", 0, False, runtime, shutil.which("mksquashfs"), None)
        self.assertEqual(out.name, "GHR-v1-Linux-x64.AppImage")
        self.assertTrue(os.access(out, os.X_OK))
        data = out.read_bytes()
        self.assertTrue(data.startswith(runtime))
        squash = self.tmp / "fs.squashfs"
        squash.write_bytes(data[len(runtime):])
        listing = subprocess.run(["unsquashfs", "-lc", "-d", "", str(squash)], capture_output=True, text=True,
                                 check=True).stdout.split()
        for name in ("/AppRun", "/godhandrearmed.desktop", "/godhandrearmed.png", "/usr/lib/ghr/GHR",
                     "/usr/lib/ghr/resources/fonts/a.ttf", "/usr/share/doc/ghr/LICENSE",
                     "/usr/share/icons/hicolor/256x256/apps/godhandrearmed.png"):
            self.assertIn(name, listing)
        self.assertNotIn("/usr/lib/ghr/GHR.ini", listing)


class AppRun(unittest.TestCase):
    """The AppRun script's play-folder rules (GHR reads KAMITE_HOME, src/kamite/paths.cpp)."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.appdir = self.tmp / "AppDir"
        program = self.appdir / "usr" / "lib" / "ghr"
        program.mkdir(parents=True)
        (self.appdir / "AppRun").write_text(package.APPRUN)
        (self.appdir / "AppRun").chmod(0o755)
        # GHR stand-in: prints where it was started and with which play folder and arguments.
        (program / "GHR").write_text('#!/bin/sh\necho "home=$KAMITE_HOME"\necho "cwd=$(pwd)"\necho "args=$*"\n')
        (program / "GHR").chmod(0o755)

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def run_apprun(self, **env):
        base = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "APPDIR": str(self.appdir), "HOME": str(self.tmp / "home")}
        base.update(env)
        out = subprocess.run([str(self.appdir / "AppRun"), "--window", "two words"], env=base, capture_output=True,
                             text=True, check=True).stdout
        return dict(line.split("=", 1) for line in out.splitlines())

    def test_default_is_xdg_data_home(self):
        got = self.run_apprun(APPIMAGE=str(self.tmp / "Downloads" / "GHR.AppImage"))
        expect = self.tmp / "home" / ".local" / "share" / "GodHandRearmed"
        self.assertEqual(got["home"], str(expect))
        self.assertEqual(os.path.realpath(got["cwd"]), os.path.realpath(expect))
        self.assertTrue(expect.is_dir())
        self.assertEqual(got["args"], "--window two words")

    def test_xdg_data_home_is_honoured(self):
        got = self.run_apprun(XDG_DATA_HOME=str(self.tmp / "xdg"))
        self.assertEqual(got["home"], str(self.tmp / "xdg" / "GodHandRearmed"))

    def test_portable_folder_next_to_the_appimage(self):
        games = self.tmp / "Games"
        (games / "GHR-data").mkdir(parents=True)
        got = self.run_apprun(APPIMAGE=str(games / "GHR.AppImage"))
        self.assertEqual(got["home"], str(games / "GHR-data"))

    def test_kamite_home_wins(self):
        got = self.run_apprun(KAMITE_HOME=str(self.tmp / "mine"))
        self.assertEqual(got["home"], str(self.tmp / "mine"))
        self.assertTrue((self.tmp / "mine").is_dir())


class DesktopEntry(unittest.TestCase):
    def test_install_script_points_at_the_folder(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp) / "God Hand Rearmed"
            folder.mkdir()
            script = folder / "install-desktop-entry.sh"
            script.write_text(package.INSTALL_DESKTOP_ENTRY)
            script.chmod(0o755)
            (folder / "godhandrearmed.png").write_bytes(b"png")
            env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": tmp, "XDG_DATA_HOME": str(Path(tmp) / "data")}
            subprocess.run([str(script)], env=env, check=True, capture_output=True)
            entry = (Path(tmp) / "data" / "applications" / "godhandrearmed.desktop").read_text()
            self.assertIn(f'Exec="{folder}/GHR"\n', entry)
            self.assertIn(f"Path={folder}\n", entry)
            self.assertTrue((Path(tmp) / "data" / "icons" / "hicolor" / "256x256" / "apps" / "godhandrearmed.png").exists())


class StubGameCode(unittest.TestCase):
    def test_writes_the_stand_in_and_never_touches_real_code(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp) / "gen"
            stub_game_code.write(folder)
            self.assertEqual(sorted(p.name for p in folder.iterdir()), sorted(stub_game_code.FILES))
            stub_game_code.write(folder)  # again: fine, it is a stand-in
            real = Path(tmp) / "real"
            real.mkdir()
            (real / "register_functions.cpp").write_text("// the game")
            with self.assertRaises(SystemExit):
                stub_game_code.write(real)
            self.assertEqual((real / "register_functions.cpp").read_text(), "// the game")


if __name__ == "__main__":
    unittest.main()
