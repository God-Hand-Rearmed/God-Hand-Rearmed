#pragma once

#include <atomic>

class PS2Runtime;

// Strafing Tiger (prototype, F2 > Controls; later.md "Strafing Tiger", the tech deck's strafing-tiger.md): in Modern's
// free camera, Gene faces where the camera points and the left stick moves him relative to the camera without turning
// him. Step 2: his own walk / run / back-pedal keep playing (the game's states, sounds and timing) and the side-steps
// built from em00.dat (strafe_motions.h) are blended on top by the stick's angle, locked to his steps; the root delta
// is the same blend of his and the side's, so he goes where the stick points with his feet planted. Off = nothing
// changes.
namespace kamite::strafing_tiger
{
    // KAMITE_STRAFING_TIGER=1 at start, or the F2 checkbox (no launcher / GHR.ini row while it's a prototype).
    std::atomic<bool> &setting();
    bool enabled();

    // Window thread (modern_controls' pad filter): where the stick points, in radians from where his walk / run carries
    // him (his yaw's sense: + = his left) or, on the back-pedal, from straight behind him; or no move at all.
    void setSlide(float angle);
    void clearSlide();

    // Hooks cObjBase_addNullSpeed (0x14F928) and the motion pose (0x158328). Install before every other wrapper of
    // the pose (main.cpp: before style_presets), so its wrapper is the innermost: the arm swing stays on top and the
    // 60 FPS half frames smooth the blend. Builds the side motions from the disc (after PS2Runtime::setIoPaths).
    void install(PS2Runtime &runtime);
}
