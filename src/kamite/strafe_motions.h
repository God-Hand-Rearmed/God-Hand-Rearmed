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
// - run: 2044 (cell 11's boxing shuffle, his right) and it mirrored, sized, hips 3 cm lower, one of its cycles on one
//   of his run's 14 frames (pl00 25 / 428), touchdowns warped onto his, steps x2.2 for 3.0 m/s.
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
        RunLeft,
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
        float travel = 0.0f;                          // the root's x over one loop
    };

    struct Pose
    {
        mot_sample::Quat rot[kBones];
        float tr[kTranslated][3];
    };

    // em00.dat's entry `index` (from its offset to the next one), false when it can't be read.
    using Fetch = std::function<bool(uint32_t index, std::vector<uint8_t> &bytes)>;
    // The four, or false (and why) when em00.dat's motions aren't the ones the recipe was made on.
    bool build(const Fetch &fetch, std::array<Motion, kSides> &out, std::string &why);

    // Frame t (any: wrapped onto the loop); the translations' x scaled by stride about their means.
    void sample(const Motion &m, float t, float stride, Pose &out);
    // The root's x at frame t, unwound (one loop's travel per loop), times stride.
    float rootX(const Motion &m, float t, float stride);
}
