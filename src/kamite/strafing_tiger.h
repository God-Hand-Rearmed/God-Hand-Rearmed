#pragma once

#include <atomic>

class PS2Runtime;

// Strafing Tiger (prototype, F2 > Controls; later.md "Strafing Tiger", the tech deck's strafing-tiger.md): in Modern's
// free camera, Gene faces where the camera points and the left stick moves him relative to the camera without turning
// him. Step 2: his own walk / run / back-pedal keep playing (the game's states, sounds and timing; straight ahead and
// straight back his own, K Nov 27) and the side-steps (strafe_motions.h, built from the disc) are blended on top by the
// stick's angle, locked to his steps; the root delta is the same blend, so he goes where the stick points with his feet
// planted. Off = nothing changes.
namespace kamite::strafing_tiger
{
    // KAMITE_STRAFING_TIGER=1 at start, or the F2 checkbox (no launcher / GHR.ini row while it's a prototype).
    std::atomic<bool> &setting();
    bool enabled();

    // Window thread (modern_controls' pad filter): where the stick points, in radians from his facing (his yaw's sense:
    // + = his left, +-pi straight back; the game is fed his walk / run forward or, past 112.5 deg, his back-pedal); or no
    // move at all.
    void setSlide(float angle);
    void clearSlide();
    // Window thread: Gene sprints (a dash, phase 6, and the run it carries into until he stops or walks): no
    // strafing, he runs where the camera points (K, Nov 26).
    void setSprint(bool sprint);
    // F2 > Controls (K, Nov 27: to judge in game; live, not saved): the slow strafe (0 = by Tension: cell 7, 16 at a
    // full gauge; 1 + strafe_motions::WalkSource = that one), 16's arm swing kept at 3 / 9 o'clock (0.69) and how far
    // his back-pedal's arms go toward 16's at 6 (0.48).
    // walkSourceBuilt: false for one em00.dat didn't give (16 plays).
    std::atomic<int> &walkChoice();
    std::atomic<float> &tensionArms();
    std::atomic<float> &tensionBack();
    bool walkSourceBuilt(int source);

    // F2 stage 1 (K, Nov 27; live, not saved): the run's side-step at full deflection. Legs: the jog (Mixamo's Jog
    // Strafe Right; its torso / arms swing on cell 7's ready posture, scaled) or the side-steps (7 / 16 by Tension).
    // Speed and pace at 3 / 9 are shares of his run's (12 o'clock), eased in by the angle; the hips' drop and bounce
    // apply to the run's side.
    struct RunStrafe
    {
        std::atomic<int> legs{0};             // 0 = the jog, 1 = the side-steps
        std::atomic<float> speed{0.25f};      // 3 / 9 speed, a share of his run's
        std::atomic<float> pace{0.5f};        // 3 / 9 step pace, a share of his run's
        std::atomic<float> bounce{1.0f};      // the hips' up-down swing (1 = as animated)
        std::atomic<float> hips{0.0f};        // the hips lowered, cm
        std::atomic<float> armSwing{0.7f};    // the jog's arm swing on 7's posture (1 = the jog's own)
        std::atomic<float> torsoSwing{0.7f};  // ... and its torso / neck / head swing
        std::atomic<float> turn{45.0f};       // his run turned toward the stick up to this (deg; K: good up to 45)
    };
    RunStrafe &runStrafe();
    bool jogBuilt(); // false: the jog didn't build (the side-steps play)
    // The read-out at 3 / 9 from his last run (0 = not run yet): the legs' pace (x its own, at 1 frame per 30 FPS
    // frame) and their step length (x its own).
    float readPace();
    float readSteps();

    // Hooks cObjBase_addNullSpeed (0x14F928) and the motion pose (0x158328). Install before every other wrapper of
    // the pose (main.cpp: before style_presets), so its wrapper is the innermost: the arm swing stays on top and the
    // 60 FPS half frames smooth the blend. Builds the side motions from the disc (after PS2Runtime::setIoPaths).
    void install(PS2Runtime &runtime);
}
