#include "kamite/strafe_motions.h"
#include "kamite/strafe_run_data.h"

#include <algorithm>
#include <cmath>

namespace kamite::strafe_motions
{
    namespace
    {
        using mot_sample::Quat;

        float wrap(float t, float span) { return t - std::floor(t / span) * span; }

        uint32_t fnv(const std::vector<uint8_t> &bytes)
        {
            uint32_t h = 2166136261u;
            for (const uint8_t b : bytes)
                h = (h ^ b) * 16777619u;
            return h;
        }

        // A motion as strafe_bake.py's classes see it: a part's rotation (whole frames slerped), translation and the
        // root (unwound) at any frame.
        struct Src
        {
            float span = 0.0f;
            virtual ~Src() = default;
            virtual bool rot(uint32_t b, float t, Quat &q) const = 0;
            virtual bool tr(uint32_t b, float t, float v[3]) const = 0;
            virtual void root(float t, float v[3]) const = 0;
        };

        struct Game : Src
        {
            std::vector<uint8_t> bytes;
            bool keyed(uint32_t b, uint32_t c) const
            {
                float v = 0.0f;
                return mot_sample::sample(bytes.data(), bytes.size(), b, c, 0.0f, v);
            }
            float get(uint32_t b, uint32_t c, float t) const
            {
                float v = 0.0f;
                return mot_sample::sample(bytes.data(), bytes.size(), b, c, t, v) ? v : 0.0f;
            }
            bool rot(uint32_t b, float t, Quat &q) const override
            {
                if (!keyed(b, 3u) && !keyed(b, 4u) && !keyed(b, 5u))
                    return false;
                t = wrap(t, span);
                const float f0 = std::floor(t), u = t - f0;
                const float e0[3] = {get(b, 3u, f0), get(b, 4u, f0), get(b, 5u, f0)};
                q = mot_sample::eulerQuat(e0);
                if (u >= 1e-6f)
                {
                    const float e1[3] = {get(b, 3u, f0 + 1.0f), get(b, 4u, f0 + 1.0f), get(b, 5u, f0 + 1.0f)};
                    q = mot_sample::slerp(q, mot_sample::eulerQuat(e1), u);
                }
                return true;
            }
            bool tr(uint32_t b, float t, float v[3]) const override
            {
                if (!keyed(b, 0u))
                    return false;
                t = wrap(t, span);
                for (uint32_t c = 0; c < 3u; ++c)
                    v[c] = get(b, c, t);
                return true;
            }
            void root(float t, float v[3]) const override
            {
                const float n = std::floor(t / span), f = t - n * span;
                for (uint32_t c = 0; c < 3u; ++c)
                    v[c] = get(255u, c, f) + n * (get(255u, c, span) - get(255u, c, 0.0f));
            }
        };

        // Sized onto Gene: translations (the hips, foot targets) and the root by k.
        struct Scaled : Src
        {
            const Src &m;
            float k;
            Scaled(const Src &m_, float k_) : m(m_), k(k_) { span = m.span; }
            bool rot(uint32_t b, float t, Quat &q) const override { return m.rot(b, t, q); }
            bool tr(uint32_t b, float t, float v[3]) const override
            {
                if (!m.tr(b, t, v))
                    return false;
                for (int c = 0; c < 3; ++c)
                    v[c] *= k;
                return true;
            }
            void root(float t, float v[3]) const override
            {
                m.root(t, v);
                for (int c = 0; c < 3; ++c)
                    v[c] *= k;
            }
        };

        // Left for right: his right arm 5-10 / left 11-16, right leg 18-21 / left 22-25 swapped, x mirrored.
        uint32_t swapped(uint32_t b)
        {
            if (b >= 5u && b <= 10u)
                return b + 6u;
            if (b >= 11u && b <= 16u)
                return b - 6u;
            if (b >= 18u && b <= 21u)
                return b + 4u;
            if (b >= 22u && b <= 25u)
                return b - 4u;
            return b;
        }
        struct Mirror : Src
        {
            const Src &m;
            explicit Mirror(const Src &m_) : m(m_) { span = m.span; }
            bool rot(uint32_t b, float t, Quat &q) const override
            {
                if (!m.rot(swapped(b), t, q))
                    return false;
                q.y = -q.y, q.z = -q.z; // S R S, S = diag(-1, 1, 1)
                return true;
            }
            bool tr(uint32_t b, float t, float v[3]) const override
            {
                if (!m.tr(swapped(b), t, v))
                    return false;
                v[0] = -v[0];
                return true;
            }
            void root(float t, float v[3]) const override
            {
                m.root(t, v);
                v[0] = -v[0];
            }
        };

