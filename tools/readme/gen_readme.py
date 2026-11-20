#!/usr/bin/env python3
"""Fills README.md's generated parts from the code, so the page can't drift from the game:

  <!-- tag:<SettingId> -->              the first preset that turns a setting on (`Remastered+`, `Rearmed+`, `Sleazy
                                        Mode`), `Toggle` for one no preset sets
  <!-- BEGIN GENERATED: options -->     every option per tab, with the help text the launcher and the game show
  <!-- BEGIN GENERATED: controls -->    the default controller and keyboard & mouse bindings
  <!-- BEGIN GENERATED: presets -->     the presets' names, taglines and texts (K's, as in game)

Sources: src/kamite/settings_table.cpp and presets.cpp (built into a small dump tool with the C++ compiler found:
c++ / g++ / clang++ / cl), runtime/ps2xRuntime/src/lib/ps2_host_input.cpp (defaultPadSettings).

    python -I tools/readme/gen_readme.py           rewrite README.md
    python -I tools/readme/gen_readme.py --check   exit 1 if README.md is out of date
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
README = ROOT / "README.md"
INPUT = ROOT / "runtime" / "ps2xRuntime" / "src" / "lib" / "ps2_host_input.cpp"
SOURCES = ["tools/readme/dump_tables.cpp", "src/kamite/settings_table.cpp", "src/kamite/presets.cpp",
           "src/kamite/ini_file.cpp"]
# Not listed: no character in a release has costumes yet (Olivia's are in test builds only).
SKIPPED = {"Costume"}


def dump_tables():
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / ("dump.exe" if os.name == "nt" else "dump")
        sources = [str(ROOT / s) for s in SOURCES]
        for compiler in ("c++", "g++", "clang++"):
            if shutil.which(compiler):
                cmd = [compiler, "-std=c++20", "-O0", "-I", str(ROOT / "src"), *sources, "-o", str(exe)]
                break
        else:
            if not shutil.which("cl"):
                sys.exit("gen_readme: no C++ compiler (c++, g++, clang++ or cl) on PATH")
            cmd = ["cl", "/nologo", "/std:c++20", "/EHsc", "/utf-8", f"/I{ROOT / 'src'}", *sources, f"/Fe{exe}",
                   f"/Fo{tmp}\\"]
        subprocess.run(cmd, check=True, cwd=tmp)
        out = subprocess.run([str(exe)], check=True, capture_output=True).stdout
    return json.loads(out.decode("utf-8"))


def tags(data):
    """id -> the README's tag text, for the settings a preset sets."""
    names = [p["name"] for p in data["presets"]]
    by_id = {s["id"]: s for s in data["settings"]}

    def tag(s):
        if not s["controlled"]:
            return "Toggle"
        values = s["preset"]
        for p in range(1, len(values)):
            if values[p] != values[0]:
                return names[p] + ("" if p == len(values) - 1 else "+")
        # The same in every preset: a sub-option on in all goes with its parent; off in all is a toggle.
        parent = by_id.get(s["parent"])
        if parent and values[0] not in ("", "0"):
            # A sub-option of one value of its parent (Modern: Hold's Auto): the first preset with that value.
            if s.get("parentValue") and parent["controlled"]:
                for p, value in enumerate(parent["preset"]):
                    if value.lower() == s["parentValue"].lower():
                        return names[p] + ("" if p == len(names) - 1 else "+")
                return "Toggle"  # no preset picks that value (Modern: Toggle's Auto)
            return tag(parent)
        return "Toggle"

    return {s["id"]: tag(s) for s in data["settings"]}


def clean(text):
    return " ".join(line.strip() for line in text.split("\n") if line.strip())


def option_lines(data, tag_of):
    out = []
    tabs = []
    for s in data["settings"]:
        if s["tab"] not in tabs:
            tabs.append(s["tab"])
    for tab in tabs:
        rows = [s for s in data["settings"] if s["tab"] == tab and s["id"] not in SKIPPED]
        if not rows:
            continue
        out += ["<details>", f"<summary><b>{tab}</b></summary>"]
        category = None
        listed = set()
        for s in rows:
            if s["category"] != category:
                category = s["category"]
                out += ["", f"**{category}**", ""]
            indent = "  " if s["parent"] in listed else ""
            listed.add(s["id"])
            line = f"{indent}- **{s['name']}:** {clean(s['description'])}"
            if s["choices"]:
                labels = [" ".join(label.split()) for _, label in s["choices"]]
                line += " *" + " / ".join(labels) + "*"
            elif s["values"]:
                line += f" *{s['values'][0]}{s['unit']} to {s['values'][-1]}{s['unit']}*"
            if s["note"]:
                line += f" ({clean(s['note'])[:-1] if s['note'].endswith('.') else clean(s['note'])})"
            if s["controlled"] and tag_of[s["id"]] != "Toggle":
                line += f" `{tag_of[s['id']]}`"
            out.append(line)
        out += ["", "</details>"]
    return out


