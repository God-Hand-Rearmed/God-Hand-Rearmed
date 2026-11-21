"""Writes a stand-in for the recompiled game code, so GHR builds without a disc (CI, packaging tests).

    python -I tools/linux/stub_game_code.py <folder>

The real generated/ folder comes from your own disc image (python -I build.py generate) and holds the game's code
translated to C++. This one holds only what GHR links against: the function table's globals (empty, one slot) and the
two headers the precompiled header names. A GHR built on it runs its launcher and stops at the first game function
(exit code 5, "missing function"); it can never play the game.

The folder also gets a KAMITE_STUB marker. src/CMakeLists.txt sees it, puts STUB-GAME-CODE.txt next to the binary, and
tools/package.py refuses to package such a build unless told it is a test (--allow-stub, which also names the
archives "-stub"), so a stand-in can never be published as a release by mistake.
"""
import sys
from pathlib import Path

HEADER = '#pragma once\n#include <cstdint>\n#include "ps2_runtime.h"\n'

REGISTER = """// Stand-in written by tools/linux/stub_game_code.py: no game code, only what GHR links against.
#include "ps2_runtime.h"
#include <ps2_recompiled_functions.h>

extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x100000u;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x100004u;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 1u;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[1u] = {};
"""

MARKER = "This folder is a stand-in written by tools/linux/stub_game_code.py, not the game's recompiled code.\n"

FILES = {
    "ps2_recompiled_functions.h": HEADER,
    "ps2_recompiled_stubs.h": HEADER,
    "register_functions.cpp": REGISTER,
    "KAMITE_STUB": MARKER,
}


def write(folder: Path) -> None:
    folder.mkdir(parents=True, exist_ok=True)
    real = folder / "register_functions.cpp"
    if real.exists() and not (folder / "KAMITE_STUB").exists():
        # Never overwrite real generated code (build.py generate's output).
        raise SystemExit(f"{folder} holds real generated code (no KAMITE_STUB marker); not touching it")
    for name, text in FILES.items():
        path = folder / name
        if not path.exists() or path.read_text() != text:  # unchanged files keep their time: no rebuild
            path.write_text(text)


def main(argv):
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[2].strip())
        return 2
    write(Path(argv[1]))
    print(f"stand-in game code: {argv[1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