        // A motion played backward (K, Nov 27: the side-run flipped, the other shoulder leading: backward it travels the
        // other way, so the mirror backward goes the same way as the clip with the body turned the other way).
        struct Reverse : Src
        {
            const Src &m;
            explicit Reverse(const Src &m_) : m(m_) { span = m.span; }
            bool rot(uint32_t b, float t, Quat &q) const override { return m.rot(b, -t, q); }
            bool tr(uint32_t b, float t, float v[3]) const override { return m.tr(b, -t, v); }
            void root(float t, float v[3]) const override
            {
                float a[3], z[3];
                m.root(-t, a), m.root(0.0f, z);
                for (int c = 0; c < 3; ++c)
                    v[c] = a[c] - z[c];
            }
        };

        // A motion on a reference grid: ref anchors (rising, the last = the first + refSpan) onto the source's (the
        // last = the same foot's next landing), piecewise linear, continuous over [0, refSpan].
        struct Warp
        {
            std::vector<float> ref, src;
            float span = 0.0f;
            float s(float tau) const
            {
                float t = tau, back = 0.0f;
                while (t < ref.front())
                    t += span, back += 1.0f;
                while (t >= ref.back())
                    t -= span, back -= 1.0f;
                size_t k = 0;
                while (k + 2 < ref.size() && ref[k + 1] <= t)
                    ++k;
                const float u = (t - ref[k]) / (ref[k + 1] - ref[k]);
                return src[k] + u * (src[k + 1] - src[k]) - back * (src.back() - src.front());
            }
        };

        // strafe_bake.py bake(): the sources (one, or two windows a cycle apart, averaged) on frames 0..span, the steps
        // scaled about their means to `travel` per loop (0: kept as animated).
        bool bake(const Src &m, const std::vector<Warp> &warps, int span, float travel, bool close, Motion &out, int axis = 0)
        {
            const size_t frames = static_cast<size_t>(span) + 1u, n = warps.size();
            out.span = span;
            out.rot.assign(frames * kBones, Quat{});
            out.tr.assign(frames * kTranslated, {});
            out.root.assign(frames, {});
            out.rotated = 0u;
            std::vector<std::array<float, 3>> start(n);
            for (size_t i = 0; i < n; ++i)
                m.root(warps[i].s(0.0f), start[i].data());
            for (size_t f = 0; f < frames; ++f)
            {
                for (uint32_t b = 0; b < static_cast<uint32_t>(kBones); ++b)
                {
                    Quat q0, q1;
                    if (!m.rot(b, warps[0].s(static_cast<float>(f)), q0))
                        continue;
                    if (n > 1u && m.rot(b, warps[1].s(static_cast<float>(f)), q1))
                        q0 = mot_sample::slerp(q0, q1, 0.5f);
                    out.rot[f * kBones + b] = q0;
                    out.rotated |= 1u << b;
                }
                for (int j = 0; j < kTranslated; ++j)
                {
                    std::array<float, 3> sum{};
                    for (size_t i = 0; i < n; ++i)
                    {
                        float v[3];
                        if (!m.tr(kTranslatedBones[j], warps[i].s(static_cast<float>(f)), v))
                            return false;
                        for (int c = 0; c < 3; ++c)
                            sum[c] += v[c] / static_cast<float>(n);
                    }
                    out.tr[f * kTranslated + j] = sum;
                }
                std::array<float, 3> r{};
                for (size_t i = 0; i < n; ++i)
                {
                    float v[3];
                    m.root(warps[i].s(static_cast<float>(f)), v);
                    for (int c = 0; c < 3; ++c)
                        r[c] += (v[c] - start[i][c]) / static_cast<float>(n);
                }
                out.root[f] = r;
            }
            if (close)
            {
                // the averaged cycle's last frame = its first (the two windows tile the loop only to a frame's fraction)
                for (int b = 0; b < kBones; ++b)
                    out.rot[span * kBones + b] = out.rot[b];
                for (int j = 0; j < kTranslated; ++j)
                    out.tr[span * kTranslated + j] = out.tr[j];
                for (int c = 0; c < 3; ++c)
                    if (c != axis)
                        out.root[span][c] = out.root[0][c];
            }
            for (int j = 0; j < kTranslated; ++j)
                for (int c = 0; c < 3; ++c)
                {
                    float sum = 0.0f;
                    for (int f = 0; f < span; ++f)
                        sum += out.tr[f * kTranslated + j][c];
                    out.mean[j][c] = sum / static_cast<float>(span);
                }
            const float native = std::fabs(out.root[span][axis] - out.root[0][axis]);
            if (!(native > 1e-3f))
                return false;
            const float k = travel > 0.0f ? travel / native : 1.0f;
            out.native = native;
            out.sourceFrames = warps[0].src.back() - warps[0].src.front();
            out.sourceRate = 1.0f;
            for (size_t f = 0; f < frames; ++f)
            {
                for (int j = 0; j < kTranslated; ++j)
                    out.tr[f * kTranslated + j][axis] = out.mean[j][axis] + k * (out.tr[f * kTranslated + j][axis] - out.mean[j][axis]);
                out.root[f][axis] *= k;
            }
            for (int b = 0; b < kBones; ++b) // each part's mean rotation (sign-aligned to frame 0's)
            {
                const Quat &q0 = out.rot[b];
                Quat sum{0.0f, 0.0f, 0.0f, 0.0f};
                for (int f = 0; f < span; ++f)
                {
                    const Quat &q = out.rot[f * kBones + b];
                    const float sgn = q.w * q0.w + q.x * q0.x + q.y * q0.y + q.z * q0.z < 0.0f ? -1.0f : 1.0f;
                    sum.w += sgn * q.w, sum.x += sgn * q.x, sum.y += sgn * q.y, sum.z += sgn * q.z;
                }
                const float n = std::sqrt(sum.w * sum.w + sum.x * sum.x + sum.y * sum.y + sum.z * sum.z);
                out.armMean[b] = n > 0.0f ? Quat{sum.w / n, sum.x / n, sum.y / n, sum.z / n} : q0;
            }
            out.axis = axis;
            out.travel = out.root[span][axis] - out.root[0][axis];
            return true;
        }

