#pragma once

#include "kamite/mot_sample.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Strafing Tiger's side motions (the tech deck's strafing-tiger.md "Step 2 plan"; its k-pc/strafing-tiger/
// strafe_bake.py / strafe_round2.py are the recipe this follows and the reference it is checked against; K's picks W2 /
// R2, Nov 26).
// Built at start from em00.dat's motions read off the player's disc (nothing of the game's is shipped), on the grid of
// Gene's own loops so that a side motion's frame is his clock:
// - walk: 3450 (his right) and it mirrored (his left), sized onto Gene, on his walk's 57 frames (pl00 21: two cycles)
//   with every foot touchdown moved onto his (a piecewise-linear time warp), the steps scaled about each foot's mean to
//   1.74 m/s;
// - run (K, Nov 27: 11 out; the side-steps at every speed): the same two cycles averaged onto one of his run's 14
//   frames (pl00 25 / 428), 3.0 m/s as built (the blend's shares stretch the steps to the speed asked);
// - back (K, Nov 27: the game's own back-pedal past 112.5 deg, untouched straight back): the same on his back-pedal's
//   42 frames (pl00 23: two cycles), 1.97 m/s (its own speed).
// - the run's sideways stride at 3 / 9 (K, Nov 27: the side-run K generated, retargeted offline: strafe_run_data.h;
//   11 and the Mixamo jog gone): one cycle on one of his run's, its own step length, its whole body.
// Dense frames (a quaternion per part, the hips' and foot targets' translations, the root), sampled with whole-frame
// slerps; frame span = frame 0 but for the root's travel.
namespace kamite::strafe_motions
{
    constexpr int kBones = 27;     // parts 0..26 (27 / 28 are the camera locators, 29-31 his head's children: his own)
    constexpr int kTranslated = 3; // the hips and the leg IK targets
    constexpr uint32_t kTranslatedBones[kTranslated] = {0u, 20u, 24u};
    enum Side
    {
        WalkLeft,
        WalkRight,
        RunLeft,  // 16 on his run's grid (strafe_motions::Set::runs has every cell's)
        RunRight,
        kSides
    };

    struct Motion
    {
        int span = 0;                                 // frames 0..span
        uint32_t rotated = 0;                         // bit b: part b turns
        std::vector<mot_sample::Quat> rot;            // (span + 1) x kBones
        std::vector<std::array<float, 3>> tr;         // (span + 1) x kTranslated
        std::vector<std::array<float, 3>> root;       // span + 1, from 0 at frame 0
        std::array<std::array<float, 3>, kTranslated> mean{}; // each translation's mean over the loop
        mot_sample::Quat armMean[kBones];             // each part's mean rotation over the loop (the arms' calming)
        int axis = 0;                                 // the steps' direction: 0 x (sideways), 2 z (the back-pedal)
        float travel = 0.0f;                          // the root along it over one loop
        float native = 0.0f;                          // ... as animated (before the steps were scaled)
        float sourceFrames = 0.0f;                    // the source's frames over one loop (its own pace: / span)
        float sourceRate = 1.0f;                      // the source's frames a 30 FPS frame as made
    };

    struct Pose
    {
        mot_sample::Quat rot[kBones];
        float tr[kTranslated][3];
    };

    // The slow strafe's candidates (K, Nov 27; strafe_round4.py): the grid's cell numbers. 16 = 3450 (built as
    // WalkLeft / WalkRight; at full Tension), 7 = em00 1604 (the rest of the time), 13 = 2438 (to compare in F2), each
    // his right and it mirrored, on his walk's grid at 1.74 m/s. (Cell 1 dropped, K: its lower back doesn't fit Gene.)
    enum WalkSource
    {
        Cell16,
        Cell7,
        Cell13,
        kWalkSources
    };

    struct Set
    {
        std::array<Motion, kSides> motions;
        std::array<std::array<Motion, 2>, kWalkSources> walks; // [source][0 his left, 1 his right] on his walk's grid
        std::array<std::array<Motion, 2>, kWalkSources> runs;  // the same, two cycles averaged on his run's grid
        std::array<std::array<Motion, 2>, kWalkSources> backs; // the same on his back-pedal's grid
        uint32_t walksBuilt = 0;                               // bit per source (16's = motions' Walk / Run sides)
        std::array<Motion, 2> sideRun;                         // the side-run on his run's grid, its own steps
        bool sideRunBuilt = false;
    };

    // A .dat's entry `index` (from its offset to the next one), false when it can't be read.
    using Fetch = std::function<bool(uint32_t index, std::vector<uint8_t> &bytes)>;
    // From em00.dat, or false (and why) when its motions aren't the ones the recipe was made on.
    bool build(const Fetch &em00, Set &out, std::string &why);

    // Frame t (any: wrapped onto the loop); the translations along the motion's axis scaled by stride about their means.
    void sample(const Motion &m, float t, float stride, Pose &out);
    // The root along the motion's axis at frame t, unwound (one loop's travel per loop), times stride.
    float rootAt(const Motion &m, float t, float stride);
}
