#include "kamite/style_pose.h"

#include "kamite/function_hooks.h"
#include "kamite/log.h"
#include "kamite/mot_sample.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>


namespace kamite::style_pose
{
    namespace
    {
        // func_00158328(obj): the pose of the motion at +0x428 (an mtb3 block) at the clock +0x430 into every part's
        // Euler +0x100..+0x108 (parts at +0x278, count +0x2B4), root outputs +0x310 / +0x340 / +0x370 (frame_patches'
        // motionPoseSmooth and game_options' roulette replay wrap it too; this wrapper chains with them). After the
        // game's pose, the arm and shoulder parts are slerped toward Gene's quick turn (motion 30 of his pack, +0x304:
        // u32 count, then offsets from the pack) at the swing's clock, decoded here from the motion in RAM (as
        // tools/models/mot.py). Running the game's pose function on the turn instead (948a609) moved more than the
        // arms: it writes translations (hips, the leg IK targets 20 / 24) through part pointers (+0xD0) outside what
        // was put back: K saw his legs freeze and the model shift.
        constexpr uint32_t kMotionPose = 0x158328u;
        constexpr uint32_t kPlayer = 0x7474A0u + 0x5F4u; // GameWork's player
        constexpr uint32_t kTurnMotion = 30u;
        constexpr uint32_t kFirstBone = 1u, kLastBone = 16u; // spine, chest, neck, head (1-4: a share), arms (R 5-10, L 11-16)
        constexpr uint32_t kFirstArm = 5u;
        constexpr int64_t kCancelMs = 100;

        PS2Runtime::RecompiledFunction g_inner = nullptr;
        uint8_t *g_ram = nullptr;

        // K's values (Nov 11): frames 0-14 of motion 30 at its own speed, full strength, eased in 200 ms / out 300 ms.
        constexpr float kFrom = 0.0f, kTo = 14.0f, kSpeed = 1.0f, kStrength = 1.0f, kEaseInMs = 200.0f, kEaseOutMs = 300.0f;
        // The spine, chest, neck and head follow the turn by this share (K: "a touch too little motion in the torso and
        // head"; he picked 0.45; the turn's spine also twists into the turn, so only a part of it).
        constexpr float kTorsoShare = 0.45f;

        std::atomic<bool> g_request{false};
        bool g_active = false;
        int64_t g_startMs = 0, g_cancelMs = 0;

        int64_t nowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }
        bool inRam(uint32_t a, uint32_t size) { return a >= 0x100000u && a + size <= PS2_RDRAM_SIZE; }
        uint32_t rd32(uint32_t a)
        {
            uint32_t v = 0;
            std::memcpy(&v, g_ram + (a & PS2_RAM_MASK), 4);
            return v;
        }
        void wrF(uint32_t a, float f) { std::memcpy(g_ram + (a & PS2_RAM_MASK), &f, 4); }
        float rdF(uint32_t a)
        {
            float f = 0.0f;
            std::memcpy(&f, g_ram + (a & PS2_RAM_MASK), 4);
            return f;
        }
        // Free: mode 0 (+0x2F4) and phase (+0x2F5) 0..3, the only phases that call the action chooser func_00123938
        // with a1 = 0 (idle 0x10BE18, 0x10C300, 0x10C5A8, 0x10C930: standing, walking, running); every other phase
        // (attacks, dodges, hits, the roulette...) calls it with 1 or not at all.
        bool isFree(uint32_t gene) { return g_ram[gene + 0x2F4u] == 0u && g_ram[gene + 0x2F5u] <= 3u; }
        uint32_t turnMotion(uint32_t gene)
        {
            const uint32_t pack = rd32(gene + 0x304u);
            if (!inRam(pack, 8u) || rd32(pack) <= kTurnMotion)
                return 0u;
            const uint32_t motion = pack + rd32(pack + 4u + 4u * kTurnMotion);
            return inRam(motion, 8u) && std::memcmp(g_ram + motion, "mtb3", 4) == 0 ? motion : 0u;
        }

        // The rotation of `bone` at frame t of the motion in RAM (mot_sample.h: the shared decoder): out[c] set where
        // the motion keys component c (found[c]).
        void sampleRotation(uint32_t motion, uint32_t bone, float t, float out[3], bool found[3])
        {
            for (uint32_t c = 0; c < 3u; ++c)
                found[c] = mot_sample::sample(g_ram + motion, PS2_RDRAM_SIZE - motion, bone, 3u + c, t, out[c]);
        }

