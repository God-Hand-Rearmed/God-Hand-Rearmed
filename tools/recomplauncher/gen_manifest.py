#!/usr/bin/env python3
"""Writes recomplauncher.json (top level) for RecompLauncher (https://recomplauncher.com/sdk), so its setup, Play and
settings page follow the game: the settings rows come from the same table the launcher's Options and the in-game
OPTIONS are drawn from (names, choices and help texts as they are there), the Preset row from presets.cpp.

Sources: src/kamite/settings_table.cpp, presets.cpp and ini_file.cpp, built into a small dump tool
(tools/recomplauncher/dump_settings.cpp) with the C++ compiler found: c++ / g++ / clang++ / cl.

    python -I tools/recomplauncher/gen_manifest.py           rewrite recomplauncher.json
    python -I tools/recomplauncher/gen_manifest.py --check   exit 1 if recomplauncher.json is out of date

The build copies recomplauncher.json next to GHR.exe (src/CMakeLists.txt), so a release zip carries it at the top.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "recomplauncher.json"
TEMPLATE = ROOT / "GHR.ini"
SOURCES = ["tools/recomplauncher/dump_settings.cpp", "src/kamite/settings_table.cpp", "src/kamite/presets.cpp",
           "src/kamite/ini_file.cpp"]
TOGGLE, CHOICE, SLIDER, CUSTOM = 0, 1, 2, 3
# Picked from the player's own files at run time (characters, texture packs): no fixed choices to offer.
SKIPPED = {"Character", "Costume", "Textures"}


def dump_settings():
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / ("dump.exe" if os.name == "nt" else "dump")
        sources = [str(ROOT / s) for s in SOURCES]
        for compiler in ("c++", "g++", "clang++"):
            if shutil.which(compiler):
                cmd = [compiler, "-std=c++20", "-O0", "-I", str(ROOT / "src"), *sources, "-o", str(exe)]
                break
        else:
            if not shutil.which("cl"):
                sys.exit("gen_manifest: no C++ compiler (c++, g++, clang++ or cl) on PATH")
            cmd = ["cl", "/nologo", "/std:c++20", "/EHsc", "/utf-8", f"/I{ROOT / 'src'}", *sources, f"/Fe{exe}",
                   f"/Fo{tmp}\\"]
        subprocess.run(cmd, check=True, cwd=tmp)
        out = subprocess.run([str(exe)], check=True, capture_output=True).stdout
    return json.loads(out.decode("utf-8"))


def ini_values(path):
    """(section, key) lower case -> value, from GHR.ini (the shipped defaults)."""
    values, section = {}, ""
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line[0] in "#;":
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip().lower()
            continue
        if "=" in line:
            key, value = line.split("=", 1)
            values[(section, key.strip().lower())] = value.strip()
    return values


def key_name(section, key):
    return f"{section}/{key}"


def keys_dict(keys):
    return {key_name(s, k): v for s, k, v in keys}


def help_text(text):
    return " ".join(part.strip() for part in text.split("\n") if part.strip())


def when_of(setting, by_id):
    """RecompLauncher's "when" for a sub-option: the parent's keys that let it show (settings::parentAllows)."""
    parent = by_id.get(setting["parent"])
    if not parent:
        return None
    if parent["id"] == "DisplayMode":  # two keys: a window only
        return {"Video/StartFullscreen": "0", "Video/StartMaximized": "0"}
    if setting["parentValue"]:
        return {key_name(parent["section"], parent["key"]): setting["parentValue"]}
    return {key_name(parent["section"], parent["key"]): "1"}


def setting_row(s, by_id):
    row = {"id": s["id"], "label": s["name"], "help": help_text(s["description"]), "group": s["category"]}
    if s["kind"] == TOGGLE:
        k = key_name(s["section"], s["key"])
        row["choices"] = [{"label": "Off", "set": {k: "0"}}, {"label": "On", "set": {k: "1"}}]
    elif s["kind"] == SLIDER:
        # The table's stops are in 5s (the volumes' extra 69 aside: a value off the step is shown as it is).
        values = s["values"]
        steps = {b - a for a, b in zip(values, values[1:])}
        row["slider"] = {"key": key_name(s["section"], s["key"]), "min": min(values), "max": max(values),
                         "step": 5 if 5 in steps else min(steps), "format": "%d"}
    else:  # a choice (Display Mode and Camera & FOV write more than one key)
        row["choices"] = [{"label": c["label"], "set": keys_dict(c["keys"])} for c in s["choices"]]
    row["defaults"] = keys_dict(s["fallbackKeys"])
    when = when_of(s, by_id)
    if when:
        row["when"] = when
    return row


def manifest(data):
    by_id = {s["id"]: s for s in data["settings"]}
    presets = data["presets"]
    custom = data["custom"]
    preset_row = {
        "id": "Preset",
        "label": "Preset",
        "group": "Presets",
        "help": " | ".join(f"{p['name']}: {p['tagline']}" for p in presets),
        # The first choice whose keys all match is shown: a preset when the settings are exactly it (presets::current),
        # Custom (no keys) otherwise. Picking Custom changes nothing.
        "choices": [{"label": p["name"], "set": keys_dict(p["keys"])} for p in presets]
        + [{"label": custom["name"], "set": {}}],
    }
    start_row = {
        "id": "Start",
        "label": "Start",
        "group": "Start",
        "in": "args",
        "help": "Straight into the game, or through God Hand: Rearmed's own launcher first (its Play starts the game).",
        "choices": [
            {"label": "Straight into the game", "args": ["--iso", "{game}"]},
            {"label": "God Hand: Rearmed's launcher", "args": []},
        ],
    }
    rows = [start_row, preset_row] + [setting_row(s, by_id) for s in data["settings"] if s["id"] not in SKIPPED]
    return {
        "$schema": "https://recomplauncher.com/schema/recomplauncher.json",
        "format": 1,
        "name": "God Hand: Rearmed",
        "repo": "God-Hand-Rearmed/God-Hand-Rearmed",
        "game": {
            "kind": "iso",
            "label": "Your disc image (.iso) of God Hand for PlayStation 2, the USA release (serial SLUS-21503).",
            "check": {"iso9660": "SLUS_215.03"},
            # Redump's verified dump (redump.org/disc/4176; its size, MD5 and SHA-1 are DISC_HASHES.md's).
            "dumps": [{"name": "USA", "size": 1567686656, "crc32": "07220ad2"}],
        },
        "start": {"program": ["GHR.exe"], "args": []},
        "prepare": {
            "program": ["GHR.exe"],
            "args": ["--prepare", "--iso", "{game}"],
            "label": "Checking your disc and writing the default settings",
            "timeout": 120,
        },
        "config": [{"id": "main", "format": "ini", "file": [{"path": "{install}/GHR.ini"}], "game_path": "Game/Iso"}],
        "saves": ["{install}/memcards", "{install}/GHR.ini", "{install}/controls.ini"],
        "folders": {
            "saves": [{"path": "{install}/memcards"}],
            "states": [{"path": "{install}/states"}],
            "logs": [{"path": "{install}/logs"}],
        },
        "actions": [
            {"id": "saves", "label": "Memory card", "help": "Your saved games.", "open": "saves"},
            {"id": "states", "label": "Save states", "help": "The save states you made in game.", "open": "states"},
            {"id": "logs", "label": "Logs", "help": "What to attach to a bug report.", "open": "logs"},
        ],
        "exit_codes": {
            "2": "No disc image was given. Pick your copy of God Hand again on the game's page.",
            "3": "That disc image isn't the USA release of God Hand (SLUS-21503), the one God Hand: Rearmed is built "
                 "from. Pick that one on the game's page.",
            "4": "Your processor is too old for God Hand: Rearmed (it needs SSE4.1).",
        },
        "looks": {
            "allowed": False,
            "reason": "God Hand: Rearmed has its own picture options (Anti-Aliasing, Texture Filtering, Post Effects).",
        },
        "settings": rows,
    }


def check_defaults(doc):
    """Every row shows a choice for the shipped GHR.ini (else the game page would show none)."""
    shipped = ini_values(TEMPLATE)
    problems = []
    for row in doc["settings"]:
        if "choices" not in row or row.get("in") == "args" or row["id"] == "Preset":
            continue

        def current(k):
            section, key = k.split("/", 1)
            return shipped.get((section.lower(), key.lower()), row["defaults"].get(k))

        if not any(all(current(k) == v for k, v in c["set"].items()) for c in row["choices"]):
            problems.append(row["id"])
    return problems


def render(doc):
    return json.dumps(doc, indent=2, ensure_ascii=False) + "\n"


def main():
    doc = manifest(dump_settings())
    problems = check_defaults(doc)
    if problems:
        sys.exit("gen_manifest: no choice matches GHR.ini for " + ", ".join(problems))
    text = render(doc)
    if "--check" in sys.argv[1:]:
        current = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if current != text:
            sys.exit("recomplauncher.json is out of date: run python -I tools/recomplauncher/gen_manifest.py")
        print("recomplauncher.json is up to date")
        return
    OUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {OUT.relative_to(ROOT)} ({len(doc['settings'])} settings)")


if __name__ == "__main__":
    main()
