#!/bin/bash
# codegen_diff.sh <SLUS_215.03> <work dir> [functions] [seeds]: the classic vs register-locals check (codegen_diff.cpp)
# on Linux. Needs cmake, ninja, g++, python3 and raylib's headers (git clone --depth 1 --branch 5.5
# https://github.com/raysan5/raylib $WORK/raylib). Picks [functions] (default 600) random functions that make no syscalls.
set -e
ELF=$(realpath "$1"); WORK=$(realpath -m "$2"); COUNT=${3:-600}; SEEDS=${4:-4}
REPO=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$WORK" && cd "$WORK"
[ -d raylib ] || git clone -q --depth 1 --branch 5.5 https://github.com/raysan5/raylib raylib
cmake -S "$REPO/runtime/ps2xRecomp" -B recomp -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null && ninja -C recomp ps2_recomp >/dev/null
for mode in classic locals; do
    python3 - "$REPO/recomp/kamite.toml" "$ELF" "$WORK/$mode" > "$mode.toml" <<'EOF'
import re, sys
from pathlib import Path
toml = Path(sys.argv[1]); text = toml.read_text()
text = re.sub(r'(?m)^input\s*=.*$', f'input = "{sys.argv[2]}"', text)
text = re.sub(r'(?m)^output\s*=.*$', f'output = "{sys.argv[3]}"', text)
text = re.sub(r'(?m)^(ghidra_output|runtime_relocations)\s*=\s*"([^"]+)"',
              lambda m: f'{m.group(1)} = "{(toml.parent / m.group(2)).resolve().as_posix()}"', text)
print(text)
EOF
    rm -rf "$mode" && mkdir "$mode" && PS2X_CODEGEN=$mode recomp/ps2_recomp "$mode.toml" > "$mode.log"
done
python3 - "$COUNT" <<'EOF'
import glob, random, re, sys
bad = re.compile(r'runtime->(handleBreak|handleSyscall|SignalException|clearLLBit|executeVU0|(?!callGuestFunction|'
                 r'callGuestFunctionIndirect|dispatchGuestBranch|eeCheckpointDue|Load|Store)\w+)|ps2_syscalls::|ps2_stubs::')
picks = []
for f in sorted(glob.glob('locals/*.cpp')):
    t = open(f).read()
    m = re.search(r'^void (\w+)\(uint8_t\* rdram, R5900Context\* ctx, PS2Runtime \*runtime\) \{', t, re.M)
    if m and '#define PS2_FLUSH_E' in t and not bad.search(t) and len(t) < 400000:
        picks.append((f.split('/')[-1], m.group(1)))
picks = random.sample(picks, min(int(sys.argv[1]), len(picks)))
open('files.txt', 'w').write(''.join(p[0] + '\n' for p in picks))
open('names.txt', 'w').write(''.join(f'_Z{len(p[1])}{p[1]}PhP12R5900ContextP10PS2Runtime\n' for p in picks))
EOF
INC="-I $REPO/runtime/ps2xRuntime/include -I $REPO/runtime/ps2xRuntime/src/lib/Kernel -I $REPO/runtime/ps2xIOP/include -I $WORK/raylib/src"
FLAGS="-std=c++20 -O2 -fPIC -msse4.1 -w"
mkdir -p pch && printf '#include "ps2_runtime_macros.h"\n#include "ps2_runtime.h"\n#include <ps2_recompiled_functions.h>\n#include <ps2_recompiled_stubs.h>\n#include "ps2_syscalls.h"\n#include "ps2_stubs.h"\n#include "ps2_cg.h"\n' > pch/all.h
g++ $FLAGS $INC -I locals -x c++-header pch/all.h -o pch/all.h.gch
for mode in classic locals; do
    mkdir -p "obj_$mode"
    xargs -P "$(nproc)" -I{} g++ $FLAGS -I pch -include all.h $INC -I locals -c "$mode/{}" -o "obj_$mode/{}.o" < files.txt
    g++ -shared -o "$mode.so" obj_$mode/*.o
done
g++ -std=c++20 -O1 -msse4.1 -w $INC -rdynamic "$REPO/tools/recomp/codegen_diff.cpp" -o codegen_diff -ldl
./codegen_diff ./classic.so ./locals.so names.txt "$SEEDS"
