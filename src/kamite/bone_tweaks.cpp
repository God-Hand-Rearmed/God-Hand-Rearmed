#include "kamite/bone_tweaks.h"
#include "kamite/body_detail.h"
#include "kamite/character_mods.h"
#include "kamite/dev_menu.h"
#include "kamite/frame_patches.h"
#include "kamite/function_hooks.h"
#include "kamite/game_layout.h"
#include "kamite/log.h"
#include "kamite/punish_fit.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_game_menu.h"
#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define KAMITE_HAS_GS_MENU 1 // set per file, as in npc_poses.cpp: without it the Money panel is compiled out
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace kamite::bone_tweaks
{
    namespace
    {
        using namespace kamite::game;

        constexpr uint32_t kGetMeshPtr = 0x0014B6F8u;    // cModel_getMeshPtr(model, index)
        constexpr uint32_t kTransCallSite = 0x0014A4C0u; // the return address of its first call in cModel_Trans
        constexpr uint32_t kPartsFirst = 0x144u, kPartNext = 0x144u, kPartMtx = 0x80u, kPartNum = 0x2B4u;
        constexpr uint32_t kMeshHead = 0x25Cu, kNodeNext = 0x404u, kNodeFlags = 0x380u, kNodeInfo = 0x414u, kInfoName = 0x8u;
        struct Saved
        {
            uint32_t address = 0;
            std::array<float, 16> original{}, mirrored{};
        };
        Saved g_saved[2]; // the two hair bones
        bool g_applied = false;

        uint32_t rd32(const uint8_t *rdram, uint32_t a) { uint32_t v; std::memcpy(&v, rdram + (a & PS2_RAM_MASK), 4); return v; }
        bool inRam(uint32_t a, uint32_t size = 4u) { a &= PS2_RAM_MASK; return a >= 0x100000u && a + size <= PS2_RDRAM_SIZE; }

        using Vec = std::array<float, 3>;
        using Mat = std::array<float, 9>; // row-major, column vectors: v' = M v
        Vec sub(const Vec &a, const Vec &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
        Vec add(const Vec &a, const Vec &b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
        Vec mulf(const Vec &a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
        float dot(const Vec &a, const Vec &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
        Vec cross(const Vec &a, const Vec &b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
        float length(const Vec &a) { return std::sqrt(dot(a, a)); }
        Vec unit(const Vec &a) { const float l = length(a); return l > 1e-9f ? mulf(a, 1.0f / l) : Vec{0, 0, 0}; }
        Mat mul(const Mat &a, const Mat &b)
        {
            Mat r{};
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
            return r;
        }
        // The game's matrix: rows are the bone's axes (row vectors), row 3 the position.
        struct Bone
        {
            std::array<float, 16> m{};
            Mat rot() const { return {m[0], m[4], m[8], m[1], m[5], m[9], m[2], m[6], m[10]}; } // the transpose
            Vec pos() const { return {m[12], m[13], m[14]}; }
            void set(const Mat &r, const Vec &p)
            {
                m[0] = r[0]; m[4] = r[1]; m[8] = r[2];
                m[1] = r[3]; m[5] = r[4]; m[9] = r[5];
                m[2] = r[6]; m[6] = r[7]; m[10] = r[8];
                m[12] = p[0]; m[13] = p[1]; m[14] = p[2];
            }
        };
        Bone readBone(const uint8_t *rdram, uint32_t part) { Bone b; std::memcpy(b.m.data(), rdram + ((part + kPartMtx) & PS2_RAM_MASK), 64); return b; }
        void writeBone(uint8_t *rdram, uint32_t part, const Bone &b) { std::memcpy(rdram + ((part + kPartMtx) & PS2_RAM_MASK), b.m.data(), 64); }

        // The player's cParts in bone order (0 on failure).
        int playerParts(const uint8_t *rdram, uint32_t parts[32])
        {
            const uint32_t model = rd32(rdram, GameWork::address(GameWork::kPlayerObj));
            if (!inRam(model, 0x2E4u))
                return 0;
            const int n = rdram[(model + kPartNum) & PS2_RAM_MASK];
            uint32_t p = rd32(rdram, model + kPartsFirst);
            int i = 0;
            for (; i < n && i < 32 && inRam(p, 0x160u); ++i)
            {
                parts[i] = p;
                p = rd32(rdram, p + kPartNext);
            }
            return i;
        }


        Mat axisAngle(const Vec &k, float a)
        {
            const float c = std::cos(a), s = std::sin(a), t = 1 - c;
            return {t * k[0] * k[0] + c, t * k[0] * k[1] - s * k[2], t * k[0] * k[2] + s * k[1],
                    t * k[0] * k[1] + s * k[2], t * k[1] * k[1] + c, t * k[1] * k[2] - s * k[0],
                    t * k[0] * k[2] - s * k[1], t * k[1] * k[2] + s * k[0], t * k[2] * k[2] + c};
        }
        Vec applyRot(const Mat &m, const Vec &v)
        {
            return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2], m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
        }

        // The bind positions (the part offsets summed up the chain; the bind rotations are identity).
        void bindPositions(const uint8_t *rdram, const uint32_t parts[32], int count, Vec bind[32])
        {
            for (int i = 0; i < count; ++i)
            {
                float t[3];
                std::memcpy(t, rdram + ((parts[i] + 0xE0u) & PS2_RAM_MASK), 12);
                const uint32_t parent = rd32(rdram, parts[i] + 0x148u);
                int pi = -1;
                for (int j = 0; j < i; ++j)
                    if (parts[j] == parent)
                        pi = j;
                bind[i] = pi >= 0 ? add(bind[pi], Vec{t[0], t[1], t[2]}) : Vec{t[0], t[1], t[2]};
            }
        }

        // A spare bone placed as its parent turned by q about a pivot (both in the parent's bind frame, pivot
        // relative to the parent's bind position): world = Rp (pivot + q (p - Bp - pivot)) + tp for a bind-space p, so
        // mtx = [Rp q | Rp (q (d - pivot) + pivot) + tp] with d = Bbone - Bp.
        void placeHinged(uint8_t *rdram, const uint32_t parts[32], const Vec bind[32], int bone, int parent, const Vec &pivot,
                         const Mat &q, Saved &s)
        {
            const Bone p = readBone(rdram, parts[parent]);
            Bone b = readBone(rdram, parts[bone]);
            if (!(s.address == parts[bone] && std::memcmp(b.m.data(), s.mirrored.data(), 64) == 0))
            {
                s.address = parts[bone];
                s.original = b.m;
            }
            const Mat rp = p.rot();
            const Vec d = sub(bind[bone], bind[parent]);
            b.set(mul(rp, q), add(applyRot(rp, add(applyRot(q, sub(d, pivot)), pivot)), p.pos()));
            writeBone(rdram, parts[bone], b);
            s.mirrored = b.m;
            g_applied = true;
        }

        // ---- hair: each strand's tip a point mass on a rod from its root, pulled back to the rest shape by a spring,
        // damped against the root's own motion, pushed by gravity when the head tilts and by a gusting wind; the
        // bone turned to point at it within its swing limits. Stepped once per game frame.
        std::atomic<float> g_tune[4] = {40.0f, 5.0f, 0.6f, 1.2f}; // stiffness, damping, gravity, wind (F2 > Debug)
        uint32_t g_frame = 0, g_stepped = ~0u;
        std::chrono::steady_clock::time_point g_lastStep{};
        float g_clock = 0.0f;
        struct Strand
        {
            bool live = false;
            Vec tip{}, vel{}, root{};
            Mat q{1, 0, 0, 0, 1, 0, 0, 0, 1};
        };
        Strand g_strands[2];

        Mat rotX(float a) { const float c = std::cos(a), s = std::sin(a); return {1, 0, 0, 0, c, -s, 0, s, c}; }
        Mat rotZ(float a) { const float c = std::cos(a), s = std::sin(a); return {c, -s, 0, s, c, 0, 0, 0, 1}; }
        Mat transpose(const Mat &m) { return {m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]}; }

        void stepStrand(Strand &st, const characters::HairStrand &cfg, const Bone &head, float dt, const characters::Hair &lim)
        {
            const Mat rh = head.rot();
            const Vec pivot{cfg.pivot[0], cfg.pivot[1], cfg.pivot[2]}, rest = unit(Vec{cfg.dir[0], cfg.dir[1], cfg.dir[2]});
            const Vec root = add(head.pos(), applyRot(rh, pivot));
            const Vec restTip = add(root, mulf(applyRot(rh, rest), cfg.length));
            if (!st.live || length(sub(root, st.root)) > 1.0f)
            {
                st.live = true;
                st.tip = restTip;
                st.vel = {0, 0, 0};
                st.root = root;
            }
            const Vec rootVel = mulf(sub(root, st.root), 1.0f / dt);
            st.root = root;
            const float k = g_tune[0].load(std::memory_order_relaxed), c = g_tune[1].load(std::memory_order_relaxed),
                        g = g_tune[2].load(std::memory_order_relaxed) * 9.8f, w = g_tune[3].load(std::memory_order_relaxed);
            const Vec down{0, -1, 0};
            const Vec gdev = mulf(sub(down, applyRot(rh, down)), g); // nothing while the head is upright
            const float gust = 0.6f + 0.4f * std::sin(g_clock * 1.3f) + 0.3f * std::sin(g_clock * 3.7f + 1.0f);
            const Vec wind = mulf(unit(Vec{1.0f, 0.0f, 0.35f}), w * gust);
            Vec acc = add(add(mulf(sub(restTip, st.tip), k), gdev), wind);
            acc = sub(acc, mulf(sub(st.vel, rootVel), c));
            st.vel = add(st.vel, mulf(acc, dt));
            st.tip = add(st.tip, mulf(st.vel, dt));
            st.tip = add(root, mulf(unit(sub(st.tip, root)), cfg.length));
            // The bone's turn, in the head's frame: pitch (towards her front +z) and roll (towards her left +x),
            // each within its limits; a hinged strand only pitches.
            const Vec dl = applyRot(transpose(rh), unit(sub(st.tip, root)));
            constexpr float kRad = 3.14159265f / 180.0f;
            float pitch = std::atan2(dl[2], -dl[1]) - std::atan2(rest[2], -rest[1]);
            float roll = std::asin(std::clamp(dl[0], -1.0f, 1.0f)) - std::asin(std::clamp(rest[0], -1.0f, 1.0f));
            if (cfg.hinge)
            {
                pitch = std::clamp(pitch, -lim.sideSwing * kRad, lim.sideSwing * kRad);
                roll = 0.0f;
            }
            else
            {
                pitch = std::clamp(pitch, -lim.backBack * kRad, lim.backForward * kRad);
                roll = std::clamp(roll, -lim.backSide * kRad, lim.backSide * kRad);
            }
            st.q = mul(rotZ(roll), rotX(-pitch));
            st.tip = add(root, mulf(applyRot(rh, applyRot(st.q, rest)), cfg.length)); // the state keeps to the limits
        }

        void placeHair(uint8_t *rdram, const uint32_t parts[32], const Vec bind[32])
        {
            const characters::Hair &hair = characters::hair();
            if (g_stepped != g_frame)
            {
                const auto now = std::chrono::steady_clock::now();
                float dt = g_stepped == ~0u ? 1.0f / 60.0f : std::chrono::duration<float>(now - g_lastStep).count();
                dt = std::clamp(dt, 1.0f / 120.0f, 1.0f / 20.0f);
                g_lastStep = now;
                g_stepped = g_frame;
                g_clock += dt;
                const Bone head = readBone(rdram, parts[4]);
                if (hair.back.bone >= 0)
                    stepStrand(g_strands[0], hair.back, head, dt, hair);
                if (hair.side.bone >= 0)
                    stepStrand(g_strands[1], hair.side, head, dt, hair);
            }
            if (hair.back.bone >= 0)
                placeHinged(rdram, parts, bind, hair.back.bone, 4, Vec{hair.back.pivot[0], hair.back.pivot[1], hair.back.pivot[2]},
                            g_strands[0].q, g_saved[0]);
            if (hair.side.bone >= 0)
                placeHinged(rdram, parts, bind, hair.side.bone, 4, Vec{hair.side.pivot[0], hair.side.pivot[1], hair.side.pivot[2]},
                            g_strands[1].q, g_saved[1]);
        }

        // ---- Money: the women's chest bone scaled (see the header) ---------------------------------------------------
        constexpr int kChestBone = 29, kChestParent = 2;
        std::atomic<float> g_money{1.0f};
        std::atomic<float> g_cash{1.0f}; // body_detail: above 1 the women's detailed seat, grown up to 130%
        constexpr float kMoneyDroop = 30.0f; // degrees the bust tips down per +100% (K: 15 at 150% looks right)
        std::vector<Saved> g_moneySaved; // game thread: the bones scaled since the frame started

        void placeMoney(uint8_t *rdram, uint32_t model)
        {
            const float scale = g_money.load(std::memory_order_relaxed);
            // Gene's hands on her chest (Buck Breaker's chest grab Suplex, K, Nov 15): the jiggle is held still while they
            // are on (it would swing the bust through his fingers), let go as they come off. At 100% too.
            const float hold = punish_fit::bustHold(rdram, model);
            if ((std::fabs(scale - 1.0f) < 0.001f && hold <= 0.0f) || !inRam(model, 0x2E4u))
                return;
            const int n = rdram[(model + kPartNum) & PS2_RAM_MASK];
            if (n <= kChestBone || n > 64)
                return;
            uint32_t part = rd32(rdram, model + kPartsFirst), parent = 0u, chest = 0u;
            for (int i = 0; i <= kChestBone; ++i)
            {
                if (!inRam(part, 0x160u))
                    return;
                if (i == kChestParent)
                    parent = part;
                if (i == kChestBone)
                    chest = part;
                part = rd32(rdram, part + kPartNext);
            }
            // The women's skeleton: bone 29 hangs off the upper spine (2), at chest height and in front of it (bind
            // (0, 0.115, 0.05) on the em00 women). Men and Gene keep 29 on the head (4) as a spare.
            if (rd32(rdram, chest + 0x148u) != parent)
                return;
            float bind[3];
            std::memcpy(bind, rdram + ((chest + 0xE0u) & PS2_RAM_MASK), 12);
            if (bind[1] < 0.03f || bind[1] > 0.25f || bind[2] < 0.0f || bind[2] > 0.15f || std::fabs(bind[0]) > 0.03f)
                return;
            for (const Saved &s : g_moneySaved)
                if (s.address == chest) // packed twice in a frame: scaled once
                    return;
            Saved s;
            s.address = chest;
            Bone b = readBone(rdram, chest);
            s.original = b.m;
            // The game's jiggle turns the bone round its own x only (against the spine bone, rest ~0): angle =
            // atan2(row y . parent z, row y . parent y). Rebuilt as rotX(angle x scale + droop) in the parent's frame,
            // the rows scaled: the bounce grows with the size and a bigger bust hangs lower (+angle tips its forward
            // axis down).
            const Bone p = readBone(rdram, parent);
            auto row = [](const Bone &m, int r) { return Vec{m.m[r * 4], m.m[r * 4 + 1], m.m[r * 4 + 2]}; };
            const Vec px = unit(row(p, 0)), py = unit(row(p, 1)), pz = unit(row(p, 2));
            const Vec by = row(b, 1);
            const float jiggle = std::atan2(dot(by, pz), dot(by, py));
            constexpr float kRad = 3.14159265f / 180.0f;
            const float angle = jiggle * scale * (1.0f - hold) + (scale - 1.0f) * kMoneyDroop * kRad;
            const float c = std::cos(angle), sn = std::sin(angle);
            const Vec rows[3] = {mulf(px, scale), mulf(add(mulf(py, c), mulf(pz, sn)), scale),
                                 mulf(sub(mulf(pz, c), mulf(py, sn)), scale)};
            for (int r = 0; r < 3; ++r)
                for (int col = 0; col < 3; ++col)
                    b.m[r * 4 + col] = rows[r][col];
            writeBone(rdram, chest, b);
            s.mirrored = b.m;
            g_moneySaved.push_back(s);
            g_applied = true;
        }

        // ---- the jiggle bones (claude/cash.md "Jiggle"; body_detail.h jiggleOf) ------------------------------------
        // The rebuilt women (and Shannon / Olivia) have the seat's pelvis share on bone 27 and the bust's right half on
        // 28 (body_detail_build reskin). Every draw places 27 as the pelvis (17) and 28 as 29, so with nothing moving
        // them the body draws as before. Above 100%: Cash moves 27 by the seat's spring (up / down, a side swing), Money
        // turns each breast (29 her left, 28 her right) round its own base by its own spring instead of the game's
        // single-bone bounce. Each spring is a point mass on its spot (the cheeks' middle, ahead of each breast's base),
        // pulled back to it and damped against the spot's own motion, so the body's moves (a run's bob, a landing, a
        // hit, a turn) drive it; stepped per game frame in fixed 1/240 s sub-steps (the same at 30 and 60 FPS).
        constexpr int kPelvisBone = 17, kSeatBone = 27, kRightBone = 28, kBustBone = 29, kBustParent = 2;
        constexpr float kOn = 1.0005f;                       // Money / Cash above 100%
        constexpr float kBreastX = 0.07f, kBreastLen = 0.08f; // the bases either side of 29's origin; the tip ahead
        constexpr float kSeatSpot[3] = {0.0f, 0.98f, -0.10f}; // the cheeks' middle (bind)
        constexpr float kSeatMax = 0.04f, kPitchMax = 30.0f, kYawMax = 20.0f;
        // Spanking (K, Nov 16): each slap kicks the seat's spring down the body (toward her legs) at this speed (m/s; the
        // launching slap x1.5), on top of the motion's own reaction (K, Nov 16: 200% of the first try).
        constexpr float kSlapSpeed = 0.70f;
        // K (Nov 16): that alone barely reads (it moves the seat along the body). Each slap also dents the cheeks inward
        // (the pelvis' z, toward her front) on a quicker, springier spring of its own that bounces back out.
        constexpr float kDentSpeed = 0.45f, kDentRate = 1.6f, kDentDamp = 0.45f;
        // The bust between 100% and 130% (K, Nov 16: at 115% too quick, "jell-o", flicking, raised in the dizzy idle; 130%
        // right): the springs' rate / damping and the blend from the game's bounce follow u = 1 - (1 - t)^2, so the
        // middle sizes are already close to the 130% weight; firm end 2.0 Hz / 0.7.
        constexpr float kBustFirmHz = 2.0f, kBustFirmDamp = 0.7f;
        float bustU(float money)
        {
            const float t = std::clamp((money - 1.0f) / 0.3f, 0.0f, 1.0f);
            return 1.0f - (1.0f - t) * (1.0f - t);
        }
        // The springs by size (t = (Money or Cash - 1) / 0.3, linear): at 130% K's values from play (Nov 16: rate 1.14 Hz,
        // damping 1.0, strength 1.0, side swing 0.69, left / right rates 14% apart, the same for bust and seat); a
        // smaller bust or seat is firmer (K): quicker, a little springier (jiggles in place), smaller peaks, less side
        // to side.
        enum JigTune { kHz, kDamp, kGain, kSide, kSpread, kJigTunes };
        constexpr float kJigFirm[kJigTunes] = {2.6f, 0.55f, 0.45f, 0.30f, 0.14f};
        constexpr float kJigFull[kJigTunes] = {1.14f, 1.0f, 1.0f, 0.69f, 0.14f};
        float jig(int i, float size)
        {
            const float t = std::clamp((size - 1.0f) / 0.3f, 0.0f, 1.0f);
            return kJigFirm[i] + (kJigFull[i] - kJigFirm[i]) * t;
        }

        struct Spring
        {
            Vec tip{}, vel{}, rest{};
        };
        struct JigState
        {
            uint32_t frame = 0, seen = 0;
            bool live = false;
            Spring seat, breast[2]; // breast 0: her left (29), 1: her right (28)
            float dent = 0.0f, dentVel = 0.0f; // the slap's dent (m, into the cheeks)
            float envGame = 0.0f, envOurs = 0.0f; // recent peak of the game's bounce / our springs' pitch (radians)
        };
        std::unordered_map<uint32_t, JigState> g_jigs; // game thread, by model
        std::vector<uint32_t> g_jigDone;               // the models placed this frame

        void stepSpring(Spring &sp, const Vec &rest, float dt, float hz, float zeta)
        {
            const float w = 2.0f * 3.14159265f * std::max(hz, 0.1f), k = w * w, c = 2.0f * std::max(zeta, 0.0f) * w;
            const Vec spotVel = mulf(sub(rest, sp.rest), 1.0f / dt);
            const int n = std::clamp(static_cast<int>(std::ceil(dt * 240.0f)), 1, 32);
            const float h = dt / static_cast<float>(n);
            for (int i = 1; i <= n; ++i)
            {
                const Vec r = add(sp.rest, mulf(sub(rest, sp.rest), static_cast<float>(i) / static_cast<float>(n)));
                const Vec acc = sub(mulf(sub(r, sp.tip), k), mulf(sub(sp.vel, spotVel), c));
                sp.vel = add(sp.vel, mulf(acc, h));
                sp.tip = add(sp.tip, mulf(sp.vel, h));
            }
            sp.rest = rest;
        }
        void resetSpring(Spring &sp, const Vec &rest) { sp = Spring{rest, {0, 0, 0}, rest}; }
        Mat rotY(float a) { const float c = std::cos(a), s = std::sin(a); return {c, 0, s, 0, 1, 0, -s, 0, c}; }
        Mat columns(const Vec &x, const Vec &y, const Vec &z) { return {x[0], y[0], z[0], x[1], y[1], z[1], x[2], y[2], z[2]}; }
        Mat scaled(const Mat &m, float s) { Mat r = m; for (float &v : r) v *= s; return r; }

        // Playable Olivia's bust (the player's model; K, Nov 20: it moved as at 130% at every size, even 85%). The
        // game's bounce (cObjSimple_MoveBust, enemies only) emulated on her and the springs with their response kept at
        // 130%'s size both ran into the 30 / 20 degree limits in Gene's moves (offline, his run: the same peaks at 85 and
        // 130%). Hers is the springs alone, sized by Money: the strength runs from the firm 0.45 at 100% to K's 1.0 at
        // 130% with no rate compensation (a firmer bust lags less), x Money squared below 100%, and the limits are soft
        // (tanh), so a bigger bust always moves more and a hard move doesn't flatten the sizes. Run: about 18 / 31
        // degrees of pitch at 100 / 130%, idle 4 / 11.
        float softLimit(float v, float limit) { return limit * std::tanh(v / limit); }

        void placeJiggle(uint8_t *rdram, uint32_t model)
        {
            const body_detail::Jiggle jg = body_detail::jiggleOf(rdram, model);
            if (jg.flags == 0 || std::find(g_jigDone.begin(), g_jigDone.end(), model) != g_jigDone.end())
                return;
            const int n = rdram[(model + kPartNum) & PS2_RAM_MASK];
            if (n <= kBustBone || n > 64)
                return;
            uint32_t parts[kBustBone + 1] = {};
            uint32_t part = rd32(rdram, model + kPartsFirst);
            for (int i = 0; i <= kBustBone; ++i)
            {
                if (!inRam(part, 0x160u))
                    return;
                parts[i] = part;
                part = rd32(rdram, part + kPartNext);
            }
            g_jigDone.push_back(model);
            constexpr float kRad = 3.14159265f / 180.0f;
            const float money = g_money.load(std::memory_order_relaxed), cash = g_cash.load(std::memory_order_relaxed);
            const Vec bPelvis{jg.pelvis[0], jg.pelvis[1], jg.pelvis[2]}, bBust{jg.bust[0], jg.bust[1], jg.bust[2]};
            auto row = [](const Bone &m, int r) { return Vec{m.m[r * 4], m.m[r * 4 + 1], m.m[r * 4 + 2]}; };

            // The spots: the cheeks' middle on the pelvis; ahead of each breast's base on the bust as Money shapes it
            // (the parent's axes tipped by the droop, scaled; placeMoney's frame without any bounce).
            const Bone pelvis = readBone(rdram, parts[kPelvisBone]), bust = readBone(rdram, parts[kBustBone]);
            const Bone parent = readBone(rdram, parts[kBustParent]);
            // The player's model (playable Olivia, olivia_player.h): no bounce of the game's; her springs (softLimit).
            const bool own = model == rd32(rdram, GameWork::address(GameWork::kPlayerObj));
            Bone gameBust = bust; // bone 29 before placeMoney's scale and droop
            for (const Saved &sv : g_moneySaved)
                if (sv.address == parts[kBustBone])
                    gameBust.m = sv.original;
            const float s = money;
            const float droop = (s - 1.0f) * kMoneyDroop * kRad, dc = std::cos(droop), ds = std::sin(droop);
            const Vec px = unit(row(parent, 0)), py = unit(row(parent, 1)), pz = unit(row(parent, 2));
            const Vec ex = px, ey = add(mulf(py, dc), mulf(pz, ds)), ez = sub(mulf(pz, dc), mulf(py, ds));
            const Mat rb = columns(ex, ey, ez);
            const Vec t29 = bust.pos(), t17 = pelvis.pos();
            const Vec base[2] = {Vec{kBreastX, 0.0f, 0.0f}, Vec{-kBreastX, 0.0f, 0.0f}}; // relative to 29's bind
            Vec breastSpot[2];
            for (int i = 0; i < 2; ++i)
                breastSpot[i] = add(t29, applyRot(scaled(rb, s), add(base[i], Vec{0.0f, 0.0f, kBreastLen})));
            const Vec seatSpot = add(t17, applyRot(pelvis.rot(), sub(Vec{kSeatSpot[0], kSeatSpot[1], kSeatSpot[2]}, bPelvis)));

            JigState &st = g_jigs[model];
            st.seen = g_frame;
            uint32_t frame = 0;
            std::memcpy(&frame, rdram + (GameWork::address(GameWork::kFrameCount) & PS2_RAM_MASK), 4);
            const int32_t elapsed = static_cast<int32_t>(frame - st.frame);
            if (!st.live || elapsed < 0 || elapsed > 4 || length(sub(seatSpot, st.seat.rest)) > 0.5f)
            {
                resetSpring(st.seat, seatSpot);
                resetSpring(st.breast[0], breastSpot[0]);
                resetSpring(st.breast[1], breastSpot[1]);
                st.dent = st.dentVel = 0.0f;
                st.live = true;
                st.frame = frame;
            }
            float stepDt = 0.0f; // > 0 when the springs stepped this draw
            if (st.frame != frame && elapsed > 0)
            {
                const float dt = static_cast<float>(elapsed) / (fps60() ? 60.0f : 30.0f);
                stepDt = dt;
                const float spread = jig(kSpread, money); // left x (1 + spread), right x (1 - spread), as K tuned it
                if (const float slap = punish_fit::seatSlap(rdram, model); slap > 0.0f && cash > kOn && (jg.flags & 1))
                {
                    st.seat.vel = sub(st.seat.vel, mulf(unit(row(pelvis, 1)), kSlapSpeed * slap));
                    st.dentVel += kDentSpeed * slap;
                }
                stepSpring(st.seat, seatSpot, dt, jig(kHz, cash), jig(kDamp, cash));
                {
                    const float w = 2.0f * 3.14159265f * jig(kHz, cash) * kDentRate, k = w * w, c = 2.0f * kDentDamp * w;
                    const int n = std::clamp(static_cast<int>(std::ceil(dt * 240.0f)), 1, 32);
                    const float h = dt / static_cast<float>(n);
                    for (int i = 0; i < n; ++i)
                    {
                        st.dentVel += (-k * st.dent - c * st.dentVel) * h;
                        st.dent += st.dentVel * h;
                    }
                }
                const float u = bustU(money);
                const float bustHz = kBustFirmHz + (kJigFull[kHz] - kBustFirmHz) * u;
                const float bustDamp = kBustFirmDamp + (kJigFull[kDamp] - kBustFirmDamp) * u;
                stepSpring(st.breast[0], breastSpot[0], dt, bustHz * (1.0f + spread), bustDamp);
                stepSpring(st.breast[1], breastSpot[1], dt, bustHz * (1.0f - spread), bustDamp);
                st.frame = frame;
            }

            // 27: the pelvis, moved by the seat's spring (in the pelvis' axes: up / down, the side swing x side).
            if (jg.flags & 1)
            {
                Vec d{0.0f, 0.0f, 0.0f};
                if (cash > kOn)
                {
                    const Vec D = sub(st.seat.tip, st.seat.rest);
                    const Vec ux = unit(row(pelvis, 0)), uy = unit(row(pelvis, 1));
                    d = mulf(Vec{dot(D, ux) * jig(kSide, cash), dot(D, uy), 0.0f}, jig(kGain, cash) * cash);
                    d[2] += st.dent * cash;
                    if (const float l = length(d); l > kSeatMax)
                        d = mulf(d, kSeatMax / l);
                }
                Bone b = readBone(rdram, parts[kSeatBone]);
                b.set(pelvis.rot(), add(t17, applyRot(pelvis.rot(), add(sub(Vec{0, 0, 0}, bPelvis), d))));
                writeBone(rdram, parts[kSeatBone], b);
                g_applied = true;
            }
            if (!(jg.flags & 2))
                return;
            // The player's bust takes the springs' path at every size (softLimit).
            if (money <= kOn && !own)
            {
                // 28 as 29 (the game's bounce, or Money's scale below 100% / the chest grab's hold as placeMoney left it).
                Bone b = readBone(rdram, parts[kRightBone]);
                b.set(bust.rot(), add(t29, applyRot(bust.rot(), sub(Vec{0, 0, 0}, bBust))));
                writeBone(rdram, parts[kRightBone], b);
                g_applied = true;
                return;
            }
            // Both breasts on their own springs: bone b of breast i = s Rb Q_i, at t29 + s Rb (Q_i (B_b - B29 - o_i) + o_i).
            // K (Nov 16): just above 100% the bust must move at least as much as the game's own bounce (Money off), the
            // side swing as strong as that up and down, growing to his 130% tuning. So by t = bustU (0 at 100%, 1 at
            // 130%): the pitch blends the game's bounce (bone 29 as cObjSimple_MoveBust left it) into our spring's; the springs'
            // response is kept at the 130% size whatever their firmness (gain x (rate / 130%'s rate)^2: quicker, not
            // smaller); the side swing is scaled from the game's bounce (recent peak of it / of our pitch) toward ours,
            // side ratio 1.0 -> K's 0.69.
            const float hold = std::clamp(punish_fit::bustHold(rdram, model), 0.0f, 1.0f);
            const float t = bustU(std::max(money, 1.0f)); // the blend weight (u, see bustU)
            const float rate = (kBustFirmHz + (kJigFull[kHz] - kBustFirmHz) * t) / kJigFull[kHz];
            const float minSize = std::min(money, 1.0f);
            const float gain = own ? jig(kGain, money) * minSize * minSize : kJigFull[kGain] * rate * rate;
            const float side = 1.0f + (kJigFull[kSide] - 1.0f) * t;
            const Vec gy = row(gameBust, 1);
            const float game = std::atan2(dot(gy, pz), dot(gy, py));
            float pitchOurs[2], yawOurs[2];
            for (int i = 0; i < 2; ++i)
            {
                const Vec D = sub(st.breast[i].tip, st.breast[i].rest);
                pitchOurs[i] = -dot(D, ey) / kBreastLen * gain;
                yawOurs[i] = dot(D, ex) / kBreastLen * gain * side;
            }
            if (stepDt > 0.0f)
            {
                const float fade = std::exp(-stepDt / 1.5f); // the peaks fade over ~1.5 s
                st.envGame = std::max(std::fabs(game), st.envGame * fade);
                st.envOurs = std::max(0.5f * (std::fabs(pitchOurs[0]) + std::fabs(pitchOurs[1])), st.envOurs * fade);
            }
            const float toGame = std::clamp(st.envGame / std::max(st.envOurs, 0.01f), 0.0f, 3.0f);
            const float yawWeight = (1.0f - t) * toGame + t;
            const int bones[2] = {kBustBone, kRightBone};
            for (int i = 0; i < 2; ++i)
            {
                const float pitch = own ? softLimit(pitchOurs[i] * (1.0f - hold), kPitchMax * kRad)
                                        : std::clamp(((1.0f - t) * game + t * pitchOurs[i]) * (1.0f - hold),
                                                     -kPitchMax * kRad, kPitchMax * kRad);
                const float yaw = own ? softLimit(yawOurs[i] * (1.0f - hold), kYawMax * kRad)
                                      : std::clamp(yawWeight * yawOurs[i] * (1.0f - hold), -kYawMax * kRad, kYawMax * kRad);
                const Mat q = mul(rotY(yaw), rotX(pitch));
                const Vec bind = i == 0 ? bBust : Vec{0, 0, 0};
                const Vec local = add(applyRot(q, sub(sub(bind, bBust), base[i])), base[i]);
                Bone b = readBone(rdram, parts[bones[i]]);
                b.set(scaled(mul(rb, q), s), add(t29, applyRot(scaled(rb, s), local)));
                writeBone(rdram, parts[bones[i]], b);
                if (i == 0) // 29 goes back at the next frame's start, like placeMoney's
                {
                    bool kept = false;
                    for (Saved &sv : g_moneySaved)
                        if (sv.address == parts[kBustBone])
                            sv.mirrored = b.m, kept = true;
                    if (!kept)
                    {
                        Saved sv;
                        sv.address = parts[kBustBone];
                        sv.original = bust.m;
                        sv.mirrored = b.m;
                        g_moneySaved.push_back(sv);
                    }
                }
            }
            g_applied = true;
        }

        void onGetMeshPtr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (GPR_U32(ctx, 31) != kTransCallSite)
                return;
            const uint32_t model = GPR_U32(ctx, 4);
            // Every model, the player's too (playable Olivia, olivia_player.h): both leave a body that isn't a woman's
            // (Gene's, Xavier's: bone 29 on the head, no rebuilt body) alone.
            placeMoney(rdram, model);
            placeJiggle(rdram, model);
            if (model != rd32(rdram, GameWork::address(GameWork::kPlayerObj)))
                return;
            if (characters::hair().back.bone < 0 && characters::hair().side.bone < 0)
                return;
            uint32_t parts[32];
            if (playerParts(rdram, parts) != 32)
                return;
            Vec bind[32];
            bindPositions(rdram, parts, 32, bind);
            placeHair(rdram, parts, bind);
        }
    }

#if defined(KAMITE_HAS_GS_MENU)
    namespace
    {
        // A slider kept in GHR.ini ([Game] <key>) once it has rested half a second (K, Nov 7: no Save button).
        void restingSlider(const Pcsx2GsUi *ui, const char *id, const char *format, float lo, float hi, const char *key,
                           std::atomic<float> &value, void (*set)(float), int64_t &changedMs)
        {
            float percent = value.load(std::memory_order_relaxed) * 100.0f;
            ui->set_next_width(340.0f);
            const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch()).count();
            if (ui->slider_float(id, &percent, lo, hi, format))
            {
                set(percent / 100.0f);
                changedMs = nowMs;
            }
            else if (changedMs >= 0 && nowMs - changedMs > 500)
            {
                changedMs = -1;
                const int saved = static_cast<int>(std::lround(value.load(std::memory_order_relaxed) * 100.0f));
                if (!dev_menu::saveIniValue("Game", key, std::to_string(saved)))
                    KM_WARN(Game, key << ": could not write GHR.ini");
            }
        }

        // The bottom of F2's NPCs tab (installed after npc_poses): Money and Cash, just the sliders (K: easter eggs).
        void drawMoneyPanel(const Pcsx2GsUi *ui)
        {
            static int64_t moneyMs = -1, cashMs = -1;
            ui->separator_text("Money");
            restingSlider(ui, "##cash", "Cash %.0f%%", 100.0f, 130.0f, "Cash", g_cash, &setCash, cashMs);
            restingSlider(ui, "##money", "Money %.0f%%", 85.0f, 130.0f, "Money", g_money, &setMoney, moneyMs);
        }
    }
#endif

    void install(PS2Runtime &runtime)
    {
#if defined(KAMITE_HAS_GS_MENU)
        ps2x::menu::addPanel("NPCs", &drawMoneyPanel);
#endif
        if (!runtime.hasFunction(kGetMeshPtr))
            return;
        FunctionHooks::instance().observeEntry(kGetMeshPtr, "bone_tweaks_pack", &onGetMeshPtr);
        const characters::Hair &h = characters::hair();
        if (const char *money = std::getenv("KAMITE_MONEY"); money && *money) // percent
        {
            setMoney(static_cast<float>(std::atof(money)) / 100.0f);
            if (std::fabs(g_money.load() - 1.0f) >= 0.001f)
                KM_INFO(Game, "Money: " << g_money.load() * 100.0f << "%");
        }
        if (const char *cash = std::getenv("KAMITE_CASH"); cash && *cash) // percent
        {
            setCash(static_cast<float>(std::atof(cash)) / 100.0f);
            if (std::fabs(g_cash.load() - 1.0f) >= 0.001f)
                KM_INFO(Game, "Cash: " << g_cash.load() * 100.0f << "%");
        }
        g_tune[0].store(h.stiffness);
        g_tune[1].store(h.damping);
        g_tune[2].store(h.gravity);
        g_tune[3].store(h.wind);
        if (hasHair())
            KM_INFO(Game, "bone tweaks: hair on");
    }

    void onFrame(uint8_t *rdram, R5900Context *, PS2Runtime *)
    {
        ++g_frame;
        g_jigDone.clear();
        if (!g_applied)
            return;
        g_applied = false;
        for (Saved &s : g_saved)
        {
            if (!inRam(s.address, 0x160u))
                continue;
            Bone now = readBone(rdram, s.address);
            if (std::memcmp(now.m.data(), s.mirrored.data(), 64) == 0) // still ours: the game's own back
                std::memcpy(rdram + ((s.address + kPartMtx) & PS2_RAM_MASK), s.original.data(), 64);
            s.address = 0;
        }
        for (const Saved &s : g_moneySaved)
        {
            if (!inRam(s.address, 0x160u))
                continue;
            Bone now = readBone(rdram, s.address);
            if (std::memcmp(now.m.data(), s.mirrored.data(), 64) == 0)
                std::memcpy(rdram + ((s.address + kPartMtx) & PS2_RAM_MASK), s.original.data(), 64);
        }
        g_moneySaved.clear();
        if (g_frame % 600u == 0u)
            for (auto it = g_jigs.begin(); it != g_jigs.end();)
                it = g_frame - it->second.seen > 600u ? g_jigs.erase(it) : std::next(it);
    }

    float money() { return g_money.load(std::memory_order_relaxed); }
    void setMoney(float scale) { g_money.store(std::clamp(scale, 0.85f, 1.3f), std::memory_order_relaxed); }
    float cash() { return g_cash.load(std::memory_order_relaxed); }
    void setCash(float scale) { g_cash.store(std::clamp(scale, 1.0f, 1.3f), std::memory_order_relaxed); }

    bool hasHair() { return characters::hair().back.bone >= 0 || characters::hair().side.bone >= 0; }
    float tuning(Tune which)
    {
        const int i = static_cast<int>(which);
        return g_tune[i].load(std::memory_order_relaxed);
    }
    void setTuning(Tune which, float value)
    {
        const int i = static_cast<int>(which);
        g_tune[i].store(value, std::memory_order_relaxed);
    }

    std::string handNodes(const uint8_t *rdram)
    {
        const uint32_t model = rd32(rdram, GameWork::address(GameWork::kPlayerObj));
        if (!inRam(model, 0x2E4u))
            return "(no player)";
        std::string out;
        uint32_t node = rd32(rdram, model + kMeshHead);
        for (int i = 0; i < 64 && inRam(node, 0x418u); ++i)
        {
            const uint32_t info = rd32(rdram, node + kNodeInfo);
            if (inRam(info, 0x10u))
            {
                char name[9] = {};
                std::memcpy(name, rdram + ((info + kInfoName) & PS2_RAM_MASK), 8);
                const bool hidden = (rd32(rdram, node + kNodeFlags) & 1u) != 0u;
                if (std::strstr(name, "H_") && !hidden)
                    out += std::string(out.empty() ? "" : " ") + name;
            }
            node = rd32(rdram, node + kNodeNext);
        }
        return out.empty() ? "(none)" : out;
    }
}
