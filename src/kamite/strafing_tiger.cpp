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
        constexpr uint32_t kAddNullSpeed = 0x0014F928u, kMotionPose = 0x00158328u;
        constexpr uint32_t kPlayerPointer = game::GameWork::address(game::GameWork::kPlayerObj);
        constexpr uint32_t kMajor = 0x2F4u, kSub = 0x2F5u, kParts = 0x278u, kPartCount = 0x2B4u, kPack = 0x304u,
                           kMotion = 0x428u, kClock = 0x430u, kDelta = 0x330u;
        constexpr uint32_t kPartRotation = 0x100u, kPartAnchor = 0xD0u;
        constexpr float kHalfTurn = 3.14159265f, kQuarterTurn = 1.57079633f;
        // The side-steps' and the back-pedal's speeds (K, Nov 25-26: the walk 1.74 every way round, so a circle keeps
        // one speed; the run R2's 3.0 sideways and back).
        constexpr float kWalkSideSpeed = 1.74f, kRunSideSpeed = 3.0f;
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
        // cadence keep their speed). Under Tiger the game only ever walks or runs forward (round 3): the back-pedal
        // is blended here too, so going round a circle never changes his state.
        struct Loop
        {
            uint32_t index, frames;
            bool run;
            int anchors; // 0: his own grid
            float from[3], to[3];
        };
        constexpr Loop kLoops[] = {
            {21u, 57u, false, 0, {}, {}},
            {25u, 14u, true, 0, {}, {}},
            {428u, 14u, true, 0, {}, {}},
            {298u, 19u, true, 3, {14.5f, 23.5f, 32.5f}, {10.5f, 17.0f, 23.5f}},
        };

        std::atomic<bool> g_setting{false};
        std::atomic<bool> g_slide{false};
        std::atomic<bool> g_sprint{false};
        std::atomic<float> g_angle{0.0f};
        // F2 (K, Nov 27; live, not saved): the slow strafe (0 = by Tension: 7, 16 at a full gauge; else one cell), 16's
        // arms (its swing kept at 3 / 9 o'clock; how far toward them his back-pedal's arms go at 6), the fast strafe's
        // guard and bounce (the hips' up-down swing kept).
        std::atomic<int> g_walkChoice{0};
        std::atomic<float> g_tensionArms{0.69f}, g_tensionBack{0.48f};
        std::atomic<float> g_runGuard{0.6f}, g_runBounce{1.0f};
        PS2Runtime::RecompiledFunction g_inner = nullptr, g_poseInner = nullptr;
        strafe_motions::Set g_set;
        bool g_ready = false;

        // Game thread.
        struct Forward // his loop's stance (each translation's mean) and speed, from the motion's own curves
        {
            uint32_t motion = 0u;
            float mean[strafe_motions::kTranslated][3] = {};
            float speed = 0.0f;
        } g_forward[2];
        float g_theta = 0.0f;    // the stick's angle from his facing, eased (+ = his left)
        float g_presence = 0.0f; // the blend's own weight: 1 with the stick out, fading to 0 once it's let go
        float g_tension = 0.0f;  // 16's weight in the slow strafe: eased toward 1 while the Tension gauge is full
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
        bool upperBody(int b) { return (b >= 1 && b <= 16) || b == 26; }
        bool arm(int b) { return b >= 5 && b <= 16; }

        bool isPlayer(const uint8_t *rdram, uint32_t obj)
        {
            return g_setting.load(std::memory_order_relaxed) && inRam(obj, 0x600u) && obj == rd32(rdram, kPlayerPointer);
        }

        // Gene walking or running under Tiger with the stick out (or let go a moment ago), not sprinting: mode 0,
        // phase 1 / 2 (3, the back-pedal, isn't fed under Tiger any more; it would slide as step 1).
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

        const Forward &forward(const uint8_t *rdram, uint32_t motion, bool run)
        {
            Forward &f = g_forward[run ? 1 : 0];
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

        // The stick's split for this frame (eases stepped in host time). a = the eased angle from his facing: up to
        // 90 deg his loop gives way to the side-step (ws = a / 90), past it the side-step to the back-pedal (u =
        // (a - 90) / 90). The speed eases his -> the side's -> the back's along the same weights and is split along
        // the three axes (his forward, the side's sideways, the back's backward) so planted feet stay planted
        // (strafing-tiger.md "Feet planted on diagonals").
        struct Blend
        {
            const Loop *loop = nullptr;
            uint32_t motion = 0u;
            int side = 0, back = 0; // strafe_motions::Side (side + 16 x the slow strafe's sources)
            bool left = true;
            const strafe_motions::Motion *sideMotion = nullptr; // the run's side, or the slow strafe's A (its grid)
            // The slow strafe: A (7, or the F2 cell) and B (16) per side [0 his left, 1 his right], B's weight wt;
            // B's arms keep `keep` of their swing; at 6 o'clock his back-pedal's arms go `toB6` of the way to B's.
            const strafe_motions::Motion *walkA[2] = {}, *walkB[2] = {};
            float wt = 0.0f, keep = 1.0f, toB6 = 0.0f;
            float guard = 0.6f, bounce = 1.0f; // the fast strafe
            bool run = false, front = true;
            float ws = 0.0f, u = 0.0f;              // the side's weight (front half); the back's (back half)
            float his = 1.0f, sd = 0.0f, bk = 0.0f; // the shares of the three swings and deltas
            float presence = 1.0f;
            float stride = 1.0f, grid = 0.0f;
            float hisSpeed = 0.0f, sideSpeed = 0.0f;
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
            b.side = b.run ? (left ? Side::RunLeft : Side::RunRight) : (left ? Side::WalkLeft : Side::WalkRight);
            b.left = left;
            b.sideMotion = &g_set.motions[b.side];
            // The Tension gauge full: 16's weight in the slow strafe eases up (K, Nov 27).
            const float gauge = rdF(rdram, kHeat + 4u), full = rdF(rdram, kHeat + 0xCu);
            const bool tense = full > 0.0f && gauge >= full;
            g_tension += ((tense ? 1.0f : 0.0f) - g_tension) * (1.0f - std::exp(-std::fmax(dt, 0.0f) / kTensionSeconds));
            if (!b.run)
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
                for (int k = 0; k < 2; ++k)
                    b.walkA[k] = &g_set.walks[a][k], b.walkB[k] = &g_set.walks[bsrc][k];
                b.sideMotion = b.walkA[left ? 0 : 1];
                b.side += 16 * (a + 4 * bsrc); // the delta's track restarts on a switch
                b.keep = bsrc == strafe_motions::Cell16 ? g_tensionArms.load(std::memory_order_relaxed) : 1.0f;
                b.toB6 = b.wt * g_tensionBack.load(std::memory_order_relaxed);
            }
            else
                b.guard = g_runGuard.load(std::memory_order_relaxed), b.bounce = g_runBounce.load(std::memory_order_relaxed);
            b.back = b.run ? Side::BackRun : Side::BackWalk;
            b.front = a <= kQuarterTurn;
            b.ws = b.front ? a / kQuarterTurn : 1.0f;
            b.u = b.front ? 0.0f : (a - kQuarterTurn) / kQuarterTurn;
            b.presence = g_presence;
            b.fwd = &forward(rdram, b.motion, b.run);
            b.hisSpeed = b.fwd->speed;
            b.sideSpeed = b.run ? kRunSideSpeed : kWalkSideSpeed; // the back-pedal's too
            if (!(b.hisSpeed > 0.1f))
                return false;
            const float m = b.front ? (1.0f - b.ws) * b.hisSpeed + b.ws * b.sideSpeed : b.sideSpeed;
            const float vx = m * std::sin(a), vz = m * std::cos(a);
            b.his = b.front ? vz / b.hisSpeed : 0.0f;
            b.bk = b.front ? 0.0f : -vz / b.sideSpeed;
            b.sd = vx / b.sideSpeed;
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

        void poseWrapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t obj = GPR_U32(ctx, 4);
            if (ctx->pc != kMotionPose || !isPlayer(rdram, obj))
                return g_poseInner(rdram, ctx, runtime);
            Blend b;
            if (!blendFor(rdram, obj, b))
            {
                g_poseInner(rdram, ctx, runtime);
                if (g_ready && !g_sprint.load(std::memory_order_relaxed))
                    stopFade(rdram, obj);
                return;
            }
            const float t = rdF(rdram, obj + kClock);
            g_poseInner(rdram, ctx, runtime); // his pose, root outputs and caches: the game's
            if (b.presence < 1e-3f || (b.front && b.ws < 1e-3f && std::fabs(b.his - 1.0f) < 1e-3f))
                return; // straight ahead / let go: his own, untouched
            uint32_t part[strafe_motions::kBones], anchor[strafe_motions::kTranslated];
            if (!partsOf(rdram, obj, part, anchor))
                return;
            const float sigma = gridClock(*b.loop, t, b.grid);
            const strafe_motions::Motion &sm = *b.sideMotion, &bm = g_set.motions[b.back];
            const int own = b.left ? 0 : 1;
            strafe_motions::Pose side, back, walkA[2], walkB[2];
            strafe_motions::sample(bm, sigma, b.stride, back);
            float ms[strafe_motions::kTranslated][3];
            if (b.run)
            {
                strafe_motions::sample(sm, sigma, b.stride, side);
                for (int j = 0; j < strafe_motions::kTranslated; ++j)
                    for (int c = 0; c < 3; ++c)
                        ms[j][c] = sm.mean[j][c];
                if (b.bounce < 1.0f) // the hips' up-down swing calmer (F2)
                    side.tr[0][1] = ms[0][1] + b.bounce * (side.tr[0][1] - ms[0][1]);
            }
            else // the slow strafe: A and B (16, its arms calmer) blended by the Tension weight
            {
                const bool both = !b.front; // past 90 deg the arms mix his left and right ones (no pop at 6 o'clock)
                for (int k = 0; k < 2; ++k)
                    if (k == own || both)
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
            const float leftShare = 0.5f + 0.5f * std::sin(g_theta); // his left's arms' share past 90 deg
            for (int i = 0; i < strafe_motions::kBones; ++i)
            {
                float e[3];
                std::memcpy(e, rdram + part[i] + kPartRotation, 12);
                if (!std::isfinite(e[0]) || !std::isfinite(e[1]) || !std::isfinite(e[2]))
                    continue;
                const mot_sample::Quat qg = mot_sample::eulerQuat(e);
                const bool hasSide = (sm.rotated >> i) & 1u, hasBack = (bm.rotated >> i) & 1u;
                mot_sample::Quat qs = hasSide ? side.rot[i] : qg;
                if (hasSide && b.run && upperBody(i) && ((g_set.guarded >> i) & 1u))
                    qs = mot_sample::slerp(qs, g_set.guard[i], b.guard); // the run: his guard mostly up
                mot_sample::Quat q;
                if (b.front)
                    q = mot_sample::slerp(qg, qs, b.ws);
                else if (!arm(i) || b.run)
                    q = mot_sample::slerp(qs, hasBack ? back.rot[i] : qg, arm(i) ? 1.0f - (1.0f - b.u) * (1.0f - b.u) : b.u);
                else
                {
                    // The arms: the slow strafe's (his left's and right's mixed by where the stick points) toward
                    // the back-pedal's, sooner than the legs (1 - (1 - u)^2); at full Tension 16's stay in by toB6 at
                    // 6 o'clock (K, Nov 27: the middle of the two shoulder sets, a bit of 16's motion).
                    const mot_sample::Quat mixL = mot_sample::slerp(walkA[0].rot[i], walkB[0].rot[i], b.wt);
                    const mot_sample::Quat mixR = mot_sample::slerp(walkA[1].rot[i], walkB[1].rot[i], b.wt);
                    const mot_sample::Quat qa = hasSide ? mot_sample::slerp(mixR, mixL, leftShare) : qg;
                    const float soon = 1.0f - (1.0f - b.u) * (1.0f - b.u), toBack = soon * (1.0f - b.toB6);
                    q = mot_sample::slerp(qa, hasBack ? back.rot[i] : qg, toBack);
                }
                if (b.presence < 1.0f)
                    q = mot_sample::slerp(qg, q, b.presence);
                writePart(rdram, part[i], e, q);
                std::memcpy(g_held.euler[i], rdram + part[i] + kPartRotation, 12);
            }
            // The hips and foot targets: the stances lerped, their swing in the floor's plane by the three shares,
            // heights lerped.
            for (int j = 0; j < strafe_motions::kTranslated; ++j)
            {
                float v[3];
                std::memcpy(v, rdram + anchor[j], 12);
                const float *mf = b.fwd->mean[j], *mside = ms[j], *mb = bm.mean[j].data(), *ts = side.tr[j], *tb = back.tr[j];
                float out[3];
                for (int c = 0; c < 3; c += 2)
                {
                    const float stance = b.front ? (1.0f - b.ws) * mf[c] + b.ws * mside[c] : (1.0f - b.u) * mside[c] + b.u * mb[c];
                    out[c] = stance + b.his * (v[c] - mf[c]) + b.sd * (ts[c] - mside[c]) + b.bk * (tb[c] - mb[c]);
                }
                out[1] = b.front ? (1.0f - b.ws) * v[1] + b.ws * ts[1] : (1.0f - b.u) * ts[1] + b.u * tb[1];
                for (int c = 0; c < 3; ++c)
                    out[c] = v[c] + (out[c] - v[c]) * b.presence;
                std::memcpy(rdram + anchor[j], out, 12);
                std::memcpy(g_held.tr[j], out, 12);
            }
            // Walk <-> run (the stick past 90 deg at full deflection walks, K Nov 27): from the pose before, 200 ms.
            const int64_t now = nowMs();
            if (g_lastRun >= 0 && g_lastRun != static_cast<int>(b.run) && g_held.valid && now - g_held.ms < kStillMs)
                g_switched = g_held, g_switchMs = now;
            g_lastRun = b.run;
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
            const float angle = g_angle.load(std::memory_order_relaxed);
            const float c = std::cos(angle), s = std::sin(angle);
            const float x = rdF(rdram, obj + kDelta), z = rdF(rdram, obj + kDelta + 8u);
            wrF(rdram, obj + kDelta, x * c + z * s);
            wrF(rdram, obj + kDelta + 8u, -x * s + z * c);
            g_inner(rdram, ctx, runtime);
            wrF(rdram, obj + kDelta, x);
            wrF(rdram, obj + kDelta + 8u, z);
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
            // The side's and the back's root deltas between the grid clocks of this call and the last (the step
            // moveMotion just made).
            const float sigma = gridClock(*b.loop, rdF(rdram, obj + kClock), b.grid);
            const float x = rdF(rdram, obj + kDelta), z = rdF(rdram, obj + kDelta + 8u);
            const strafe_motions::Motion &sm = *b.sideMotion, &bm = g_set.motions[b.back];
            float side = 0.0f, back = 0.0f;
            bool measured = false;
            if (g_track.valid && g_track.motion == b.motion && g_track.side == b.side)
            {
                float step = sigma - g_track.sigma;
                if (step < -0.5f * b.grid)
                    step += b.grid;
                if (step >= 0.0f && step < 0.5f * b.grid)
                {
                    side = strafe_motions::rootAt(sm, g_track.sigma + step, b.stride) - strafe_motions::rootAt(sm, g_track.sigma, b.stride);
                    if (!b.run && b.wt > 0.0f)
                    {
                        const strafe_motions::Motion &mb2 = *b.walkB[b.left ? 0 : 1];
                        const float sideB = strafe_motions::rootAt(mb2, g_track.sigma + step, b.stride) - strafe_motions::rootAt(mb2, g_track.sigma, b.stride);
                        side += (sideB - side) * b.wt;
                    }
                    back = strafe_motions::rootAt(bm, g_track.sigma + step, b.stride) - strafe_motions::rootAt(bm, g_track.sigma, b.stride);
                    measured = true;
                }
            }
            if (!measured) // a new loop or side: his step's length at the others' speed
            {
                const float step = std::fabs(z) * b.sideSpeed / b.hisSpeed;
                side = step * (b.left ? 1.0f : -1.0f);
                back = -step;
            }
            g_track = Track{true, b.motion, b.side, sigma};
            const float nx = b.his * x + b.sd * side, nz = b.his * z + b.bk * back;
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

        // The side-steps, the back-pedal and his guard from em00.dat / pl00.dat on the disc (strafe_motions.h);
        // false: step 1's slide only.
        bool buildSides()
        {
            DatFile em, pl;
            if (!disc_files::open() || !em.open("em/em00.dat") || !pl.open("pl/pl00.dat"))
            {
                KM_WARN(Game, "Strafing Tiger: no side-steps (em00.dat / pl00.dat not read): the walk slides");
                return false;
            }
            std::string why;
            if (!strafe_motions::build([&](uint32_t i, std::vector<uint8_t> &bytes) { return em.fetch(i, bytes); },
                                       [&](uint32_t i, std::vector<uint8_t> &bytes) { return pl.fetch(i, bytes); }, g_set, why))
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
    std::atomic<float> &tensionArms() { return g_tensionArms; }
    std::atomic<float> &tensionBack() { return g_tensionBack; }
    std::atomic<float> &runGuard() { return g_runGuard; }
    std::atomic<float> &runBounce() { return g_runBounce; }
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