        // The recipe (strafe_round2.py on the US disc; K's picks W2 / R2, Nov 26): the source sizes and every touchdown.
        constexpr uint32_t kWalk = 3450u, kWalkHash = 0xB9D8E5E3u; // em00: cell 16's right; his left = mirrored
        constexpr float kWalkSize = 0.998343871f;
        constexpr int kWalkSpan = 56, kRunSpan = 13, kBackSpan = 42; // his walk (pl00 21), run (25 / 428), back-pedal (23)
        constexpr float kWalkSpeed = 1.74f, kRunSpeed = 3.0f, kBackSpeed = 1.97f;

        // The other slow strafes (strafe_round4.py on the US disc, Nov 27): em00 index, hash, size, his right's and
        // the mirrored left's touchdowns (feet 24, 20, 24, 20, 24) onto the walk's.
        struct Candidate
        {
            WalkSource source;
            uint32_t index, hash;
            float size, right[5], left[5];
        };
        // The run's sideways stride at 3 / 9 (K, Nov 27: the side-run K generated, its own arms; the Mixamo jog and
        // cell 11 gone): retargeted onto Gene offline (strafe_run_data.h), his right as made: the locals of every bone
        // it maps, the hips' and foot targets' translations, the root along x.
        struct SideRun : Src
        {
            SideRun() { span = static_cast<float>(strafe_run::kFrames - 1); }
            static int index(uint32_t b)
            {
                for (int i = 0; i < strafe_run::kBoneCount; ++i)
                    if (strafe_run::kBones[i] == static_cast<int>(b))
                        return i;
                return -1;
            }
            static void at(float t, float span, int &f0, float &u)
            {
                f0 = std::min(static_cast<int>(t), strafe_run::kFrames - 2);
                u = t - static_cast<float>(f0);
                (void)span;
            }
            bool rot(uint32_t b, float t, Quat &q) const override
            {
                const int k = index(b);
                if (k < 0)
                    return false;
                int f0;
                float u;
                at(wrap(t, span), span, f0, u);
                const float *a = strafe_run::kRot[f0][k], *c = strafe_run::kRot[f0 + 1][k];
                q = mot_sample::slerp(Quat{a[0], a[1], a[2], a[3]}, Quat{c[0], c[1], c[2], c[3]}, u);
                return true;
            }
            bool tr(uint32_t b, float t, float v[3]) const override
            {
                const int j = b == 0u ? 0 : b == 20u ? 1 : b == 24u ? 2 : -1;
                if (j < 0)
                    return false;
                int f0;
                float u;
                at(wrap(t, span), span, f0, u);
                for (int c = 0; c < 3; ++c)
                    v[c] = strafe_run::kTr[f0][j][c] + (strafe_run::kTr[f0 + 1][j][c] - strafe_run::kTr[f0][j][c]) * u;
                return true;
            }
            void root(float t, float v[3]) const override
            {
                const float n = std::floor(t / span), f = t - n * span;
                int f0;
                float u;
                at(f, span, f0, u);
                v[0] = strafe_run::kRootX[f0] + (strafe_run::kRootX[f0 + 1] - strafe_run::kRootX[f0]) * u +
                       n * (strafe_run::kRootX[strafe_run::kFrames - 1] - strafe_run::kRootX[0]);
                v[1] = v[2] = 0.0f;
            }
        };
        constexpr Candidate kCandidates[] = {
            {Cell7, 1604u, 0xA04F6E34u, 1.062444885f, {23.5f, 42.5f, 69.75f, 90.75f, 119.5f}, {42.5f, 69.75f, 90.75f, 119.5f, 138.5f}},
            {Cell13, 2438u, 0x416F340Eu, 0.996367880f, {15.5f, 35.25f, 63.5f, 79.5f, 115.5f}, {35.25f, 63.5f, 79.5f, 115.5f, 135.25f}},
        };
    }

