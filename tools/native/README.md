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

    F="sceVu0MulMatrix_0x30a2e0 sceVu0ApplyMatrix_0x30a2b0 _sceVu0ecossin_0x30a5d0 sceVu0RotMatrixX_0x30a700
       sceVu0RotMatrixY_0x30a7b8 sceVu0RotMatrixZ_0x30a650 sceVu0CopyMatrix_0x30a548 sceVu0UnitMatrix_0x30a5a0
       sceVu0MulVector_0x30a4d0 sceVu0ScaleVector_0x30a4e8 sceVu0OuterProduct_0x30a328 sceVu0Normalize_0x30a370
       sceVu0TransMatrix_0x30a508 sceVu0TransposeMatrix_0x30a3b0 sceVu0InversMatrix_0x30a3f8 sceVu0CameraMatrix_0x30a8c8
       sceVu0NormalLightMatrix_0x30a978 capVu0Length_0x1f7df8 Tramp_001F7DF8_00101E28_0x101e28 MtxCopy_0x147528
       MtxMultiply_0x147548 func_0014FF00_0x14ff00 func_0031A650_0x31a650 func_0031A600_0x31a600"
    # three at a time: more runs the sandbox out of memory
    echo $F | tr ' ' '\n' | xargs -P 3 -I{} g++ -std=c++20 -O2 -msse4.1 -ffp-contract=off $INC -c /tmp/gen/{}.cpp -o /tmp/nt/{}.o
    g++ -std=c++20 -O2 -msse4.1 -ffp-contract=off $INC tools/native/shadow_test.cpp src/kamite/native/shadow.cpp \
        src/kamite/native/libvu0.cpp src/kamite/native/packet.cpp src/kamite/native/recording.cpp \
        runtime/ps2xRuntime/src/lib/ps2_ee_float.cpp /tmp/nt/*.o -o /tmp/nt/shadow_test
    /tmp/nt/shadow_test 20000 /tmp/nt/vectors.bin    # natives, footprints, invisibility, detection; writes vectors
    /tmp/nt/shadow_test bench                        # ns per call (GCC: a hint only)
    /tmp/nt/shadow_test rerun <case>.<rate>.native.bin   # a replay's saved mismatches, again here

The maths alone on every path (SSE, plain C++, NEON under qemu):

    g++ -std=c++20 -O2 -msse4.1 -ffp-contract=off -Isrc tools/native/libvu0_check.cpp -o c && ./c /tmp/nt/vectors.bin
    g++ -std=c++20 -O2 -ffp-contract=off -DKAMITE_VU_SCALAR -Isrc tools/native/libvu0_check.cpp -o c && ./c ...
    aarch64-linux-gnu-g++ -std=c++20 -O2 -ffp-contract=off -static -Isrc tools/native/libvu0_check.cpp -o c && qemu-aarch64 ./c ...

A new native function: add its recompiled function to the build lines and to shadow_test's tables (the declarations,
main's recompiled list, setupBatch1 or its own setup for its arguments, the nested list if it calls another), and its
core maths to a vectors check. A native with a recorded footprint (footprint = nullptr) gets two more checks for free:
the recording run (ShadowEnv filled from RAM on first touch) must agree with the real run, and every byte the
recompiled function writes must lie in what the native recorded. KM_DIFF=1 prints the first mismatches' differing
context words and RAM.
