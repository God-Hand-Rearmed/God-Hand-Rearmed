"""Gate tests of the Windows zip (tools/package.py; tools/linux/test_package.py tests its Linux forms).

    python -I -m unittest discover -s tools/win -p "test_*.py" -v

Deterministic, no network, no Windows and no compiler: the programs are small PE files made here, with the import
tables a linker writes (load-time imports and delay-loaded ones).
"""
import hashlib
import os
import zlib
import shutil
import struct
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))  # tools/package.py

import package  # noqa: E402

ROOT = package.ROOT


def make_pe(imports=(), delayed=(), machine=0x8664, magic=0x20B, codeview=None) -> bytes:
    """A PE file (one .idata section at RVA 0x1000, file offset 0x200) importing `imports` and delay-loading
    `delayed`: IMAGE_IMPORT_DESCRIPTOR (20 bytes, Name at 12) and ImgDelayDescr (32 bytes, Name at 4) arrays, each
    ended by an all-zero entry, as a linker writes them. `codeview` (16 bytes): a debug directory naming the PDB with
    that GUID (IMAGE_DEBUG_DIRECTORY, type 2, pointing at an "RSDS" record)."""
    va, raw = 0x1000, 0x200
    imports_at = 0
    delayed_at = imports_at + 20 * (len(imports) + 1)
    names_at = delayed_at + 32 * (len(delayed) + 1)
    section = bytearray(names_at)
    names = bytearray()
    for i, name in enumerate(imports):
        struct.pack_into("<I", section, imports_at + 20 * i + 12, va + names_at + len(names))
        names += name.encode() + b"\0"
    for i, name in enumerate(delayed):
        struct.pack_into("<II", section, delayed_at + 32 * i, 1, va + names_at + len(names))
        names += name.encode() + b"\0"
    section += names
    debug_at = len(section)
    if codeview is not None:
        record = b"RSDS" + codeview + struct.pack("<I", 1) + b"GHR.pdb\0"
        entry = struct.pack("<IIHHIIII", 0, 0, 0, 0, 2, len(record), va + debug_at + 28, raw + debug_at + 28)
        section += entry + record
    section += bytes(-len(section) % 0x200)

    directories = [(0, 0)] * 16
    if imports:
        directories[1] = (va + imports_at, 20 * (len(imports) + 1))
    if delayed:
        directories[13] = (va + delayed_at, 32 * (len(delayed) + 1))
    if codeview is not None:
        directories[6] = (va + debug_at, 28)
    optional = bytearray(240)
    struct.pack_into("<H", optional, 0, magic)
    struct.pack_into("<I", optional, 108, 16)
    for i, (rva, size) in enumerate(directories):
        struct.pack_into("<II", optional, 112 + 8 * i, rva, size)
    coff = struct.pack("<HHIIIHH", machine, 1, 0, 0, 0, len(optional), 0x22)
    header = struct.pack("<8sIIIIIIHHI", b".idata", len(section), va, len(section), raw, 0, 0, 0, 0, 0xC0000040)
    dos = bytearray(0x40)
    dos[:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, 0x40)
    head = bytes(dos) + b"PE\0\0" + coff + bytes(optional) + header
    return head + bytes(raw - len(head)) + bytes(section)


def make_pdb(guid: bytes, block: int = 512) -> bytes:
    """An MSF 7.0 container (what MSVC's and LLVM's linkers write) whose stream 1, the PDB info stream, holds `guid`:
    block 0 the header, 1-2 the free maps, 3 the block map, 4 the stream directory (2 streams: 0 empty, 1 in block 5)."""
    blocks = [bytes(block)] * 6
    blocks[0] = (package.MSF_MAGIC + struct.pack("<IIIIII", block, 1, 6, 4 * 4, 0, 3)).ljust(block, b"\0")
    blocks[3] = struct.pack("<I", 4).ljust(block, b"\0")
    blocks[4] = struct.pack("<IIII", 2, 0, 28, 5).ljust(block, b"\0")
    blocks[5] = (struct.pack("<III", 20000404, 0, 1) + guid).ljust(block, b"\0")
    return b"".join(blocks)


GUID = bytes(range(16))

RELEASE_IMPORTS = ("KERNEL32.dll", "USER32.dll", "SDL3.dll", "MSVCP140.dll", "VCRUNTIME140.dll",
                   "api-ms-win-crt-runtime-l1-1-0.dll")