    bool build(const Fetch &em00, Set &set, std::string &why)
    {
        auto &out = set.motions;
        Game game;
        if (!em00(kWalk, game.bytes) || !mot_sample::valid(game.bytes.data(), game.bytes.size()))
        {
            why = "em00.dat motion " + std::to_string(kWalk) + " not read";
            return false;
        }
        if (fnv(game.bytes) != kWalkHash)
        {
            why = "em00.dat motion " + std::to_string(kWalk) + " is not the one the recipe was made on";
            return false;
        }
        game.span = static_cast<float>(mot_sample::frames(game.bytes.data())) - 1.0f;
        const std::vector<float> walkRef = {22.25f, 36.75f, 50.25f, 64.75f, 78.25f}; // feet 24, 20, 24, 20, 24
        const std::vector<float> runRef = {10.5f, 17.0f, 23.5f};                     // feet 24, 20, 24
        const std::vector<float> backRef = {6.75f, 17.0f, 27.75f, 38.0f, 48.75f};     // feet 24, 20, 24, 20, 24 (pl00 23)
        const float walkTravel = kWalkSpeed * kWalkSpan / 30.0f, runTravel = kRunSpeed * kRunSpan / 30.0f,
                    backTravel = kBackSpeed * kBackSpan / 30.0f;
        // A side-step (his right, and it mirrored for his left) on his walk's grid, its two cycles averaged onto one
        // of his run's (K, Nov 27: the slow strafes are the strafes at every speed; 11 is out), and on his
        // back-pedal's (K, Nov 27: the game's own back-pedal plays past 112.5 deg; the side-steps blend into it).
        auto strafe = [&](const Src &right, const float r[5], const float l[5], std::array<Motion, 2> &walk, std::array<Motion, 2> &run,
                          std::array<Motion, 2> &back) {
            const Mirror left(right);
            const Src *src[2] = {&left, &right};
            const float *at[2] = {l, r};
            for (int k = 0; k < 2; ++k)
                if (!bake(*src[k], {Warp{walkRef, std::vector<float>(at[k], at[k] + 5), float(kWalkSpan)}}, kWalkSpan, walkTravel, false, walk[k]) ||
                    !bake(*src[k], {Warp{runRef, {at[k][0], at[k][1], at[k][2]}, float(kRunSpan)}, Warp{runRef, {at[k][2], at[k][3], at[k][4]}, float(kRunSpan)}},
                          kRunSpan, runTravel, true, run[k]) ||
                    !bake(*src[k], {Warp{backRef, std::vector<float>(at[k], at[k] + 5), float(kBackSpan)}}, kBackSpan, backTravel, false, back[k]))
                    return false;
            return true;
        };
        const Scaled walkRight(game, kWalkSize); // 3450 (cell 16)
        const float r16[5] = {19.5f, 39.5f, 68.5f, 90.5f, 118.5f}, l16[5] = {39.5f, 68.5f, 90.5f, 118.5f, 138.5f};
        if (!strafe(walkRight, r16, l16, set.walks[Cell16], set.runs[Cell16], set.backs[Cell16]))
        {
            why = "the side-steps didn't build";
            return false;
        }
        out[WalkLeft] = set.walks[Cell16][0], out[WalkRight] = set.walks[Cell16][1];
        out[RunLeft] = set.runs[Cell16][0], out[RunRight] = set.runs[Cell16][1];
        set.walksBuilt = 1u << Cell16;
        for (const Candidate &c : kCandidates) // optional: one that doesn't build is left out of the choice
        {
            Game g;
            if (!em00(c.index, g.bytes) || !mot_sample::valid(g.bytes.data(), g.bytes.size()) || fnv(g.bytes) != c.hash)
                continue;
            g.span = static_cast<float>(mot_sample::frames(g.bytes.data())) - 1.0f;
            const Scaled right(g, c.size);
            if (strafe(right, c.right, c.left, set.walks[c.source], set.runs[c.source], set.backs[c.source]))
                set.walksBuilt |= 1u << c.source;
        }
        { // the run's sideways stride at 3 / 9 (his right as made, mirrored for his left), at its own steps; flipped:
          // backward (his right = the mirror backward, his left = the clip backward)
            const SideRun right;
            const Mirror left(right);
            const Reverse flipRight(left), flipLeft(right);
            // One cycle on one of his run's, its first and last touchdowns on his foot 24's and the middle one where it
            // falls in its own cycle (K, Nov 27: on his run's even middle the step-together's two steps played at two
            // speeds, 1.6x apart: a pause each cycle), so the whole cycle plays at one speed.
            auto one = [&](const Src &src, const float touch[3], float split, Motion &out) {
                const std::vector<float> ref = {runRef[0], runRef[0] + split * float(kRunSpan), runRef[0] + float(kRunSpan)};
                return bake(src, {Warp{ref, {touch[0], touch[1], touch[2]}, float(kRunSpan)}}, kRunSpan, 0.0f, false, out);
            };
            set.sideRunBuilt = one(left, strafe_run::kLeftTouch, strafe_run::kLeftSplit, set.sideRun[0][0]) &&
                               one(right, strafe_run::kRightTouch, strafe_run::kRightSplit, set.sideRun[0][1]) &&
                               one(flipLeft, strafe_run::kFlipLeftTouch, strafe_run::kFlipLeftSplit, set.sideRun[1][0]) &&
                               one(flipRight, strafe_run::kFlipRightTouch, strafe_run::kFlipRightSplit, set.sideRun[1][1]);
            for (auto &pair : set.sideRun)
                for (Motion &m : pair)
                    m.sourceRate = strafe_run::kSourceRate;
        }
        return true;
    }

