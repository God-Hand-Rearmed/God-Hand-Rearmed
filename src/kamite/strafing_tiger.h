#pragma once

#include <atomic>

class PS2Runtime;

// Strafing Tiger (prototype, F2 > Controls; later.md "Strafing Tiger", the tech deck's strafing-tiger.md): in Modern's
// free camera, Gene faces where the camera points and the left stick moves him relative to the camera without turning
// him. Step 2: his own walk / run keep playing forward (the game's states, sounds and timing) and the side-steps and
// his back-pedal (strafe_motions.h, built from the disc) are blended on top by the stick's angle, locked to his steps;
// the root delta is the same blend, so he goes where the stick points with his feet planted. Off = nothing changes.
namespace kamite::strafing_tiger
{
    // KAMITE_STRAFING_TIGER=1 at start, or the F2 checkbox (no launcher / GHR.ini row while it's a prototype).
    std::atomic<bool> &setting();
    bool enabled();

    // Window thread (modern_controls' pad filter): where the stick points, in radians from his facing (his yaw's sense:
    // + = his left, +-pi straight back; the game is fed his walk / run forward); or no move at all.
    void setSlide(float angle);
    void clearSlide();
    // Window thread: Gene sprints (a dash, phase 6, and the run it carries into until he stops or walks): no
    // strafing, he runs where the camera points (K, Nov 26).
    void setSprint(bool sprint);

    // Hooks cObjBase_addNullSpeed (0x14F928) and the motion pose (0x158328). Install before every other wrapper of
    // the pose (main.cpp: before style_presets), so its wrapper is the innermost: the arm swing stays on top and the
    // 60 FPS half frames smooth the blend. Builds the side motions from the disc (after PS2Runtime::setIoPaths).
    void install(PS2Runtime &runtime);
}
