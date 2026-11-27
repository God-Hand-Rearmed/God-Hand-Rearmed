#include "kamite/strafe_motions.h"

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

        // The hips lowered by d (the foot targets stay: the knees bend more; K's run, Nov 26: the longer steps in reach).
        struct Drop : Src
        {
            const Src &m;
            float d;
            Drop(const Src &m_, float d_) : m(m_), d(d_) { span = m.span; }
            bool rot(uint32_t b, float t, Quat &q) const override { return m.rot(b, t, q); }
            bool tr(uint32_t b, float t, float v[3]) const override
            {
                if (!m.tr(b, t, v))
                    return false;
                if (b == 0u)
                    v[1] -= d;
                return true;
            }
            void root(float t, float v[3]) const override { m.root(t, v); }
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
        // scaled about their means to `travel` per loop.
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
            const float k = travel / native;
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
        constexpr uint32_t kWalk = 3450u, kRun = 2044u; // em00: his right; his left = mirrored
        constexpr uint32_t kBack = 23u, kGuard = 9u;     // pl00: his back-pedal, his idle (the guard)
        constexpr uint32_t kHashes[4] = {0xB9D8E5E3u, 0xE14A1A60u, 0xBD72D879u, 0x4CD3A493u};
        constexpr float kWalkSize = 0.998343871f, kRunSize = 1.167537936f, kRunHips = 0.03f;
        constexpr int kWalkSpan = 56, kRunSpan = 13; // his walk (pl00 21) and run (25 / 428)
        constexpr float kWalkSpeed = 1.74f, kRunSpeed = 3.0f;

        // The other slow strafes (strafe_round4.py on the US disc, Nov 27): em00 index, hash, size, his right's and
        // the mirrored left's touchdowns (feet 24, 20, 24, 20, 24) onto the walk's.
        struct Candidate
        {
            WalkSource source;
            uint32_t index, hash;
            float size, right[5], left[5];
        };
        constexpr Candidate kCandidates[] = {
            {Cell1, 50u, 0xF636A2A1u, 0.993350972f, {27.5f, 47.75f, 69.75f, 89.75f, 109.5f}, {7.75f, 27.5f, 47.75f, 69.75f, 89.75f}},
            {Cell7, 1604u, 0xA04F6E34u, 1.062444885f, {23.5f, 42.5f, 69.75f, 90.75f, 119.5f}, {42.5f, 69.75f, 90.75f, 119.5f, 138.5f}},
            {Cell13, 2438u, 0x416F340Eu, 0.996367880f, {15.5f, 35.25f, 63.5f, 79.5f, 115.5f}, {35.25f, 63.5f, 79.5f, 115.5f, 135.25f}},
        };
    }

    bool build(const Fetch &em00, const Fetch &pl00, Set &set, std::string &why)
    {
        auto &out = set.motions;
        const struct
        {
            const Fetch &fetch;
            const char *file;
            uint32_t index, hash;
        } sources[4] = {{em00, "em00.dat", kWalk, kHashes[0]},
                        {em00, "em00.dat", kRun, kHashes[1]},
                        {pl00, "pl00.dat", kBack, kHashes[2]},
                        {pl00, "pl00.dat", kGuard, kHashes[3]}};
        Game game[4];
        for (int i = 0; i < 4; ++i)
        {
            const std::string name = std::string(sources[i].file) + " motion " + std::to_string(sources[i].index);
            if (!sources[i].fetch(sources[i].index, game[i].bytes) || !mot_sample::valid(game[i].bytes.data(), game[i].bytes.size()))
            {
                why = name + " not read";
                return false;
            }
            if (fnv(game[i].bytes) != sources[i].hash)
            {
                why = name + " is not the one the recipe was made on";
                return false;
            }
            game[i].span = static_cast<float>(mot_sample::frames(game[i].bytes.data())) - 1.0f;
        }
        // walk: 3450 (cell 16's right) and it mirrored, on his walk's grid (feet 24, 20, 24, 20, 24)
        const Scaled walkRight(game[0], kWalkSize);
        const Mirror walkLeft(walkRight);
        const std::vector<float> walkRef = {22.25f, 36.75f, 50.25f, 64.75f, 78.25f};
        const float walkTravel = kWalkSpeed * kWalkSpan / 30.0f;
        if (!bake(walkRight, {Warp{walkRef, {19.5f, 39.5f, 68.5f, 90.5f, 118.5f}, float(kWalkSpan)}}, kWalkSpan, walkTravel, false,
                  out[WalkRight]) ||
            !bake(walkLeft, {Warp{walkRef, {39.5f, 68.5f, 90.5f, 118.5f, 138.5f}, float(kWalkSpan)}}, kWalkSpan, walkTravel, false,
                  out[WalkLeft]))
        {
            why = "the walk didn't build";
            return false;
        }
        // run: 2044 (cell 11's right: the boxing shuffle) and it mirrored, hips 3 cm lower, one of its cycles on one of
        // his run's (feet 24, 20, 24), 1.85x its own pace
        const Scaled sized(game[1], kRunSize);
        const Drop runRight(sized, kRunHips);
        const Mirror runLeft(runRight);
        const std::vector<float> runRef = {10.5f, 17.0f, 23.5f};
        const float runTravel = kRunSpeed * kRunSpan / 30.0f;
        if (!bake(runRight, {Warp{runRef, {17.75f, 27.75f, 41.75f}, float(kRunSpan)}}, kRunSpan, runTravel, false, out[RunRight]) ||
            !bake(runLeft, {Warp{runRef, {3.75f, 17.75f, 27.75f}, float(kRunSpan)}}, kRunSpan, runTravel, false, out[RunLeft]))
        {
            why = "the run didn't build";
            return false;
        }
        // back: his back-pedal's foot-24 landings onto the walk's (1.74 m/s), and its two cycles averaged onto one of
        // his run's (3.0 m/s); along z
        if (!bake(game[2], {Warp{{22.25f, 50.25f, 78.25f}, {6.75f, 27.75f, 48.75f}, float(kWalkSpan)}}, kWalkSpan, walkTravel, false,
                  out[BackWalk], 2) ||
            !bake(game[2], {Warp{{10.5f, 23.5f}, {6.75f, 27.75f}, float(kRunSpan)}, Warp{{10.5f, 23.5f}, {27.75f, 48.75f}, float(kRunSpan)}},
                  kRunSpan, runTravel, true, out[BackRun], 2))
        {
            why = "the back-pedal didn't build";
            return false;
        }
        set.walks[Cell16] = {out[WalkLeft], out[WalkRight]};
        set.walksBuilt = 1u << Cell16;
        for (const Candidate &c : kCandidates) // optional: one that doesn't build is left out of the choice
        {
            Game g;
            if (!em00(c.index, g.bytes) || !mot_sample::valid(g.bytes.data(), g.bytes.size()) || fnv(g.bytes) != c.hash)
                continue;
            g.span = static_cast<float>(mot_sample::frames(g.bytes.data())) - 1.0f;
            const Scaled right(g, c.size);
            const Mirror left(right);
            auto &w = set.walks[c.source];
            if (bake(right, {Warp{walkRef, std::vector<float>(c.right, c.right + 5), float(kWalkSpan)}}, kWalkSpan, walkTravel, false, w[1]) &&
                bake(left, {Warp{walkRef, std::vector<float>(c.left, c.left + 5), float(kWalkSpan)}}, kWalkSpan, walkTravel, false, w[0]))
                set.walksBuilt |= 1u << c.source;
        }
        set.guarded = 0u;
        for (uint32_t b = 0; b < static_cast<uint32_t>(kBones); ++b)
            if (game[3].rot(b, 0.0f, set.guard[b]))
                set.guarded |= 1u << b;
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
