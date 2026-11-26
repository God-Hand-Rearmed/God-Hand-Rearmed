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
        constexpr float kWalkSideSpeed = 1.74f, kRunSideSpeed = 4.0f; // m/s (K, Nov 25-26)
        constexpr float kEaseSeconds = 0.1f;                          // the side weight's ease (a KBM key's 45 deg steps)
        constexpr int64_t kStillMs = 250;                             // no blend this long: the next one starts at its weight

        using strafe_motions::Side;

        // His loops the side motions lock onto (pl00's entries, the same bytes in every costume; the frame count is
        // checked): the side's grid is his walk 21's (walk) or run 25's (run); the back-pedal 23 and the run 298 map
        // onto it by their touchdowns (strafing-tiger.md "Step 2 plan"); stride = his loop's length / the grid's (the
        // side's steps at his cadence keep its speed).
        struct Loop
        {
            uint32_t index, frames;
            bool run;
            int anchors; // 0: his own grid
            float from[3], to[3];
        };
        constexpr Loop kLoops[] = {
            {21u, 57u, false, 0, {}, {}},
            {23u, 43u, false, 3, {6.75f, 27.75f, 48.75f}, {22.25f, 50.25f, 78.25f}},
            {25u, 14u, true, 0, {}, {}},
            {428u, 14u, true, 0, {}, {}},
            {298u, 19u, true, 3, {14.5f, 23.5f, 32.5f}, {10.5f, 17.0f, 23.5f}},
        };

        std::atomic<bool> g_setting{false};
        std::atomic<bool> g_slide{false};
        std::atomic<float> g_angle{0.0f};
        PS2Runtime::RecompiledFunction g_inner = nullptr, g_poseInner = nullptr;
        std::array<strafe_motions::Motion, strafe_motions::kSides> g_sides;
        bool g_ready = false;

        // Game thread.
        struct Forward // his loop's stance (each translation's mean) and speed, from the motion's own curves
        {
            uint32_t motion = 0u;
            float mean[strafe_motions::kTranslated][3] = {};
            float speed = 0.0f;
        } g_forward[2];
        float g_weight = 0.0f; // the side's weight, + = his left
        int64_t g_lastMs = -100000;
        uint32_t g_loads = 0u;
        struct Track // the side clock at the last addNullSpeed (its root delta between two)
        {
            bool valid = false;
            uint32_t motion = 0u;
            int side = -1;
            float sigma = 0.0f;
        } g_track;

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

        // Gene walking, running or back-pedalling under Tiger with the stick out: mode 0, phase 1 / 2 / 3.
        bool moving(const uint8_t *rdram, uint32_t obj)
        {
            if (!g_setting.load(std::memory_order_relaxed) || !g_slide.load(std::memory_order_relaxed) || !inRam(obj, 0x600u) ||
                obj != rd32(rdram, kPlayerPointer))
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

        // The side motion's frame for his clock t (strafing-tiger.md "Step 2 plan": the lock), on [0, grid).
        float sideClock(const Loop &loop, float t, float grid)
        {
            if (loop.anchors == 0)
                return wrapTo(t, grid);
            const float span = loop.from[loop.anchors - 1] - loop.from[0];
            float tt = wrapTo(t - loop.from[0], span) + loop.from[0];
            int k = 0;
            while (k + 2 < loop.anchors && loop.from[k + 1] <= tt)
                ++k;
            const float u = (tt - loop.from[k]) / (loop.from[k + 1] - loop.from[k]);
            return wrapTo(loop.to[k] + u * (loop.to[k + 1] - loop.to[k]), grid);
        }

        struct Blend
        {
            const Loop *loop = nullptr;
            uint32_t motion = 0u;
            int side = 0;         // strafe_motions::Side
            float w = 0.0f;       // the side's weight (rotations slerped, the means and heights lerped)
            float his = 1.0f;     // his share of his swing and delta
            float share = 0.0f;   // the side's share of its swing and delta
            float stride = 1.0f;  // the side's steps at his cadence
            float sideSpeed = 0.0f, hisSpeed = 0.0f;
            float grid = 0.0f;
            const Forward *fwd = nullptr;
        };

        // The stick's split for this frame (ease stepped in host time). w = the stick's angle from his loop's own
        // direction / 90 deg, signed by side; the speed eases from his to the side's by w and is split along his axis
        // and the side's (strafing-tiger.md "Feet planted on diagonals").
        bool blendFor(const uint8_t *rdram, uint32_t obj, Blend &b)
        {
            if (!g_ready || !moving(rdram, obj) || !(b.loop = playing(rdram, obj, b.motion)))
                return false;
            const bool back = rdram[obj + kSub] == 3u;
            const float angle = g_angle.load(std::memory_order_relaxed);
            const float along = std::cos(angle), lateral = (back ? -1.0f : 1.0f) * std::sin(angle);
            const float target = std::fabs(lateral) < 1e-4f ? 0.0f : std::copysign(std::fmin(std::fabs(angle) / kQuarterTurn, 1.0f), lateral);
            const int64_t now = nowMs();
            const uint32_t loads = ps2x::savestate::loadCount();
            if (now - g_lastMs > kStillMs || loads != g_loads)
                g_weight = target, g_track.valid = false;
            else
                g_weight += (target - g_weight) * (1.0f - std::exp(-static_cast<float>(now - g_lastMs) / 1000.0f / kEaseSeconds));
            g_lastMs = now, g_loads = loads;
            const bool left = g_weight > 0.0f || (g_weight == 0.0f && lateral >= 0.0f);
            const bool run = b.loop->run;
            b.side = run ? (left ? Side::RunLeft : Side::RunRight) : (left ? Side::WalkLeft : Side::WalkRight);
            b.w = std::fabs(g_weight);
            b.fwd = &forward(rdram, b.motion, run);
            b.hisSpeed = b.fwd->speed;
            b.sideSpeed = run ? kRunSideSpeed : kWalkSideSpeed;
            if (!(b.hisSpeed > 0.1f))
                return false;
            const float m = (1.0f - b.w) * b.hisSpeed + b.w * b.sideSpeed;
            b.his = m * along / b.hisSpeed;
            b.share = m * lateral * (left ? 1.0f : -1.0f) / b.sideSpeed;
            b.grid = static_cast<float>(g_sides[b.side].span);
            b.stride = static_cast<float>(b.loop->frames - 1u) / b.grid;
            return true;
        }

        void poseWrapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t obj = GPR_U32(ctx, 4);
            Blend b;
            if (ctx->pc != kMotionPose || !blendFor(rdram, obj, b))
                return g_poseInner(rdram, ctx, runtime);
            const uint32_t parts = rd32(rdram, obj + kParts);
            const float t = rdF(rdram, obj + kClock);
            g_poseInner(rdram, ctx, runtime); // his pose, root outputs and caches: the game's
            if (b.w < 1e-3f && std::fabs(b.his - 1.0f) < 1e-3f && std::fabs(b.share) < 1e-3f)
                return; // straight ahead / back: his own, untouched
            if (rdram[obj + kPartCount] < strafe_motions::kBones || !inRam(parts, 4u * strafe_motions::kBones))
                return;
            uint32_t part[strafe_motions::kBones];
            for (int i = 0; i < strafe_motions::kBones; ++i)
                if (!inRam(part[i] = rd32(rdram, parts + 4u * i), 0x10Cu))
                    return;
            strafe_motions::Pose side;
            strafe_motions::sample(g_sides[b.side], sideClock(*b.loop, t, b.grid), b.stride, side);
            const strafe_motions::Motion &sm = g_sides[b.side];
            for (int i = 0; i < strafe_motions::kBones; ++i)
            {
                if (!((sm.rotated >> i) & 1u))
                    continue;
                float e[3], rc[3][3], ec[3];
                std::memcpy(e, rdram + part[i] + kPartRotation, 12);
                if (!std::isfinite(e[0]) || !std::isfinite(e[1]) || !std::isfinite(e[2]))
                    continue;
                mot_sample::quatMatrix(mot_sample::slerp(mot_sample::eulerQuat(e), side.rot[i], b.w), rc);
                if (mot_sample::eulerNear(rc, e, ec))
                    std::memcpy(rdram + part[i] + kPartRotation, ec, 12);
            }
            // The hips' and foot targets' stance lerped, their swing in the floor's plane by the two shares, heights lerped.
            for (int j = 0; j < strafe_motions::kTranslated; ++j)
            {
                const uint32_t at = rd32(rdram, part[strafe_motions::kTranslatedBones[j]] + kPartAnchor) & PS2_RAM_MASK;
                if (!inRam(at, 12u))
                    continue;
                float v[3];
                std::memcpy(v, rdram + at, 12);
                const float *mf = b.fwd->mean[j], *ms = sm.mean[j].data(), *ts = side.tr[j];
                for (int c = 0; c < 3; c += 2)
                    v[c] = (1.0f - b.w) * mf[c] + b.w * ms[c] + b.his * (v[c] - mf[c]) + b.share * (ts[c] - ms[c]);
                v[1] = (1.0f - b.w) * v[1] + b.w * ts[1];
                std::memcpy(rdram + at, v, 12);
            }
        }

        // Step 1's slide (his loop not one the side motions lock onto, or no side motions): his delta turned by the
        // stick's angle.
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
            // The side's root delta between the clocks of this call and the last (the step moveMotion just made).
            const float sigma = sideClock(*b.loop, rdF(rdram, obj + kClock), b.grid);
            const float x = rdF(rdram, obj + kDelta), z = rdF(rdram, obj + kDelta + 8u);
            float side = 0.0f;
            bool measured = false;
            if (g_track.valid && g_track.motion == b.motion && g_track.side == b.side)
            {
                float step = sigma - g_track.sigma;
                if (step < -0.5f * b.grid)
                    step += b.grid;
                if (step >= 0.0f && step < 0.5f * b.grid)
                {
                    const strafe_motions::Motion &sm = g_sides[b.side];
                    side = strafe_motions::rootX(sm, g_track.sigma + step, b.stride) - strafe_motions::rootX(sm, g_track.sigma, b.stride);
                    measured = true;
                }
            }
            if (!measured) // a new loop or side: his step's length at the side's speed
                side = std::fabs(z) * b.sideSpeed / b.hisSpeed * (b.side == Side::WalkLeft || b.side == Side::RunLeft ? 1.0f : -1.0f);
            g_track = Track{true, b.motion, b.side, sigma};
            wrF(rdram, obj + kDelta, b.his * x + b.share * side);
            wrF(rdram, obj + kDelta + 8u, b.his * z);
            g_inner(rdram, ctx, runtime);
            wrF(rdram, obj + kDelta, x);
            wrF(rdram, obj + kDelta + 8u, z);
        }

        // The four side motions from em00.dat on the disc (strafe_motions.h); false: step 1's slide only.
        bool buildSides()
        {
            const auto file = disc_files::open() ? disc_files::find("em/em00.dat") : std::nullopt;
            std::vector<uint8_t> head;
            if (!file || !disc_files::readRange(*file, 0u, 4u, head))
                return false;
            uint32_t count = 0;
            std::memcpy(&count, head.data(), 4);
            if (count == 0u || count > 0x10000u || !disc_files::readRange(*file, 4u, 4u * count, head))
                return false;
            std::set<uint32_t> ends{file->size};
            for (uint32_t i = 0; i < count; ++i)
            {
                uint32_t o = 0;
                std::memcpy(&o, head.data() + 4u * i, 4);
                if (o)
                    ends.insert(o);
            }
            const strafe_motions::Fetch fetch = [&](uint32_t index, std::vector<uint8_t> &bytes) {
                if (index >= count)
                    return false;
                uint32_t o = 0;
                std::memcpy(&o, head.data() + 4u * index, 4);
                const auto end = ends.upper_bound(o);
                return o != 0u && end != ends.end() && disc_files::readRange(*file, o, *end - o, bytes);
            };
            std::string why;
            if (!strafe_motions::build(fetch, g_sides, why))
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