def fake_bin(folder: Path, imports=RELEASE_IMPORTS) -> Path:
    """A Windows build's bin folder as the build leaves it: what ships, and what a build or a run leaves beside it
    that must never ship (symbols, incremental-link files, settings, logs, saves, caches)."""
    folder.mkdir(parents=True)
    (folder / "GHR.exe").write_bytes(make_pe(imports, delayed=("dbghelp.dll",), codeview=GUID))
    (folder / "SDL3.dll").write_bytes(make_pe(("KERNEL32.dll",)))
    (folder / "pcsx2gs.dll").write_bytes(make_pe(("KERNEL32.dll", "d3d11.dll"), delayed=("dxcompiler.dll",)))
    (folder / "shaderc_shared.dll").write_bytes(make_pe(("KERNEL32.dll",)))
    (folder / "recomplauncher.json").write_text("{}\n")
    (folder / "resources" / "fonts").mkdir(parents=True)
    (folder / "resources" / "fonts" / "a.ttf").write_bytes(b"font")
    (folder / "resources" / "characters" / "xavier").mkdir(parents=True)
    (folder / "resources" / "characters" / "xavier" / "character.ini").write_text("[Character]\n")  # a resource: ships
    (folder / "unused.dll").write_bytes(make_pe(("KERNEL32.dll",)))  # nothing loads it: not shipped
    (folder / "GHR.pdb").write_bytes(make_pdb(GUID))  # GHR.exe's own: copied beside the zip, never into it
    for leftover in ("pcsx2gs.pdb", "SDL3.ilk", "GHR.ini", "controls.ini", "kamite.exe", "ps2_recomp.exe"):
        (folder / leftover).write_bytes(b"x")
    for folder_name in ("logs", "memcards", "states", "cases", "mods", "cache", "shadercache_pcsx2"):
        (folder / folder_name).mkdir()
        (folder / folder_name / "left.bin").write_bytes(b"x")
    return folder


def updater_reads(path: Path) -> list[str]:
    """The zip as the launcher's updater reads it (launcher_update.cpp readZip): the end record's count, every entry
    stored or deflated and not encrypted, the one shared top folder stripped. Raises where the updater refuses."""
    data = path.read_bytes()
    end = data.rindex(b"PK\x05\x06")
    count, = struct.unpack_from("<H", data, end + 10)
    at, = struct.unpack_from("<I", data, end + 16)
    names = []
    for _ in range(count):
        assert data[at:at + 4] == b"PK\x01\x02", "not a central directory entry"
        flags, method = struct.unpack_from("<HH", data, at + 8)
        assert not flags & 1 and method in (0, 8), f"flags {flags}, method {method}"
        name_length, extra, comment = struct.unpack_from("<HHH", data, at + 28)
        names.append(data[at + 46:at + 46 + name_length].decode().replace("\\", "/"))
        at += 46 + name_length + extra + comment
    tops = {name.split("/", 1)[0] for name in names}
    assert len(tops) == 1 and all("/" in name for name in names), f"not one top folder: {sorted(tops)}"
    return [name.split("/", 1)[1] for name in names]


