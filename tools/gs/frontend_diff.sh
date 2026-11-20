#!/bin/bash
# frontend_diff.sh [base rev] [streams]: runtime gs_frontend.cpp at [base rev] (default HEAD) vs the working tree, on
# Linux with g++ (frontend_diff.cpp: random GIF streams, raw and classic back end; every back end call, SIGNAL / FINISH /
# LABEL effect, trace event, the debug snapshot and history hashed). Also times a vertex-heavy stream on both.
# The classic back end's SIGNAL-stall re-tag reads past a cut packet (pre-existing, raw mode never takes it), so a
# classic-mode seed can differ run to run: compare raw first.
set -e
REV=${1:-HEAD}; STREAMS=${2:-1000}
REPO=$(cd "$(dirname "$0")/../.." && pwd); RT=$REPO/runtime/ps2xRuntime
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
INC="-I$RT/include -I$RT/include/runtime -I$RT/src/lib -I$REPO/runtime/ps2xIOP/include"
git -C "$REPO" show "$REV:runtime/ps2xRuntime/src/lib/gs/gs_frontend.cpp" > "$RT/src/lib/gs/gs_frontend_base_tmp.cpp"
g++ -std=c++20 -O2 -w -c "$RT/src/lib/gs/gs_frontend_base_tmp.cpp" $INC -o "$WORK/base.o"; rm "$RT/src/lib/gs/gs_frontend_base_tmp.cpp"
g++ -std=c++20 -O2 -w -c "$RT/src/lib/gs/gs_frontend.cpp" $INC -o "$WORK/new.o"
g++ -std=c++20 -O2 -w -c "$RT/src/lib/gs/gs_recording.cpp" $INC -o "$WORK/rec.o"
g++ -std=c++20 -O2 -w -c "$REPO/tools/gs/frontend_diff.cpp" $INC -o "$WORK/h.o"
g++ -o "$WORK/base" "$WORK/h.o" "$WORK/base.o" "$WORK/rec.o" -lpthread
g++ -o "$WORK/new" "$WORK/h.o" "$WORK/new.o" "$WORK/rec.o" -lpthread
fail=0
for raw in 1 0; do
    for seed in 1 2 3 4; do
        a=$("$WORK/base" $seed $STREAMS $raw); b=$("$WORK/new" $seed $STREAMS $raw)
        if [ "$a" == "$b" ]; then echo "same  raw=$raw seed=$seed"; else echo "DIFF  raw=$raw seed=$seed: $a / $b"; fail=1; fi
    done
done
"$WORK/base" 7 20 1 bench; "$WORK/new" 7 20 1 bench
exit $fail