    void sample(const Motion &m, float t, float stride, Pose &out)
    {
        t = wrap(t, static_cast<float>(m.span));
        const int f0 = std::min(static_cast<int>(t), m.span - 1), f1 = f0 + 1;
        const float u = t - static_cast<float>(f0);
        for (int b = 0; b < kBones; ++b)
            out.rot[b] = (m.rotated >> b) & 1u ? mot_sample::slerp(m.rot[f0 * kBones + b], m.rot[f1 * kBones + b], u) : Quat{};
        for (int j = 0; j < kTranslated; ++j)
        {
            const auto &a = m.tr[f0 * kTranslated + j], &b = m.tr[f1 * kTranslated + j];
            for (int c = 0; c < 3; ++c)
                out.tr[j][c] = a[c] + (b[c] - a[c]) * u;
            out.tr[j][m.axis] = m.mean[j][m.axis] + stride * (out.tr[j][m.axis] - m.mean[j][m.axis]);
        }
    }

    float rootAt(const Motion &m, float t, float stride)
    {
        const float span = static_cast<float>(m.span), n = std::floor(t / span), f = t - n * span;
        const int f0 = std::min(static_cast<int>(f), m.span - 1), a = m.axis;
        const float u = f - static_cast<float>(f0);
        const float x = m.root[f0][a] + (m.root[f0 + 1][a] - m.root[f0][a]) * u;
        return (x + n * m.travel - m.root[0][a]) * stride;
    }
}
