# tools/native: host-side checks of the native EE functions

The native functions (src/kamite/native, claude/native-next.md) and the shadow harness, checked without a Windows
build. Needs the recompiled functions, which come from the game's ELF (not in the repo): build ps2_recomp and run it on
a copy of recomp/ whose kamite.toml points at SLUS_215.03:

    cmake -G Ninja -S runtime/ps2xRecomp -B /tmp/recomp && ninja -C /tmp/recomp
    cp -r recomp /tmp/rc && cp <SLUS_215.03> /tmp/rc/
    sed -i 's|^input = .*|input = "SLUS_215.03"|; s|^output = .*|output = "/tmp/gen"|' /tmp/rc/kamite.toml
    (cd /tmp/rc && /tmp/recomp/ps2_recomp kamite.toml)

Then (INC = the include list in claude/workspace.md "Checks without a Windows build", plus -I/tmp/gen and a stub dir
with kamite/git_commit.h):

    for f in sceVu0MulMatrix_0x30a2e0 sceVu0ApplyMatrix_0x30a2b0; do
        g++ -std=c++20 -O2 -msse4.1 -ffp-contract=off $INC -c /tmp/gen/$f.cpp -o /tmp/nt/$f.o; done
    g++ -std=c++20 -O2 -msse4.1 -ffp-contract=off $INC tools/native/shadow_test.cpp src/kamite/native/shadow.cpp \
        src/kamite/native/libvu0.cpp runtime/ps2xRuntime/src/lib/ps2_ee_float.cpp /tmp/nt/*.o -o /tmp/nt/shadow_test
    /tmp/nt/shadow_test 20000 /tmp/nt/vectors.bin    # natives, footprints, invisibility, detection; writes vectors
    /tmp/nt/shadow_test bench                        # ns per call (GCC: a hint only)
    /tmp/nt/shadow_test rerun <case>.<rate>.native.bin   # a replay's saved mismatches, again here

The maths alone on every path (SSE, plain C++, NEON under qemu):

    g++ -std=c++20 -O2 -msse4.1 -ffp-contract=off -Isrc tools/native/libvu0_check.cpp -o c && ./c /tmp/nt/vectors.bin
    g++ -std=c++20 -O2 -ffp-contract=off -DKAMITE_VU_SCALAR -Isrc tools/native/libvu0_check.cpp -o c && ./c ...
    aarch64-linux-gnu-g++ -std=c++20 -O2 -ffp-contract=off -static -Isrc tools/native/libvu0_check.cpp -o c && qemu-aarch64 ./c ...

A new native function: add its recompiled function to the build lines and to shadow_test's tables (checkNative /
checkInvisible / rerun), and its core maths to a vectors check.
