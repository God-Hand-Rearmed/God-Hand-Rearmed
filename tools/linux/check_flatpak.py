"""Checks a .flatpak that tools/package.py made, without a Flatpak system: the bundle is imported into a fresh ostree
repository of its own (flatpak build-import-bundle), and what an install would put in /app is read from there.

    python -I tools/linux/check_flatpak.py dist/GHR-v<version>-Linux-x64.flatpak [--flatpak FILE] [--ostree FILE]

Exit code 0 when every rule holds, else 1 with what failed. The rules (also run by test_package.py and the Nix check
flatpak-bundle):
- one ref, app/<FLATPAK_ID>/x86_64/stable, and the bundle names Flathub as where its runtime comes from;
- its metadata is exactly package.flatpak_metadata(): the runtime and the permissions, nothing added on the way;
- /app holds bin/ghr (the entry point), lib/ghr/GHR and resources/, the docs, and exports the desktop entry and icons
  under the app id, the entry starting `ghr`;
- no settings, saves, logs or caches (GHR writes those in the play folder), and the stand-in marker only in a -stub.
"""
import argparse
import configparser
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # tools/package.py
import package  # noqa: E402

REF = f"app/{package.FLATPAK_ID}/x86_64/{package.FLATPAK_BRANCH}"
NEVER_SHIPPED = ("GHR.ini", "controls.ini", "logs", "memcards", "states", "cache", "mods", "textures", "game")


def read_bundle(bundle: Path, flatpak: str, ostree: str) -> tuple[list[str], dict[str, str], str, str]:
    """(the refs, {path in the commit: its mode as ostree prints it}, the metadata, the exported desktop entry)."""
    with tempfile.TemporaryDirectory() as tmp:
        repo = Path(tmp) / "repo"
        env = dict(os.environ, FLATPAK_USER_DIR=str(Path(tmp) / "user"), FLATPAK_SYSTEM_DIR=str(Path(tmp) / "system"))
        env.setdefault("HOME", tmp)
        subprocess.run([ostree, "init", f"--repo={repo}", "--mode=archive-z2"], check=True, capture_output=True,
                       env=env)
        subprocess.run([flatpak, "build-import-bundle", "--no-update-summary", str(repo), str(bundle)], check=True,
                       capture_output=True, env=env)

        def run(*args):
            return subprocess.run([ostree, f"--repo={repo}", *args], check=True, capture_output=True, text=True,
                                  env=env).stdout

        refs = run("refs").split()
        files = {}
        for line in run("ls", "-R", REF).splitlines() if REF in refs else ():
            parts = line.split(None, 4)  # mode uid gid size path [-> target]
            if len(parts) == 5:
                files[parts[4].split(" -> ")[0]] = parts[0]
        metadata = run("cat", REF, "/metadata") if REF in refs else ""
        entry = f"/export/share/applications/{package.FLATPAK_ID}.desktop"
        desktop = run("cat", REF, entry) if entry in files else ""
    return refs, files, metadata, desktop


def problems(bundle: Path, flatpak: str, ostree: str) -> list[str]:
    found = []
    refs, files, metadata, desktop = read_bundle(bundle, flatpak, ostree)
    if refs != [REF]:
        return [f"refs are {refs}, expected [{REF}]"]
    if metadata != package.flatpak_metadata():
        found.append("the metadata is not package.flatpak_metadata():\n" + metadata)
    parsed = configparser.ConfigParser(interpolation=None)
    parsed.optionxform = str
    parsed.read_string(metadata)
    context = parsed["Context"] if parsed.has_section("Context") else {}
    filesystems = [f for f in context.get("filesystems", "").split(";") if f]
    for wide in ("host", "home", "host-os", "host-etc", "~"):
        if any(f.split(":")[0] == wide for f in filesystems):
            found.append(f"the sandbox gets the whole {wide} filesystem")
    if [f for f in filesystems if not f.endswith(":ro")]:
        found.append(f"a filesystem permission is writable: {filesystems}")
    if package.FLATPAK_RUNTIME_REPO.encode() not in bundle.read_bytes():
        found.append(f"the bundle does not name {package.FLATPAK_RUNTIME_REPO} for its runtime")
    want = {"/files/bin/ghr": "-00755", "/files/lib/ghr/GHR": "-00755", "/files/lib/ghr/resources": "d00755",
            "/files/share/doc/ghr/README.txt": "-00644", "/files/share/doc/ghr/LICENSE": "-00644",
            f"/export/share/applications/{package.FLATPAK_ID}.desktop": "-00644",
            f"/export/share/icons/hicolor/256x256/apps/{package.FLATPAK_ID}.png": "-00644"}
    for path, mode in want.items():
        if files.get(path) != mode:
            found.append(f"{path}: {files.get(path, 'missing')}, expected {mode}")
    for name in NEVER_SHIPPED:
        for prefix in ("/files/lib/ghr/", "/files/"):
            if prefix + name in files:
                found.append(f"{prefix + name} is shipped (GHR makes it in the play folder)")
    stub = "-stub-" in bundle.name
    if ("/files/lib/ghr/STUB-GAME-CODE.txt" in files) != stub:
        found.append("the stand-in marker and the -stub name disagree")
    if "\nExec=ghr\n" not in desktop or f"\nIcon={package.FLATPAK_ID}\n" not in desktop:
        found.append("the exported desktop entry does not start ghr with the app's icon:\n" + desktop)
    validate = shutil.which("desktop-file-validate")
    if validate and desktop:
        with tempfile.TemporaryDirectory() as tmp:
            entry = Path(tmp) / f"{package.FLATPAK_ID}.desktop"
            entry.write_text(desktop, encoding="utf-8")
            result = subprocess.run([validate, str(entry)], capture_output=True, text=True)
            if result.returncode or result.stdout.strip():
                found.append("desktop-file-validate: " + (result.stdout + result.stderr).strip())
    return found


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--flatpak", default=shutil.which("flatpak") or "flatpak")
    parser.add_argument("--ostree", default=shutil.which("ostree") or "ostree")
    args = parser.parse_args(argv)
    found = problems(args.bundle, args.flatpak, args.ostree)
    for problem in found:
        print(f"check_flatpak: {problem}", file=sys.stderr)
    if not found:
        print(f"check_flatpak: {args.bundle.name}: {REF}, metadata and /app as packed")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
