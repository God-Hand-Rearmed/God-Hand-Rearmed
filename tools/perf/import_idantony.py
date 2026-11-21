"""Address -> module map from idantony/godhand-windows (MIT), for subsystems.py's grouping.

    python3 tools/perf/import_idantony.py <godhand-windows checkout>

Their hand-written C names each function after the EE address it reimplements (gh_cloth_frame_00104010) and groups
them in files by system (game_cloth_finish.c, game_enemy_move.c ...). This writes tools/perf/idantony_modules.csv:
address,module (the file's stem; an address defined in several files gets them joined by '|'). Facts only (which
system an address belongs to); no code is copied.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "idantony_modules.csv")
DEF = re.compile(r"^[A-Za-z_][\w \*]*?\b\w+?_(00[0-9a-fA-F]{6})\s*\(", re.M)


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    core = os.path.join(argv[1], "src", "core")
    found = {}
    for name in sorted(os.listdir(core)):
        if not name.endswith(".c"):
            continue
        with open(os.path.join(core, name), encoding="utf-8", errors="replace") as f:
            for m in DEF.finditer(f.read()):
                found.setdefault(int(m.group(1), 16), set()).add(name[:-2])
    try:
        head = subprocess.run(["git", "-C", argv[1], "log", "-1", "--format=%h"], capture_output=True, text=True).stdout.strip()
    except OSError:
        head = "?"
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"# idantony/godhand-windows {head}: EE address -> the module (file) that reimplements it\n")
        f.write("address,module\n")
        for address in sorted(found):
            f.write(f"0x{address:08X},{'|'.join(sorted(found[address]))}\n")
    print(f"{len(found)} addresses -> {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