PAD_NAMES = {
    "FaceSouth": "A / ✕", "FaceEast": "B / ○", "FaceWest": "X / ▢", "FaceNorth": "Y / △",
    "LeftShoulder": "LB / L1", "RightShoulder": "RB / R1", "+LeftTrigger": "LT / L2", "+RightTrigger": "RT / R2",
    "LeftStick": "Left stick click", "RightStick": "Right stick click", "Start": "Start / Options", "Back": "Back / Select",
    "DPadUp": "D-pad up", "DPadRight": "D-pad right", "DPadDown": "D-pad down", "DPadLeft": "D-pad left",
}
KEY_NAMES = {"Keyboard/Escape": "Esc", "Pointer-0/LeftButton": "Left click", "Pointer-0/RightButton": "Right click",
             "Pointer-0/MiddleButton": "Middle click"}
# The rows (the in-game BUTTON MAPPING page's actions), the pad keys they come from, and when they apply.
ROWS = [
    ("Move", ["LUp", "LLeft", "LDown", "LRight"], ""),
    ("Dodge", ["DODGE"], ""),
    ("Combo", ["Square"], ""),
    ("Assigned move", ["Triangle"], ""),
    ("Assigned move", ["Cross"], ""),
    ("Action (talk, pick up, open)", ["Circle"], ""),
    ("180° turn", ["L1"], ""),
    ("Roulette", ["R1"], ""),
    ("Taunt", ["L2"], ""),
    ("Unleash the God Hand", ["R2"], ""),
    ("Brawl mode / freecam", ["R3"], "Modern: Toggle"),
    ("Brawl mode while held, double-tap for the 180° turn", ["L1HOLD"], "Modern: Hold"),
    ("Dash", ["L3"], "Click-Stick Dash on the controller"),
    ("Styles S / E / X / Y", ["Up", "Right", "Down", "Left"], "Moveset Presets"),
    ("Pause", ["Start"], ""),
]


def control_lines():
    text = INPUT.read_text(encoding="utf-8")
    body = text[text.index("defaultPadSettings(uint32_t pad)"):]
    body = body[:body.index("return settings;")]
    keyboard, pad = {}, {}
    for key, kb, element in re.findall(r'add\("(\w+)", (nullptr|"[^"]*"), "([^"]*)"\);', body):
        pad[key] = element
        if kb != "nullptr":
            binding = kb.strip('"')
            keyboard[key] = KEY_NAMES.get(binding, binding.split("/", 1)[-1])
    dodge = re.search(r'kDodgeKey\), "Keyboard/(\w+)"', body)

    def kb_of(keys, action):
        if keys == ["DODGE"]:
            return f"{dodge.group(1)} + {' '.join(keyboard[k] for k in ['LUp', 'LLeft', 'LDown', 'LRight'])}"
        if keys == ["L3"]:
            return "Double-tap a direction"
        if keys == ["L1HOLD"]:
            return "—"  # the controller's only (the keyboard's Dodge Modifier is its hold)
        names = [keyboard.get(k) for k in keys]
        return " ".join(n for n in names if n) or "—"

    def pad_of(keys):
        if keys == ["DODGE"]:
            return "Right stick"
        if keys[0] == "LUp":
            return "Left stick"
        if keys[0] == "Up":
            return "D-pad up / right / down / left"
        if keys == ["L1HOLD"]:
            keys = ["L1"]
        return PAD_NAMES.get(pad.get(keys[0], ""), pad.get(keys[0], "—"))

    out = ["| Action | Controller | Keyboard & mouse |", "| --- | --- | --- |"]
    for action, keys, condition in ROWS:
        name = f"{action} *({condition})*" if condition else action
        out.append(f"| {name} | {pad_of(keys)} | {kb_of(keys, action)} |")
    return out


def preset_lines(data):
    out = ["| Preset | Tagline | What you get |", "| --- | --- | --- |"]
    for p in data["presets"]:
        out.append(f"| **{p['name']}** | {p['tagline']} | {clean(p['band'])} |")
    return out


def fill(readme, name, lines):
    pattern = re.compile(rf"(<!-- BEGIN GENERATED: {name} [^>]*-->\n).*?(<!-- END GENERATED: {name} -->)", re.S)
    if not pattern.search(readme):
        sys.exit(f"gen_readme: README.md has no '{name}' block")
    return pattern.sub(lambda m: m.group(1) + "\n".join(lines) + "\n" + m.group(2), readme)


def main():
    data = dump_tables()
    tag_of = tags(data)
    readme = README.read_text(encoding="utf-8")
    new = re.sub(r"<!-- tag:(\w+) -->( `[^`]*`)?",
                 lambda m: f"<!-- tag:{m.group(1)} --> `{tag_of[m.group(1)]}`" if m.group(1) in tag_of
                 else sys.exit(f"gen_readme: no setting {m.group(1)}"), readme)
    new = fill(new, "options", option_lines(data, tag_of))
    new = fill(new, "controls", control_lines())
    new = fill(new, "presets", preset_lines(data))
    if "--check" in sys.argv:
        if new != readme:
            print("README.md is out of date: run python -I tools/readme/gen_readme.py")
            return 1
        return 0
    if new != readme:
        README.write_text(new, encoding="utf-8", newline="\n")
        print("README.md updated")
    else:
        print("README.md already up to date")
    return 0


if __name__ == "__main__":
    sys.exit(main())
