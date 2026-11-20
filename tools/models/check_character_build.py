"""Check GHR's C++ character build (src/kamite/character_build.cpp) against this folder's Python build, byte for byte.

    python -I tools/models/check_character_build.py --extract <folder of the disc's files: pl/pl00.dat ...> [id ...]
        [--also pl/pl02.dat=pl/pl00.dat ...]

For every assets/characters/<id>/character.json (or the ids given): each file it lists is merged by
build_characters.merge_file and by the C++ (compiled here with c++ / g++ / clang++), and the two must be equal; so must
each painted texture's full-resolution image (overlay_hd without a pack) and its replacement name. --also merges
another game file with an entry's settings too (GHR serves pl00's result for every costume, but a costume's own file
is a good second test of the trims and strips). Needs numpy, Pillow and xxhash. Run it after changing either side.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "xavier"))
import build_characters  # noqa: E402
import gamemodel  # noqa: E402
import paint  # noqa: E402
from gametex import tim3_decode  # noqa: E402
from pcsx2tex import replacement_name  # noqa: E402


def compile_checker(tmp):
    exe = os.path.join(tmp, "check.exe" if os.name == "nt" else "check")
    sources = [os.path.join(HERE, "check_character_build.cpp"), os.path.join(ROOT, "src", "kamite", "character_build.cpp")]
    for compiler in ("c++", "g++", "clang++"):
        if shutil.which(compiler):
            subprocess.check_call([compiler, "-std=c++20", "-O2", "-ffp-contract=off", "-I", os.path.join(ROOT, "src"), *sources, "-o", exe])
            return exe
    sys.exit("no C++ compiler (c++, g++ or clang++) on PATH")


def main(argv):
    args = list(argv[1:])
    extract, also = None, []
    while "--extract" in args:
        i = args.index("--extract"); extract = args[i + 1]; del args[i:i + 2]
    while "--also" in args:
        i = args.index("--also"); also.append(args[i + 1].split("=", 1)); del args[i:i + 2]
    if not extract:
        print(__doc__)
        return 2
    base = os.path.join(ROOT, "assets", "characters")
    ids = args or sorted(d for d in os.listdir(base) if os.path.isfile(os.path.join(base, d, "character.json")))
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        exe = compile_checker(tmp)
        for cid in ids:
            folder = os.path.join(base, cid)
            spec = json.load(open(os.path.join(folder, "character.json"), encoding="utf-8"))
            jobs = [(rel, rel) for rel in spec["files"]] + [(game, rel) for game, rel in also if rel in spec["files"]]
            for game_rel, rel in jobs:
                how = spec["files"][rel]
                game = open(os.path.join(extract, *game_rel.split("/")), "rb").read()
                py, layers = build_characters.merge_file(game, folder, how)
                out = os.path.join(tmp, "out.dat")
                cmd = [exe, folder, rel, os.path.join(extract, *game_rel.split("/")), out]
                for name, layer in layers.items():
                    raw = os.path.join(tmp, name + ".rgba")
                    np.ascontiguousarray(layer).tofile(raw)
                    cmd += [name, raw, str(layer.shape[1]), str(layer.shape[0])]
                print(" ", subprocess.check_output(cmd, text=True).strip())
                cpp = open(out, "rb").read()
                same = cpp == py
                bad += not same
                print("  %s %s (with %s's settings): %s" % (cid, game_rel, rel, "same" if same else "DIFFERENT (%d vs %d bytes)" % (len(cpp), len(py))))
                if not layers:
                    continue
                game_tex = {nm: tb for t, d, o in gamemodel.dat_read(game) if t == "TM3" and d is not None for nm, tb, _ in gamemodel.tm3_read(d)}
                new_tex = {nm: tb for t, d, o in gamemodel.dat_read(py) if t == "TM3" and d is not None for nm, tb, _ in gamemodel.tm3_read(d)}
                for name, layer in layers.items():
                    if name not in game_tex or name not in new_tex:
                        continue
                    rgba, info = tim3_decode(game_tex[name])
                    hd = np.asarray(Image.fromarray(rgba, "RGBA").resize((info["w"] * 4, info["h"] * 4), Image.LANCZOS))
                    want = paint.overlay(hd, layer)[..., :3]
                    got = np.fromfile(out + "." + name + ".rgb", np.uint8).reshape(want.shape)
                    names = (replacement_name(new_tex[name]), replacement_name(open(out + "." + name + ".tim3", "rb").read()))
                    ok = (got == want).all() and names[0] == names[1]
                    bad += not ok
                    print("    %s: %s, image %s" % (name, names[1], "same" if (got == want).all() else
                                                    "differs (max %d)" % np.abs(got.astype(int) - want).max()) + ("" if names[0] == names[1] else ", NAME DIFFERS"))
    print("all equal" if not bad else "%d difference(s)" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
