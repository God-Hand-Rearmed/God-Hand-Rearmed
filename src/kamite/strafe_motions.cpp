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

        bool upper(uint32_t b) { return (b >= 1u && b <= 16u) || b == 26u || b == 29u; }

        // The frame of foot target 24's lowest point (strafe_mix.plant).
        float lowest(const Src &m)
        {
            float best = 1e30f, at = 0.0f;
            for (int f = 0; f < static_cast<int>(m.span); ++f)
            {
                float v[3];
                if (m.tr(24u, static_cast<float>(f), v) && v[1] < best)
                    best = v[1], at = static_cast<float>(f);
            }
            return at;
        }

        // legs' hips, legs and root + upper's spine, arms and head, the upper loop once per `per` of the legs' loop,
        // phased on foot 24's lowest frame of each.
        struct Mix : Src
        {
            const Src &legs, &up;
            float pl, pu, sl, su;
            Mix(const Src &legs_, const Src &up_, float per) : legs(legs_), up(up_)
            {
                span = legs.span;
                pl = lowest(legs), pu = lowest(up), sl = legs.span * per, su = up.span;
            }
            float tu(float t) const { return wrap((t - pl) / sl * su + pu, su); }
            bool rot(uint32_t b, float t, Quat &q) const override { return upper(b) ? up.rot(b, tu(t), q) : legs.rot(b, t, q); }
            bool tr(uint32_t b, float t, float v[3]) const override { return legs.tr(b, t, v); }
            void root(float t, float v[3]) const override { legs.root(t, v); }
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
        bool bake(const Src &m, const std::vector<Warp> &warps, int span, float travel, bool close, Motion &out)
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
                out.root[span][1] = out.root[0][1], out.root[span][2] = out.root[0][2];
            }
            for (int j = 0; j < kTranslated; ++j)
                for (int c = 0; c < 3; ++c)
                {
                    float sum = 0.0f;
                    for (int f = 0; f < span; ++f)
                        sum += out.tr[f * kTranslated + j][c];
                    out.mean[j][c] = sum / static_cast<float>(span);
                }
            const float native = std::fabs(out.root[span][0] - out.root[0][0]);
            if (!(native > 1e-3f))
                return false;
            const float k = travel / native;
            for (size_t f = 0; f < frames; ++f)
            {
                for (int j = 0; j < kTranslated; ++j)
                    out.tr[f * kTranslated + j][0] = out.mean[j][0] + k * (out.tr[f * kTranslated + j][0] - out.mean[j][0]);
                out.root[f][0] *= k;
            }
            out.travel = out.root[span][0] - out.root[0][0];
            return true;
        }

        // The recipe (strafe_bake.py on the US disc, Nov 26): the source sizes and every touchdown.
        constexpr uint32_t kWalkLeft = 3452u, kWalkRight = 3450u, kRunLegs = 837u, kRunUpper = 2044u;
        constexpr uint32_t kHashes[4] = {0x7525C81Au, 0xB9D8E5E3u, 0x4AE9D2D4u, 0xE14A1A60u};
        constexpr float kWalkSize = 0.998343871f, kRunSize = 1.440726171f;
        constexpr int kWalkSpan = 56, kRunSpan = 13; // his walk (pl00 21) and run (25 / 428)
        constexpr float kWalkSpeed = 1.74f, kRunSpeed = 4.0f;
    }

    bool build(const Fetch &fetch, std::array<Motion, kSides> &out, std::string &why)
    {
        const uint32_t indices[4] = {kWalkLeft, kWalkRight, kRunLegs, kRunUpper};
        Game game[4];
        for (int i = 0; i < 4; ++i)
        {
            if (!fetch(indices[i], game[i].bytes) || !mot_sample::valid(game[i].bytes.data(), game[i].bytes.size()))
            {
                why = "em00.dat motion " + std::to_string(indices[i]) + " not read";
                return false;
            }
            if (fnv(game[i].bytes) != kHashes[i])
            {
                why = "em00.dat motion " + std::to_string(indices[i]) + " is not the one the recipe was made on";
                return false;
            }
            game[i].span = static_cast<float>(mot_sample::frames(game[i].bytes.data())) - 1.0f;
        }
        const Scaled walkLeft(game[0], kWalkSize), walkRight(game[1], kWalkSize), legs(game[2], kRunSize);
        const std::vector<float> walkRef = {22.25f, 36.75f, 50.25f, 64.75f, 78.25f}; // feet 24, 20, 24, 20, 24
        const float walkTravel = kWalkSpeed * kWalkSpan / 30.0f;
        if (!bake(walkLeft, {Warp{walkRef, {38.0f, 62.75f, 82.5f, 109.5f, 126.0f}, float(kWalkSpan)}}, kWalkSpan, walkTravel,
                  false, out[WalkLeft]) ||
            !bake(walkRight, {Warp{walkRef, {19.5f, 39.5f, 68.5f, 90.5f, 118.5f}, float(kWalkSpan)}}, kWalkSpan, walkTravel,
                  false, out[WalkRight]))
        {
            why = "the walk didn't build";
            return false;
        }
        const Mix right(legs, game[3], 0.5f);
        const Mirror left(right);
        const std::vector<float> runRef = {10.5f, 17.0f, 23.5f}; // feet 24, 20, 24
        const float runTravel = kRunSpeed * kRunSpan / 30.0f;
        if (!bake(right, {Warp{runRef, {4.75f, 11.25f, 18.75f}, float(kRunSpan)}, Warp{runRef, {18.75f, 25.0f, 32.75f}, float(kRunSpan)}},
                  kRunSpan, runTravel, true, out[RunRight]) ||
            !bake(left, {Warp{runRef, {11.25f, 18.75f, 25.0f}, float(kRunSpan)}, Warp{runRef, {25.0f, 32.75f, 39.25f}, float(kRunSpan)}},
                  kRunSpan, runTravel, true, out[RunLeft]))
        {
            why = "the run didn't build";
            return false;
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
            out.tr[j][0] = m.mean[j][0] + stride * (out.tr[j][0] - m.mean[j][0]);
        }
    }

    float rootX(const Motion &m, float t, float stride)
    {
        const float span = static_cast<float>(m.span), n = std::floor(t / span), f = t - n * span;
        const int f0 = std::min(static_cast<int>(f), m.span - 1);
        const float u = f - static_cast<float>(f0);
        const float x = m.root[f0][0] + (m.root[f0 + 1][0] - m.root[f0][0]) * u;
        return (x + n * m.travel - m.root[0][0]) * stride;
    }
}