class Pe(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_reads_imports_and_delay_imports(self):
        path = self.tmp / "a.exe"
        path.write_bytes(make_pe(("KERNEL32.dll", "SDL3.dll"), delayed=("dbghelp.dll",)))
        self.assertEqual(package.pe_imports(path), {"kernel32.dll", "sdl3.dll", "dbghelp.dll"})

    def test_no_imports(self):
        path = self.tmp / "a.dll"
        path.write_bytes(make_pe())
        self.assertEqual(package.pe_imports(path), set())

    def test_not_pe_is_none(self):
        path = self.tmp / "GHR"
        path.write_bytes(b"\x7fELF\x02\x01\x01" + bytes(64))
        self.assertIsNone(package.pe_imports(path))
        path.write_bytes(b"MZ" + bytes(0x3E))  # e_lfanew 0: no PE signature there
        self.assertIsNone(package.pe_imports(path))

    def test_32_bit_is_refused(self):
        path = self.tmp / "a.exe"
        path.write_bytes(make_pe(("KERNEL32.dll",), machine=0x14C, magic=0x10B))
        with self.assertRaises(package.PackageError):
            package.pe_imports(path)

    def test_debug_runtime_is_a_problem(self):
        bin_dir = fake_bin(self.tmp / "bin", imports=("KERNEL32.dll", "ucrtbased.dll", "MSVCP140D.dll"))
        problems, _ = package.check_windows(package.program_files_windows(bin_dir))
        self.assertEqual(len(problems), 2)
        self.assertTrue(all("debug C runtime" in p for p in problems))

    def test_outside_lists_windows_and_redist_not_shipped(self):
        bin_dir = fake_bin(self.tmp / "bin")
        problems, outside = package.check_windows(package.program_files_windows(bin_dir))
        self.assertEqual(problems, [])
        self.assertNotIn("sdl3.dll", outside)  # shipped beside GHR.exe
        for name in ("kernel32.dll", "msvcp140.dll", "vcruntime140.dll", "dbghelp.dll", "dxcompiler.dll"):
            self.assertIn(name, outside)
        self.assertEqual([n for n in outside if package.VC_REDIST.match(n)], ["msvcp140.dll", "vcruntime140.dll"])


class Damaged(unittest.TestCase):
    """A broken file is refused (PackageError), never a traceback and never shipped as a DLL importing nothing."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_every_truncation_is_refused_or_not_pe(self):
        whole = make_pe(("KERNEL32.dll", "SDL3.dll"), delayed=("dbghelp.dll",))
        path = self.tmp / "a.dll"
        for length in range(len(whole)):
            path.write_bytes(whole[:length])
            try:
                found = package.pe_imports(path)
            except package.PackageError:
                continue
            self.assertIsNone(found, f"cut at {length} bytes read as a DLL importing {found}")

    def test_name_without_terminator(self):
        whole = bytearray(make_pe(("KERNEL32.dll",)))
        end = whole.rindex(b"KERNEL32.dll") + len("KERNEL32.dll")
        path = self.tmp / "a.dll"
        path.write_bytes(bytes(whole[:end]))  # the name runs to the end of the file
        with self.assertRaises(package.PackageError):
            package.pe_imports(path)

    def test_import_outside_the_sections(self):
        whole = bytearray(make_pe(("KERNEL32.dll",)))
        optional = 0x40 + 4 + 20
        struct.pack_into("<I", whole, optional + 112 + 8, 0x7000)  # the import directory's RVA: in no section
        path = self.tmp / "a.dll"
        path.write_bytes(bytes(whole))
        with self.assertRaises(package.PackageError):
            package.pe_imports(path)

    def test_rva_in_a_sections_zero_tail_is_refused(self):
        whole = bytearray(make_pe(("KERNEL32.dll",)))
        header = 0x40 + 4 + 20 + 240
        vsize, va, raw_size, raw = struct.unpack_from("<IIII", whole, header + 8)
        struct.pack_into("<I", whole, header + 8, raw_size + 0x1000)  # virtual size past the file's data
        struct.pack_into("<I", whole, 0x40 + 4 + 20 + 112 + 8, va + raw_size)  # imports in that tail
        # Past the section's data the file holds a valid-looking import table (overlay bytes, which the loader never
        # maps: the tail is zeros when loaded). Read as the imports, it would name KERNEL32.dll.
        overlay = bytes(whole[raw:raw + 40]).ljust(0x200, b"\0")
        path = self.tmp / "a.dll"
        path.write_bytes(bytes(whole) + overlay)
        with self.assertRaises(package.PackageError):
            package.pe_imports(path)

    def test_old_style_delay_descriptor_is_refused(self):
        whole = bytearray(make_pe(delayed=("dbghelp.dll",)))
        delayed_rva, _ = struct.unpack_from("<II", whole, 0x40 + 4 + 20 + 112 + 8 * 13)
        struct.pack_into("<I", whole, 0x200 + delayed_rva - 0x1000, 0)  # Attributes 0: the name is a VA
        path = self.tmp / "a.dll"
        path.write_bytes(bytes(whole))
        with self.assertRaises(package.PackageError):
            package.pe_imports(path)

    def test_shipped_dll_that_is_not_pe_is_refused(self):
        bin_dir = fake_bin(self.tmp / "bin")
        (bin_dir / "SDL3.dll").write_bytes(b"MZ" + bytes(0x3A) + struct.pack("<I", 0x10000))  # e_lfanew past the end
        problems, _ = package.check_windows(package.program_files_windows(bin_dir))
        self.assertEqual(problems, ["SDL3.dll is not a Windows program or DLL"])

    def test_truncated_dll_refuses_the_zip(self):
        bin_dir = fake_bin(self.tmp / "bin")
        data = (bin_dir / "SDL3.dll").read_bytes()
        (bin_dir / "SDL3.dll").write_bytes(data[:len(data) // 2])
        dist = self.tmp / "dist"
        self.assertEqual(package.main(["--bin", str(bin_dir), "--out", str(dist), "--version", "1"]), 1)
        self.assertFalse(dist.exists())


class Zip(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_only_program_files_are_shipped(self):
        bin_dir = fake_bin(self.tmp / "bin")
        names = sorted(p.name for p in package.program_files_windows(bin_dir))
        self.assertEqual(names, ["GHR.exe", "SDL3.dll", "pcsx2gs.dll", "recomplauncher.json", "resources",
                                 "shaderc_shared.dll"])

    def test_overlay_dll_is_refused(self):
        bin_dir = fake_bin(self.tmp / "bin")
        (bin_dir / "dxgi.dll").write_bytes(make_pe(("KERNEL32.dll",)))  # ReShade, say
        with self.assertRaises(package.PackageError):
            package.program_files_windows(bin_dir)

    def test_both_systems_in_one_folder_is_refused(self):
        bin_dir = fake_bin(self.tmp / "bin")
        (bin_dir / "GHR").write_bytes(b"\x7fELF")
        with self.assertRaises(package.PackageError):
            package.detect_system(bin_dir)

    def test_dlls_loaded_only_through_another_dll_ship(self):
        bin_dir = fake_bin(self.tmp / "bin")
        (bin_dir / "SDL3.dll").write_bytes(make_pe(("KERNEL32.dll", "helper.dll")))
        (bin_dir / "helper.dll").write_bytes(make_pe(("KERNEL32.dll",)))
        names = [p.name for p in package.program_files_windows(bin_dir)]
        self.assertIn("helper.dll", names)
        self.assertNotIn("unused.dll", names)

    def test_missing_pieces_are_refused(self):
        with self.assertRaises(package.PackageError):
            package.program_files_windows(self.tmp)
        bin_dir = fake_bin(self.tmp / "bin")
        (bin_dir / "recomplauncher.json").unlink()
        with self.assertRaises(package.PackageError):
            package.program_files_windows(bin_dir)
        (bin_dir / "GHR.exe").write_bytes(b"\x7fELF not windows")
        with self.assertRaises(package.PackageError):
            package.program_files_windows(bin_dir)

    def test_zip_is_reproducible_and_laid_out(self):
        bin_dir = fake_bin(self.tmp / "bin")
        files = package.program_files_windows(bin_dir)
        outs = []
        for name in ("a", "b"):
            out_dir = self.tmp / name
            out_dir.mkdir()
            outs.append(package.build_zip(bin_dir, out_dir, "GHR-v1", "1", 1700000000, False, files))
        zips = [made[0] for made in outs]
        self.assertEqual(hashlib.sha256(zips[0].read_bytes()).digest(), hashlib.sha256(zips[1].read_bytes()).digest())
        self.assertEqual(zips[0].name, "GHR-v1-Windows-x64.zip")
        self.assertEqual(outs[0][1].name, "GHR-v1.pdb")  # the symbols, beside the zip and not in it
        inside = updater_reads(zips[0])
        for name in ("GHR.exe", "SDL3.dll", "pcsx2gs.dll", "shaderc_shared.dll", "recomplauncher.json", "README.txt",
                     "LICENSE", "resources/fonts/a.ttf", "resources/characters/xavier/character.ini"):
            self.assertIn(name, inside)
        self.assertTrue(any(name.startswith("docs/licenses/") for name in inside))
        self.assertFalse(any(name.endswith("/") for name in inside))  # files only, as the maintainer's zips
        # Never the player's settings (an update unpacked over the folder must keep them), symbols, logs or saves.
        for name in inside:
            self.assertNotIn(name.split("/")[0], {"GHR.ini", "controls.ini", "GHR.pdb", "pcsx2gs.pdb", "SDL3.ilk",
                                                  "kamite.exe", "ps2_recomp.exe", "logs", "memcards", "states",
                                                  "cases", "mods", "cache", "shadercache_pcsx2"})
        with zipfile.ZipFile(zips[0]) as archive:
            infos = archive.infolist()
            self.assertTrue(all(i.date_time == (2023, 11, 14, 22, 13, 20) for i in infos))
            # MS-DOS attributes only: no Unix mode of the packing system leaks into the high 16 bits.
            self.assertTrue(all(i.create_system == 0 and i.external_attr == 0x20 for i in infos))
            # Deflated at level 9: the same bytes zlib makes at 9 (a raw deflate stream, as a zip stores it).
            for info in infos:
                self.assertEqual(info.compress_type, zipfile.ZIP_DEFLATED)
                packer = zlib.compressobj(9, zlib.DEFLATED, -15)
                self.assertEqual(info.compress_size, len(packer.compress(archive.read(info)) + packer.flush()), info)
            licenses = [i for i in infos if "/docs/licenses/" in i.filename]
            self.assertTrue(licenses)
            for info in licenses:
                self.assertNotIn(b"\n", archive.read(info).replace(b"\r\n", b""), info.filename)
            readme = archive.read(f"{package.FOLDER_NAME}/README.txt")
            license_text = archive.read(f"{package.FOLDER_NAME}/LICENSE")
        template = (ROOT / "tools" / "win" / "README-windows.txt").read_text().replace("{version}", "1")
        self.assertEqual(readme, template.replace("\n", "\r\n").encode())
        self.assertNotIn(b"\n", license_text.replace(b"\r\n", b""))  # Windows line ends only
        self.assertEqual(license_text.replace(b"\r\n", b"\n"), (ROOT / "LICENSE").read_bytes().replace(b"\r\n", b"\n"))

    def test_no_zip64_and_no_partial_zip(self):
        bin_dir = fake_bin(self.tmp / "bin")
        files = package.program_files_windows(bin_dir)
        limit = zipfile.ZIP64_LIMIT
        zipfile.ZIP64_LIMIT = 16  # every file is "big": a zip64 entry, which the updater can't read
        try:
            with self.assertRaises(package.PackageError):
                package.build_zip(bin_dir, self.tmp, "GHR-v1", "1", 1700000000, False, files)
        finally:
            zipfile.ZIP64_LIMIT = limit
        self.assertEqual([p for p in self.tmp.iterdir() if p.suffix in (".zip", ".partial")], [])

    def test_only_ghr_exes_own_pdb_is_copied(self):
        bin_dir = fake_bin(self.tmp / "bin")
        files = package.program_files_windows(bin_dir)
        self.assertEqual(package.pe_pdb_guid(bin_dir / "GHR.exe"), GUID)
        self.assertEqual(package.pdb_guid(bin_dir / "GHR.pdb"), GUID)
        for pdb in (make_pdb(bytes(16)), b"", b"not a pdb"):  # another build's, empty, not a PDB
            (bin_dir / "GHR.pdb").write_bytes(pdb)
            out = self.tmp / f"out{len(pdb)}"
            out.mkdir()
            made = package.build_zip(bin_dir, out, "GHR-v1", "1", 1700000000, False, files)
            self.assertEqual([p.name for p in made], ["GHR-v1-Windows-x64.zip"])

    def test_too_many_entries_for_the_updater(self):
        bin_dir = fake_bin(self.tmp / "bin")
        files = package.program_files_windows(bin_dir)
        limit = package.MAX_ZIP_ENTRIES
        package.MAX_ZIP_ENTRIES = 5
        try:
            with self.assertRaises(package.PackageError):
                package.build_zip(bin_dir, self.tmp, "GHR-v1", "1", 1700000000, False, files)
        finally:
            package.MAX_ZIP_ENTRIES = limit

    def test_entries_are_sorted(self):
        bin_dir = fake_bin(self.tmp / "bin")
        for name in ("zz.txt", "aa.txt", "Mm.txt"):
            (bin_dir / "resources" / name).write_text(name)
        zip_path = package.build_zip(bin_dir, self.tmp, "GHR-v1", "1", 1700000000, False,
                                     package.program_files_windows(bin_dir))[0]
        with zipfile.ZipFile(zip_path) as archive:
            names = [i.filename for i in archive.infolist()]
        self.assertEqual(names, sorted(names))

    def test_zip_time_floor_is_1980(self):
        self.assertEqual(package.zip_time(0), (1980, 1, 1, 0, 0, 0))

    def test_main_stub_formats_and_system(self):
        bin_dir = fake_bin(self.tmp / "bin")
        dist = self.tmp / "dist"
        self.assertEqual(package.detect_system(bin_dir), "windows")
        self.assertEqual(package.main(["--bin", str(bin_dir), "--out", str(dist), "--formats", "tar"]), 1)
        (bin_dir / "STUB-GAME-CODE.txt").write_text("stub")
        self.assertEqual(package.main(["--bin", str(bin_dir), "--out", str(dist)]), 1)
        self.assertFalse(dist.exists())
        self.assertEqual(package.main(["--bin", str(bin_dir), "--out", str(dist), "--allow-stub", "--version", "2"]), 0)
        self.assertEqual(sorted(p.name for p in dist.iterdir()), ["GHR-v2-stub-Windows-x64.zip", "GHR-v2-stub.pdb"])
        self.assertIn("STUB-GAME-CODE.txt", updater_reads(dist / "GHR-v2-stub-Windows-x64.zip"))

    def test_debug_build_is_refused(self):
        bin_dir = fake_bin(self.tmp / "bin", imports=("KERNEL32.dll", "VCRUNTIME140D.dll"))
        dist = self.tmp / "dist"
        self.assertEqual(package.main(["--bin", str(bin_dir), "--out", str(dist), "--version", "1"]), 1)
        self.assertFalse(dist.exists())


if __name__ == "__main__":
    unittest.main()