        void poseWrapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t obj = GPR_U32(ctx, 4);
            if (g_request.exchange(false, std::memory_order_relaxed))
            {
                const uint32_t player = rd32(kPlayer);
                if (inRam(player, 0x1700u) && isFree(player))
                    g_active = true, g_startMs = nowMs(), g_cancelMs = 0;
            }
            if (!g_active || ctx->pc != kMotionPose || obj != rd32(kPlayer) || !inRam(obj, 0x1700u))
                return g_inner(rdram, ctx, runtime);

            // The swing's clock and weight (host time: the swing is drawn only).
            const int64_t now = nowMs();
            const float from = kFrom, to = kTo, speed = kSpeed;
            const float lengthMs = (to - from) / 30.0f / speed * 1000.0f;
            const float t = static_cast<float>(now - g_startMs);
            if (!isFree(obj) && g_cancelMs == 0)
                g_cancelMs = now; // he does something else: fade out (the swing never holds him)
            float weight = kStrength;
            {
                // eased (smoothstep) in and out
                float k = std::clamp(std::min(t / kEaseInMs, (lengthMs - t) / kEaseOutMs), 0.0f, 1.0f);
                weight *= k * k * (3.0f - 2.0f * k);
            }
            if (g_cancelMs)
                weight *= std::clamp(1.0f - static_cast<float>(now - g_cancelMs) / kCancelMs, 0.0f, 1.0f);
            if (t >= lengthMs || (g_cancelMs && now - g_cancelMs >= kCancelMs))
                g_active = false;
            const uint32_t turn = turnMotion(obj), parts = rd32(obj + 0x278u), count = g_ram[obj + 0x2B4u];
            if (!(weight > 0.001f) || !turn || count <= kLastBone || !inRam(parts, 4u * count))
                return g_inner(rdram, ctx, runtime);
            uint32_t part[kLastBone + 1];
            for (uint32_t i = kFirstBone; i <= kLastBone; ++i)
                if (!inRam(part[i] = rd32(parts + 4u * i), 0x10Cu))
                    return g_inner(rdram, ctx, runtime);
            g_inner(rdram, ctx, runtime); // the game's pose, untouched

            const float swingClock = std::min(from + t / 1000.0f * 30.0f * speed, to);
            for (uint32_t i = kFirstBone; i <= kLastBone; ++i)
            {
                float e[3], target[3];
                bool found[3];
                std::memcpy(e, g_ram + part[i] + 0x100u, 12);
                if (!std::isfinite(e[0]) || !std::isfinite(e[1]) || !std::isfinite(e[2]))
                    continue;
                sampleRotation(turn, i, swingClock, target, found);
                if (!found[0] && !found[1] && !found[2])
                    continue;
                for (int c = 0; c < 3; ++c)
                    if (!found[c])
                        target[c] = e[c]; // not keyed by the turn: the game's
                float rc[3][3], ec[3];
                const float share = i < kFirstArm ? kTorsoShare : 1.0f;
                mot_sample::quatMatrix(mot_sample::slerp(mot_sample::eulerQuat(e), mot_sample::eulerQuat(target), weight * share), rc);
                if (mot_sample::eulerNear(rc, e, ec))
                    std::memcpy(g_ram + part[i] + 0x100u, ec, 12);
            }
        }
    }

    void trigger()
    {
        g_request.store(true, std::memory_order_relaxed);
    }

    void install(PS2Runtime &runtime)
    {
        g_ram = runtime.memory().getRDRAM();
        if (!g_ram || !runtime.hasFunction(kMotionPose) || !(g_inner = runtime.lookupFunction(kMotionPose)) ||
            !FunctionHooks::instance().replace(kMotionPose, "motion pose (Signal Style Switch arm swing)", &poseWrapped,
                                               ReplacementStatus::Native, "the quick turn's arm swing on a style switch, drawn only"))
        {
            g_inner = nullptr;
            KM_WARN(Game, "styles: the motion pose (0x158328) is not hookable: no arm swing");
        }
    }
}
