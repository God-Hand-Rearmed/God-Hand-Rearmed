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
        // The game's own back-pedal past this (the pad filter's split, backPedal(): wanted past 122.5 deg on the stick
        // and left under 102.5, switched as the drawn angle crosses 112.5): straight back it is his, untouched (K, Nov
        // 27).
        constexpr float kBackFrom = 112.5f * kHalfTurn / 180.0f, kBackLeave = 102.5f * kHalfTurn / 180.0f;
        constexpr float kEaseSeconds = 0.1f;   // the stick's angle eases (a KBM key's 45 deg steps; a flick across)
        // A switch across to the other side (A -> D, a flick through the centre) walking or running: no easing through
        // straight ahead (K, Nov 27: his walk, a nudge forward); the two sides cross over this, his sideways speed
        // through 0, from both angles this far out.
        constexpr float kFlipFrom = 67.5f * kHalfTurn / 180.0f;
        constexpr int64_t kFlipMs = 250;
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
        // A walk / run <-> back-pedal flip (K, Nov 27: spinning the stick): the game starts the new loop at frame 0
        // (func_002A8578's start, ELF) with a 10-frame blend toward the pose before (setMotionInfo 0x157568: mtxBind,
        // blend +0x27C; moveMotion counts +0x438 down by the step). Under Tiger the new loop starts at the step the
        // side motion was at instead (by its touchdowns), and the game's blend is cancelled (K, Nov 28: with our offset's
        // ease it held the pose for frames, the stutter out of the back-pedal; the offset is inertialized now).
        constexpr uint32_t kBlendLeft = 0x438u, kBlendLen = 0x43Cu, kRootLast = 0x320u;
        // The drawn angle (K, Nov 28: spinning the stick as other action games do, spin mode out): it follows the stick
        // at most kTurnRate (1.5 turns a second), the way the stick turns (a spin faster than that skips a lap, it
        // doesn't turn back), the shortest way once the stick slows (kRateSeconds smoothing, under a quarter of the
        // rate). A jump over kJumpFrom in a frame isn't turning: from one side to the other (A -> D, a flick through the
        // centre) the two sides cross over (kFlipFrom); over kSnapFrom otherwise (W -> S) it is there at once.
        constexpr float kTurnRate = 3.0f * kHalfTurn; // rad/s
        constexpr float kRateSeconds = 0.1f;
        constexpr float kJumpFrom = 75.0f * kHalfTurn / 180.0f, kSnapFrom = 120.0f * kHalfTurn / 180.0f;
        constexpr float kBackEnter = 122.5f * kHalfTurn / 180.0f;

        std::atomic<bool> g_setting{false};
        std::atomic<bool> g_slide{false};
        std::atomic<bool> g_sprint{false};
        std::atomic<float> g_angle{0.0f};
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
        // One step rhythm round the circle (K, Nov 28: spinning, the steps changed pace at each walk / run <->
        // back-pedal flip; his back-pedal speeds up toward the run's, K's pick): the steps a clock frame at pace 1
        // his walk / run showed last (by the side motion's touchdowns), per walk / run.
        float g_frontRate[2] = {0.0f, 0.0f};
        std::atomic<bool> g_runStick{false}; // the pad's deflection would run (the game's x^2 + y^2 >= 10000)
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
        } g_held, g_switched, g_heldPrev, g_switchedPrev; // the last blended pose; the one before a walk <-> run switch;
                                                           // each one's frame before
        int64_t g_switchMs = -100000;
        // A switch's offset (K, Nov 27: walk <-> back-pedal stuttered once: the hips and foot targets weren't faded, the
        // feet jumped 0.6 m in a frame): the pose before less the first pose after, on top of the live pose, inertialized
        // (K, Nov 28: out of the back-pedal it still stuttered: the offset's ease-in and the game's start blend held the
        // pose a few frames, the feet stopping dead; Bollo, "Inertialization", GDC 2018): it starts moving at the
        // pose's speed before the switch (toward the live pose; away from it, at 0) and is gone by kStopMs.
        struct Inert // one channel's offset: x0 at the switch, a quintic to 0 by t1 (seconds), never overshooting
        {
            float sign = 1.0f, x0 = 0.0f, v0 = 0.0f, a0 = 0.0f, a = 0.0f, b = 0.0f, c = 0.0f, t1 = 0.0f;
            void init(float x, float v, float most)
            {
                sign = x < 0.0f ? -1.0f : 1.0f, x0 = std::fabs(x), v0 = std::fmin(v * sign, 0.0f);
                t1 = v0 < 0.0f ? std::fmin(most, -5.0f * x0 / v0) : most;
                if (!(t1 > 1e-4f) || !(x0 > 1e-7f))
                {
                    t1 = 0.0f;
                    return;
                }
                a0 = std::fmax((-8.0f * v0 * t1 - 20.0f * x0) / (t1 * t1), 0.0f);
                a = -(a0 * t1 * t1 + 6.0f * v0 * t1 + 12.0f * x0) / (2.0f * t1 * t1 * t1 * t1 * t1);
                b = (3.0f * a0 * t1 * t1 + 16.0f * v0 * t1 + 30.0f * x0) / (2.0f * t1 * t1 * t1 * t1);
                c = -(3.0f * a0 * t1 * t1 + 12.0f * v0 * t1 + 20.0f * x0) / (2.0f * t1 * t1 * t1);
            }
            float at(float t) const
            {
                if (!(t < t1))
                    return 0.0f;
                return sign * ((((a * t + b) * t + c) * t + 0.5f * a0) * t * t + v0 * t + x0);
            }
        };
        struct Offset
        {
            bool set = false;
            float axis[strafe_motions::kBones][3]; // each part's offset turn: about this, by its Inert
            Inert rot[strafe_motions::kBones];
            float dir[strafe_motions::kTranslated][3]; // each translation's: along this
            Inert tr[strafe_motions::kTranslated];
        } g_offset;
        int64_t g_flipMs = -100000; // the last switch across (kFlipFrom)
        struct LastMove // his loop and clock after the last moveMotion (a flip starts the new loop in step with it)
        {
            const Loop *loop = nullptr;
            const strafe_motions::Motion *side = nullptr; // the side motion the last pose drew on it
            float clock = 0.0f;
            int64_t ms = -100000;
            uint32_t loads = 0u;
        } g_lastMove;
        // The drawn angle's state (blendFor): the way left to the stick (+ = toward his left; past half a turn while
        // a spin outruns kTurnRate), the stick's last angle and its turning rate (rad/s, smoothed).
        float g_chase = 0.0f, g_lastStick = 0.0f, g_stickRate = 0.0f, g_rateStick = 0.0f;
        int64_t g_rateMs = -100000;
        // Published for the pad filter (backPedal, the window thread): the drawn angle in the stick's frame (camera),
        // and when.
        std::atomic<float> g_offsetYaw{0.0f}; // setSlide: the stick's frame less his facing
        std::atomic<float> g_drawnStick{0.0f};
        std::atomic<int64_t> g_drawnMs{-100000};
        // A jump from one side to the other (kFlipFrom out on both): the sides cross over (blendFor; backPedal the same).
        bool across(float from, float to)
        {
            return std::fabs(from) > kFlipFrom && std::fabs(to) > kFlipFrom && (from >= 0.0f) != (to >= 0.0f);
        }
        bool g_padBack = false, g_padWant = false, g_padHave = false; // backPedal's (window thread)
        float g_padLast = 0.0f;
        const strafe_motions::Motion *g_drawnSide = nullptr; // the side motion the last blended pose drew

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
            if (!isPlayer(rdram, obj) || (g_sprint.load(std::memory_order_relaxed) && !g_runStrafe.sprintTurn.load(std::memory_order_relaxed)) ||
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

        // The step's phase at frame t of a side motion, by its touchdowns: k + the way from its k-th to the next (feet 24,
        // 20, 24, ...: even k = foot 24 down); its frame at phase p (wrapped onto its touchdowns, so a one-cycle grid takes
        // a two-cycle one's phase mod 2: the feet stay the same).
        float touchPhase(const strafe_motions::Motion &m, float t)
        {
            const std::vector<float> &r = m.touch;
            if (r.size() < 3u)
                return 0.0f;
            const float x = wrapTo(t - r.front(), r.back() - r.front()) + r.front();
            size_t k = 0;
            while (k + 2 < r.size() && r[k + 1] <= x)
                ++k;
            return static_cast<float>(k) + (x - r[k]) / (r[k + 1] - r[k]);
        }
        float touchFrame(const strafe_motions::Motion &m, float p)
        {
            const std::vector<float> &r = m.touch;
            if (r.size() < 3u)
                return 0.0f;
            p = wrapTo(p, static_cast<float>(r.size() - 1u));
            const size_t k = std::min(static_cast<size_t>(p), r.size() - 2u);
            return wrapTo(r[k] + (p - static_cast<float>(k)) * (r[k + 1] - r[k]), static_cast<float>(m.span));
        }
        // His clock for the grid's frame sigma (gridClock's inverse).
        float loopClock(const Loop &loop, float sigma)
        {
            const float span = static_cast<float>(loop.frames - 1u);
            if (loop.anchors == 0)
                return wrapTo(sigma, span);
            const float a = loop.to[0], z = loop.to[loop.anchors - 1];
            const float x = wrapTo(sigma - a, z - a) + a;
            int k = 0;
            while (k + 2 < loop.anchors && loop.to[k + 1] <= x)
                ++k;
            const float u = (x - loop.to[k]) / (loop.to[k + 1] - loop.to[k]);
            return wrapTo(loop.from[k] + u * (loop.from[k + 1] - loop.from[k]), span);
        }
        // A side motion's steps per clock frame of his loop (its touchdowns; the loop's anchors map his clock onto the
        // grid); 0 if it has none.
        float stepsPerClock(const Loop &loop, const strafe_motions::Motion &m)
        {
            const std::vector<float> &r = m.touch;
            if (r.size() < 3u || !(r.back() - r.front() > 1e-3f))
                return 0.0f;
            const float grid = loop.anchors ? (loop.to[loop.anchors - 1] - loop.to[0]) / (loop.from[loop.anchors - 1] - loop.from[0]) : 1.0f;
            return static_cast<float>(r.size() - 1u) / (r.back() - r.front()) * grid;
        }
        // The side motion the blend draws on a loop for a side (0 his left): blendFor's choice.
        const strafe_motions::Motion &sideFor(const Loop &loop, int side)
        {
            if (!loop.back && loop.run && g_set.sideRunBuilt && g_runStrafe.legs.load(std::memory_order_relaxed) == 0)
                return g_set.sideRun[g_runStrafe.flip.load(std::memory_order_relaxed) ? 1 : 0][side];
            const int a = ((g_set.walksBuilt >> strafe_motions::Cell7) & 1u) ? strafe_motions::Cell7 : strafe_motions::Cell16;
            return (loop.back ? g_set.backs : loop.run ? g_set.runs : g_set.walks)[a][side];
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
            int side = 0; // strafe_motions::Side (+ 256 his back-pedal's, + 1024 the side-run's)
            bool left = true;
            const strafe_motions::Motion *sideMotion = nullptr; // the run's side-run, or the slow strafe (its grid)
            // The slow strafe (K, Nov 27: 7 at any Tension; 16 is out) per side [0 his left, 1 his right]; armLeft: his
            // left's share of the arms in his back-pedal.
            const strafe_motions::Motion *walkA[2] = {};
            float armLeft = 1.0f;
            bool run = false, front = true; // front: his walk / run plays; else his back-pedal
            float ws = 0.0f, u = 0.0f;  // the side's weight (his walk / run); his back-pedal's (his back-pedal)
            float his = 1.0f, sd = 0.0f; // the shares of the two swings and deltas
            float presence = 1.0f;
            float stride = 1.0f, grid = 0.0f;
            float hisSpeed = 0.0f, sideSpeed = 0.0f;
            float pace = 1.0f;     // his clock's step x this (the run's own pace toward 3 / 9)
            bool sideRun = false;  // the run's side is the side-run (its own body; arms / torso toward 7's by F2)
            float phi = 0.0f;      // the run turned toward the stick (+ = his left), the chest turned back
            // A switch across (kFlipFrom): mix the new side's share of the side pose (0 .. 1, eased), cross = 2 mix - 1
            // on the side's delta and on phi (the side before at -1).
            float mix = 1.0f, cross = 1.0f;
            const strafe_motions::Motion *sides[2] = {}; // the side's motion per side [0 his left, 1 his right]
            const Forward *fwd = nullptr;
        };

        bool blendFor(const uint8_t *rdram, uint32_t obj, Blend &b)
        {
            if (!g_ready || !moving(rdram, obj) || !(b.loop = playing(rdram, obj, b.motion)))
                return false;
            const bool stick = g_slide.load(std::memory_order_relaxed), sprint = g_sprint.load(std::memory_order_relaxed);
            const float angle = g_angle.load(std::memory_order_relaxed);
            const int64_t now = nowMs();
            const uint32_t loads = ps2x::savestate::loadCount();
            const float dt = static_cast<float>(now - g_lastMs) / 1000.0f;
            if (now - g_lastMs > kStillMs || loads != g_loads)
            {
                g_theta = stick && !sprint ? angle : 0.0f, g_presence = stick ? 1.0f : 0.0f; // a sprint eases into the turn
                g_track.valid = false;
                g_flipMs = -100000;
                g_chase = wrapAngle(angle - g_theta), g_lastStick = g_rateStick = angle, g_stickRate = 0.0f, g_rateMs = now;
            }
            else
            {
                if (stick)
                {
                    // the stick's turning rate (once a frame: the pose runs up to three times one); a jump isn't turning
                    const float rdt = static_cast<float>(now - g_rateMs) / 1000.0f;
                    if (rdt >= 0.008f)
                    {
                        const float rs = wrapAngle(angle - g_rateStick);
                        g_stickRate = std::fabs(rs) > kJumpFrom || rdt > 0.1f
                                          ? 0.0f
                                          : g_stickRate + (rs / rdt - g_stickRate) * (1.0f - std::exp(-rdt / kRateSeconds));
                        g_rateStick = angle, g_rateMs = now;
                    }
                    const float step = wrapAngle(angle - g_lastStick), to = wrapAngle(angle - g_theta);
                    if (std::fabs(step) <= kJumpFrom)
                    {
                        g_chase += step; // the way the stick turns (kept a whole number of turns off the stick)
                        g_chase = to + 2.0f * kHalfTurn * std::round((g_chase - to) / (2.0f * kHalfTurn));
                        if (std::fabs(g_chase) > 2.0f * kHalfTurn) // a spin outrunning kTurnRate: a lap skipped
                            g_chase -= std::copysign(2.0f * kHalfTurn, g_chase);
                        if (std::fabs(g_chase) > kHalfTurn && std::fabs(g_stickRate) < 0.25f * kTurnRate)
                            g_chase = to; // the stick slowed: the shortest way
                    }
                    else if (!b.loop->back && across(g_theta, angle))
                    {
                        // across to the other side: the same angle on it, the poses and speeds crossing over (a switch
                        // back before it's done turns back from where it got to)
                        const float x = std::fmin(static_cast<float>(now - g_flipMs) / static_cast<float>(kFlipMs), 1.0f);
                        g_theta = -g_theta;
                        g_flipMs = now - static_cast<int64_t>(std::lround((1.0f - x) * static_cast<float>(kFlipMs)));
                        g_chase = wrapAngle(angle - g_theta);
                    }
                    else if (std::fabs(step) > kSnapFrom) // W -> S: there at once (backPedal judges the stick too)
                        g_theta = angle, g_chase = 0.0f;
                    else
                        g_chase = to;
                    g_lastStick = angle;
                    const float most = kTurnRate * std::fmax(dt, 0.0f);
                    const float move = std::fmin(std::fmax(g_chase * (1.0f - std::exp(-dt / kEaseSeconds)), -most), most);
                    g_theta = wrapAngle(g_theta + move);
                    g_chase -= move;
                }
                g_presence += ((stick ? 1.0f : 0.0f) - g_presence) * (1.0f - std::exp(-dt / kLetGoSeconds));
            }
            if (sprint) // sprinting (K, Nov 27): only his run turned toward the stick, no side-steps
            {
                const float most = std::fmin(std::fmax(g_runStrafe.turn.load(std::memory_order_relaxed), 0.0f), 60.0f) * kHalfTurn / 180.0f;
                g_theta = std::fmin(std::fmax(g_theta, -most), most);
                g_chase = wrapAngle(angle - g_theta);
                g_flipMs = -100000;
            }
            else if (stick) // the pad filter's split
            {
                g_drawnStick.store(wrapAngle(g_offsetYaw.load(std::memory_order_relaxed) - g_theta), std::memory_order_relaxed);
                g_drawnMs.store(now, std::memory_order_relaxed);
            }
            g_lastMs = now, g_loads = loads;
            if (stick)
                g_stickMs = now;
            if (b.loop->back)
                g_flipMs = -100000; // his back-pedal: the angle eases round through straight back
            {
                const float x = std::fmin(std::fmax(static_cast<float>(now - g_flipMs) / static_cast<float>(kFlipMs), 0.0f), 1.0f);
                b.mix = x * x * (3.0f - 2.0f * x);
                b.cross = 2.0f * b.mix - 1.0f;
            }
            const float a = std::fabs(g_theta);
            const bool left = g_theta >= 0.0f;
            b.run = b.loop->run;
            b.front = !b.loop->back;
            b.side = b.run ? (left ? Side::RunLeft : Side::RunRight) : (left ? Side::WalkLeft : Side::WalkRight);
            b.left = left;
            b.sideMotion = &g_set.motions[b.side];
            { // 7 (16 if this disc's 7 didn't build: the recipe's base) on his walk's, run's or back-pedal's grid
                const int a = ((g_set.walksBuilt >> strafe_motions::Cell7) & 1u) ? strafe_motions::Cell7 : strafe_motions::Cell16;
                const auto &grid = !b.front ? g_set.backs : b.run ? g_set.runs : g_set.walks;
                for (int k = 0; k < 2; ++k)
                    b.walkA[k] = &grid[a][k];
                b.sideMotion = b.walkA[left ? 0 : 1];
                b.sides[0] = b.walkA[0], b.sides[1] = b.walkA[1];
                b.side += 16 * a + (b.front ? 0 : 256); // the delta's track restarts on a switch
            }
            // The run's sideways stride at 3 / 9 (K, Nov 27): the side-run.
            const RunStrafe &rs = g_runStrafe;
            b.sideRun = b.front && b.run && g_set.sideRunBuilt && rs.legs.load(std::memory_order_relaxed) == 0;
            if (b.sideRun)
            {
                const int f = rs.flip.load(std::memory_order_relaxed) ? 1 : 0;
                b.sides[0] = &g_set.sideRun[f][0], b.sides[1] = &g_set.sideRun[f][1];
                b.sideMotion = b.sides[left ? 0 : 1];
                b.side += 1024 + (rs.flip.load(std::memory_order_relaxed) ? 2048 : 0); // the delta's track restarts on a switch
            }
            // The arms' side in his back-pedal: the side drawn (K, Nov 28: spinning, the arms should sweep round with
            // the stick; the old latch past 150 deg, eased over 0.25 s, held the other side's arms for a third of a
            // turn). Straight back the side-step's share of the arms is 0 ((1 - u)^2), so a 6 o'clock stick wobbling
            // across moves them by next to nothing (averaging the two mirrored swings cancelled them, K Nov 27).
            b.armLeft = left ? 1.0f : 0.0f;
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
            {
                // his back-pedal's steps at the rhythm of the walk / run the stick would give, by the split, his own
                // straight back; the speed kept (the motions' speed at his clock, it steps x pace)
                const float own = stepsPerClock(*b.loop, *b.sideMotion) * step, to = g_frontRate[g_runStick.load(std::memory_order_relaxed) ? 1 : 0];
                if (g_moveInner && own > 1e-6f && to > 1e-6f)
                {
                    const float w = std::fmin(std::fmax((kHalfTurn - a) / (kHalfTurn - kBackFrom), 0.0f), 1.0f);
                    b.pace = 1.0f + (std::fmin(std::fmax(to / own, 0.5f), 3.0f) - 1.0f) * w;
                }
                m = (b.sideSpeed + (b.hisSpeed - b.sideSpeed) * b.u) / b.pace;
            }
            else
            {
                float r90 = b.sideSpeed / b.hisSpeed; // the walk: the side-step's own speed at 3 / 9
                if (!b.run && g_moveInner) // ... its steps quicker and shorter by the F2 walk pace (K, Nov 27)
                    b.pace = 1.0f + (std::fmin(std::fmax(rs.walkPace.load(std::memory_order_relaxed), 0.5f), 2.0f) - 1.0f) * b.ws;
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
                g_frontRate[b.run ? 1 : 0] = stepsPerClock(*b.loop, *b.sideMotion) * step * b.pace;
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
                g_drawnSide = nullptr;
                g_poseInner(rdram, ctx, runtime);
                restoreOurs(rdram, obj);
                if (g_ready && !g_sprint.load(std::memory_order_relaxed))
                    stopFade(rdram, obj);
                return;
            }
            g_pace = b.pace; // his clock's next step (moveMotionWrapped)
            g_drawnSide = b.sideMotion;
            const float t = rdF(rdram, obj + kClock);
            // Walk <-> run <-> back-pedal: from the pose before (kept before this frame's overwrites it), 200 ms.
            const int64_t now = nowMs();
            const int state = b.front ? static_cast<int>(b.run) : 2;
            if (g_lastRun >= 0 && g_lastRun != state && g_held.valid && now - g_held.ms < kStillMs)
                g_switched = g_held, g_switchedPrev = g_heldPrev, g_switchMs = g_held.ms, g_offset.set = false; // from the pose before's time: this frame shows it a frame on
            g_lastRun = state;
            if (g_held.valid && now - g_held.ms >= 8) // a new frame (the pose runs up to three times one)
                g_heldPrev = g_held;
            g_poseInner(rdram, ctx, runtime); // his pose, root outputs and caches: the game's
            restoreOurs(rdram, obj);
            if (b.presence < 1e-3f || (b.front && b.ws < 1e-3f && std::fabs(b.phi) < 1e-4f && std::fabs(b.his - 1.0f) < 1e-3f) ||
                (!b.front && b.u >= 1.0f && std::fabs(b.his - 1.0f) < 1e-3f))
            {
                g_held.valid = false; // his own: a switch from here is the game's
                return; // straight ahead / straight back / let go: his own, untouched
            }
            uint32_t part[strafe_motions::kBones], anchor[strafe_motions::kTranslated];
            if (!partsOf(rdram, obj, part, anchor))
                return;
            // His run turned toward the stick (hips, legs, foot targets and the feet's turn by phi about y; spine and chest each half back,
            // so the chest, arms and head keep facing the camera), before the blend reads it.
            const float phi = b.phi * b.cross * b.presence;
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
            const int own = b.left ? 0 : 1;
            const bool crossing = b.front && b.mix < 1.0f; // a switch across: the side before still in
            uint32_t sideMask = 0u;
            strafe_motions::Pose side, walkA[2];
            float ms[strafe_motions::kTranslated][3];
            for (int k = 0; k < 2; ++k)
                if (k == own || !b.front || crossing) // his back-pedal's arms take the latched side's (both sampled)
                    strafe_motions::sample(*b.walkA[k], sigma, b.stride, walkA[k]);
            // One side's pose:            // One side's pose: the side-step (7); the run's 3 / 9 the side-run's whole body, its arms / torso toward 7's
            // ready posture by F2 (1 = its own).
            auto sidePose = [&](int k, strafe_motions::Pose &out, float mo[][3], uint32_t &mask) {
                mask = b.sides[k]->rotated;
                out = walkA[k];
                for (int j = 0; j < strafe_motions::kTranslated; ++j)
                    for (int c = 0; c < 3; ++c)
                        mo[j][c] = b.walkA[k]->mean[j][c];
                if (!b.sideRun)
                    return;
                const strafe_motions::Motion &sm = *b.sides[k], &a7 = *b.walkA[k];
                strafe_motions::Pose sr;
                strafe_motions::sample(sm, sigma, b.stride, sr);
                const float arms = g_runStrafe.arms.load(std::memory_order_relaxed);
                const float torso = g_runStrafe.torso.load(std::memory_order_relaxed);
                mask = 0u;
                for (int i = 0; i < strafe_motions::kBones; ++i)
                {
                    const bool mapped = (sm.rotated >> i) & 1u, upper = i >= 1 && i <= 16;
                    if (!mapped) // the bones the clip doesn't map (pads, fingers): 7's
                    {
                        out.rot[i] = a7.armMean[i], mask |= a7.rotated & (1u << i);
                        continue;
                    }
                    out.rot[i] = sr.rot[i], mask |= 1u << i;
                    if (upper && ((a7.rotated >> i) & 1u))
                        out.rot[i] = mot_sample::slerp(a7.armMean[i], sr.rot[i], arm(i) ? arms : torso);
                }
                for (int j = 0; j < strafe_motions::kTranslated; ++j)
                    for (int c = 0; c < 3; ++c)
                        out.tr[j][c] = sr.tr[j][c], mo[j][c] = sm.mean[j][c];
            };
            sidePose(own, side, ms, sideMask);
            if (crossing) // the side before giving way (K, Nov 27: A -> D had dropped into his idle a moment)
            {
                strafe_motions::Pose before;
                float mb[strafe_motions::kTranslated][3];
                uint32_t maskBefore = 0u;
                sidePose(1 - own, before, mb, maskBefore);
                for (int i = 0; i < strafe_motions::kBones; ++i)
                {
                    const bool inNew = (sideMask >> i) & 1u, inBefore = (maskBefore >> i) & 1u;
                    if (inNew && inBefore)
                        side.rot[i] = mot_sample::slerp(before.rot[i], side.rot[i], b.mix);
                    else if (inBefore)
                        side.rot[i] = before.rot[i];
                }
                sideMask |= maskBefore;
                for (int j = 0; j < strafe_motions::kTranslated; ++j)
                    for (int c = 0; c < 3; ++c)
                    {
                        side.tr[j][c] = mb[j][c] + (ms[j][c] - mb[j][c]) * b.mix + (side.tr[j][c] - ms[j][c]) * b.mix +
                                        (before.tr[j][c] - mb[j][c]) * (1.0f - b.mix);
                        ms[j][c] = mb[j][c] + (ms[j][c] - mb[j][c]) * b.mix;
                    }
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
                    // his back-pedal's own, sooner than the legs (1 - (1 - u)^2).
                    const mot_sample::Quat qa = hasSide ? mot_sample::slerp(walkA[1].rot[i], walkA[0].rot[i], b.armLeft) : qg;
                    q = mot_sample::slerp(qa, qg, 1.0f - (1.0f - b.u) * (1.0f - b.u));
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
                auto inv = [](const mot_sample::Quat &q) { return mot_sample::Quat{q.w, -q.x, -q.y, -q.z}; };
                if (!g_offset.set)
                {
                    // the speed before: the pose a frame before the switch, as an offset from this first pose after
                    const float dtPrev = static_cast<float>(g_switched.ms - g_switchedPrev.ms) / 1000.0f;
                    const bool speed = g_switchedPrev.valid && dtPrev > 0.004f && dtPrev < 0.1f;
                    const float most = static_cast<float>(kStopMs) / 1000.0f;
                    for (int i = 0; i < strafe_motions::kBones; ++i)
                    {
                        float e[3];
                        std::memcpy(e, rdram + part[i] + kPartRotation, 12);
                        const mot_sample::Quat live = inv(mot_sample::eulerQuat(e));
                        mot_sample::Quat q = mot_sample::mul(mot_sample::eulerQuat(g_switched.euler[i]), live);
                        if (q.w < 0.0f)
                            q = {-q.w, -q.x, -q.y, -q.z};
                        const float s = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
                        const float angle = 2.0f * std::atan2(s, q.w);
                        float *ax = g_offset.axis[i];
                        if (!(s > 1e-6f))
                        {
                            ax[0] = 1.0f, ax[1] = ax[2] = 0.0f, g_offset.rot[i].init(0.0f, 0.0f, most);
                            continue;
                        }
                        ax[0] = q.x / s, ax[1] = q.y / s, ax[2] = q.z / s;
                        float v = 0.0f;
                        if (speed)
                        {
                            const mot_sample::Quat p = mot_sample::mul(mot_sample::eulerQuat(g_switchedPrev.euler[i]), live);
                            const float along = 2.0f * std::atan2((p.x * ax[0] + p.y * ax[1] + p.z * ax[2]) * (p.w < 0.0f ? -1.0f : 1.0f), std::fabs(p.w));
                            v = (angle - along) / dtPrev;
                        }
                        g_offset.rot[i].init(angle, v, most);
                    }
                    for (int j = 0; j < strafe_motions::kTranslated; ++j)
                    {
                        float x[3], len = 0.0f;
                        for (int c = 0; c < 3; ++c)
                            x[c] = g_switched.tr[j][c] - rdF(rdram, anchor[j] + 4u * static_cast<uint32_t>(c)), len += x[c] * x[c];
                        len = std::sqrt(len);
                        float *d = g_offset.dir[j];
                        if (!(len > 1e-7f))
                        {
                            d[0] = 1.0f, d[1] = d[2] = 0.0f, g_offset.tr[j].init(0.0f, 0.0f, most);
                            continue;
                        }
                        float along = 0.0f;
                        for (int c = 0; c < 3; ++c)
                            d[c] = x[c] / len, along += (g_switchedPrev.tr[j][c] - rdF(rdram, anchor[j] + 4u * static_cast<uint32_t>(c))) * d[c];
                        g_offset.tr[j].init(len, speed ? (len - along) / dtPrev : 0.0f, most);
                    }
                    g_offset.set = true;
                }
                const float since = static_cast<float>(now - g_switchMs) / 1000.0f;
                for (int i = 0; i < strafe_motions::kBones; ++i)
                {
                    float e[3];
                    std::memcpy(e, rdram + part[i] + kPartRotation, 12);
                    const float h = 0.5f * g_offset.rot[i].at(since), sn = std::sin(h);
                    const float *ax = g_offset.axis[i];
                    const mot_sample::Quat turn{std::cos(h), ax[0] * sn, ax[1] * sn, ax[2] * sn};
                    writePart(rdram, part[i], e, mot_sample::mul(turn, mot_sample::eulerQuat(e)));
                    std::memcpy(g_held.euler[i], rdram + part[i] + kPartRotation, 12);
                }
                for (int j = 0; j < strafe_motions::kTranslated; ++j)
                {
                    float v[3];
                    std::memcpy(v, rdram + anchor[j], 12);
                    const float x = g_offset.tr[j].at(since);
                    for (int c = 0; c < 3; ++c)
                        v[c] += g_offset.dir[j][c] * x;
                    std::memcpy(rdram + anchor[j], v, 12);
                    std::memcpy(g_held.tr[j], v, 12);
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

        // Before moveMotion steps: a walk / run <-> back-pedal flip that setMotionInfo has just started (its blend still
        // whole) starts at the step the last pose's side motion was at.
        // His clock moved to t: the root delta's base too (K, Nov 27: he teleported at the flips; moveMotion's delta is
        // the pose's root +0x310 less the last one +0x320, which setMotionInfo zeroes: from frame 0 to t in one frame).
        // The root's x is negated in a mirrored motion (+0x434 bit 1, as the pose).
        void setClock(uint8_t *rdram, uint32_t obj, uint32_t motion, float t)
        {
            wrF(rdram, obj + kClock, t);
            uint16_t flags = 0;
            std::memcpy(&flags, rdram + ((obj + 0x434u) & PS2_RAM_MASK), 2);
            for (uint32_t c = 0; c < 3u; ++c)
            {
                float v = 0.0f;
                mot_sample::sample(rdram + motion, PS2_RDRAM_SIZE - motion, 255u, c, t, v);
                wrF(rdram, obj + kRootLast + 4u * c, c == 0u && ((flags >> 1) & 1u) ? -v : v);
            }
        }

        void handOver(uint8_t *rdram, uint32_t obj)
        {
            uint32_t motion = 0u;
            const Loop *loop = g_ready && moving(rdram, obj) ? playing(rdram, obj, motion) : nullptr;
            if (!loop)
                return;
            const LastMove &last = g_lastMove;
            const strafe_motions::Motion &next = sideFor(*loop, g_theta >= 0.0f ? 0 : 1);
            const float left = rdF(rdram, obj + kBlendLeft);
            if (last.loop && last.side && last.loop->back != loop->back && last.loads == ps2x::savestate::loadCount() &&
                nowMs() - last.ms < kStillMs && left > 0.0f && left == rdF(rdram, obj + kBlendLen))
            {
                const float was = touchPhase(*last.side, gridClock(*last.loop, last.clock, static_cast<float>(last.side->span)));
                setClock(rdram, obj, motion, loopClock(*loop, touchFrame(next, was)));
                wrF(rdram, obj + kBlendLeft, 0.0f); // the game's start blend held the pose at the flip (K, Nov 28): ours
            }
        }
        void noteMove(const uint8_t *rdram, uint32_t obj)
        {
            uint32_t motion = 0u;
            g_lastMove = LastMove{g_ready ? playing(rdram, obj, motion) : nullptr, g_drawnSide, rdF(rdram, obj + kClock), nowMs(), ps2x::savestate::loadCount()};
        }

        // His clock's step x the run's pace while the blend asks for one (K, Nov 27: the side-steps at their own pace
        // toward 3 / 9; everything stays locked to his clock). The step is put back after, unless moveMotion set a new
        // one (a motion change), so nothing outside moveMotion (a save state, the 60 FPS hold's record) sees it scaled.
        void moveMotionWrapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t obj = GPR_U32(ctx, 4);
            if (!isPlayer(rdram, obj))
                return g_moveInner(rdram, ctx, runtime);
            handOver(rdram, obj);
            const float k = g_pace;
            if (std::fabs(k - 1.0f) < 1e-4f)
            {
                g_moveInner(rdram, ctx, runtime);
                return noteMove(rdram, obj);
            }
            const float v = rdF(rdram, obj + kMotionStep), scaled = v * k;
            wrF(rdram, obj + kMotionStep, scaled);
            g_paceApplied = k;
            g_moveInner(rdram, ctx, runtime);
            g_paceApplied = 1.0f;
            if (rdF(rdram, obj + kMotionStep) == scaled)
                wrF(rdram, obj + kMotionStep, v);
            noteMove(rdram, obj);
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
                if (g_sprint.load(std::memory_order_relaxed))
                    return g_inner(rdram, ctx, runtime); // sprinting outside his run loop: straight, as the game's
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
                    measured = true;
                }
            }
            if (!measured) // a new loop or side: his step's length at the side's speed
                side = std::fabs(z) * b.sideSpeed / b.hisSpeed * (b.left ? 1.0f : -1.0f);
            g_track = Track{true, b.motion, b.side, sigma};
            side *= b.cross; // a switch across: the side before's way still, through 0, to the new side's
            const float phi = b.phi * b.cross * b.presence, c = std::cos(phi), sn = std::sin(phi);
            const float xr = x * c + z * sn, zr = -x * sn + z * c; // his step turned toward the stick
            float nx = b.his * xr + b.sd * side, nz = b.his * zr;
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

    void setSlide(float angle, float offset)
    {
        g_angle.store(angle, std::memory_order_relaxed);
        g_offsetYaw.store(offset, std::memory_order_relaxed);
        g_slide.store(true, std::memory_order_relaxed);
    }
    // The pad filter's split (window thread): his back-pedal wanted past kBackEnter on the stick, his walk / run again
    // under kBackLeave (the stickiness: K, Nov 26), and switched to as the angle the blend draws crosses kBackFrom, or
    // the stick's own edges whatever the stick (K, Nov 28: on the stick alone, spinning it flipped him ahead of the
    // pose, the flips landing far from the split, and a stick flicked across from 9 to 3 that read behind him on the way
    // flipped him for a frame); at once with no blend lately or on a jump over kSnapFrom (W -> S: the blend is there at
    // once too).
    void setRunStick(bool run) { g_runStick.store(run, std::memory_order_relaxed); }
    bool backPedal(float stick, bool sprinting)
    {
        if (sprinting)
            return g_padBack = g_padWant = g_padHave = false;
        g_padWant = std::fabs(stick) > (g_padWant ? kBackLeave : kBackEnter);
        const float signedDrawn = g_drawnStick.load(std::memory_order_relaxed), drawn = std::fabs(signedDrawn);
        const bool stale = nowMs() - g_drawnMs.load(std::memory_order_relaxed) >= kStillMs;
        // a jump the blend snaps to (one across while walking / running crosses over instead: judged on the drawn)
        const bool jump = !g_padHave || (std::fabs(wrapAngle(stick - g_padLast)) > kSnapFrom && (g_padBack || stale || !across(signedDrawn, stick)));
        g_padLast = stick, g_padHave = true;
        if (jump || stale)
            g_padBack = g_padWant;
        else if (g_padBack) // the drawn angle past the stick's own edges too (a spin outrunning kTurnRate)
            g_padBack = !((!g_padWant && drawn < kBackFrom) || drawn < kBackLeave);
        else
            g_padBack = (g_padWant && drawn > kBackFrom) || drawn > kBackEnter;
        return g_padBack;
    }
    void clearSlide() { g_slide.store(false, std::memory_order_relaxed); }
    void setSprint(bool sprint) { g_sprint.store(sprint, std::memory_order_relaxed); }
    RunStrafe &runStrafe() { return g_runStrafe; }
    bool sideRunBuilt() { return g_ready && g_set.sideRunBuilt; }
    void resetTuning()
    {
        g_runStrafe.reset();
    }
    float readPace() { return g_readPace.load(std::memory_order_relaxed); }
    float readSteps() { return g_readSteps.load(std::memory_order_relaxed); }

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
