#include "kamite/strafing_tiger.h"

#include "kamite/disc_files.h"
#include "kamite/function_hooks.h"
#include "kamite/game_layout.h"
#include "kamite/log.h"
#include "kamite/mot_sample.h"
#include "kamite/strafe_motions.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_save_state.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace kamite::strafing_tiger
{
    namespace
    {
        // cObjBase_addNullSpeed (obj in a0): position *(+0xF0) += the motion's root delta (+0x330 / +0x334 / +0x338,
        // the model's own space) x scale (+0x110..) x rate (f12), turned by the rotation +0x100 (yaw +0x104). The
        // walking steps run it every frame after moveMotion filled the delta (frame_patches' held vblank too).
        // The pose 0x158328 (obj in a0, from moveMotion before the clock steps): the motion at +0x428 at the clock
        // +0x430 into every part's Euler +0x100 (parts at +0x278, count +0x2B4) and the hips' / leg IK targets'
        // translations through the parts' +0xD0; root outputs +0x310 / +0x340 / +0x370.
        // moveMotion (0x157C18; obj in a0): the clock steps by +0x444 (SetMotionStep 0x158288 / setMotionInfo
        // 0x157568) x +0x440 x the frame's share (half at 60 FPS), read at 0x157C98 before the pose (0x157D5C); the
        // root delta +0x330 = the pose's root output +0x310 - the last one (+0x320). His run (phase 2) steps 1.3 / 1.6
        // (0x10C748..), his walk 1.3 / 1.6 (0x10C338..), his back-pedal 1.5 (0x10C954).
        constexpr uint32_t kAddNullSpeed = 0x0014F928u, kMotionPose = 0x00158328u, kMoveMotion = 0x00157C18u;
        constexpr uint32_t kMotionStep = 0x444u;
        constexpr float kBackPedalStep = 1.5f;
        constexpr uint32_t kPlayerPointer = game::GameWork::address(game::GameWork::kPlayerObj);
        constexpr uint32_t kMajor = 0x2F4u, kSub = 0x2F5u, kParts = 0x278u, kPartCount = 0x2B4u, kPack = 0x304u,
                           kMotion = 0x428u, kClock = 0x430u, kDelta = 0x330u;
        constexpr uint32_t kPartRotation = 0x100u, kPartAnchor = 0xD0u;
        constexpr float kHalfTurn = 3.14159265f, kQuarterTurn = 1.57079633f;
        // The side-steps' speeds as built (strafe_motions): on his walk's grid, his run's, his back-pedal's.
        constexpr float kWalkSideSpeed = 1.74f, kRunSideSpeed = 3.0f, kBackSideSpeed = 1.97f;
        // At full deflection (K, Nov 27): his run (12 o'clock) toward his own back-pedal's speed (6), the middle at
        // 4:30, linear (so 3 / 9 is a bit over the middle); past 90 deg down to the back-pedal's by the split.
        constexpr float kBackPedalSpeed = 1.97f; // pl00 23's root, 2.76 m per 42 frames
        // The game's own back-pedal past this (the pad filter's split, modern_controls: 112.5 deg, entered at 122.5 and
        // left at 102.5): straight back it is his, untouched but for 16's arms at full Tension (K, Nov 27).
        constexpr float kBackFrom = 112.5f * kHalfTurn / 180.0f, kBackLeave = 102.5f * kHalfTurn / 180.0f;
        constexpr float kArmSideSeconds = 0.25f; // past 150 deg the arms keep the side they had; a switch eases
        constexpr float kEaseSeconds = 0.1f;   // the stick's angle eases (a KBM key's 45 deg steps; a flick across)
        constexpr int64_t kStillMs = 250;      // no blend this long: the next one starts where the stick points
        constexpr int64_t kLetGoMs = 300;      // the stick let go while his loop still plays: the blend fades out
        constexpr float kLetGoSeconds = 0.12f; // ... at this ease
        constexpr int64_t kStopMs = 200;       // into standing: from the last blended pose to his own over this (K, Nov
                                               // 26: stopping from a side-step snapped his elbows into the idle)

        using strafe_motions::Side;

        // His loops the side-steps and the back-pedal lock onto (pl00's entries, the same bytes in every costume; the
        // frame count is checked): the grid is his walk 21's (walk) or run 25's (run); 298 maps onto the run's by its
        // touchdowns (strafing-tiger.md "Step 2 plan"); stride = his loop's length / the grid's (the steps at his
        // cadence keep their speed). Past 112.5 deg the game back-pedals itself (pl00 23, K Nov 27: his back-pedal
        // stays his own; round 3 had blended a baked one on his walk / run, at their cadence).
        struct Loop
        {
            uint32_t index, frames;
            bool run, back;
            int anchors; // 0: his own grid
            float from[3], to[3];
        };
        constexpr Loop kLoops[] = {
            {21u, 57u, false, false, 0, {}, {}},
            {25u, 14u, true, false, 0, {}, {}},
            {428u, 14u, true, false, 0, {}, {}},
            {298u, 19u, true, false, 3, {14.5f, 23.5f, 32.5f}, {10.5f, 17.0f, 23.5f}},
            {23u, 43u, false, true, 0, {}, {}},
        };

        std::atomic<bool> g_setting{false};
        std::atomic<bool> g_slide{false};
        std::atomic<bool> g_sprint{false};
        std::atomic<float> g_angle{0.0f};
        // F2 (K, Nov 27; live, not saved): the side-step (0 = by Tension: 7, 16 at a full gauge; else one cell), 16's
        // arms (its swing kept at 3 / 9 o'clock; how far toward them his back-pedal's arms go at 6).
        std::atomic<int> g_walkChoice{0};
        constexpr float kTensionArmsDefault = 0.69f, kTensionBackDefault = 0.48f; // K, Nov 27
        std::atomic<float> g_tensionArms{kTensionArmsDefault}, g_tensionBack{kTensionBackDefault};
        PS2Runtime::RecompiledFunction g_inner = nullptr, g_poseInner = nullptr, g_moveInner = nullptr;
        RunStrafe g_runStrafe;
        // The run's pace (K, Nov 27): his clock's step x g_pace in moveMotion (the blend's, from the last pose; 1 = his).
        float g_pace = 1.0f, g_paceApplied = 1.0f;
        std::atomic<float> g_readPace{0.0f}, g_readSteps{0.0f}; // F2's read-out (0: not run yet)
        strafe_motions::Set g_set;
        bool g_ready = false;

        // Game thread.
        struct Forward // his loop's stance (each translation's mean) and speed, from the motion's own curves
        {
            uint32_t motion = 0u;
            float mean[strafe_motions::kTranslated][3] = {};
            float speed = 0.0f;
        } g_forward[3]; // walk, run, back-pedal
        float g_theta = 0.0f;    // the stick's angle from his facing, eased (+ = his left)
        float g_presence = 0.0f; // the blend's own weight: 1 with the stick out, fading to 0 once it's let go
        float g_tension = 0.0f;  // 16's weight in the slow strafe: eased toward 1 while the Tension gauge is full
        float g_armLeft = 1.0f, g_armTarget = 1.0f; // past 90 deg: his left side-step's share of the arms
        int g_lastRun = -1;      // the tier of the last blend (a switch fades from the pose before it)
        int64_t g_lastMs = -100000, g_stickMs = -100000; // the last blend; the last one with the stick out
        uint32_t g_loads = 0u;
        struct Track // the grid clock at the last addNullSpeed (the side's / back's root delta between two)
        {
            bool valid = false;
            uint32_t motion = 0u;
            int side = -1;
            float sigma = 0.0f;
        } g_track;
        struct Held // the last blended pose, for the stop's fade into standing
        {
            bool valid = false;
            int64_t ms = -100000;
            float euler[strafe_motions::kBones][3];
            float tr[strafe_motions::kTranslated][3];
        } g_held, g_switched; // the last blended pose; the one before a walk <-> run switch
        int64_t g_switchMs = -100000;
        constexpr uint32_t kHeat = 0x5CB000u; // cHeatSys (game_options.cpp): +4 the gauge, +0xC the full mark (floats)
        constexpr float kTensionSeconds = 0.3f;

        uint32_t rd32(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0;
            std::memcpy(&v, rdram + (a & PS2_RAM_MASK), sizeof(v));
            return v;
        }
        float rdF(const uint8_t *rdram, uint32_t a)
        {
            float v = 0.0f;
            std::memcpy(&v, rdram + (a & PS2_RAM_MASK), sizeof(v));
            return v;
        }
        void wrF(uint8_t *rdram, uint32_t a, float v) { std::memcpy(rdram + (a & PS2_RAM_MASK), &v, sizeof(v)); }
        bool inRam(uint32_t a, uint32_t size = 4u) { return a >= 0x100000u && a + size <= PS2_RDRAM_SIZE; }
        int64_t nowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }
        float wrapTo(float t, float span) { return t - std::floor(t / span) * span; }
        float wrapAngle(float a) { return a - 2.0f * kHalfTurn * std::round(a / (2.0f * kHalfTurn)); }
        bool arm(int b) { return b >= 5 && b <= 16; }

        bool isPlayer(const uint8_t *rdram, uint32_t obj)
        {
            return g_setting.load(std::memory_order_relaxed) && inRam(obj, 0x600u) && obj == rd32(rdram, kPlayerPointer);
        }

        // Gene walking, running or back-pedalling under Tiger with the stick out (or let go a moment ago), not
        // sprinting: mode 0, phase 1 / 2 / 3.
        bool moving(const uint8_t *rdram, uint32_t obj)
        {
            if (!isPlayer(rdram, obj) || g_sprint.load(std::memory_order_relaxed) ||
                (!g_slide.load(std::memory_order_relaxed) && nowMs() - g_stickMs >= kLetGoMs))
                return false;
            const uint8_t major = rdram[obj + kMajor], sub = rdram[obj + kSub];
            return major == 0u && sub >= 1u && sub <= 3u;
        }

        // Which of his loops plays (null: another motion, e.g. a start or a stop: no blend).
        const Loop *playing(const uint8_t *rdram, uint32_t obj, uint32_t &motion)
        {
            motion = rd32(rdram, obj + kMotion);
            const uint32_t pack = rd32(rdram, obj + kPack);
            if (!inRam(motion, 8u) || !inRam(pack, 8u) || !mot_sample::valid(rdram + motion, PS2_RDRAM_SIZE - motion))
                return nullptr;
            const uint32_t count = rd32(rdram, pack);
            for (const Loop &loop : kLoops)
                if (loop.index < count && pack + rd32(rdram, pack + 4u + 4u * loop.index) == motion &&
                    mot_sample::frames(rdram + motion) == loop.frames)
                    return &loop;
            return nullptr;
        }

        const Forward &forward(const uint8_t *rdram, uint32_t motion, int slot)
        {
            Forward &f = g_forward[slot];
            if (f.motion == motion)
                return f;
            const uint8_t *mot = rdram + motion;
            const size_t size = PS2_RDRAM_SIZE - motion;
            const uint32_t span = mot_sample::frames(mot) - 1u;
            for (int j = 0; j < strafe_motions::kTranslated; ++j)
                for (uint32_t c = 0; c < 3u; ++c)
                {
                    float sum = 0.0f;
                    for (uint32_t t = 0; t < span; ++t)
                    {
                        float v = 0.0f;
                        mot_sample::sample(mot, size, strafe_motions::kTranslatedBones[j], c, static_cast<float>(t), v);
                        sum += v;
                    }
                    f.mean[j][c] = sum / static_cast<float>(span);
                }
            float z0 = 0.0f, z1 = 0.0f;
            mot_sample::sample(mot, size, 255u, 2u, 0.0f, z0);
            mot_sample::sample(mot, size, 255u, 2u, static_cast<float>(span), z1);
            f.speed = std::fabs(z1 - z0) * 30.0f / static_cast<float>(span);
            f.motion = motion;
            return f;
        }

        // The grid's frame for his clock t (the lock), on [0, grid).
        float gridClock(const Loop &loop, float t, float grid)
        {
            if (loop.anchors == 0)
                return wrapTo(t, grid);
            const float span = loop.from[loop.anchors - 1] - loop.from[0];
            const float tt = wrapTo(t - loop.from[0], span) + loop.from[0];
            int k = 0;
            while (k + 2 < loop.anchors && loop.from[k + 1] <= tt)
                ++k;
            const float u = (tt - loop.from[k]) / (loop.from[k + 1] - loop.from[k]);
            return wrapTo(loop.to[k] + u * (loop.to[k + 1] - loop.to[k]), grid);
        }

        // The stick's split for this frame (eases stepped in host time). a = the eased angle from his facing. His walk
        // / run (the game's, up to the split): his loop gives way to the side-step up to 90 deg (ws = a / 90), the
        // side-step alone past it. His back-pedal (the game's, past the split): the side-step gives way to it from
        // 112.5 deg to straight back (u). The speed is split along his loop's axis and the side's (his share, the
        // side's) so planted feet stay planted (strafing-tiger.md "Feet planted on diagonals").
        struct Blend
        {
            const Loop *loop = nullptr;
            uint32_t motion = 0u;
            int side = 0; // strafe_motions::Side (side + 16 x the slow strafe's sources)
            bool left = true;
            const strafe_motions::Motion *sideMotion = nullptr; // the run's side, or the slow strafe's A (its grid)
            // The slow strafe: A (7, or the F2 cell) and B (16) per side [0 his left, 1 his right], B's weight wt;
            // B's arms keep `keep` of their swing; at 6 o'clock his back-pedal's arms go `toB6` of the way to B's.
            const strafe_motions::Motion *walkA[2] = {}, *walkB[2] = {};
            float wt = 0.0f, keep = 1.0f, toB6 = 0.0f, armLeft = 1.0f;
            bool run = false, front = true; // front: his walk / run plays; else his back-pedal
            float ws = 0.0f, u = 0.0f;  // the side's weight (his walk / run); his back-pedal's (his back-pedal)
            float his = 1.0f, sd = 0.0f; // the shares of the two swings and deltas
            float presence = 1.0f;
            float stride = 1.0f, grid = 0.0f;
            float hisSpeed = 0.0f, sideSpeed = 0.0f;
            float pace = 1.0f;     // his clock's step x this (the run's own pace toward 3 / 9)
            bool sideRun = false;  // the run's side is the side-run (its own body; arms / torso toward 7's by F2)
            float phi = 0.0f;      // the run turned toward the stick (+ = his left), the chest turned back
            const Forward *fwd = nullptr;
        };

        bool blendFor(const uint8_t *rdram, uint32_t obj, Blend &b)
        {
            if (!g_ready || !moving(rdram, obj) || !(b.loop = playing(rdram, obj, b.motion)))
                return false;
            const bool stick = g_slide.load(std::memory_order_relaxed);
            const float angle = g_angle.load(std::memory_order_relaxed);
            const int64_t now = nowMs();
            const uint32_t loads = ps2x::savestate::loadCount();
            const float dt = static_cast<float>(now - g_lastMs) / 1000.0f;
            if (now - g_lastMs > kStillMs || loads != g_loads)
            {
                g_theta = stick ? angle : 0.0f, g_presence = stick ? 1.0f : 0.0f;
                g_track.valid = false;
            }
            else
            {
                if (stick) // the shortest way round
                    g_theta = wrapAngle(g_theta + wrapAngle(angle - g_theta) * (1.0f - std::exp(-dt / kEaseSeconds)));
                g_presence += ((stick ? 1.0f : 0.0f) - g_presence) * (1.0f - std::exp(-dt / kLetGoSeconds));
            }
            g_lastMs = now, g_loads = loads;
            if (stick)
                g_stickMs = now;
            const float a = std::fabs(g_theta);
            const bool left = g_theta >= 0.0f;
            b.run = b.loop->run;
            b.front = !b.loop->back;
            b.side = b.run ? (left ? Side::RunLeft : Side::RunRight) : (left ? Side::WalkLeft : Side::WalkRight);
            b.left = left;
            b.sideMotion = &g_set.motions[b.side];
            // The Tension gauge full: 16's weight in the slow strafe eases up (K, Nov 27).
            const float gauge = rdF(rdram, kHeat + 4u), full = rdF(rdram, kHeat + 0xCu);
            const bool tense = full > 0.0f && gauge >= full;
            g_tension += ((tense ? 1.0f : 0.0f) - g_tension) * (1.0f - std::exp(-std::fmax(dt, 0.0f) / kTensionSeconds));
            {
                auto built = [](int c) { return ((g_set.walksBuilt >> c) & 1u) != 0u; };
                const int choice = g_walkChoice.load(std::memory_order_relaxed); // 0 by Tension, else 1 + the cell's index
                int a = built(strafe_motions::Cell7) ? strafe_motions::Cell7 : strafe_motions::Cell16, bsrc = strafe_motions::Cell16;
                b.wt = g_tension;
                if (choice > 0 && choice <= strafe_motions::kWalkSources)
                {
                    a = built(choice - 1) ? choice - 1 : strafe_motions::Cell16;
                    bsrc = a, b.wt = 0.0f;
                }
                const auto &grid = !b.front ? g_set.backs : b.run ? g_set.runs : g_set.walks;
                for (int k = 0; k < 2; ++k)
                    b.walkA[k] = &grid[a][k], b.walkB[k] = &grid[bsrc][k];
                b.sideMotion = b.walkA[left ? 0 : 1];
                b.side += 16 * (a + 4 * bsrc) + (b.front ? 0 : 256); // the delta's track restarts on a switch
                b.keep = bsrc == strafe_motions::Cell16 ? g_tensionArms.load(std::memory_order_relaxed) : 1.0f;
                b.toB6 = b.wt * g_tensionBack.load(std::memory_order_relaxed);
            }
            // The run's sideways stride at 3 / 9 (K, Nov 27): the side-run.
            const RunStrafe &rs = g_runStrafe;
            b.sideRun = b.front && b.run && g_set.sideRunBuilt && rs.legs.load(std::memory_order_relaxed) == 0;
            if (b.sideRun)
            {
                b.sideMotion = &g_set.sideRun[left ? 0 : 1];
                b.side += 1024;
            }
            // The arms' side in his back-pedal: kept from 150 deg round to straight back (a 6 o'clock stick wobbling
            // across doesn't flip it; averaging the two mirrored swings cancelled them, K Nov 27), eased on a switch.
            if (a <= 150.0f * kHalfTurn / 180.0f)
                g_armTarget = left ? 1.0f : 0.0f;
            g_armLeft += (g_armTarget - g_armLeft) * (1.0f - std::exp(-std::fmax(dt, 0.0f) / kArmSideSeconds));
            b.armLeft = g_armLeft;
            b.ws = b.front ? std::fmin(a / kQuarterTurn, 1.0f) : 1.0f;
            // The run (K, Nov 27): his own run turned toward the stick up to the F2 angle (40 deg), the side-step
            // taking over from there to 3 / 9.
            float turnMax = 0.0f;
            if (b.front && b.run)
            {
                turnMax = std::fmin(std::fmax(rs.turn.load(std::memory_order_relaxed), 0.0f), 60.0f) * kHalfTurn / 180.0f;
                b.phi = std::fmin(a, turnMax) * (left ? 1.0f : -1.0f);
                b.ws = std::fmin(std::fmax((a - turnMax) / (kQuarterTurn - turnMax), 0.0f), 1.0f);
            }
            b.u = b.front ? 0.0f : std::fmin(std::fmax((a - kBackFrom) / (kHalfTurn - kBackFrom), 0.0f), 1.0f);
            b.presence = g_presence;
            b.fwd = &forward(rdram, b.motion, b.front ? (b.run ? 1 : 0) : 2);
            b.hisSpeed = b.fwd->speed;
            b.sideSpeed = !b.front ? kBackSideSpeed : b.run ? kRunSideSpeed : kWalkSideSpeed;
            if (b.sideRun)
                b.sideSpeed = std::fabs(b.sideMotion->travel) * 30.0f / static_cast<float>(b.sideMotion->span);
            if (!(b.hisSpeed > 0.1f) || !(b.sideSpeed > 0.01f))
                return false;
            // Speeds as he moves (a motion's speed x his clock's step, +0x444, read unscaled): in his back-pedal the
            // side's speed into his (one clock); in his walk / run a share R of his own, his clock's step x pace.
            const float step = std::fabs(rdF(rdram, obj + kMotionStep)) / g_paceApplied;
            float m;
            if (!b.front)
                m = b.sideSpeed + (b.hisSpeed - b.sideSpeed) * b.u;
            else
            {
                float r90 = b.sideSpeed / b.hisSpeed; // the walk: the side-step's own speed at 3 / 9
                if (b.run)
                {
                    if (!g_moveInner) // no hook on his clock: his pace
                        b.pace = 1.0f;
                    r90 = std::fmin(std::fmax(rs.speed.load(std::memory_order_relaxed), 0.05f), 1.0f);
                    if (g_moveInner)
                        b.pace = 1.0f + (std::fmin(std::fmax(rs.pace.load(std::memory_order_relaxed), 0.2f), 1.5f) - 1.0f) * b.ws;
                }
                // past 90 deg down to his back-pedal's own speed by the split's near edge, so it takes over at it
                const float back = step > 0.01f ? kBackPedalSpeed * kBackPedalStep / (b.hisSpeed * step) : r90;
                const float r = a <= kQuarterTurn ? 1.0f + (r90 - 1.0f) * b.ws // his own speed while only turned
                                                  : r90 + (back - r90) * std::fmin((a - kQuarterTurn) / (kBackLeave - kQuarterTurn), 1.0f);
                m = r * b.hisSpeed / b.pace; // the motions' speed at his clock (it steps x pace)
                if (b.run && step > 0.01f) // F2's read-out at 3 / 9
                {
                    const float k39 = std::fmin(std::fmax(rs.pace.load(std::memory_order_relaxed), 0.2f), 1.5f);
                    const strafe_motions::Motion &sm = *b.sideMotion;
                    g_readPace.store(sm.sourceFrames / static_cast<float>(sm.span) * step * k39 / sm.sourceRate, std::memory_order_relaxed);
                    g_readSteps.store(r90 * b.hisSpeed / (k39 * b.sideSpeed) * std::fabs(sm.travel / sm.native), std::memory_order_relaxed);
                }
            }
            // V = m (sin a, cos a) on his loop's axis (forward; his back-pedal's backward) and the side's.
            // V = m (sin a, cos a) on his loop's axis (turned by phi) and the side's: V = his h + side x
            const float vx = m * std::sin(a), vz = m * std::cos(a), turn = std::fabs(b.phi);
            const float along = b.front ? vz / std::cos(turn) : -vz;
            b.his = along / b.hisSpeed;
            b.sd = (vx - (b.front ? along * std::sin(turn) : 0.0f)) / b.sideSpeed;
            b.grid = static_cast<float>(b.sideMotion->span);
            b.stride = static_cast<float>(b.loop->frames - 1u) / b.grid;
            return true;
        }

        void writePart(uint8_t *rdram, uint32_t part, const float e[3], const mot_sample::Quat &q)
        {
            float rc[3][3], ec[3];
            mot_sample::quatMatrix(q, rc);
            if (mot_sample::eulerNear(rc, e, ec))
                std::memcpy(rdram + part + kPartRotation, ec, 12);
        }

        bool partsOf(const uint8_t *rdram, uint32_t obj, uint32_t part[strafe_motions::kBones], uint32_t anchor[strafe_motions::kTranslated])
        {
            const uint32_t parts = rd32(rdram, obj + kParts);
            if (rdram[obj + kPartCount] < strafe_motions::kBones || !inRam(parts, 4u * strafe_motions::kBones))
                return false;
            for (int i = 0; i < strafe_motions::kBones; ++i)
                if (!inRam(part[i] = rd32(rdram, parts + 4u * i), 0x10Cu))
                    return false;
            for (int j = 0; j < strafe_motions::kTranslated; ++j)
                if (!inRam(anchor[j] = rd32(rdram, part[strafe_motions::kTranslatedBones[j]] + kPartAnchor) & PS2_RAM_MASK, 12u))
                    return false;
            return true;
        }

        // Standing again right after a blend: his own pose eased in from the last blended one.
        void stopFade(uint8_t *rdram, uint32_t obj)
        {
            const int64_t since = nowMs() - g_held.ms;
            if (!g_held.valid || since >= kStopMs || rdram[obj + kMajor] != 0u || (rdram[obj + kSub] != 0u && rdram[obj + kSub] != 10u))
                return;
            uint32_t part[strafe_motions::kBones], anchor[strafe_motions::kTranslated];
            if (!partsOf(rdram, obj, part, anchor))
                return;
            const float x = 1.0f - static_cast<float>(since) / static_cast<float>(kStopMs), f = x * x * (3.0f - 2.0f * x);
            for (int i = 0; i < strafe_motions::kBones; ++i)
            {
                float e[3];
                std::memcpy(e, rdram + part[i] + kPartRotation, 12);
                if (std::isfinite(e[0]) && std::isfinite(e[1]) && std::isfinite(e[2]))
                    writePart(rdram, part[i], e, mot_sample::slerp(mot_sample::eulerQuat(e), mot_sample::eulerQuat(g_held.euler[i]), f));
            }
            for (int j = 0; j < strafe_motions::kTranslated; ++j)
            {
                float v[3];
                std::memcpy(v, rdram + anchor[j], 12);
                for (int c = 0; c < 3; ++c)
                    v[c] += (g_held.tr[j][c] - v[c]) * f;
                std::memcpy(rdram + anchor[j], v, 12);
            }
        }

        // What we changed in his parts, and from what. The pose writes only the channels his motion keys (an unkeyed
        // rotation stays as it was: Gene's hips' (part 0) in every one of his loops, native/motion.cpp "unkeyed with
        // 0x8000: 0"), so a part we wrote and the game didn't still holds ours at the next pose: put the game's back
        // first, every call (the 60 FPS hold poses three times a frame: the turned run's hips span with it).
        struct Ours
        {
            bool valid = false;
            float base[3], wrote[3];
        };
        Ours g_oursRot[strafe_motions::kBones], g_oursTr[strafe_motions::kTranslated];
        float g_beforeRot[strafe_motions::kBones][3], g_beforeTr[strafe_motions::kTranslated][3];
        bool g_beforeValid = false;

        // Right after the game's pose: ours that it left alone back to the game's, and the result kept to compare.
        void restoreOurs(uint8_t *rdram, uint32_t obj)
        {
            uint32_t part[strafe_motions::kBones], anchor[strafe_motions::kTranslated];
            g_beforeValid = partsOf(rdram, obj, part, anchor);
            if (!g_beforeValid)
                return;
            auto put = [&](Ours &o, uint32_t at, float before[3]) {
                if (o.valid && std::memcmp(rdram + at, o.wrote, 12) == 0)
                    std::memcpy(rdram + at, o.base, 12);
                o.valid = false;
                std::memcpy(before, rdram + at, 12);
            };
            for (int i = 0; i < strafe_motions::kBones; ++i)
                put(g_oursRot[i], part[i] + kPartRotation, g_beforeRot[i]);
            for (int j = 0; j < strafe_motions::kTranslated; ++j)
                put(g_oursTr[j], anchor[j], g_beforeTr[j]);
        }

        // After our writes: what differs from the game's own, and its own.
        void noteOurs(uint8_t *rdram, uint32_t obj)
        {
            uint32_t part[strafe_motions::kBones], anchor[strafe_motions::kTranslated];
            if (!g_beforeValid || !partsOf(rdram, obj, part, anchor))
                return;
            g_beforeValid = false;
            auto note = [&](Ours &o, uint32_t at, const float before[3]) {
                if (std::memcmp(rdram + at, before, 12) == 0)
                    return;
                o.valid = true;
                std::memcpy(o.base, before, 12);
                std::memcpy(o.wrote, rdram + at, 12);
            };
            for (int i = 0; i < strafe_motions::kBones; ++i)
                note(g_oursRot[i], part[i] + kPartRotation, g_beforeRot[i]);
            for (int j = 0; j < strafe_motions::kTranslated; ++j)
                note(g_oursTr[j], anchor[j], g_beforeTr[j]);
        }

        void poseGene(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t obj);

        void poseWrapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t obj = GPR_U32(ctx, 4);
            const bool pose = ctx->pc == kMotionPose;
            if (!pose || !isPlayer(rdram, obj))
            {
                g_poseInner(rdram, ctx, runtime);
                // Tiger turned off with ours still in his parts: the game's back once
                if (pose && inRam(obj, 0x600u) && obj == rd32(rdram, kPlayerPointer))
                    restoreOurs(rdram, obj), g_beforeValid = false;
                return;
            }
            g_beforeValid = false;
            poseGene(rdram, ctx, runtime, obj); // restoreOurs right after the game's pose, inside
            noteOurs(rdram, obj);
        }

        void poseGene(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t obj)
        {
            Blend b;
            if (!blendFor(rdram, obj, b))
            {
                g_pace = 1.0f;
                g_poseInner(rdram, ctx, runtime);
                restoreOurs(rdram, obj);
                if (g_ready && !g_sprint.load(std::memory_order_relaxed))
                    stopFade(rdram, obj);
                return;
            }
            g_pace = b.pace; // his clock's next step (moveMotionWrapped)
            const float t = rdF(rdram, obj + kClock);
            // Walk <-> run <-> back-pedal: from the pose before (kept before this frame's overwrites it), 200 ms.
            const int64_t now = nowMs();
            const int state = b.front ? static_cast<int>(b.run) : 2;
            if (g_lastRun >= 0 && g_lastRun != state && g_held.valid && now - g_held.ms < kStillMs)
                g_switched = g_held, g_switchMs = now;
            g_lastRun = state;
            g_poseInner(rdram, ctx, runtime); // his pose, root outputs and caches: the game's
            restoreOurs(rdram, obj);
            if (b.presence < 1e-3f || (b.front && b.ws < 1e-3f && std::fabs(b.phi) < 1e-4f && std::fabs(b.his - 1.0f) < 1e-3f) ||
                (!b.front && b.u >= 1.0f && b.toB6 < 1e-3f && std::fabs(b.his - 1.0f) < 1e-3f))
                return; // straight ahead / straight back (no Tension arms) / let go: his own, untouched
            uint32_t part[strafe_motions::kBones], anchor[strafe_motions::kTranslated];
            if (!partsOf(rdram, obj, part, anchor))
                return;
            // His run turned toward the stick (hips, legs, foot targets and the feet's turn by phi about y; spine and chest each half back,
            // so the chest, arms and head keep facing the camera), before the blend reads it.
            const float phi = b.phi * b.presence;
            float mh[strafe_motions::kTranslated][3];
            for (int j = 0; j < strafe_motions::kTranslated; ++j)
                std::memcpy(mh[j], b.fwd->mean[j], 12);
            if (std::fabs(phi) > 1e-4f)
            {
                const float c = std::cos(phi), sn = std::sin(phi);
                const mot_sample::Quat turn{std::cos(0.5f * phi), 0.0f, std::sin(0.5f * phi), 0.0f};
                const mot_sample::Quat back{std::cos(0.25f * phi), 0.0f, -std::sin(0.25f * phi), 0.0f};
                auto inv = [](const mot_sample::Quat &q) { return mot_sample::Quat{q.w, -q.x, -q.y, -q.z}; };
                auto get = [&](int i, float e[3]) {
                    std::memcpy(e, rdram + part[i] + kPartRotation, 12);
                    return std::isfinite(e[0]) && std::isfinite(e[1]) && std::isfinite(e[2]);
                };
                float e0[3], e1[3], e2[3];
                if (get(0, e0) && get(1, e1) && get(2, e2))
                {
                    const mot_sample::Quat w0 = mot_sample::mul(turn, mot_sample::eulerQuat(e0));
                    const mot_sample::Quat w1 = mot_sample::mul(back, mot_sample::mul(w0, mot_sample::eulerQuat(e1)));
                    const mot_sample::Quat w2 = mot_sample::mul(back, mot_sample::mul(w1, mot_sample::eulerQuat(e2)));
                    writePart(rdram, part[0], e0, w0);
                    writePart(rdram, part[1], e1, mot_sample::mul(inv(w0), w1));
                    writePart(rdram, part[2], e2, mot_sample::mul(inv(w1), w2));
                }
                // the feet too: the foot targets' turn is model space, not under the hips (K, Nov 27: they kept facing
                // ahead while the legs turned)
                for (int i : {20, 24})
                {
                    float e[3];
                    if (get(i, e))
                        writePart(rdram, part[i], e, mot_sample::mul(turn, mot_sample::eulerQuat(e)));
                }
                for (int j = 0; j < strafe_motions::kTranslated; ++j)
                {
                    float v[3];
                    std::memcpy(v, rdram + anchor[j], 12);
                    const float x = v[0], z = v[2], mx = mh[j][0], mz = mh[j][2];
                    v[0] = x * c + z * sn, v[2] = -x * sn + z * c;
                    mh[j][0] = mx * c + mz * sn, mh[j][2] = -mx * sn + mz * c;
                    std::memcpy(rdram + anchor[j], v, 12);
                }
            }
            const float sigma = gridClock(*b.loop, t, b.grid);
            const strafe_motions::Motion &sm = *b.sideMotion;
            uint32_t sideMask = sm.rotated;
            const int own = b.left ? 0 : 1;
            strafe_motions::Pose side, walkA[2], walkB[2];
            float ms[strafe_motions::kTranslated][3];
            { // the side-steps: A and B (16, its arms calmer) blended by the Tension weight
                for (int k = 0; k < 2; ++k)
                    if (k == own || !b.front) // his back-pedal's arms take the latched side's (both sampled)
                    {
                        strafe_motions::sample(*b.walkA[k], sigma, b.stride, walkA[k]);
                        strafe_motions::sample(*b.walkB[k], sigma, b.stride, walkB[k]);
                        for (int i = 5; i <= 16; ++i)
                            if (b.keep < 1.0f && ((b.walkB[k]->rotated >> i) & 1u))
                                walkB[k].rot[i] = mot_sample::slerp(b.walkB[k]->armMean[i], walkB[k].rot[i], b.keep);
                    }
                for (int i = 0; i < strafe_motions::kBones; ++i)
                    side.rot[i] = mot_sample::slerp(walkA[own].rot[i], walkB[own].rot[i], b.wt);
                for (int j = 0; j < strafe_motions::kTranslated; ++j)
                    for (int c = 0; c < 3; ++c)
                    {
                        side.tr[j][c] = walkA[own].tr[j][c] + (walkB[own].tr[j][c] - walkA[own].tr[j][c]) * b.wt;
                        ms[j][c] = b.walkA[own]->mean[j][c] + (b.walkB[own]->mean[j][c] - b.walkA[own]->mean[j][c]) * b.wt;
                    }
            }
            if (b.sideRun) // the side-run's whole body; its arms / torso toward cell 7's ready posture by F2 (1 = its own)
            {
                strafe_motions::Pose sr;
                strafe_motions::sample(sm, sigma, b.stride, sr);
                const strafe_motions::Motion &a7 = *b.walkA[own];
                const float arms = g_runStrafe.arms.load(std::memory_order_relaxed);
                const float torso = g_runStrafe.torso.load(std::memory_order_relaxed);
                sideMask = 0u;
                for (int i = 0; i < strafe_motions::kBones; ++i)
                {
                    const bool mapped = (sm.rotated >> i) & 1u, upper = i >= 1 && i <= 16;
                    if (!mapped) // the bones the clip doesn't map (pads, fingers): 7's
                    {
                        side.rot[i] = a7.armMean[i], sideMask |= a7.rotated & (1u << i);
                        continue;
                    }
                    side.rot[i] = sr.rot[i], sideMask |= 1u << i;
                    if (upper && ((a7.rotated >> i) & 1u))
                        side.rot[i] = mot_sample::slerp(a7.armMean[i], sr.rot[i], arm(i) ? arms : torso);
                }
                for (int j = 0; j < strafe_motions::kTranslated; ++j)
                    for (int c = 0; c < 3; ++c)
                        side.tr[j][c] = sr.tr[j][c], ms[j][c] = sm.mean[j][c];
            }
            if (b.front && b.run) // the run's hips: lowered (reach for the longer steps) and their bounce scaled
            {
                const float drop = 0.01f * g_runStrafe.hips.load(std::memory_order_relaxed);
                side.tr[0][1] = ms[0][1] - drop + g_runStrafe.bounce.load(std::memory_order_relaxed) * (side.tr[0][1] - ms[0][1]);
            }
            for (int i = 0; i < strafe_motions::kBones; ++i)
            {
                float e[3];
                std::memcpy(e, rdram + part[i] + kPartRotation, 12);
                if (!std::isfinite(e[0]) || !std::isfinite(e[1]) || !std::isfinite(e[2]))
                    continue;
                const mot_sample::Quat qg = mot_sample::eulerQuat(e);
                const bool hasSide = (sideMask >> i) & 1u;
                if (!b.front && b.u >= 1.0f && !arm(i) && b.presence >= 1.0f)
                {
                    std::memcpy(g_held.euler[i], e, 12); // straight back: his own
                    continue;
                }
                mot_sample::Quat q;
                if (b.front)
                    q = mot_sample::slerp(qg, hasSide ? side.rot[i] : qg, b.ws);
                else if (!arm(i))
                    q = hasSide ? mot_sample::slerp(side.rot[i], qg, b.u) : qg;
                else
                {
                    // The arms: the side-step's (his left's or right's, kept past 150 deg, eased on a switch) toward
                    // his back-pedal's own, sooner than the legs (1 - (1 - u)^2); at full Tension 16's stay in by toB6
                    // at 6 o'clock (K, Nov 27: a bit of 16's hand motion, at any deflection).
                    const mot_sample::Quat mixL = mot_sample::slerp(walkA[0].rot[i], walkB[0].rot[i], b.wt);
                    const mot_sample::Quat mixR = mot_sample::slerp(walkA[1].rot[i], walkB[1].rot[i], b.wt);
                    const mot_sample::Quat qa = hasSide ? mot_sample::slerp(mixR, mixL, b.armLeft) : qg;
                    const float soon = 1.0f - (1.0f - b.u) * (1.0f - b.u), toHis = soon * (1.0f - b.toB6);
                    q = mot_sample::slerp(qa, qg, toHis);
                }
                if (b.presence < 1.0f)
                    q = mot_sample::slerp(qg, q, b.presence);
                writePart(rdram, part[i], e, q);
                std::memcpy(g_held.euler[i], rdram + part[i] + kPartRotation, 12);
            }
            // The hips and foot targets: the stances lerped (his loop's and the side's), their swings in the floor's
            // plane by the two shares, heights lerped.
            for (int j = 0; j < strafe_motions::kTranslated; ++j)
            {
                float v[3];
                std::memcpy(v, rdram + anchor[j], 12);
                if (!b.front && b.u >= 1.0f && std::fabs(b.his - 1.0f) < 1e-3f && std::fabs(b.sd) < 1e-3f)
                {
                    std::memcpy(g_held.tr[j], v, 12); // straight back: his own
                    continue;
                }
                const float *mside = ms[j], *ts = side.tr[j];
                const float wSide = b.front ? b.ws : 1.0f - b.u; // the side's share of the stance and height
                float out[3];
                for (int c = 0; c < 3; c += 2)
                {
                    const float stance = (1.0f - wSide) * mh[j][c] + wSide * mside[c];
                    out[c] = stance + b.his * (v[c] - mh[j][c]) + b.sd * (ts[c] - mside[c]);
                }
                out[1] = (1.0f - wSide) * v[1] + wSide * ts[1];
                for (int c = 0; c < 3; ++c)
                    out[c] = v[c] + (out[c] - v[c]) * b.presence;
                std::memcpy(rdram + anchor[j], out, 12);
                std::memcpy(g_held.tr[j], out, 12);
            }
            if (now - g_switchMs < kStopMs)
            {
                const float x = 1.0f - static_cast<float>(now - g_switchMs) / static_cast<float>(kStopMs), f = x * x * (3.0f - 2.0f * x);
                for (int i = 0; i < strafe_motions::kBones; ++i)
                {
                    float e[3];
                    std::memcpy(e, rdram + part[i] + kPartRotation, 12);
                    writePart(rdram, part[i], e, mot_sample::slerp(mot_sample::eulerQuat(e), mot_sample::eulerQuat(g_switched.euler[i]), f));
                    std::memcpy(g_held.euler[i], rdram + part[i] + kPartRotation, 12);
                }
            }
            g_held.valid = true;
            g_held.ms = now;
        }

        // Step 1's slide (his loop not one the motions lock onto, e.g. a start): his delta turned by the stick's angle.
        void slide(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t obj)
        {
            // The yaw turns the model's +z (his facing) to (sin yaw, cos yaw) (Ry, frame_patches' eulerMatrix), so
            // turning the delta by Ry(angle) first sends him toward his yaw + angle.
            // His back-pedal (phase 3) moves him backward already: turned by the angle from straight back.
            const float angle = rdram[obj + kSub] == 3u ? wrapAngle(g_angle.load(std::memory_order_relaxed) - kHalfTurn)
                                                         : g_angle.load(std::memory_order_relaxed);
            const float c = std::cos(angle), s = std::sin(angle);
            const float x = rdF(rdram, obj + kDelta), z = rdF(rdram, obj + kDelta + 8u);
            wrF(rdram, obj + kDelta, x * c + z * s);
            wrF(rdram, obj + kDelta + 8u, -x * s + z * c);
            g_inner(rdram, ctx, runtime);
            wrF(rdram, obj + kDelta, x);
            wrF(rdram, obj + kDelta + 8u, z);
        }

        // His clock's step x the run's pace while the blend asks for one (K, Nov 27: the side-steps at their own pace
        // toward 3 / 9; everything stays locked to his clock). The step is put back after, unless moveMotion set a new
        // one (a motion change), so nothing outside moveMotion (a save state, the 60 FPS hold's record) sees it scaled.
        void moveMotionWrapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t obj = GPR_U32(ctx, 4);
            const float k = g_pace;
            if (std::fabs(k - 1.0f) < 1e-4f || !isPlayer(rdram, obj))
                return g_moveInner(rdram, ctx, runtime);
            const float v = rdF(rdram, obj + kMotionStep), scaled = v * k;
            wrF(rdram, obj + kMotionStep, scaled);
            g_paceApplied = k;
            g_moveInner(rdram, ctx, runtime);
            g_paceApplied = 1.0f;
            if (rdF(rdram, obj + kMotionStep) == scaled)
                wrF(rdram, obj + kMotionStep, v);
        }

        void addNullSpeedWrapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t obj = GPR_U32(ctx, 4);
            if (!moving(rdram, obj))
                return g_inner(rdram, ctx, runtime);
            Blend b;
            if (!blendFor(rdram, obj, b))
            {
                g_track.valid = false;
                return slide(rdram, ctx, runtime, obj);
            }
            // The side's root delta between the grid clocks of this call and the last (the step moveMotion just made).
            const float sigma = gridClock(*b.loop, rdF(rdram, obj + kClock), b.grid);
            const float x = rdF(rdram, obj + kDelta), z = rdF(rdram, obj + kDelta + 8u);
            const strafe_motions::Motion &sm = *b.sideMotion;
            float side = 0.0f;
            bool measured = false;
            if (g_track.valid && g_track.motion == b.motion && g_track.side == b.side)
            {
                float step = sigma - g_track.sigma;
                if (step < -0.5f * b.grid)
                    step += b.grid;
                if (step >= 0.0f && step < 0.5f * b.grid)
                {
                    side = strafe_motions::rootAt(sm, g_track.sigma + step, b.stride) - strafe_motions::rootAt(sm, g_track.sigma, b.stride);
                    if (b.wt > 0.0f && !b.sideRun)
                    {
                        const strafe_motions::Motion &mb2 = *b.walkB[b.left ? 0 : 1];
                        const float sideB = strafe_motions::rootAt(mb2, g_track.sigma + step, b.stride) - strafe_motions::rootAt(mb2, g_track.sigma, b.stride);
                        side += (sideB - side) * b.wt;
                    }
                    measured = true;
                }
            }
            if (!measured) // a new loop or side: his step's length at the side's speed
                side = std::fabs(z) * b.sideSpeed / b.hisSpeed * (b.left ? 1.0f : -1.0f);
            g_track = Track{true, b.motion, b.side, sigma};
            const float phi = b.phi * b.presence, c = std::cos(phi), sn = std::sin(phi);
            const float xr = x * c + z * sn, zr = -x * sn + z * c; // his step turned toward the stick
            const float nx = b.his * xr + b.sd * side, nz = b.his * zr;
            wrF(rdram, obj + kDelta, x + (nx - x) * b.presence);
            wrF(rdram, obj + kDelta + 8u, z + (nz - z) * b.presence);
            g_inner(rdram, ctx, runtime);
            wrF(rdram, obj + kDelta, x);
            wrF(rdram, obj + kDelta + 8u, z);
        }

        // A .dat's table off the disc: entry i from its offset to the next one.
        struct DatFile
        {
            std::optional<disc_files::File> file;
            std::vector<uint8_t> head;
            uint32_t count = 0u;
            std::set<uint32_t> ends;
            bool open(const char *path)
            {
                file = disc_files::find(path);
                std::vector<uint8_t> first;
                if (!file || !disc_files::readRange(*file, 0u, 4u, first))
                    return false;
                std::memcpy(&count, first.data(), 4);
                if (count == 0u || count > 0x10000u || !disc_files::readRange(*file, 4u, 4u * count, head))
                    return false;
                ends = {file->size};
                for (uint32_t i = 0; i < count; ++i)
                {
                    uint32_t o = 0;
                    std::memcpy(&o, head.data() + 4u * i, 4);
                    if (o)
                        ends.insert(o);
                }
                return true;
            }
            bool fetch(uint32_t index, std::vector<uint8_t> &bytes) const
            {
                if (index >= count)
                    return false;
                uint32_t o = 0;
                std::memcpy(&o, head.data() + 4u * index, 4);
                const auto end = ends.upper_bound(o);
                return o != 0u && end != ends.end() && disc_files::readRange(*file, o, *end - o, bytes);
            }
        };

        // The side-steps from em00.dat on the disc (strafe_motions.h);
        // false: step 1's slide only.
        bool buildSides()
        {
            DatFile em;
            if (!disc_files::open() || !em.open("em/em00.dat"))
            {
                KM_WARN(Game, "Strafing Tiger: no side-steps (em00.dat not read): the walk slides");
                return false;
            }
            std::string why;
            if (!strafe_motions::build([&](uint32_t i, std::vector<uint8_t> &bytes) { return em.fetch(i, bytes); }, g_set, why))
            {
                KM_WARN(Game, "Strafing Tiger: no side-steps (" << why << "): the walk slides");
                return false;
            }
            return true;
        }
    }

    std::atomic<bool> &setting() { return g_setting; }
    bool enabled() { return g_setting.load(std::memory_order_relaxed); }

    void setSlide(float angle)
    {
        g_angle.store(angle, std::memory_order_relaxed);
        g_slide.store(true, std::memory_order_relaxed);
    }
    void clearSlide() { g_slide.store(false, std::memory_order_relaxed); }
    void setSprint(bool sprint) { g_sprint.store(sprint, std::memory_order_relaxed); }
    std::atomic<int> &walkChoice() { return g_walkChoice; }
    RunStrafe &runStrafe() { return g_runStrafe; }
    bool sideRunBuilt() { return g_ready && g_set.sideRunBuilt; }
    void resetTuning()
    {
        g_runStrafe.reset();
        g_walkChoice = 0;
        g_tensionArms = kTensionArmsDefault;
        g_tensionBack = kTensionBackDefault;
    }
    float readPace() { return g_readPace.load(std::memory_order_relaxed); }
    float readSteps() { return g_readSteps.load(std::memory_order_relaxed); }
    std::atomic<float> &tensionArms() { return g_tensionArms; }
    std::atomic<float> &tensionBack() { return g_tensionBack; }
    bool walkSourceBuilt(int source) { return g_ready && source >= 0 && source < strafe_motions::kWalkSources && ((g_set.walksBuilt >> source) & 1u); }

    void install(PS2Runtime &runtime)
    {
        const char *env = std::getenv("KAMITE_STRAFING_TIGER");
        g_setting = env && std::string_view(env) == "1";
        if (!runtime.hasFunction(kAddNullSpeed) || !(g_inner = runtime.lookupFunction(kAddNullSpeed)) ||
            !FunctionHooks::instance().replace(kAddNullSpeed, "cObjBase_addNullSpeed (Strafing Tiger)", &addNullSpeedWrapped,
                                               ReplacementStatus::Native, "Strafing Tiger's root delta; off: the original"))
        {
            g_inner = nullptr;
            g_setting = false;
            KM_WARN(Game, "Strafing Tiger: cObjBase_addNullSpeed (0x14F928) is not hookable: off");
            return;
        }
        g_ready = buildSides();
        if (!runtime.hasFunction(kMoveMotion) || !(g_moveInner = runtime.lookupFunction(kMoveMotion)) ||
            !FunctionHooks::instance().replace(kMoveMotion, "moveMotion (Strafing Tiger run pace)", &moveMotionWrapped,
                                               ReplacementStatus::Native, "Strafing Tiger's run pace toward 3 / 9; 1 = the original"))
        {
            g_moveInner = nullptr;
            KM_WARN(Game, "Strafing Tiger: moveMotion (0x157C18) is not hookable: the run keeps his pace");
        }
        if (!runtime.hasFunction(kMotionPose) || !(g_poseInner = runtime.lookupFunction(kMotionPose)) ||
            !FunctionHooks::instance().replace(kMotionPose, "motion pose (Strafing Tiger side-steps)", &poseWrapped,
                                               ReplacementStatus::Native, "Strafing Tiger's side-steps on his walk / run, drawn"))
        {
            g_poseInner = nullptr;
            g_ready = false;
            KM_WARN(Game, "Strafing Tiger: the motion pose (0x158328) is not hookable: the walk slides");
        }
    }
}
