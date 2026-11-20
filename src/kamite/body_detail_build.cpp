// The women's detailed bust / seat, built from the disc (see body_detail_build.h). A port of the signed-off offline
// prototypes (tools/models/body_detail: xfer.subdivide / transfer3, transplant.transplant / build2, strip.py,
// gamemodel.restrip_mdb), step for step, so the two can be compared.
#include "kamite/body_detail_build.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace kamite::body_detail
{
    namespace
    {
        // ---- bytes -------------------------------------------------------------------------------------------------
        uint32_t r32(const uint8_t *p)
        {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }
        uint16_t r16(const uint8_t *p)
        {
            uint16_t v;
            std::memcpy(&v, p, 2);
            return v;
        }
        float rf(const uint8_t *p)
        {
            float v;
            std::memcpy(&v, p, 4);
            return v;
        }
        void w32(std::vector<uint8_t> &b, size_t at, uint32_t v) { std::memcpy(b.data() + at, &v, 4); }
        void w16(std::vector<uint8_t> &b, size_t at, uint16_t v) { std::memcpy(b.data() + at, &v, 2); }
        void put32(std::vector<uint8_t> &b, uint32_t v)
        {
            const size_t at = b.size();
            b.resize(at + 4);
            w32(b, at, v);
        }
        void putf(std::vector<uint8_t> &b, float v)
        {
            uint32_t u;
            std::memcpy(&u, &v, 4);
            put32(b, u);
        }
        void pad16(std::vector<uint8_t> &b)
        {
            while (b.size() % 16u)
                b.push_back(0u);
        }

        // ---- vectors -----------------------------------------------------------------------------------------------
        D3 operator+(D3 a, D3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
        D3 operator-(D3 a, D3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
        D3 operator*(D3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
        double dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        D3 cross(D3 a, D3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
        double len(D3 a) { return std::sqrt(dot(a, a)); }
        double dist2(D3 a, D3 b) { return dot(a - b, a - b); }

        // ---- model files (tools/models/gamemodel.py) ---------------------------------------------------------------
        bool inside(size_t size, size_t at, size_t bytes) { return at <= size && bytes <= size - at; }

        struct NodeRef
        {
            uint32_t record = 0; // the node's 112-byte record
            uint32_t mdb = 0;
            char name[9] = {};
            uint32_t flags = 0;  // record +0x34: cMeshInfo.flags (0x01000000 = a distance mesh)
        };

        bool readNodes(const uint8_t *scr, size_t size, std::vector<NodeRef> &out)
        {
            if (size < 16 || std::memcmp(scr, "scr", 4) != 0)
                return false;
            const uint32_t n = r32(scr + 8);
            if (n == 0u || n > 256u || !inside(size, 16, 4u * n))
                return false;
            for (uint32_t i = 0; i < n; ++i)
            {
                NodeRef r;
                r.record = r32(scr + 16 + 4 * i);
                if (!inside(size, r.record, 112))
                    return false;
                int32_t rel;
                std::memcpy(&rel, scr + r.record, 4);
                const int64_t m = static_cast<int64_t>(r.record) + rel;
                if (m < 16 || !inside(size, static_cast<size_t>(m), 32) || std::memcmp(scr + m, "mdb", 4) != 0)
                    return false;
                r.mdb = static_cast<uint32_t>(m);
                std::memcpy(r.name, scr + r.record + 8, 8);
                r.flags = r32(scr + r.record + 0x34);
                out.push_back(r);
            }
            return true;
        }

        // An mdb's character vertex buffers (not room geometry). Offsets relative to the buffer.
        struct VbRef
        {
            uint32_t at = 0, pos = 0, nrm = 0, uv = 0, col = 0, wt = 0;
            uint16_t count = 0, mat = 0;
            uint8_t extra[8] = {};
        };

        bool readVbs(const uint8_t *scr, size_t size, uint32_t mdb, std::vector<VbRef> &out)
        {
            if (!inside(size, mdb, 32) || r16(scr + mdb + 30) == 0u) // room geometry (int16 positions)
                return false;
            const uint16_t nv = r16(scr + mdb + 10);
            if (!inside(size, mdb + 32, 4u * nv))
                return false;
            for (uint16_t v = 0; v < nv; ++v)
            {
                VbRef b;
                b.at = mdb + r32(scr + mdb + 32 + 4 * v);
                if (!inside(size, b.at, 32))
                    return false;
                b.pos = r32(scr + b.at);
                b.nrm = r32(scr + b.at + 4);
                b.uv = r32(scr + b.at + 8);
                b.col = r32(scr + b.at + 12);
                b.wt = r32(scr + b.at + 16);
                b.count = r16(scr + b.at + 20);
                b.mat = r16(scr + b.at + 22);
                std::memcpy(b.extra, scr + b.at + 24, 8);
                const size_t n = b.count;
                if (!inside(size, b.at + b.pos, 16 * n) || !inside(size, b.at + b.uv, 4 * n) ||
                    (b.nrm && !inside(size, b.at + b.nrm, 4 * n)) || (b.col && !inside(size, b.at + b.col, 4 * n)) ||
                    (b.wt && !inside(size, b.at + b.wt, 8 * n)))
                    return false;
                out.push_back(b);
            }
            return true;
        }

        // xfer.load_node: the records of every buffer, triangles from the strip flags (gamemodel.strip_tris).
        bool loadMesh(const uint8_t *scr, size_t size, uint32_t mdb, Mesh &m, std::vector<VbRef> *vbsOut = nullptr)
        {
            std::vector<VbRef> vbs;
            if (!readVbs(scr, size, mdb, vbs))
                return false;
            for (size_t v = 0; v < vbs.size(); ++v)
            {
                const VbRef &b = vbs[v];
                const int base = static_cast<int>(m.P.size());
                for (uint32_t k = 0; k < b.count; ++k)
                {
                    const uint8_t *p = scr + b.at + b.pos + 16 * k;
                    m.P.push_back({rf(p), rf(p + 4), rf(p + 8)});
                    if (b.nrm)
                    {
                        const int8_t *n = reinterpret_cast<const int8_t *>(scr + b.at + b.nrm + 4 * k);
                        m.N.push_back({n[0] / 127.0, n[1] / 127.0, n[2] / 127.0});
                    }
                    else
                        m.N.push_back({0.0, 1.0, 0.0});
                    int16_t uv[2];
                    std::memcpy(uv, scr + b.at + b.uv + 4 * k, 4);
                    m.U.push_back(uv[0]);
                    m.V.push_back(uv[1]);
                    for (int c = 0; c < 4; ++c)
                        m.C.push_back(b.col ? scr[b.at + b.col + 4 * k + c] : 128.0f);
                    for (int s = 0; s < 3; ++s)
                    {
                        m.J.push_back(b.wt ? scr[b.at + b.wt + 8 * k + 1 + s] / 4 : 0);
                        m.W.push_back(b.wt ? scr[b.at + b.wt + 8 * k + 4 + s] : 0.0f);
                    }
                    m.VB.push_back(static_cast<int>(v));
                }
                for (uint32_t k = 2; k < b.count; ++k)
                {
                    int32_t flag;
                    std::memcpy(&flag, scr + b.at + b.pos + 16 * k + 12, 4);
                    if (flag == 0x8000)
                        continue;
                    const int a = base + static_cast<int>(k);
                    if (flag == 0)
                        m.T.insert(m.T.end(), {a - 2, a - 1, a});
                    else
                        m.T.insert(m.T.end(), {a - 1, a - 2, a});
                    m.TM.push_back(b.mat);
                }
            }
            if (vbsOut)
                *vbsOut = std::move(vbs);
            return !m.P.empty();
        }

        // ---- geometry helpers (xfer.py) ----------------------------------------------------------------------------
        struct KeyHash
        {
            size_t operator()(const std::array<long long, 3> &k) const
            {
                return std::hash<long long>()(k[0] * 73856093LL ^ k[1] * 19349663LL ^ k[2] * 83492791LL);
            }
        };

        // pos_ids: records at the same position (5 decimals) share an id, numbered in order of appearance.
        std::vector<int> posIds(const std::vector<D3> &P, int &count)
        {
            std::unordered_map<std::array<long long, 3>, int, KeyHash> key;
            std::vector<int> ids(P.size());
            for (size_t i = 0; i < P.size(); ++i)
            {
                const std::array<long long, 3> k{std::llround(P[i].x * 1e5), std::llround(P[i].y * 1e5),
                                                 std::llround(P[i].z * 1e5)};
                ids[i] = key.emplace(k, static_cast<int>(key.size())).first->second;
            }
            count = static_cast<int>(key.size());
            return ids;
        }

        // smooth_normals over the triangles tris (3 per triangle): per position, the area-weighted face normals.
        std::vector<D3> smoothNormals(const std::vector<D3> &P, const std::vector<int> &tris, size_t triCount)
        {
            int count = 0;
            const std::vector<int> pid = posIds(P, count);
            std::vector<D3> n(count);
            for (size_t t = 0; t < triCount; ++t)
            {
                const int a = tris[3 * t], b = tris[3 * t + 1], c = tris[3 * t + 2];
                const D3 f = cross(P[b] - P[a], P[c] - P[a]);
                n[pid[a]] = n[pid[a]] + f;
                n[pid[b]] = n[pid[b]] + f;
                n[pid[c]] = n[pid[c]] + f;
            }
            for (D3 &v : n)
                v = v * (1.0 / (len(v) + 1e-12));
            std::vector<D3> out(P.size());
            for (size_t i = 0; i < P.size(); ++i)
                out[i] = n[pid[i]];
            return out;
        }

        // ray_hit: the ray's parameter at the triangle, NaN when it misses.
        double rayHit(D3 o, D3 d, D3 a, D3 b, D3 c)
        {
            const D3 e1 = b - a, e2 = c - a, pv = cross(d, e2);
            const double det = dot(e1, pv);
            if (std::fabs(det) <= 1e-12)
                return std::numeric_limits<double>::quiet_NaN();
            const double inv = 1.0 / det;
            const D3 tv = o - a;
            const double u = dot(tv, pv) * inv;
            const D3 qv = cross(tv, e1);
            const double v = dot(qv, d) * inv, t = dot(e2, qv) * inv;
            if (u >= -1e-6 && v >= -1e-6 && u + v <= 1 + 1e-6)
                return t;
            return std::numeric_limits<double>::quiet_NaN();
        }

        // tri_closest: the closest point of a triangle (Ericson, Real-Time Collision Detection 5.1.5).
        D3 triClosest(D3 p, D3 a, D3 b, D3 c)
        {
            const D3 ab = b - a, ac = c - a, ap = p - a;
            const double d1 = dot(ab, ap), d2 = dot(ac, ap);
            if (d1 <= 0 && d2 <= 0)
                return a;
            const D3 bp = p - b;
            const double d3 = dot(ab, bp), d4 = dot(ac, bp);
            if (d3 >= 0 && d4 <= d3)
                return b;
            const double vc = d1 * d4 - d3 * d2;
            if (vc <= 0 && d1 >= 0 && d3 <= 0)
            {
                const double den = d1 - d3;
                return a + ab * (den == 0 ? 0 : d1 / den);
            }
            const D3 cp = p - c;
            const double d5 = dot(ab, cp), d6 = dot(ac, cp);
            if (d6 >= 0 && d5 <= d6)
                return c;
            const double vb = d5 * d2 - d1 * d6;
            if (vb <= 0 && d2 >= 0 && d6 <= 0)
            {
                const double den = d2 - d6;
                return a + ac * (den == 0 ? 0 : d2 / den);
            }
            const double va = d3 * d6 - d5 * d4;
            if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
            {
                const double den = (d4 - d3) + (d5 - d6);
                return b + (c - b) * (den == 0 ? 0 : (d4 - d3) / den);
            }
            double den = va + vb + vc;
            if (std::fabs(den) < 1e-20)
                den = 1e-20;
            return a + ab * (vb / den) + ac * (vc / den);
        }

        D3 bary(D3 p, D3 a, D3 b, D3 c)
        {
            const D3 v0 = b - a, v1 = c - a, v2 = p - a;
            const double d00 = dot(v0, v0), d01 = dot(v0, v1), d11 = dot(v1, v1), d20 = dot(v2, v0), d21 = dot(v2, v1);
            double den = d00 * d11 - d01 * d01;
            if (den == 0)
                den = 1e-20;
            const double v = (d11 * d20 - d01 * d21) / den, w = (d00 * d21 - d01 * d20) / den;
            return {1 - v - w, v, w};
        }

        double w_on(const Mesh &m, size_t i, int bone)
        {
            double s = 0;
            for (int k = 0; k < 3; ++k)
                if (m.J[3 * i + k] == bone)
                    s += m.W[3 * i + k];
            return s;
        }

        // The three heaviest bones of a weighted mix, in percent summing to 100 (xfer.subdivide's mid, build2's
        // record): the mix in order of first appearance, sorted stably by weight.
        void topWeights(const std::vector<std::pair<int, double>> &mix, int J[3], float W[3])
        {
            std::vector<std::pair<int, double>> top = mix;
            std::stable_sort(top.begin(), top.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
            if (top.size() > 3)
                top.resize(3);
            double s = 0;
            for (const auto &t : top)
                s += t.second;
            if (s == 0)
                s = 1;
            int sum = 0;
            for (int k = 0; k < 3; ++k)
            {
                J[k] = k < static_cast<int>(top.size()) ? top[k].first : 0;
                const int w = k < static_cast<int>(top.size()) ? static_cast<int>(std::nearbyint(100.0 * top[k].second / s)) : 0;
                W[k] = static_cast<float>(w);
                sum += w;
            }
            W[0] += static_cast<float>(100 - sum);
        }

        // raw: the source weight (a bone with weight on it joins the mix even when its share here is 0, as in Python)
        void addMix(std::vector<std::pair<int, double>> &mix, int bone, double raw, double w)
        {
            if (raw <= 0)
                return;
            for (auto &e : mix)
                if (e.first == bone)
                {
                    e.second += w;
                    return;
                }
            mix.emplace_back(bone, w);
        }

        size_t pushRecord(Mesh &m, D3 p, D3 n, double u, double v, const float c[4], const int J[3], const float W[3], int vb)
        {
            m.P.push_back(p);
            m.N.push_back(n);
            m.U.push_back(static_cast<float>(u));
            m.V.push_back(static_cast<float>(v));
            m.C.insert(m.C.end(), c, c + 4);
            m.J.insert(m.J.end(), J, J + 3);
            m.W.insert(m.W.end(), W, W + 3);
            m.VB.push_back(vb);
            return m.P.size() - 1;
        }

        uint64_t pairKey(int a, int b)
        {
            const uint32_t lo = static_cast<uint32_t>(std::min(a, b)), hi = static_cast<uint32_t>(std::max(a, b));
            return (static_cast<uint64_t>(lo) << 32) | hi;
        }

        // xfer.subdivide, one level: red-green over the triangles flagged in splitOn (and their neighbours across a split
        // edge). Midpoints per record pair (so per vertex buffer), positions shared by position id. carried: per-triangle
        // flags the children inherit (may be splitOn itself).
        void subdivide(Mesh &m, const std::vector<char> &splitOn, std::vector<char> &region)
        {
            int count = 0;
            const std::vector<int> pid = posIds(m.P, count);
            std::unordered_set<uint64_t> split;
            for (size_t t = 0; t < m.triangles(); ++t)
                if (splitOn[t])
                    for (int e = 0; e < 3; ++e)
                        split.insert(pairKey(pid[m.T[3 * t + e]], pid[m.T[3 * t + (e + 1) % 3]]));
            auto isSplit = [&](int a, int b) { return split.count(pairKey(pid[a], pid[b])) != 0; };
            std::unordered_map<uint64_t, int> mids;
            auto mid = [&](int a, int b) -> int {
                const uint64_t key = (static_cast<uint64_t>(std::min(a, b)) << 32) | static_cast<uint32_t>(std::max(a, b));
                if (const auto it = mids.find(key); it != mids.end())
                    return it->second;
                std::vector<std::pair<int, double>> mix;
                for (int r : {a, b})
                    for (int k = 0; k < 3; ++k)
                        addMix(mix, m.J[3 * r + k], m.W[3 * r + k], m.W[3 * r + k]);
                int J[3];
                float W[3], C[4];
                topWeights(mix, J, W);
                for (int k = 0; k < 4; ++k)
                    C[k] = (m.C[4 * a + k] + m.C[4 * b + k]) * 0.5f;
                const D3 p = (m.P[a] + m.P[b]) * 0.5, n = (m.N[a] + m.N[b]) * 0.5;
                const double u = (m.U[a] + m.U[b]) * 0.5, v = (m.V[a] + m.V[b]) * 0.5;
                const int vb = m.VB[a];
                const int id = static_cast<int>(pushRecord(m, p, n, u, v, C, J, W, vb));
                mids.emplace(key, id);
                return id;
            };
            std::vector<int> T;
            std::vector<int> TM;
            std::vector<char> reg;
            auto emit = [&](int a, int b, int c, int tm, char r) {
                T.insert(T.end(), {a, b, c});
                TM.push_back(tm);
                reg.push_back(r);
            };
            const size_t tris = m.triangles();
            for (size_t t = 0; t < tris; ++t)
            {
                int a = m.T[3 * t], b = m.T[3 * t + 1], c = m.T[3 * t + 2];
                const int tm = m.TM[t];
                const char r = region[t];
                const int n = isSplit(a, b) + isSplit(b, c) + isSplit(c, a);
                if (n == 0)
                {
                    emit(a, b, c, tm, r);
                    continue;
                }
                if (n == 3)
                {
                    const int ab = mid(a, b), bc = mid(b, c), ca = mid(c, a);
                    emit(a, ab, ca, tm, r);
                    emit(ab, b, bc, tm, r);
                    emit(ca, bc, c, tm, r);
                    emit(ab, bc, ca, tm, r);
                    continue;
                }
                if (n == 1)
                {
                    while (!isSplit(a, b))
                    {
                        const int x = a;
                        a = b;
                        b = c;
                        c = x;
                    }
                    const int ab = mid(a, b);
                    emit(a, ab, c, tm, r);
                    emit(ab, b, c, tm, r);
                }
                else
                {
                    while (!(isSplit(a, b) && isSplit(b, c)))
                    {
                        const int x = a;
                        a = b;
                        b = c;
                        c = x;
                    }
                    const int ab = mid(a, b), bc = mid(b, c);
                    emit(ab, b, bc, tm, r);
                    emit(a, ab, bc, tm, r);
                    emit(a, bc, c, tm, r);
                }
            }
            m.T = std::move(T);
            m.TM = std::move(TM);
            region = std::move(reg);
        }

        // ---- the bust: xfer.transfer3(woman, bunny, levels=1) --------------------------------------------------------
        // m is subdivided in place; detail gets the detailed positions / normals, moved flags the records it moves.
        void bust(Mesh &m, const Mesh &donor, std::vector<D3> &detailP, std::vector<D3> &detailN, std::vector<char> &moved)
        {
            constexpr int kBone = 29;
            constexpr double kMaxT = 0.05;
            constexpr int kSmooth = 12;
            constexpr double kAnchor = 0.8;                     // the projected height's share in each smoothing step
            constexpr double kBackFrom = 0.02, kBackTo = 0.05;  // straight back within kBackFrom of x = 0, none past kBackTo
            constexpr double kBand = 0.004;                     // the cleavage band: triangles with a vertex this near x = 0
            const size_t records0 = m.records();
            std::vector<char> region(m.triangles());
            double g_lo[3] = {1e9, 1e9, 1e9}, g_hi[3] = {-1e9, -1e9, -1e9};
            for (size_t i = 0; i < records0; ++i)
                if (w_on(m, i, kBone) > 0)
                {
                    const double c[3] = {m.P[i].x, m.P[i].y, m.P[i].z};
                    for (int k = 0; k < 3; ++k)
                    {
                        g_lo[k] = std::min(g_lo[k], c[k]);
                        g_hi[k] = std::max(g_hi[k], c[k]);
                    }
                }
            for (size_t t = 0; t < m.triangles(); ++t)
                for (int k = 0; k < 3; ++k)
                    if (w_on(m, m.T[3 * t + k], kBone) > 0)
                        region[t] = 1;
            subdivide(m, std::vector<char>(region), region);
            // The cleavage: the triangles touching the centre line subdivided once more (one strip of triangles between the
            // crease and the breast's inner edge zig-zagged up and down, K Nov 15), not where one reaches the region's
            // border (under the bust the donor has no surface ahead).
            {
                int n1 = 0;
                const std::vector<int> pid1 = posIds(m.P, n1);
                std::vector<char> inr(n1), onb(n1);
                for (size_t t = 0; t < m.triangles(); ++t)
                    for (int k = 0; k < 3; ++k)
                        (region[t] ? inr : onb)[pid1[m.T[3 * t + k]]] = 1;
                std::vector<char> band(m.triangles());
                for (size_t t = 0; t < m.triangles(); ++t)
                {
                    if (!region[t])
                        continue;
                    bool touch = false, border = false;
                    for (int k = 0; k < 3; ++k)
                    {
                        const int v = m.T[3 * t + k];
                        touch = touch || std::fabs(m.P[v].x) < kBand;
                        border = border || (inr[pid1[v]] && onb[pid1[v]]);
                    }
                    band[t] = touch && !border;
                }
                subdivide(m, band, region);
            }

            // The donor's bone-29 surface, box-mapped onto the woman's bone-29 extents (x symmetric).
            std::vector<char> dOn(donor.records());
            double d_lo[3] = {1e9, 1e9, 1e9}, d_hi[3] = {-1e9, -1e9, -1e9};
            for (size_t i = 0; i < donor.records(); ++i)
                if (w_on(donor, i, kBone) > 0)
                {
                    dOn[i] = 1;
                    const double c[3] = {donor.P[i].x, donor.P[i].y, donor.P[i].z};
                    for (int k = 0; k < 3; ++k)
                    {
                        d_lo[k] = std::min(d_lo[k], c[k]);
                        d_hi[k] = std::max(d_hi[k], c[k]);
                    }
                }
            const double sLo[3] = {-d_hi[0], d_lo[1], d_lo[2]}, sHi[3] = {d_hi[0], d_hi[1], d_hi[2]};
            const double tLo[3] = {-g_hi[0], g_lo[1], g_lo[2]}, tHi[3] = {g_hi[0], g_hi[1], g_hi[2]};
            auto fx = [&](D3 p) {
                const double c[3] = {p.x, p.y, p.z};
                double o[3];
                for (int k = 0; k < 3; ++k)
                    o[k] = (c[k] - sLo[k]) * ((tHi[k] - tLo[k]) / (sHi[k] - sLo[k])) + tLo[k];
                return D3{o[0], o[1], o[2]};
            };
            std::vector<D3> A, B, C;
            for (size_t t = 0; t < donor.triangles(); ++t)
            {
                const int a = donor.T[3 * t], b = donor.T[3 * t + 1], c = donor.T[3 * t + 2];
                if (dOn[a] || dOn[b] || dOn[c])
                {
                    A.push_back(fx(donor.P[a]));
                    B.push_back(fx(donor.P[b]));
                    C.push_back(fx(donor.P[c]));
                }
            }

            const std::vector<D3> &P = m.P;
            const std::vector<D3> nrm = smoothNormals(P, m.T, m.triangles());
            int npos = 0;
            const std::vector<int> pid = posIds(P, npos);
            std::vector<D3> Ppos(npos), Npos(npos);
            for (size_t i = 0; i < P.size(); ++i)
            {
                Ppos[pid[i]] = P[i];
                Npos[pid[i]] = nrm[i];
            }
            std::vector<char> inreg(npos), outv(npos);
            for (size_t t = 0; t < m.triangles(); ++t)
                for (int k = 0; k < 3; ++k)
                    (region[t] ? inreg : outv)[pid[m.T[3 * t + k]]] = 1;
            std::vector<char> fixed(npos);
            for (int k = 0; k < npos; ++k)
                fixed[k] = !inreg[k] || outv[k];

            // The push direction: the smoothed normal, turning to straight back within 2-5 cm of the centre line (the
            // generic women's cleavage bulges forward; pushed back along its up / down tilted normals the centre
            // vertices met in a horizontal ledge, K Nov 15).
            std::vector<D3> Dir(npos);
            for (int k = 0; k < npos; ++k)
            {
                double f = std::clamp((kBackTo - std::fabs(Ppos[k].x)) / (kBackTo - kBackFrom), 0.0, 1.0);
                f = f * f * (3 - 2 * f);
                const D3 d = Npos[k] * (1 - f) + D3{0.0, 0.0, 1.0} * f;
                Dir[k] = d * (1.0 / (len(d) + 1e-12));
            }
            // The height along it onto the donor (the nearest hit within 5 cm, else the closest point).
            std::vector<double> H(npos, 0.0);
            for (int k = 0; k < npos; ++k)
            {
                if (fixed[k])
                    continue;
                const D3 o = Ppos[k], d = Dir[k];
                double best = std::numeric_limits<double>::infinity(), bt = 0;
                for (size_t t = 0; t < A.size(); ++t)
                {
                    const double h = rayHit(o, d, A[t], B[t], C[t]);
                    if (std::isfinite(h) && std::fabs(h) < kMaxT && std::fabs(h) < best)
                    {
                        best = std::fabs(h);
                        bt = h;
                    }
                }
                D3 q;
                if (std::isfinite(best))
                    q = o + d * bt;
                else
                {
                    double bd = std::numeric_limits<double>::infinity();
                    for (size_t t = 0; t < A.size(); ++t)
                    {
                        const D3 c = triClosest(o, A[t], B[t], C[t]);
                        const double dd = dist2(c, o);
                        if (dd < bd)
                        {
                            bd = dd;
                            q = c;
                        }
                    }
                }
                H[k] = dot(q - o, d);
            }
            // Smoothed as a height over the region, the border held (Jacobi, the projected heights as anchors).
            std::vector<std::vector<int>> nb(npos);
            for (size_t t = 0; t < m.triangles(); ++t)
            {
                if (!region[t])
                    continue;
                for (int e = 0; e < 3; ++e)
                {
                    const int a = pid[m.T[3 * t + e]], b = pid[m.T[3 * t + (e + 1) % 3]];
                    if (std::find(nb[a].begin(), nb[a].end(), b) == nb[a].end())
                        nb[a].push_back(b);
                    if (std::find(nb[b].begin(), nb[b].end(), a) == nb[b].end())
                        nb[b].push_back(a);
                }
            }
            // The centre line is the bottom of the groove: never shallower than its neighbours' mean (the donor's own mesh
            // has a little ridge at its cleavage's foot, which left a peak there).
            for (int k = 0; k < npos; ++k)
            {
                if (fixed[k] || std::fabs(Ppos[k].x) >= 0.002 || nb[k].empty())
                    continue;
                double sum = 0;
                for (const int j : nb[k])
                    sum += H[j];
                H[k] = std::min(H[k], sum / static_cast<double>(nb[k].size()));
            }
            const std::vector<double> H0 = H;
            for (int it = 0; it < kSmooth; ++it)
            {
                std::vector<double> Hn = H;
                for (int k = 0; k < npos; ++k)
                {
                    if (fixed[k] || nb[k].empty())
                        continue;
                    double s = 0;
                    for (int j : nb[k])
                        s += H[j];
                    Hn[k] = kAnchor * H0[k] + (1 - kAnchor) * (s / static_cast<double>(nb[k].size()));
                }
                H = std::move(Hn);
            }
            detailP.resize(P.size());
            for (size_t i = 0; i < P.size(); ++i)
                detailP[i] = P[i] + Dir[pid[i]] * H[pid[i]];
            const std::vector<D3> nrm2 = smoothNormals(detailP, m.T, m.triangles());
            detailN = m.N;
            moved.assign(P.size(), 0);
            for (size_t i = 0; i < P.size(); ++i)
                if (!fixed[pid[i]])
                {
                    detailN[i] = nrm2[i];
                    moved[i] = 1;
                }
        }

        // ---- xfer.crease: the cleavage's centre line as a shading crease ---------------------------------------------
        // Each moved centre record gets a left and a right copy (same position, same flat state), each with its own wall's
        // normal (the groove-wall faces within +-1.5 cm of its height on that side), blended in by the walls' angle (none
        // below 20 deg, full above 50): the breasts shade as two shapes instead of one smeared surface (K Nov 15).
        void crease(Mesh &m, std::vector<D3> &detailP, std::vector<D3> &detailN)
        {
            constexpr double kHalf = 0.002, kA0 = 20.0, kA1 = 50.0, kWin = 0.015;
            const size_t R = m.records(), tris = m.triangles();
            std::vector<char> moved(R), cset(R);
            for (size_t i = 0; i < R; ++i)
            {
                moved[i] = std::fabs(detailP[i].x - m.P[i].x) > 1e-7 || std::fabs(detailP[i].y - m.P[i].y) > 1e-7 ||
                           std::fabs(detailP[i].z - m.P[i].z) > 1e-7;
                cset[i] = std::fabs(m.P[i].x) < kHalf;
            }
            std::vector<D3> fn(tris), ctr(tris);
            std::vector<char> wall(tris);
            for (size_t t = 0; t < tris; ++t)
            {
                const int a = m.T[3 * t], b = m.T[3 * t + 1], c = m.T[3 * t + 2];
                fn[t] = cross(detailP[b] - detailP[a], detailP[c] - detailP[a]);
                ctr[t] = (detailP[a] + detailP[b] + detailP[c]) * (1.0 / 3.0);
                wall[t] = (cset[a] || cset[b] || cset[c]) && (moved[a] || moved[b] || moved[c]);
            }
            auto unit = [](D3 v) { return v * (1.0 / (len(v) + 1e-12)); };
            for (size_t r = 0; r < R; ++r)
            {
                if (!moved[r] || !cset[r])
                    continue;
                std::vector<size_t> L, Rt;
                for (size_t t = 0; t < tris; ++t)
                    if (m.T[3 * t] == static_cast<int>(r) || m.T[3 * t + 1] == static_cast<int>(r) || m.T[3 * t + 2] == static_cast<int>(r))
                    {
                        if (ctr[t].x < 0)
                            L.push_back(t);
                        else if (ctr[t].x > 0)
                            Rt.push_back(t);
                    }
                if (L.empty() || Rt.empty())
                    continue;
                D3 nl, nr;
                for (size_t t = 0; t < tris; ++t)
                    if (wall[t] && std::fabs(ctr[t].y - detailP[r].y) < kWin)
                    {
                        if (ctr[t].x < 0)
                            nl = nl + fn[t];
                        else if (ctr[t].x > 0)
                            nr = nr + fn[t];
                    }
                nl = unit(nl);
                nr = unit(nr);
                const D3 avg = unit(detailN[r]);
                const double ang = std::acos(std::clamp(dot(nl, nr), -1.0, 1.0)) * 180.0 / 3.14159265358979323846;
                double w = std::clamp((ang - kA0) / (kA1 - kA0), 0.0, 1.0);
                w = w * w * (3 - 2 * w);
                if (w <= 0)
                    continue;
                detailN[r] = unit(avg * (1 - w) + nl * w);
                const int J[3] = {m.J[3 * r], m.J[3 * r + 1], m.J[3 * r + 2]};
                const float W[3] = {m.W[3 * r], m.W[3 * r + 1], m.W[3 * r + 2]};
                const float C[4] = {m.C[4 * r], m.C[4 * r + 1], m.C[4 * r + 2], m.C[4 * r + 3]};
                const int copy = static_cast<int>(pushRecord(m, m.P[r], m.N[r], m.U[r], m.V[r], C, J, W, m.VB[r]));
                detailP.push_back(detailP[r]);
                detailN.push_back(unit(avg * (1 - w) + nr * w));
                for (const size_t t : Rt)
                    for (int k = 0; k < 3; ++k)
                        if (m.T[3 * t + k] == static_cast<int>(r))
                            m.T[3 * t + k] = copy;
            }
        }

        // ---- the seat: transplant.transplant / build2(woman, shannon, (-0.15, 0.88, -1), (0.15, 1.16, -0.04),
        // falloff=0.05, mapping='own', one_vb=True), CUT_MIN = 1 ------------------------------------------------------

        // boundary_loops: the boundary edges (used by one triangle) chained into loops, in order of appearance.
        std::vector<std::vector<int>> boundaryLoops(const std::vector<std::array<int, 3>> &tris)
        {
            std::vector<uint64_t> order;
            std::unordered_map<uint64_t, int> cnt;
            for (const auto &t : tris)
                for (int e = 0; e < 3; ++e)
                {
                    const uint64_t k = pairKey(t[e], t[(e + 1) % 3]);
                    if (cnt[k]++ == 0)
                        order.push_back(k);
                }
            std::vector<int> keys;
            std::unordered_map<int, std::vector<int>> adj;
            for (const uint64_t k : order)
            {
                if (cnt[k] != 1)
                    continue;
                const int a = static_cast<int>(k >> 32), b = static_cast<int>(k & 0xFFFFFFFFu);
                if (!adj.count(a))
                    keys.push_back(a);
                adj[a].push_back(b);
                if (!adj.count(b))
                    keys.push_back(b);
                adj[b].push_back(a);
            }
            std::unordered_set<int> seen;
            std::vector<std::vector<int>> loops;
            for (const int s : keys)
            {
                if (seen.count(s))
                    continue;
                std::vector<int> loop{s};
                seen.insert(s);
                int prev = -1, cur = s;
                for (;;)
                {
                    int next = -1;
                    for (const int n : adj[cur])
                        if (n != prev && !seen.count(n))
                        {
                            next = n;
                            break;
                        }
                    if (next < 0)
                        break;
                    prev = cur;
                    cur = next;
                    loop.push_back(cur);
                    seen.insert(cur);
                }
                loops.push_back(std::move(loop));
            }
            return loops;
        }

        const std::vector<int> &largest(const std::vector<std::vector<int>> &loops)
        {
            size_t best = 0;
            for (size_t i = 1; i < loops.size(); ++i)
                if (loops[i].size() > loops[best].size())
                    best = i;
            return loops[best];
        }

        // transplant.pav: the non-decreasing least-squares fit (pool adjacent violators).
        std::vector<double> pav(const std::vector<double> &y)
        {
            std::vector<double> v;
            std::vector<int> w;
            for (const double x : y)
            {
                v.push_back(x);
                w.push_back(1);
                while (v.size() > 1 && v[v.size() - 2] > v.back())
                {
                    const int n = w[w.size() - 2] + w.back();
                    const double m = (v[v.size() - 2] * w[w.size() - 2] + v.back() * w.back()) / n;
                    v.pop_back();
                    w.pop_back();
                    v.back() = m;
                    w.back() = n;
                }
            }
            std::vector<double> out;
            for (size_t i = 0; i < v.size(); ++i)
                out.insert(out.end(), static_cast<size_t>(w[i]), v[i]);
            return out;
        }

        double pmod(double a, double b) // Python's a % b
        {
            const double r = std::fmod(a, b);
            return r != 0 && ((r < 0) != (b < 0)) ? r + b : r;
        }

        // transplant.fan: triangle t with its edge a-b split at mids (ordered from a to b), the fan from its third corner
        // in t's own winding.
        template <typename I>
        std::vector<std::array<I, 3>> fan(const std::array<I, 3> &t, I a, I b, const std::vector<I> &mids)
        {
            std::vector<I> ch;
            I c{};
            for (int x = 0; x < 3 && ch.empty(); ++x)
            {
                if (t[x] == a && t[(x + 1) % 3] == b)
                {
                    ch.push_back(a);
                    ch.insert(ch.end(), mids.begin(), mids.end());
                    ch.push_back(b);
                    c = t[(x + 2) % 3];
                }
                else if (t[x] == b && t[(x + 1) % 3] == a)
                {
                    ch.push_back(b);
                    ch.insert(ch.end(), mids.rbegin(), mids.rend());
                    ch.push_back(a);
                    c = t[(x + 2) % 3];
                }
            }
            if (ch.empty())
                return {t};
            std::vector<std::array<I, 3>> out;
            for (size_t y = 0; y + 1 < ch.size(); ++y)
                out.push_back({ch[y], ch[y + 1], c});
            return out;
        }

        // transplant.no_ears: a triangle with two edges on the boundary has all three corners on the patch's edge, and
        // laid onto the hole's boundary it would be a line (repeated while any remain).
        std::vector<std::array<int, 3>> noEars(std::vector<std::array<int, 3>> T)
        {
            for (;;)
            {
                std::unordered_map<uint64_t, int> cnt;
                for (const auto &t : T)
                    for (int e = 0; e < 3; ++e)
                        ++cnt[pairKey(t[e], t[(e + 1) % 3])];
                std::vector<std::array<int, 3>> keep;
                for (const auto &t : T)
                {
                    int b = 0;
                    for (int e = 0; e < 3; ++e)
                        b += cnt[pairKey(t[e], t[(e + 1) % 3])] == 1;
                    if (b < 2)
                        keep.push_back(t);
                }
                if (keep.size() == T.size())
                    return T;
                T = std::move(keep);
            }
        }

        struct SeatResult
        {
            std::vector<char> cut;        // per triangle of the input
            std::vector<D3> flatP, flatN; // per new record (appended after the input's records)
            std::vector<D3> detailN;      // per record of the output (ring + new: the patch's normals)
            std::vector<char> ring;       // per input record: on the hole's edge (its normal changes)
            int patch = 0, shared = 0; // patch triangles, points added on the shared edge
        };

        // m gets the new records and loses the cut triangles; false when the window holds nothing.
        bool seat(Mesh &m, const Mesh &donor, SeatResult &res)
        {
            const D3 lo{-0.15, 0.88, -1.0}, hi{0.15, 1.16, -0.04};
            constexpr double kFalloff = 0.05;
            const size_t R = m.records();
            const std::vector<D3> P = m.P; // the input's positions (the new records go into m)
            int npid = 0;
            const std::vector<int> pid = posIds(P, npid);
            std::vector<char> inwin(R);
            for (size_t i = 0; i < R; ++i)
                inwin[i] = P[i].x >= lo.x && P[i].x <= hi.x && P[i].y >= lo.y && P[i].y <= hi.y && P[i].z >= lo.z &&
                           P[i].z <= hi.z;
            const size_t tris = m.triangles();
            std::vector<char> cut(tris);
            std::map<int, int> vbCount;
            for (size_t t = 0; t < tris; ++t)
            {
                cut[t] = (inwin[m.T[3 * t]] + inwin[m.T[3 * t + 1]] + inwin[m.T[3 * t + 2]]) >= 1;
                if (cut[t])
                    ++vbCount[m.VB[m.T[3 * t]]];
            }
            if (vbCount.empty())
                return false;
            // The costume piece holding the seat: the patch ends on its own seam.
            int main = vbCount.begin()->first;
            for (const auto &[vb, n] : vbCount)
                if (n > vbCount[main])
                    main = vb;
            std::vector<size_t> cutI;
            for (size_t t = 0; t < tris; ++t)
            {
                if (cut[t] && m.VB[m.T[3 * t]] != main)
                    cut[t] = 0;
                if (cut[t])
                    cutI.push_back(t);
            }
            std::vector<std::array<int, 3>> cutT, cutPid;
            for (const size_t t : cutI)
            {
                cutT.push_back({m.T[3 * t], m.T[3 * t + 1], m.T[3 * t + 2]});
                cutPid.push_back({pid[m.T[3 * t]], pid[m.T[3 * t + 1]], pid[m.T[3 * t + 2]]});
            }
            std::vector<int> rep(npid, -1);
            for (size_t i = 0; i < R; ++i)
                if (rep[pid[i]] < 0)
                    rep[pid[i]] = static_cast<int>(i);
            const auto holes = boundaryLoops(cutPid);
            if (holes.empty())
                return false;
            const std::vector<int> &hole = largest(holes);

            // Shannon's triangles inside the hole's outline (back view, behind the hip axis), small holes filled.
            std::vector<std::array<double, 2>> poly;
            for (const int k : hole)
                poly.push_back({P[rep[k]].x, P[rep[k]].y});
            auto insidePoly = [&](double x, double y) {
                bool c = false;
                const size_t n = poly.size();
                for (size_t i = 0; i < n; ++i)
                {
                    const double x1 = poly[i][0], y1 = poly[i][1], x2 = poly[(i + 1) % n][0], y2 = poly[(i + 1) % n][1];
                    if ((y1 > y) != (y2 > y) && x < (x2 - x1) * (y - y1) / (y2 - y1 + 1e-20) + x1)
                        c = !c;
                }
                return c;
            };
            const size_t dTris = donor.triangles();
            std::vector<char> dsel(dTris);
            for (size_t t = 0; t < dTris; ++t)
            {
                const D3 c = (donor.P[donor.T[3 * t]] + donor.P[donor.T[3 * t + 1]] + donor.P[donor.T[3 * t + 2]]) * (1.0 / 3.0);
                dsel[t] = insidePoly(c.x, c.y) && c.z < 0.0;
            }
            int ndp = 0;
            const std::vector<int> dpid = posIds(donor.P, ndp);
            for (int it = 0; it < 3; ++it)
            {
                std::vector<char> used(ndp);
                for (size_t t = 0; t < dTris; ++t)
                    if (dsel[t])
                        for (int k = 0; k < 3; ++k)
                            used[dpid[donor.T[3 * t + k]]] = 1;
                bool any = false;
                std::vector<char> add(dTris);
                for (size_t t = 0; t < dTris; ++t)
                    if (!dsel[t] && used[dpid[donor.T[3 * t]]] && used[dpid[donor.T[3 * t + 1]]] && used[dpid[donor.T[3 * t + 2]]])
                        add[t] = any = true;
                if (!any)
                    break;
                for (size_t t = 0; t < dTris; ++t)
                    dsel[t] |= add[t];
            }
            // At her own size, only her hip width matched to the woman's at seat height.
            auto hipw = [](const std::vector<D3> &PP) {
                double w = 0;
                for (const D3 &p : PP)
                    if (p.y > 0.95 && p.y < 1.05 && p.z < 0.0)
                        w = std::max(w, std::fabs(p.x));
                return w;
            };
            const double dw = hipw(donor.P);
            if (dw <= 0)
                return false;
            const double sx = hipw(P) / dw;
            std::vector<std::array<int, 3>> patchT; // donor position ids
            std::vector<int> drep(ndp, -1);
            for (size_t i = 0; i < donor.records(); ++i)
                if (drep[dpid[i]] < 0)
                    drep[dpid[i]] = static_cast<int>(i);
            std::vector<char> usedD(ndp);
            for (size_t t = 0; t < dTris; ++t)
                if (dsel[t])
                {
                    patchT.push_back({dpid[donor.T[3 * t]], dpid[donor.T[3 * t + 1]], dpid[donor.T[3 * t + 2]]});
                    for (int k = 0; k < 3; ++k)
                        usedD[patchT.back()[k]] = 1;
                }
            if (patchT.empty())
                return false;
            std::map<int, D3> Q; // sorted by donor position id, as np.unique
            for (int u = 0; u < ndp; ++u)
                if (usedD[u])
                {
                    const D3 p = donor.P[drep[u]];
                    Q[u] = {p.x * sx, p.y, p.z};
                }
            patchT = noEars(std::move(patchT));
            const auto patchLoops = boundaryLoops(patchT);
            const std::vector<int> &pl = largest(patchLoops);

            // Deepened per height (z scaled about the hip axis) wherever her own seat reaches further back (plus 3 mm: the
            // blend onto her surface gives a little back), never made shallower: Cash at 101% is never smaller than her
            // seat (em0d's women: theirs is 1-2 cm deeper than Shannon's, K Nov 15). transplant.depth_match.
            {
                constexpr int kYs = 29;      // y 0.88 .. 1.16
                constexpr double kMargin = 0.003, kMost = 1.5;
                auto rear = [](const std::vector<std::array<D3, 3>> &tri, double y) {
                    double best = std::numeric_limits<double>::quiet_NaN();
                    for (int xi = 0; xi < 8; ++xi) // x 0.03 .. 0.10
                    {
                        const D3 o{0.03 + 0.01 * xi, y, -1.0}, d{0.0, 0.0, 1.0};
                        double tmin = std::numeric_limits<double>::infinity();
                        for (const auto &t : tri)
                        {
                            const double h = rayHit(o, d, t[0], t[1], t[2]);
                            if (std::isfinite(h))
                                tmin = std::min(tmin, h);
                        }
                        if (std::isfinite(tmin) && !(best <= -1.0 + tmin))
                            best = -1.0 + tmin;
                    }
                    return best;
                };
                std::vector<std::array<D3, 3>> herT, hisT;
                for (const auto &t : cutT)
                    herT.push_back({P[t[0]], P[t[1]], P[t[2]]});
                for (const auto &t : patchT)
                    hisT.push_back({Q[t[0]], Q[t[1]], Q[t[2]]});
                double ys[kYs], sc[kYs];
                for (int i = 0; i < kYs; ++i)
                {
                    ys[i] = 0.88 + 0.01 * i;
                    const double her = rear(herT, ys[i]), his = rear(hisT, ys[i]);
                    sc[i] = std::isfinite(her) && std::isfinite(his) && his < 0 ? std::clamp((her - kMargin) / his, 1.0, kMost) : 1.0;
                }
                for (int it = 0; it < 2; ++it)
                {
                    double n[kYs];
                    for (int i = 0; i < kYs; ++i)
                        n[i] = std::max(1.0, 0.25 * sc[std::max(i - 1, 0)] + 0.5 * sc[i] + 0.25 * sc[std::min(i + 1, kYs - 1)]);
                    std::copy(n, n + kYs, sc);
                }
                for (auto &[k, q] : Q)
                {
                    double f = sc[0];
                    if (q.y >= ys[kYs - 1])
                        f = sc[kYs - 1];
                    else if (q.y > ys[0])
                    {
                        const int i = std::min(kYs - 2, static_cast<int>((q.y - ys[0]) / 0.01));
                        const double t = (q.y - ys[i]) / (ys[i + 1] - ys[i]);
                        f = sc[i] + (sc[i + 1] - sc[i]) * t;
                    }
                    q.z *= f;
                }
            }

            // Blended onto her surface near its edge.
            auto closestCut = [&](D3 q, size_t *which) {
                double bd = std::numeric_limits<double>::infinity();
                D3 best;
                for (size_t c = 0; c < cutT.size(); ++c)
                {
                    const D3 p = triClosest(q, P[cutT[c][0]], P[cutT[c][1]], P[cutT[c][2]]);
                    const double d = dist2(p, q);
                    if (d < bd)
                    {
                        bd = d;
                        best = p;
                        if (which)
                            *which = c;
                    }
                }
                return best;
            };
            std::map<int, double> fall; // per donor position: the share of Shannon's shape (0 at the patch's edge)
            {
                std::vector<D3> edge;
                for (const int k : pl)
                    edge.push_back(Q[k]);
                for (auto &[k, q] : Q)
                {
                    const D3 s0 = closestCut(q, nullptr);
                    double d = std::numeric_limits<double>::infinity();
                    for (const D3 &e : edge)
                        d = std::min(d, std::sqrt(dist2(e, q)));
                    double f = std::clamp(d / kFalloff, 0.0, 1.0);
                    f = f * f * (3 - 2 * f);
                    q = s0 + (q - s0) * f;
                    fall[k] = f;
                }
            }

            // One shared edge: the patch's edge vertices laid onto the hole's polyline in loop order (a run landing within
            // 1 mm one vertex), each side's edge triangles then split at the other
            // side's vertices: no stitch, no T-junctions (the stitch's slivers showed as dots along the waistband from low
            // angles, K Nov 15). transplant.join_edge.
            constexpr double kMerge = 0.001;
            constexpr uint64_t kDonor = 1ull << 40, kKept = 1ull << 41;
            const size_t nH = hole.size();
            std::vector<D3> HP(nH), seg(nH);
            std::vector<double> sl(nH), S(nH + 1, 0.0);
            for (size_t i = 0; i < nH; ++i)
                HP[i] = P[rep[hole[i]]];
            for (size_t i = 0; i < nH; ++i)
            {
                seg[i] = HP[(i + 1) % nH] - HP[i];
                sl[i] = std::sqrt(dot(seg[i], seg[i]));
                S[i + 1] = S[i] + sl[i];
            }
            const double L = S[nH];
            std::vector<int> Pl = pl;
            {
                auto area = [](const std::vector<D3> &pts) {
                    double a = 0;
                    for (size_t i = 0; i < pts.size(); ++i)
                    {
                        const D3 &p = pts[i], &q = pts[(i + 1) % pts.size()];
                        a += p.x * q.y - q.x * p.y;
                    }
                    return 0.5 * a;
                };
                std::vector<D3> b;
                for (const int k : Pl)
                    b.push_back(Q[k]);
                auto sign = [](double x) { return (x > 0) - (x < 0); };
                if (sign(area(HP)) != sign(area(b)))
                    std::reverse(Pl.begin(), Pl.end());
                size_t k0 = 0;
                double bd = std::numeric_limits<double>::infinity();
                for (size_t k = 0; k < Pl.size(); ++k)
                    if (const double d = dist2(Q[Pl[k]], HP[0]); d < bd)
                    {
                        bd = d;
                        k0 = k;
                    }
                std::rotate(Pl.begin(), Pl.begin() + static_cast<long>(k0), Pl.end());
            }
            std::vector<double> u;
            for (size_t j = 0; j < Pl.size(); ++j)
            {
                const D3 q = Q[Pl[j]];
                double bd = std::numeric_limits<double>::infinity(), x = 0;
                for (size_t i = 0; i < nH; ++i)
                {
                    const double t = std::clamp(dot(q - HP[i], seg[i]) / std::max(dot(seg[i], seg[i]), 1e-20), 0.0, 1.0);
                    const double d = dist2(HP[i] + seg[i] * t, q);
                    if (d < bd)
                    {
                        bd = d;
                        x = S[i] + t * sl[i];
                    }
                }
                if (j == 0)
                    x = x < L / 2 ? x : x - L;
                else
                    x += L * std::nearbyint((u.back() - x) / L);
                u.push_back(x);
            }
            u = pav(u);
            std::vector<size_t> first, head;
            std::vector<double> s;
            for (size_t j = 0; j < u.size(); ++j)
            {
                if (j && u[j] - s.back() < kMerge)
                {
                    first.push_back(first.back());
                    continue;
                }
                first.push_back(j);
                s.push_back(u[j]);
                head.push_back(j);
            }
            while (s.size() > 1 && s.back() > s[0] + L - kMerge)
            {
                for (size_t &f : first)
                    if (f == head.back())
                        f = 0;
                s.pop_back();
                head.pop_back();
            }
            const size_t nP = s.size();
            // Every hole vertex onto its nearest patch vertex (the closest pairs first), each staying between its
            // neighbours: the patch's edge triangles then hold few hole vertices to fan around.
            std::vector<int> snapped(nP, -1);
            {
                const std::vector<double> s0 = s;
                std::vector<std::tuple<double, size_t, size_t, double>> cand;
                for (size_t i = 0; i < nH; ++i)
                {
                    const double x = s0[0] + pmod(S[i] - s0[0], L);
                    size_t bj = 0;
                    double bd = std::numeric_limits<double>::infinity();
                    for (size_t j = 0; j <= nP; ++j)
                        if (const double d = std::fabs(x - (j < nP ? s0[j] : s0[0] + L)); d < bd)
                        {
                            bd = d;
                            bj = j;
                        }
                    const double y = bj == nP ? s0[0] + L : s0[bj];
                    cand.emplace_back(std::fabs(x - y), i, bj % nP, x - y);
                }
                std::sort(cand.begin(), cand.end());
                for (const auto &[d, i, j, dx] : cand)
                {
                    if (snapped[j] >= 0)
                        continue;
                    const double lo = j ? s[j - 1] : s[nP - 1] - L, hi = j + 1 < nP ? s[j + 1] : s[0] + L, x = s0[j] + dx;
                    if (!(lo + kMerge < x && x < hi - kMerge))
                        continue;
                    s[j] = x;
                    snapped[j] = static_cast<int>(i);
                }
            }
            std::vector<int> Pr; // the runs' first vertices: the patch's edge loop
            std::vector<std::pair<int, int>> run;
            for (size_t j = 0; j < Pl.size(); ++j)
                if (first[j] != j)
                    run.emplace_back(Pl[j], Pl[first[j]]);
                else
                    Pr.push_back(Pl[j]);
            std::map<int, uint64_t> keyOf; // donor position -> key: a hole vertex's position id, or kDonor | donor id
            for (const auto &[k, q] : Q)
                keyOf[k] = kDonor | static_cast<uint64_t>(k);
            std::map<size_t, std::vector<std::pair<double, uint64_t>>> keptPts; // hole edge -> (t, patch key)
            std::unordered_set<uint64_t> edgeKeys;
            std::unordered_set<int> takenPid;
            for (size_t j = 0; j < nP; ++j)
            {
                const int k = Pr[j];
                if (snapped[j] >= 0)
                {
                    keyOf[k] = static_cast<uint64_t>(hole[snapped[j]]);
                    Q[k] = HP[snapped[j]];
                    takenPid.insert(hole[snapped[j]]);
                    continue;
                }
                const double x = pmod(s[j], L);
                const size_t i = std::min(static_cast<size_t>(std::upper_bound(S.begin(), S.end(), x) - S.begin()) - 1, nH - 1);
                const double t = (x - S[i]) / std::max(S[i + 1] - S[i], 1e-20);
                Q[k] = HP[i] + seg[i] * t;
                keptPts[i].emplace_back(t, keyOf[k]);
                edgeKeys.insert(keyOf[k]);
            }
            for (const auto &[k, r0] : run)
            {
                keyOf[k] = keyOf[r0];
                Q[k] = Q[r0];
            }
            std::map<size_t, std::vector<std::pair<double, uint64_t>>> patchPts; // patch edge -> (u, hole position id)
            {
                std::vector<double> se = s;
                se.push_back(s[0] + L);
                for (size_t i = 0; i < nH; ++i)
                {
                    if (takenPid.count(hole[i]))
                        continue;
                    const double x = s[0] + pmod(S[i] - s[0], L);
                    const size_t j = std::min(static_cast<size_t>(std::upper_bound(se.begin(), se.end(), x) - se.begin()) - 1, nP - 1);
                    patchPts[j].emplace_back(x, static_cast<uint64_t>(hole[i]));
                }
            }
            auto kpos = [&](uint64_t k) { return (k & kDonor) ? Q[static_cast<int>(k & 0xFFFFFFFFu)] : P[rep[static_cast<int>(k)]]; };

            // Records: every new triangle takes the buffer of the cut triangle under it; a new record the woman's UV,
            // colour and weights by closest point on her cut surface in that buffer. A hole vertex's own record comes from
            // the cut triangles first: the first record at its position can sit across a UV seam (the crotch's stitch then
            // spanned the whole texture, K Nov 15).
            std::vector<int> cutVb(cutT.size());
            std::map<int, int> vbTm;
            for (size_t c = 0; c < cutT.size(); ++c)
            {
                cutVb[c] = m.VB[cutT[c][0]];
                vbTm[cutVb[c]] = m.TM[cutI[c]];
            }
            auto vbAt = [&](D3 p) {
                size_t c = 0;
                closestCut(p, &c);
                return cutVb[c];
            };
            std::unordered_map<uint64_t, int> recs; // (key, vb) -> record
            std::vector<uint64_t> newKey;          // per new record: its key
            auto rk = [](uint64_t key, int vb) { return (key << 8) ^ static_cast<uint64_t>(vb); };
            for (const auto &t : cutT)
                for (const int r : t)
                    recs.emplace(rk(static_cast<uint64_t>(pid[r]), m.VB[r]), r);
            for (size_t i = 0; i < R; ++i)
                recs.emplace(rk(static_cast<uint64_t>(pid[i]), m.VB[i]), static_cast<int>(i));
            auto record = [&](uint64_t key, D3 pos, int vb) -> int {
                if (const auto it = recs.find(rk(key, vb)); it != recs.end())
                    return it->second;
                double bd = std::numeric_limits<double>::infinity();
                size_t k = 0;
                D3 cq;
                for (size_t c = 0; c < cutT.size(); ++c)
                {
                    if (cutVb[c] != vb)
                        continue;
                    const D3 p = triClosest(pos, P[cutT[c][0]], P[cutT[c][1]], P[cutT[c][2]]);
                    const double d = dist2(p, pos);
                    if (d < bd)
                    {
                        bd = d;
                        k = c;
                        cq = p;
                    }
                }
                const std::array<int, 3> &t = cutT[k];
                D3 bc = bary(cq, P[t[0]], P[t[1]], P[t[2]]);
                bc = {std::clamp(bc.x, 0.0, 1.0), std::clamp(bc.y, 0.0, 1.0), std::clamp(bc.z, 0.0, 1.0)};
                const double s = bc.x + bc.y + bc.z;
                const double w[3] = {bc.x / s, bc.y / s, bc.z / s};
                double u = 0, v = 0;
                float C[4] = {};
                std::vector<std::pair<int, double>> mix;
                for (int i = 0; i < 3; ++i)
                {
                    u += w[i] * m.U[t[i]];
                    v += w[i] * m.V[t[i]];
                    for (int c = 0; c < 4; ++c)
                        C[c] += static_cast<float>(w[i] * m.C[4 * t[i] + c]);
                    for (int j = 0; j < 3; ++j)
                        addMix(mix, m.J[3 * t[i] + j], m.W[3 * t[i] + j], w[i] * m.W[3 * t[i] + j]);
                }
                int J[3];
                float W[3];
                topWeights(mix, J, W);
                const int id = static_cast<int>(pushRecord(m, pos, D3{}, u, v, C, J, W, vb));
                recs.emplace(rk(key, vb), id);
                newKey.push_back(key);
                return id;
            };

            // The patch, its edge triangles split at the hole's vertices.
            std::vector<std::array<uint64_t, 3>> pT;
            for (const auto &t : patchT)
                pT.push_back({keyOf[t[0]], keyOf[t[1]], keyOf[t[2]]});
            std::vector<char> alive(pT.size(), 1);
            auto distinct = [](const auto &t) { return t[0] != t[1] && t[1] != t[2] && t[0] != t[2]; };
            for (auto &[j, pts] : patchPts)
            {
                std::sort(pts.begin(), pts.end());
                const uint64_t a = keyOf[Pr[j]], b = keyOf[Pr[(j + 1) % nP]];
                std::vector<uint64_t> mids;
                for (const auto &pt : pts)
                    mids.push_back(pt.second);
                for (size_t x = 0; x < pT.size(); ++x)
                {
                    const auto &t = pT[x];
                    if (!alive[x] || std::find(t.begin(), t.end(), a) == t.end() || std::find(t.begin(), t.end(), b) == t.end() ||
                        !distinct(t))
                        continue;
                    alive[x] = 0;
                    for (const auto &f : fan(pT[x], a, b, mids))
                    {
                        pT.push_back(f);
                        alive.push_back(1);
                    }
                    break;
                }
            }
            {
                std::vector<std::array<uint64_t, 3>> live;
                for (size_t x = 0; x < pT.size(); ++x)
                    if (alive[x] && distinct(pT[x]))
                        live.push_back(pT[x]);
                pT = std::move(live);
            }
            // A hole corner that turns away from the patch's chord folds its fan: such a triangle's inner edge flipped
            // while both new triangles face the way her surface under them faces. The patch keeps Shannon's own winding
            // (consistent with hers along the shared edge).
            auto facing = [&](const std::array<uint64_t, 3> &t) {
                const D3 a = kpos(t[0]), b = kpos(t[1]), c = kpos(t[2]);
                size_t k = 0;
                const D3 q = closestCut((a + b + c) * (1.0 / 3.0), &k);
                const std::array<int, 3> &ct = cutT[k];
                D3 bc = bary(q, P[ct[0]], P[ct[1]], P[ct[2]]);
                bc = {std::clamp(bc.x, 0.0, 1.0), std::clamp(bc.y, 0.0, 1.0), std::clamp(bc.z, 0.0, 1.0)};
                return dot(cross(b - a, c - a), m.N[ct[0]] * bc.x + m.N[ct[1]] * bc.y + m.N[ct[2]] * bc.z);
            };
            for (int round = 0; round < 8; ++round)
            {
                bool changed = false;
                for (size_t x = 0; x < pT.size(); ++x)
                {
                    const std::array<uint64_t, 3> t = pT[x];
                    if (facing(t) >= 0)
                        continue;
                    for (int e = 0; e < 3; ++e)
                    {
                        const uint64_t p = t[e], q = t[(e + 1) % 3], r = t[(e + 2) % 3];
                        size_t y = 0, ny = 0;
                        for (size_t z = 0; z < pT.size(); ++z)
                            if (z != x && std::find(pT[z].begin(), pT[z].end(), p) != pT[z].end() &&
                                std::find(pT[z].begin(), pT[z].end(), q) != pT[z].end())
                            {
                                y = z;
                                ++ny;
                            }
                        if (ny != 1)
                            continue;
                        uint64_t sv = 0;
                        for (const uint64_t k : pT[y])
                            if (k != p && k != q)
                                sv = k;
                        const std::array<uint64_t, 3> a{p, sv, r}, b{sv, q, r};
                        if (facing(a) > 0 && facing(b) > 0)
                        {
                            pT[x] = a;
                            pT[y] = b;
                            changed = true;
                            break;
                        }
                    }
                }
                if (!changed)
                    break;
            }
            std::vector<int> newT, newTM;
            for (size_t x = 0; x < pT.size(); ++x)
            {
                const D3 ps[3] = {kpos(pT[x][0]), kpos(pT[x][1]), kpos(pT[x][2])};
                const int vb = vbAt((ps[0] + ps[1] + ps[2]) * (1.0 / 3.0));
                for (int k = 0; k < 3; ++k)
                    newT.push_back(record(pT[x][k], ps[k], vb));
                newTM.push_back(vbTm[vb]);
            }
            res.patch = static_cast<int>(newTM.size());

            // Her triangles without the cut, the ones on the hole's edge split at the patch's edge vertices (each new
            // record lerped along the edge, in that triangle's buffer).
            std::vector<std::array<int, 3>> keepT;
            std::vector<int> keepTM;
            for (size_t t = 0; t < tris; ++t)
                if (!cut[t])
                {
                    keepT.push_back({m.T[3 * t], m.T[3 * t + 1], m.T[3 * t + 2]});
                    keepTM.push_back(m.TM[t]);
                }
            std::vector<char> keepAlive(keepT.size(), 1);
            for (auto &[i, pts] : keptPts)
            {
                std::sort(pts.begin(), pts.end());
                const int ha = hole[i], hb = hole[(i + 1) % nH];
                const size_t n = keepT.size();
                for (size_t x = 0; x < n; ++x)
                {
                    if (!keepAlive[x])
                        continue;
                    const std::array<int, 3> t = keepT[x];
                    int a = -1, b = -1, na = 0, nb = 0;
                    for (const int v : t)
                        if (v < static_cast<int>(R))
                        {
                            if (pid[v] == ha)
                            {
                                a = v;
                                ++na;
                            }
                            if (pid[v] == hb)
                            {
                                b = v;
                                ++nb;
                            }
                        }
                    if (na != 1 || nb != 1)
                        continue;
                    std::vector<int> mids;
                    for (const auto &[tt, k] : pts)
                    {
                        D3 n3 = m.N[a] * (1 - tt) + m.N[b] * tt;
                        n3 = n3 * (1.0 / (len(n3) + 1e-12));
                        float C[4];
                        for (int c = 0; c < 4; ++c)
                            C[c] = static_cast<float>(m.C[4 * a + c] * (1 - tt) + m.C[4 * b + c] * tt);
                        std::vector<std::pair<int, double>> mix;
                        for (int j = 0; j < 3; ++j)
                            addMix(mix, m.J[3 * a + j], m.W[3 * a + j], (1 - tt) * m.W[3 * a + j]);
                        for (int j = 0; j < 3; ++j)
                            addMix(mix, m.J[3 * b + j], m.W[3 * b + j], tt * m.W[3 * b + j]);
                        int J[3];
                        float W[3];
                        topWeights(mix, J, W);
                        const double uu = m.U[a] * (1 - tt) + m.U[b] * tt, vv = m.V[a] * (1 - tt) + m.V[b] * tt;
                        mids.push_back(static_cast<int>(pushRecord(m, kpos(k), n3, uu, vv, C, J, W, m.VB[a])));
                        newKey.push_back(kKept);
                    }
                    keepAlive[x] = 0;
                    const int tm = keepTM[x];
                    for (const auto &f : fan(t, a, b, mids))
                    {
                        keepT.push_back(f);
                        keepTM.push_back(tm);
                        keepAlive.push_back(1);
                    }
                }
            }
            for (const auto &v : keptPts)
                res.shared += static_cast<int>(v.second.size());
            for (const auto &v : patchPts)
                res.shared += static_cast<int>(v.second.size());

            // The triangles: hers, then the patch.
            std::vector<int> T;
            std::vector<int> TM;
            for (size_t x = 0; x < keepT.size(); ++x)
                if (keepAlive[x])
                {
                    T.insert(T.end(), keepT[x].begin(), keepT[x].end());
                    TM.push_back(keepTM[x]);
                }
            T.insert(T.end(), newT.begin(), newT.end());
            TM.insert(TM.end(), newTM.begin(), newTM.end());
            m.T = std::move(T);
            m.TM = std::move(TM);

            // Normals: shading follows the geometry's blend: her own normal (at the closest point of the cut) at the
            // patch's edge, the patch's own over the same falloff. Normals recomputed across the patch's edge left a hard
            // line down the side of the hip (K, Nov 15). The flat state: each new record at its closest point on the cut
            // surface, her normal; the shared edge's records are where they are in both states.
            const std::vector<D3> sn = smoothNormals(m.P, m.T, m.triangles());
            res.detailN = m.N;
            res.ring.assign(R, 0);
            for (size_t r = R; r < m.records(); ++r)
            {
                const uint64_t key = newKey[r - R];
                if (key == kKept)
                {
                    res.flatP.push_back(m.P[r]);
                    res.flatN.push_back(m.N[r]);
                    continue;
                }
                size_t c = 0;
                const D3 q = closestCut(m.P[r], &c);
                const std::array<int, 3> &t = cutT[c];
                D3 bc = bary(q, P[t[0]], P[t[1]], P[t[2]]);
                bc = {std::clamp(bc.x, 0.0, 1.0), std::clamp(bc.y, 0.0, 1.0), std::clamp(bc.z, 0.0, 1.0)};
                const double bs = bc.x + bc.y + bc.z;
                D3 her = (m.N[t[0]] * bc.x + m.N[t[1]] * bc.y + m.N[t[2]] * bc.z) * (1.0 / bs);
                her = her * (1.0 / (len(her) + 1e-12));
                double f = 0.0;
                if (key & kDonor)
                    if (const auto it = fall.find(static_cast<int>(key & 0xFFFFFFFFu)); it != fall.end())
                        f = it->second;
                const D3 n = her * (1 - f) + sn[r] * f;
                res.detailN[r] = n * (1.0 / (len(n) + 1e-12));
                res.flatP.push_back(edgeKeys.count(key) ? m.P[r] : q);
                res.flatN.push_back(her);
            }
            res.cut = std::move(cut);
            return true;
        }

        // ---- strips (tools/models/strip.py) ------------------------------------------------------------------------
        struct Rng
        {
            uint64_t s;
            explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
            uint32_t next()
            {
                s ^= s << 13;
                s ^= s >> 7;
                s ^= s << 17;
                return static_cast<uint32_t>(s >> 11);
            }
        };

        using Strip = std::vector<std::pair<int, int>>; // (vertex, the triangle it closes or -1)

        std::vector<Strip> stripifyOnce(const std::vector<std::array<int, 3>> &tris, Rng *rng)
        {
            std::unordered_map<uint64_t, std::vector<int>> edgeTris;
            for (size_t t = 0; t < tris.size(); ++t)
                for (int e = 0; e < 3; ++e)
                    edgeTris[pairKey(tris[t][e], tris[t][(e + 1) % 3])].push_back(static_cast<int>(t));
            std::vector<char> used(tris.size());
            auto across = [&](int t, int p, int q, int &x) -> int {
                const auto it = edgeTris.find(pairKey(p, q));
                if (it == edgeTris.end())
                    return -1;
                for (const int u : it->second)
                {
                    if (u == t || used[u])
                        continue;
                    int rest = -1, n = 0;
                    for (int k = 0; k < 3; ++k)
                        if (tris[u][k] != p && tris[u][k] != q)
                        {
                            rest = tris[u][k];
                            ++n;
                        }
                    if (n == 1)
                    {
                        x = rest;
                        return u;
                    }
                }
                return -1;
            };
            auto degree = [&](int t) {
                int d = 0;
                for (int e = 0; e < 3; ++e)
                    for (const int u : edgeTris[pairKey(tris[t][e], tris[t][(e + 1) % 3])])
                        d += (u != t && !used[u]);
                return d;
            };
            auto grow = [&](Strip &seq, int t, std::vector<int> &taken) {
                for (;;)
                {
                    int x = -1;
                    const int u = across(t, seq[seq.size() - 2].first, seq.back().first, x);
                    if (u < 0)
                        return;
                    t = u;
                    used[t] = 1;
                    taken.push_back(t);
                    seq.emplace_back(x, t);
                }
            };
            auto build = [&](int start, int rot, bool keepIt, Strip &seq) {
                const int a = tris[start][rot], b = tris[start][(rot + 1) % 3], c = tris[start][(rot + 2) % 3];
                used[start] = 1;
                Strip back{{c, -1}, {b, -1}, {a, start}};
                std::vector<int> takenB, takenF;
                grow(back, start, takenB);
                const size_t n = back.size();
                seq.clear();
                for (size_t i = 0; i < n; ++i) // _reverse
                    seq.emplace_back(back[n - 1 - i].first, i >= 2 ? back[n + 1 - i].second : -1);
                grow(seq, start, takenF);
                if (!keepIt)
                {
                    for (const int u : takenB)
                        used[u] = 0;
                    for (const int u : takenF)
                        used[u] = 0;
                    used[start] = 0;
                }
                return 1 + static_cast<int>(takenB.size() + takenF.size());
            };
            std::vector<Strip> strips;
            size_t left = tris.size();
            while (left)
            {
                int start = -1, bestD = 1 << 30;
                std::vector<int> ties;
                for (size_t t = 0; t < tris.size(); ++t)
                {
                    if (used[t])
                        continue;
                    const int d = degree(static_cast<int>(t));
                    if (d < bestD)
                    {
                        bestD = d;
                        ties.clear();
                    }
                    if (d == bestD)
                        ties.push_back(static_cast<int>(t));
                }
                start = rng ? ties[rng->next() % ties.size()] : ties[0];
                int rots[3] = {0, 1, 2};
                if (rng)
                    for (int k = 2; k > 0; --k)
                        std::swap(rots[k], rots[rng->next() % (k + 1)]);
                int bestRot = rots[0], bestN = -1;
                size_t bestLen = 0;
                Strip seq;
                for (const int r : rots)
                {
                    const int n = build(start, r, false, seq);
                    if (n > bestN || (n == bestN && seq.size() < bestLen))
                    {
                        bestN = n;
                        bestLen = seq.size();
                        bestRot = r;
                    }
                }
                build(start, bestRot, true, seq);
                strips.push_back(seq);
                left = 0;
                for (const char u : used)
                    left += !u;
            }
            return strips;
        }

        // build_buffers: strips into buffers of at most maxVerts, each triangle's flag from its own winding.
        std::vector<std::vector<std::pair<int, int>>> buildBuffers(const std::vector<std::array<int, 3>> &tris, size_t maxVerts)
        {
            std::vector<Strip> best;
            size_t bestCost = ~size_t(0);
            for (int k = 0; k < 24; ++k)
            {
                Rng rng(static_cast<uint64_t>(k));
                std::vector<Strip> s = stripifyOnce(tris, k ? &rng : nullptr);
                size_t cost = 0;
                for (const Strip &x : s)
                    cost += x.size();
                if (cost < bestCost)
                {
                    bestCost = cost;
                    best = std::move(s);
                }
            }
            std::vector<std::vector<std::pair<int, int>>> bufs;
            std::vector<std::pair<int, int>> cur;
            for (const Strip &s : best)
            {
                if (cur.size() + 3 > maxVerts)
                {
                    bufs.push_back(cur);
                    cur.clear();
                }
                int last[2] = {-1, -1};
                int lastN = 0;
                for (const auto &[v, tid] : s)
                {
                    if (tid >= 0)
                    {
                        if (cur.size() + 1 > maxVerts)
                        {
                            bufs.push_back(cur);
                            cur = {{last[0], 0x8000}, {last[1], 0x8000}};
                        }
                        const auto &t = tris[tid];
                        const bool same = (last[0] == t[0] && last[1] == t[1] && v == t[2]) ||
                                          (last[0] == t[1] && last[1] == t[2] && v == t[0]) ||
                                          (last[0] == t[2] && last[1] == t[0] && v == t[1]);
                        cur.emplace_back(v, same ? 0 : 1);
                    }
                    else
                        cur.emplace_back(v, 0x8000);
                    if (lastN < 2)
                        last[lastN++] = v;
                    else
                    {
                        last[0] = last[1];
                        last[1] = v;
                    }
                }
            }
            if (!cur.empty())
                bufs.push_back(cur);
            return bufs;
        }

        // ---- Cash's growth (tools/models/body_detail/cash.py) -------------------------------------------------------
        // At 130% per height (K's side-profile reference, Nov 15): y = 0.79, 0.80, ... 1.23, mm.
        constexpr double kCashProfile[] = {11.4, 20.3, 20.4, 20.7, 20.7, 20.7, 20.9, 19.9, 20.7, 20.0, 18.5, 17.7,
                                           17.2, 17.6, 21.1, 30.9, 37.8, 40.8, 41.2, 39.3, 37.2, 36.0, 35.6, 35.0,
                                           34.2, 33.4, 32.4, 30.7, 28.7, 25.0, 23.0, 18.7, 15.1, 11.0, 7.0,  4.6,
                                           3.5,  1.9,  0.3,  0.3,  0.7,  0.0,  0.0,  0.0,  0.0};
        constexpr double kCashY0 = 0.79, kCashDy = 0.01;
        constexpr double kThigh = 0.35, kCrease = 0.925, kHip = 0.35, kDown = 0.45, kUp = 0.3;
        constexpr double kAxisX = 0.06, kAxisY = 1.00, kAxisZ = -0.03; // the cheeks' centre line

        double sstep(double a, double b, double x)
        {
            const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
            return t * t * (3.0 - 2.0 * t);
        }
        double cashProfile(double y) // np.interp, the ends held (the top end 0)
        {
            constexpr int n = static_cast<int>(std::size(kCashProfile));
            const double f = (y - kCashY0) / kCashDy;
            if (f <= 0.0)
                return kCashProfile[0] / 1000.0;
            if (f >= n - 1)
                return 0.0;
            const int i = static_cast<int>(f);
            const double t = f - i;
            return (kCashProfile[i] * (1.0 - t) + kCashProfile[i + 1] * t) / 1000.0;
        }

        // cash.field: the move at 130% at a position (position only, so a flap or a hem over the body moves with it).
        D3 cashField(D3 p)
        {
            double g = cashProfile(p.y);
            if (p.y < kCrease)
                g *= kThigh + (1.0 - kThigh) * sstep(kCrease - 0.03, kCrease, p.y);
            g *= sstep(0.72, 0.80, p.y);
            const double ax = std::fabs(p.x);
            const double lat = (ax < 0.02 ? 0.5 + 0.5 * sstep(0.0, 0.02, ax) : 1.0) * (1.0 - (1.0 - kHip) * sstep(0.10, 0.165, ax));
            D3 r = p - D3{std::clamp(p.x, -kAxisX, kAxisX), kAxisY, kAxisZ};
            r = r * (1.0 / (len(r) + 1e-12));
            const double h = std::hypot(0.6 * r.x, r.z);
            const double slope = std::clamp(r.y / std::max(h, 1e-6), -kDown, kUp);
            D3 d{0.6 * r.x, slope * h, r.z};
            d = d * (1.0 / (len(d) + 1e-12));
            const double face = std::clamp((-r.z + 0.2) / 0.5, 0.0, 1.0);
            const double below = 1.0 - sstep(kCrease - 0.03, kCrease, p.y); // between the thighs: little
            const double inner = 1.0 - below * (1.0 - (0.3 + 0.7 * sstep(0.02, 0.05, ax)));
            return d * (g * lat * face * inner);
        }

        // cash.growth: each record's move at 130% (D) and its normal there (N130), from the detailed state (P, N).
        void cashGrowth(const std::vector<D3> &P, const std::vector<D3> &N, const std::vector<int> &tris, size_t triCount,
                        std::vector<D3> &D, std::vector<D3> &N130)
        {
            const size_t R = P.size();
            D.assign(R, D3{});
            std::vector<D3> moved(R);
            for (size_t i = 0; i < R; ++i)
            {
                D[i] = cashField(P[i]);
                moved[i] = P[i] + D[i];
            }
            const std::vector<D3> sn = smoothNormals(P, tris, triCount), sn2 = smoothNormals(moved, tris, triCount);
            N130.assign(R, D3{});
            for (size_t i = 0; i < R; ++i)
            {
                const D3 n = N[i] + (sn2[i] - sn[i]);
                N130[i] = n * (1.0 / (len(n) + 1e-12));
            }
        }

        // ---- output ------------------------------------------------------------------------------------------------
        int8_t q8(double v) { return static_cast<int8_t>(std::clamp(std::lround(v * 127.0), -127L, 127L)); }
        void quantN(D3 n, bool normalize, int8_t out[3])
        {
            if (normalize)
            {
                const double l = len(n);
                n = l > 1e-9 ? n * (1.0 / l) : D3{0.0, 1.0, 0.0};
            }
            out[0] = q8(n.x);
            out[1] = q8(n.y);
            out[2] = q8(n.z);
        }

        // A finished record: both states, quantised as stored.
        struct Rec
        {
            int vb = 0;
            float flat[3] = {}, detail[3] = {};
            int8_t flatN[3] = {}, detailN[3] = {};
            float grow[3] = {};  // Cash: the move at 130% on top of the detailed state
            int8_t growN[3] = {};
            int16_t uv[2] = {};
            uint8_t col[4] = {};
            uint8_t wt[8] = {};
            uint8_t region = 0;
            std::string key() const
            {
                std::string k;
                auto add = [&k](const void *p, size_t n) { k.append(static_cast<const char *>(p), n); };
                add(&vb, sizeof vb);
                add(flat, sizeof flat);
                add(detail, sizeof detail);
                add(flatN, 3);
                add(detailN, 3);
                add(grow, sizeof grow);
                add(growN, 3);
                add(uv, sizeof uv);
                add(col, 4);
                add(wt, 8);
                return k;
            }
        };

        std::vector<uint8_t> emptyMdb(const uint8_t *scr, uint32_t mdb, uint16_t mat)
        {
            const uint16_t nb = r16(scr + mdb + 8);
            const uint32_t boneAt = mdb + r32(scr + mdb + 4);
            std::vector<uint8_t> out(32, 0u);
            std::memcpy(out.data(), "mdb\0", 4);
            w16(out, 8, nb);
            w16(out, 10, 1);
            std::memcpy(out.data() + 12, scr + mdb + 12, 20);
            put32(out, 0); // the buffer table
            pad16(out);
            w32(out, 4, static_cast<uint32_t>(out.size()));
            out.insert(out.end(), scr + boneAt, scr + boneAt + 16u * nb);
            pad16(out);
            const size_t vb = out.size();
            w32(out, 32, static_cast<uint32_t>(vb));
            out.resize(vb + 32, 0u);
            const uint32_t po = 32;
            for (int k = 0; k < 3; ++k)
            {
                putf(out, 0.0f);
                putf(out, 0.0f);
                putf(out, 0.0f);
                put32(out, k < 2 ? 0x8000u : 0u);
            }
            pad16(out);
            const uint32_t no = static_cast<uint32_t>(out.size() - vb);
            for (int k = 0; k < 3; ++k)
                put32(out, 0x00007F00u); // (0, 127, 0)
            pad16(out);
            const uint32_t uo = static_cast<uint32_t>(out.size() - vb);
            for (int k = 0; k < 3; ++k)
                put32(out, 0u);
            pad16(out);
            const uint32_t co = static_cast<uint32_t>(out.size() - vb);
            for (int k = 0; k < 3; ++k)
                put32(out, 0x80808080u);
            pad16(out);
            const uint32_t wo = static_cast<uint32_t>(out.size() - vb);
            const uint8_t bone = static_cast<uint8_t>((nb > 4 ? 4 : 0) * 4);
            for (int k = 0; k < 3; ++k)
            {
                const uint8_t w[8] = {0, bone, 0, 0, 100, 0, 0, 0};
                out.insert(out.end(), w, w + 8);
            }
            pad16(out);
            w32(out, vb, po);
            w32(out, vb + 4, no);
            w32(out, vb + 8, uo);
            w32(out, vb + 12, co);
            w32(out, vb + 16, wo);
            w16(out, vb + 20, 3);
            w16(out, vb + 22, mat);
            return out;
        }
    }

    // ---- the jiggle bones (body_detail_build.h) -------------------------------------------------------------------
    namespace
    {
        constexpr int kPelvisBone = 17, kSeatBone = 27, kRightBone = 28, kBustBone = 29;
        constexpr double kCleavage = 0.02; // the bust's blend half width round x = 0
        double cashPeak()
        {
            double m = 0.0;
            for (const double v : kCashProfile)
                m = std::max(m, v);
            return m / 1000.0;
        }
    }

    // The cheeks under the growth (none at the front, the hip sides or the waist), faded out above y 1.00..1.12: the
    // upper cheeks are nearly all pelvis, the lower ones half thigh, so without the fade the bounce sat at the waist.
    float seatMask(D3 p)
    {
        return static_cast<float>(sstep(0.15, 0.6, len(cashField(p)) / cashPeak()) * (1.0 - sstep(1.00, 1.12, p.y)));
    }
    float rightShare(double x) { return static_cast<float>(sstep(kCleavage, -kCleavage, x)); }

    bool jiggleWeights(uint8_t wt[8], D3 p, bool seat, bool bust)
    {
        std::vector<std::pair<int, double>> mix;
        int total = 0;
        for (int s = 0; s < 3; ++s)
        {
            const int j = wt[1 + s] / 4, w = wt[4 + s];
            if (w <= 0)
                continue;
            if (j == kSeatBone || j == kRightBone)
                return false; // already re-skinned
            total += w;
            bool found = false;
            for (auto &e : mix)
                if (e.first == j)
                    e.second += w, found = true;
            if (!found)
                mix.emplace_back(j, w);
        }
        if (total <= 0)
            return false;
        const auto move = [&mix](int from, int to, double share) {
            if (share <= 0.0)
                return;
            for (size_t i = 0; i < mix.size(); ++i)
                if (mix[i].first == from)
                {
                    const double m = mix[i].second * share;
                    mix[i].second -= m;
                    mix.emplace_back(to, m);
                    return;
                }
        };
        // A record with no free slot (Shannon's cleft line: pelvis and both thighs) moves all or nothing: dropping a
        // thigh would pull a centre vertex to one side.
        double seatShare = seat ? seatMask(p) : 0.0, bustShare = bust ? rightShare(p.x) : 0.0;
        const auto has = [&mix](int b) {
            for (const auto &e : mix)
                if (e.first == b)
                    return true;
            return false;
        };
        const size_t extra = (seatShare > 0.0 && has(kPelvisBone) ? 1u : 0u) + (bustShare > 0.0 && has(kBustBone) ? 1u : 0u);
        if (mix.size() + extra > 3u)
        {
            seatShare = seatShare > 0.5 ? 1.0 : 0.0;
            bustShare = bustShare > 0.5 ? 1.0 : 0.0;
        }
        move(kPelvisBone, kSeatBone, seatShare);
        move(kBustBone, kRightBone, bustShare);
        mix.erase(std::remove_if(mix.begin(), mix.end(), [](const auto &e) { return e.second <= 0.0; }), mix.end());
        // The three heaviest in whole percents summing to the record's own total (largest remainder).
        std::stable_sort(mix.begin(), mix.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        if (mix.size() > 3)
            mix.resize(3);
        double sum = 0.0;
        for (const auto &e : mix)
            sum += e.second;
        int J[3] = {0, 0, 0}, W[3] = {0, 0, 0}, used = 0;
        double rem[3] = {0.0, 0.0, 0.0};
        for (size_t k = 0; k < mix.size(); ++k)
        {
            const double exact = total * mix[k].second / sum;
            J[k] = mix[k].first;
            W[k] = static_cast<int>(std::floor(exact));
            rem[k] = exact - W[k];
            used += W[k];
        }
        for (; used < total; ++used)
        {
            size_t best = 0;
            for (size_t k = 1; k < mix.size(); ++k)
                if (rem[k] > rem[best])
                    best = k;
            ++W[best];
            rem[best] = -1.0;
        }
        bool changed = false;
        for (int k = 0; k < 3; ++k)
            if (W[k] > 0 && (J[k] == kSeatBone || J[k] == kRightBone))
                changed = true;
        if (!changed)
            return false; // the moved share rounded away: the record stays as it was
        for (int k = 0; k < 3; ++k)
        {
            wt[1 + k] = static_cast<uint8_t>(W[k] > 0 ? J[k] * 4 : 0);
            wt[4 + k] = static_cast<uint8_t>(W[k]);
        }
        return true;
    }

    bool jiggleSpare(const uint8_t *scr, size_t size, uint32_t mdb, JiggleBind *bind)
    {
        if (!inside(size, mdb, 32))
            return false;
        const uint16_t nb = r16(scr + mdb + 8);
        const uint32_t at = mdb + r32(scr + mdb + 4);
        if (nb < 30 || !inside(size, at, 16u * nb))
            return false;
        const auto parent = [&](int i) { int16_t v; std::memcpy(&v, scr + at + 16 * i + 14, 2); return static_cast<int>(v); };
        const auto t = [&](int i, int k) { return rf(scr + at + 16 * i + 4 * k); };
        for (const int b : {kSeatBone, kRightBone})
            if (parent(b) != -1 || t(b, 0) != 0.0f || t(b, 1) != 0.0f || t(b, 2) != 0.0f)
                return false;
        if (parent(kBustBone) != 2 || parent(kPelvisBone) != 0)
            return false;
        if (bind)
        {
            const auto place = [&](int i, float out[3]) {
                double x = 0, y = 0, z = 0;
                for (int b = i, guard = 0; b >= 0 && b < nb && guard < 64; b = parent(b), ++guard)
                    x += t(b, 0), y += t(b, 1), z += t(b, 2);
                out[0] = static_cast<float>(x), out[1] = static_cast<float>(y), out[2] = static_cast<float>(z);
            };
            place(kPelvisBone, bind->pelvis);
            place(kBustBone, bind->bust);
        }
        return true;
    }

    int liftStraps(uint8_t *scr, size_t size, const char *node, float lift)
    {
        std::vector<NodeRef> nodes;
        if (!readNodes(scr, size, nodes))
            return 0;
        int moved = 0;
        for (const NodeRef &r : nodes)
        {
            std::vector<VbRef> vbs;
            if (std::strncmp(r.name, node, 8) != 0 || (r.flags & 0x01000000u) || !readVbs(scr, size, r.mdb, vbs))
                continue;
            // Every vertex (buffer, index), merged where the strips share a position; joined by the strips' triangles.
            struct V
            {
                uint32_t at;
                D3 p;
            };
            std::vector<V> verts;
            std::vector<std::array<size_t, 3>> tris;
            for (const VbRef &b : vbs)
            {
                const size_t base = verts.size();
                for (uint32_t k = 0; k < b.count; ++k)
                {
                    const uint8_t *q = scr + b.at + b.pos + 16 * k;
                    verts.push_back({b.at + b.pos + 16 * k, D3{rf(q), rf(q + 4), rf(q + 8)}});
                    if (k >= 2 && r32(q + 12) != 0x8000u)
                        tris.push_back({base + k - 2, base + k - 1, base + k});
                }
            }
            std::vector<size_t> parent(verts.size());
            for (size_t i = 0; i < parent.size(); ++i)
                parent[i] = i;
            const auto root = [&](size_t x) {
                while (parent[x] != x)
                    x = parent[x] = parent[parent[x]];
                return x;
            };
            std::map<std::array<int64_t, 3>, size_t> at;
            for (size_t i = 0; i < verts.size(); ++i)
            {
                const std::array<int64_t, 3> key{std::llround(verts[i].p.x * 1e5), std::llround(verts[i].p.y * 1e5), std::llround(verts[i].p.z * 1e5)};
                const auto [it, added] = at.emplace(key, i);
                if (!added)
                    parent[root(i)] = root(it->second);
            }
            for (const auto &t : tris)
                for (int e = 0; e < 2; ++e)
                    parent[root(t[static_cast<size_t>(e)])] = root(t[static_cast<size_t>(e) + 1]);
            std::map<size_t, std::vector<size_t>> parts;
            for (size_t i = 0; i < verts.size(); ++i)
                parts[root(i)].push_back(i);
            // The straps: small pieces (16 vertices at most) below the hips that wrap the thigh (8 cm across or more);
            // with them the buckles on them (small pieces at a strap's height).
            struct Span
            {
                double lo, hi, xs;
            };
            const auto spanOf = [&](const std::vector<size_t> &m) {
                Span sp{1e9, -1e9, 0};
                double x0 = 1e9, x1 = -1e9;
                for (const size_t i : m)
                    sp.lo = std::min(sp.lo, verts[i].p.y), sp.hi = std::max(sp.hi, verts[i].p.y), x0 = std::min(x0, verts[i].p.x),
                    x1 = std::max(x1, verts[i].p.x);
                sp.xs = x1 - x0;
                return sp;
            };
            std::vector<Span> straps;
            for (const auto &[root_, m] : parts)
                if (const Span sp = spanOf(m); m.size() <= 16 && sp.hi < 0.8 && sp.xs >= 0.06)
                    straps.push_back(sp);
            for (const auto &[root_, m] : parts)
            {
                const Span sp = spanOf(m);
                bool lifted = m.size() <= 16 && sp.hi < 0.8 && sp.xs >= 0.06;
                for (const Span &s : straps)
                    lifted = lifted || (m.size() <= 16 && sp.lo < s.hi + 0.005 && sp.hi > s.lo - 0.005);
                if (!lifted)
                    continue;
                for (const size_t i : m) // along the vertex's own normal
                {
                    const VbRef *b = nullptr;
                    for (const VbRef &vb : vbs)
                        if (verts[i].at >= vb.at + vb.pos && verts[i].at < vb.at + vb.pos + 16u * vb.count)
                            b = &vb;
                    if (!b || !b->nrm)
                        continue;
                    const uint32_t k = (verts[i].at - b->at - b->pos) / 16u;
                    const int8_t *n = reinterpret_cast<const int8_t *>(scr + b->at + b->nrm + 4 * k);
                    const double len = std::sqrt(double(n[0]) * n[0] + double(n[1]) * n[1] + double(n[2]) * n[2]);
                    if (len < 1.0)
                        continue;
                    for (int c = 0; c < 3; ++c)
                    {
                        float v;
                        std::memcpy(&v, scr + verts[i].at + 4 * c, 4);
                        v += static_cast<float>(lift * n[c] / len);
                        std::memcpy(scr + verts[i].at + 4 * c, &v, 4);
                    }
                    ++moved;
                }
            }
        }
        return moved;
    }

    int reskin(uint8_t *scr, size_t size, const char *const *names, int count, int jiggle)
    {
        std::vector<NodeRef> nodes;
        if (jiggle == 0 || !readNodes(scr, size, nodes))
            return 0;
        int applied = 0;
        for (const NodeRef &r : nodes)
        {
            bool named = false;
            for (int k = 0; k < count; ++k)
                named = named || std::strncmp(r.name, names[k], 8) == 0;
            std::vector<VbRef> vbs;
            if (!named || (r.flags & 0x01000000u) || !jiggleSpare(scr, size, r.mdb, nullptr) || !readVbs(scr, size, r.mdb, vbs))
                continue;
            for (const VbRef &b : vbs)
            {
                if (!b.wt)
                    continue;
                for (uint32_t k = 0; k < b.count; ++k)
                {
                    const uint8_t *q = scr + b.at + b.pos + 16 * k;
                    jiggleWeights(scr + b.at + b.wt + 8 * k, D3{rf(q), rf(q + 4), rf(q + 8)}, (jiggle & kJiggleSeat) != 0,
                                  (jiggle & kJiggleBust) != 0);
                }
            }
            applied = jiggle;
        }
        return applied;
    }

    bool loadDonor(const uint8_t *dat, size_t size, const char *node, Mesh &out)
    {
        if (size < 8)
            return false;
        const uint32_t n = r32(dat);
        if (n < 2 || !inside(size, 4, 4u * n))
            return false;
        const uint32_t at = r32(dat + 8);
        uint32_t end = static_cast<uint32_t>(size);
        for (uint32_t i = 0; i < n; ++i)
        {
            const uint32_t o = r32(dat + 4 + 4 * i);
            if (o > at && o < end)
                end = o;
        }
        if (at == 0 || at >= end)
            return false;
        const uint8_t *scr = dat + at;
        const size_t scrSize = end - at;
        std::vector<NodeRef> nodes;
        if (!readNodes(scr, scrSize, nodes))
            return false;
        for (const NodeRef &r : nodes)
            if (std::strcmp(r.name, node) == 0)
                return loadMesh(scr, scrSize, r.mdb, out);
        return false;
    }

    bool build(const uint8_t *scr, size_t size, const Mesh &bunny, const Mesh &shannon, bool withBust, bool withSeat, Built &out,
               bool grow)
    {
        out = Built{};
        std::vector<NodeRef> nodes;
        if (!readNodes(scr, size, nodes))
        {
            out.error = "not a model";
            return false;
        }
        const NodeRef *body = nullptr;
        for (const NodeRef &r : nodes)
            if (std::strcmp(r.name, "body") == 0)
            {
                body = &r;
                break;
            }
        if (!body)
        {
            out.error = "no body node";
            return false;
        }
        Mesh m;
        std::vector<VbRef> vbs;
        if (!loadMesh(scr, size, body->mdb, m, &vbs))
        {
            out.error = "body unreadable";
            return false;
        }
        for (const VbRef &b : vbs)
            if (!b.nrm || !b.col || !b.wt)
            {
                out.error = "body buffer without normals / colours / weights";
                return false;
            }
        const size_t R0 = m.records();

        // The bust (every record keeps its own flat state; the bust's moved records their detailed one).
        std::vector<D3> bustP, bustN;
        std::vector<char> moved;
        if (withBust)
        {
            bust(m, bunny, bustP, bustN, moved);
            crease(m, bustP, bustN);
            moved.resize(m.records(), 1); // the crease's copies are moved records
        }
        else
        {
            bustP = m.P;
            bustN = m.N;
            moved.assign(m.records(), 0);
        }
        out.bustAdded = static_cast<int>(m.records() - R0);
        const size_t R1 = m.records();
        const std::vector<D3> flatN1 = m.N; // the subdivided normals (midpoints lerped)

        SeatResult sr;
        bool seated = false;
        if (withSeat)
        {
            seated = seat(m, shannon, sr);
            if (!seated)
                out.error = "no seat cut (bust only)";
            out.seatPatch = sr.patch;
            out.seatShared = sr.shared;
        }

        // Every record in both states.
        const size_t R = m.records();
        std::vector<Rec> rec(R);
        std::vector<D3> detP(R), detN(R); // the detailed state, for Cash's growth
        for (size_t i = 0; i < R; ++i)
        {
            Rec &r = rec[i];
            r.vb = m.VB[i];
            D3 fp = m.P[i], dp = m.P[i], fn = m.N[i], dn = m.N[i];
            const bool isNew = i >= R0; // an original record keeps its stored normal exactly in the flat state
            if (i < R1)
            {
                fn = flatN1[i];
                if (moved[i])
                {
                    dp = bustP[i];
                    dn = bustN[i];
                    r.region = 1;
                }
                else
                    dn = fn;
                if (seated && sr.ring[i])
                {
                    dn = sr.detailN[i];
                    r.region = 2;
                }
            }
            else
            {
                fp = sr.flatP[i - R1];
                fn = sr.flatN[i - R1];
                dp = m.P[i];
                dn = sr.detailN[i];
                r.region = 2;
            }
            detP[i] = dp;
            detN[i] = dn;
            r.flat[0] = static_cast<float>(fp.x);
            r.flat[1] = static_cast<float>(fp.y);
            r.flat[2] = static_cast<float>(fp.z);
            r.detail[0] = static_cast<float>(dp.x);
            r.detail[1] = static_cast<float>(dp.y);
            r.detail[2] = static_cast<float>(dp.z);
            quantN(fn, isNew, r.flatN);
            quantN(dn, isNew || r.region != 0, r.detailN);
            r.uv[0] = static_cast<int16_t>(std::clamp(std::lround(m.U[i]), -32768L, 32767L));
            r.uv[1] = static_cast<int16_t>(std::clamp(std::lround(m.V[i]), -32768L, 32767L));
            for (int c = 0; c < 4; ++c)
                r.col[c] = static_cast<uint8_t>(std::clamp(std::lround(m.C[4 * i + c]), 0L, 255L));
            for (int s = 0; s < 3; ++s)
            {
                r.wt[1 + s] = static_cast<uint8_t>(m.J[3 * i + s] * 4);
                r.wt[4 + s] = static_cast<uint8_t>(std::clamp(std::lround(m.W[3 * i + s]), 0L, 255L));
            }
            if (std::memcmp(r.flat, r.detail, 12) == 0 && std::memcmp(r.flatN, r.detailN, 3) == 0)
                r.region = 0;
        }

        // The jiggle bones: the seat's pelvis share to 27 (with the detailed seat only), the bust's right half to 28.
        if (jiggleSpare(scr, size, body->mdb, &out.bind))
        {
            for (size_t i = 0; i < R; ++i)
                if (jiggleWeights(rec[i].wt, detP[i], seated, true))
                    ++out.jiggleRecords;
            out.jiggle = (seated ? kJiggleSeat : 0) | kJiggleBust;
        }

        // Cash's growth, on the bodies with the detailed seat (em07's dress is too coarse: 35 positions round the seat
        // turned into big facets, Nov 15): seat-region records (2) on top of the detailed state.
        if (seated)
        {
            std::vector<D3> D, N130;
            cashGrowth(detP, detN, m.T, m.triangles(), D, N130);
            for (size_t i = 0; i < R; ++i)
            {
                Rec &r = rec[i];
                if (r.region == 1 || len(D[i]) < 1e-5)
                    continue;
                r.grow[0] = static_cast<float>(D[i].x);
                r.grow[1] = static_cast<float>(D[i].y);
                r.grow[2] = static_cast<float>(D[i].z);
                quantN(N130[i], true, r.growN);
                if (r.region == 0)
                    r.region = 2; // flat = detailed: only Cash moves it
                ++out.cashGrown;
            }
        }

        // Restripped per buffer (identical records merged first), each buffer at most kMaxVerts.
        constexpr size_t kMaxVerts = 480;
        struct OutVb
        {
            int src = 0;
            std::vector<std::pair<int, int>> verts; // (record, flag)
        };
        std::vector<OutVb> outVbs;
        for (size_t v = 0; v < vbs.size(); ++v)
        {
            std::unordered_map<std::string, int> uniq;
            std::vector<int> first;
            std::vector<std::array<int, 3>> tris;
            for (size_t t = 0; t < m.triangles(); ++t)
            {
                const int a = m.T[3 * t];
                if (m.VB[a] != static_cast<int>(v))
                    continue;
                std::array<int, 3> u{};
                for (int k = 0; k < 3; ++k)
                {
                    const int r = m.T[3 * t + k];
                    const auto it = uniq.emplace(rec[r].key(), static_cast<int>(first.size()));
                    if (it.second)
                        first.push_back(r);
                    u[k] = it.first->second;
                }
                if (u[0] != u[1] && u[1] != u[2] && u[0] != u[2])
                    tris.push_back(u);
            }
            if (tris.empty())
                continue;
            for (auto &buf : buildBuffers(tris, kMaxVerts))
            {
                OutVb o;
                o.src = static_cast<int>(v);
                for (const auto &[id, flag] : buf)
                    o.verts.emplace_back(first[id], flag);
                outVbs.push_back(std::move(o));
            }
        }

        // The body's mdb: the original's header and bone table, the new buffers (flat state).
        const uint32_t bm = body->mdb;
        const uint16_t nb = r16(scr + bm + 8);
        const uint32_t boneAt = bm + r32(scr + bm + 4);
        if (!inside(size, boneAt, 16u * nb))
        {
            out.error = "bone table";
            return false;
        }
        std::vector<uint8_t> mdb(32, 0u);
        std::memcpy(mdb.data(), "mdb\0", 4);
        w16(mdb, 8, nb);
        w16(mdb, 10, static_cast<uint16_t>(outVbs.size()));
        std::memcpy(mdb.data() + 12, scr + bm + 12, 20);
        mdb.resize(32 + 4 * outVbs.size(), 0u);
        pad16(mdb);
        w32(mdb, 4, static_cast<uint32_t>(mdb.size()));
        mdb.insert(mdb.end(), scr + boneAt, scr + boneAt + 16u * nb);
        pad16(mdb);
        std::vector<Change> changes;
        for (size_t b = 0; b < outVbs.size(); ++b)
        {
            const OutVb &o = outVbs[b];
            const VbRef &src = vbs[o.src];
            const size_t at = mdb.size();
            w32(mdb, 32 + 4 * b, static_cast<uint32_t>(at - 0));
            mdb.resize(at + 32, 0u);
            const uint32_t n = static_cast<uint32_t>(o.verts.size());
            const uint32_t po = 32;
            for (const auto &[r, flag] : o.verts)
            {
                putf(mdb, rec[r].flat[0]);
                putf(mdb, rec[r].flat[1]);
                putf(mdb, rec[r].flat[2]);
                put32(mdb, static_cast<uint32_t>(flag));
            }
            pad16(mdb);
            const uint32_t no = static_cast<uint32_t>(mdb.size() - at);
            for (const auto &[r, flag] : o.verts)
            {
                (void)flag;
                mdb.insert(mdb.end(), reinterpret_cast<const uint8_t *>(rec[r].flatN), reinterpret_cast<const uint8_t *>(rec[r].flatN) + 3);
                mdb.push_back(0u);
            }
            pad16(mdb);
            const uint32_t uo = static_cast<uint32_t>(mdb.size() - at);
            for (const auto &[r, flag] : o.verts)
            {
                (void)flag;
                const uint8_t *p = reinterpret_cast<const uint8_t *>(rec[r].uv);
                mdb.insert(mdb.end(), p, p + 4);
            }
            pad16(mdb);
            const uint32_t co = static_cast<uint32_t>(mdb.size() - at);
            for (const auto &[r, flag] : o.verts)
            {
                (void)flag;
                mdb.insert(mdb.end(), rec[r].col, rec[r].col + 4);
            }
            pad16(mdb);
            const uint32_t wo = static_cast<uint32_t>(mdb.size() - at);
            for (const auto &[r, flag] : o.verts)
            {
                (void)flag;
                mdb.insert(mdb.end(), rec[r].wt, rec[r].wt + 8);
            }
            pad16(mdb);
            w32(mdb, at, po);
            w32(mdb, at + 4, no);
            w32(mdb, at + 8, uo);
            w32(mdb, at + 12, co);
            w32(mdb, at + 16, wo);
            w16(mdb, at + 20, static_cast<uint16_t>(n));
            w16(mdb, at + 22, src.mat);
            std::memcpy(mdb.data() + at + 24, src.extra, 8);
            for (uint32_t k = 0; k < n; ++k)
            {
                const Rec &r = rec[o.verts[k].first];
                if (r.region == 0)
                    continue;
                Change c;
                c.pos = static_cast<uint32_t>(at + po + 16 * k);
                c.nrm = static_cast<uint32_t>(at + no + 4 * k);
                std::memcpy(c.flat, r.flat, 12);
                std::memcpy(c.detail, r.detail, 12);
                std::memcpy(c.flatN, r.flatN, 3);
                std::memcpy(c.detailN, r.detailN, 3);
                std::memcpy(c.grow, r.grow, 12);
                std::memcpy(c.growN, r.growN, 3);
                c.region = r.region;
                changes.push_back(c);
            }
        }

        // The model laid out again: the same header and node order, the mdbs in their original order (the body's new,
        // the distance meshes' empty, every other copied as it is), then the node records.
        std::vector<uint32_t> mdbs;
        for (const NodeRef &r : nodes)
            mdbs.push_back(r.mdb);
        std::sort(mdbs.begin(), mdbs.end());
        mdbs.erase(std::unique(mdbs.begin(), mdbs.end()), mdbs.end());
        uint32_t firstRecord = static_cast<uint32_t>(size);
        for (const NodeRef &r : nodes)
            firstRecord = std::min(firstRecord, r.record);
        if (mdbs.back() >= firstRecord)
        {
            out.error = "unexpected layout";
            return false;
        }
        const uint32_t anchor = nodes[0].record + r32(scr + nodes[0].record + 4);
        std::vector<uint8_t> o(scr, scr + 16);
        o.resize(16 + 4 * nodes.size(), 0u);
        pad16(o);
        std::map<uint32_t, uint32_t> moved_to;
        for (size_t k = 0; k < mdbs.size(); ++k)
        {
            const uint32_t from = mdbs[k], to = k + 1 < mdbs.size() ? mdbs[k + 1] : firstRecord;
            const NodeRef *owner = nullptr;
            bool far = true;
            for (const NodeRef &r : nodes)
                if (r.mdb == from)
                {
                    owner = &r;
                    far = far && (r.flags & 0x01000000u) != 0u;
                }
            pad16(o);
            moved_to[from] = static_cast<uint32_t>(o.size());
            if (from == bm)
            {
                out.bodyMdb = static_cast<uint32_t>(o.size());
                out.bodyMdbSize = static_cast<uint32_t>(mdb.size());
                o.insert(o.end(), mdb.begin(), mdb.end());
            }
            else if (owner && far)
            {
                std::vector<VbRef> fv;
                const uint16_t mat = readVbs(scr, size, from, fv) && !fv.empty() ? fv[0].mat : 0;
                const std::vector<uint8_t> e = emptyMdb(scr, from, mat);
                o.insert(o.end(), e.begin(), e.end());
            }
            else
                o.insert(o.end(), scr + from, scr + to);
        }
        pad16(o);
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            const uint32_t at = static_cast<uint32_t>(o.size());
            o.insert(o.end(), scr + nodes[i].record, scr + nodes[i].record + 112);
            const int32_t rel = static_cast<int32_t>(moved_to[nodes[i].mdb]) - static_cast<int32_t>(at);
            std::memcpy(o.data() + at, &rel, 4);
            w32(o, at + 4, anchor - at);
            w32(o, 16 + 4 * i, at);
        }
        out.used = o.size();
        if (o.size() > size && !grow)
        {
            out.error = "does not fit (" + std::to_string(o.size()) + " > " + std::to_string(size) + " bytes)";
            return false;
        }
        o.resize(std::max(o.size(), size), 0u);
        out.scr = std::move(o);
        out.changes = std::move(changes);
        // Per region a change whose position tells the states apart: the largest growth (any Cash value shows), else
        // the first whose flat and detailed positions differ.
        float most[3] = {0.0f, 0.0f, 0.0f};
        for (size_t i = 0; i < out.changes.size(); ++i)
        {
            const Change &c = out.changes[i];
            const float g = std::fabs(c.grow[0]) + std::fabs(c.grow[1]) + std::fabs(c.grow[2]);
            if (g > 1e-3f && g > most[c.region])
                most[c.region] = g, out.sentinel[c.region] = static_cast<int>(i);
        }
        for (size_t i = 0; i < out.changes.size(); ++i)
        {
            const Change &c = out.changes[i];
            if (out.sentinel[c.region] >= 0)
                continue;
            for (int k = 0; k < 3; ++k)
                if (std::fabs(c.flat[k] - c.detail[k]) > 1e-4f)
                {
                    out.sentinel[c.region] = static_cast<int>(i);
                    break;
                }
        }
        return true;
    }

    namespace
    {
        constexpr double kApex = 0.965, kMargin = 0.002;

        D3 axisPoint(D3 p) { return {std::clamp(p.x, -kAxisX, kAxisX), kAxisY, kAxisZ}; }

        D3 skirtField(D3 p) // cash.skirt_field
        {
            if (p.y >= kApex)
                return cashField(p);
            D3 d = cashField({p.x, kApex, p.z});
            d.y = 0.0;
            return d;
        }

        // cash.ray_hits: the distance along a unit ray to the nearest triangle in front of it (inf: none).
        double rayHit(D3 o, D3 d, const std::vector<D3> &V, const std::vector<int> &T, size_t triCount)
        {
            double best = std::numeric_limits<double>::infinity();
            for (size_t t = 0; t < triCount; ++t)
            {
                const D3 a = V[T[3 * t]], e1 = V[T[3 * t + 1]] - a, e2 = V[T[3 * t + 2]] - a;
                const D3 p = cross(d, e2);
                const double det = dot(e1, p);
                if (std::fabs(det) <= 1e-12)
                    continue;
                const double inv = 1.0 / det;
                const D3 s = o - a;
                const double u = dot(s, p) * inv;
                const D3 q = cross(s, e1);
                const double v = dot(d, q) * inv, h = dot(e2, q) * inv;
                if (u >= 0.0 && v >= 0.0 && u + v <= 1.0 && h > 1e-6)
                    best = std::min(best, h);
            }
            return best;
        }

        struct Ray
        {
            D3 c, u;
            double d;
        };
        Ray rayOf(D3 p)
        {
            Ray r;
            r.c = axisPoint(p);
            const D3 v = p - r.c;
            r.d = len(v);
            r.u = v * (1.0 / std::max(r.d, 1e-12));
            return r;
        }
    }

    bool growOnly(const uint8_t *scr, size_t size, const GrowRules &rules, std::vector<GrownNode> &out)
    {
        out.clear();
        std::vector<NodeRef> nodes;
        if (!readNodes(scr, size, nodes))
            return false;
        struct Loaded
        {
            NodeRef ref;
            Mesh m;
            std::vector<VbRef> vbs;
            std::vector<D3> D;
        };
        std::vector<Loaded> all;
        for (const NodeRef &r : nodes)
        {
            if (r.flags & 0x01000000u) // a distance mesh: never drawn (model_detail)
                continue;
            Loaded l;
            l.ref = r;
            if (!loadMesh(scr, size, r.mdb, l.m, &l.vbs))
                continue;
            const bool skirt = rules.skirt && std::strncmp(r.name, rules.skirt, 8) == 0;
            l.D.resize(l.m.records());
            for (size_t i = 0; i < l.D.size(); ++i)
                l.D[i] = skirt ? skirtField(l.m.P[i]) : cashField(l.m.P[i]);
            all.push_back(std::move(l));
        }
        const auto find = [&](const char *name) -> Loaded * {
            if (!name)
                return nullptr;
            for (Loaded &l : all)
                if (std::strncmp(l.ref.name, name, 8) == 0)
                    return &l;
            return nullptr;
        };
        const auto grown = [](const Loaded &l) {
            std::vector<D3> g(l.m.P.size());
            for (size_t i = 0; i < g.size(); ++i)
                g[i] = l.m.P[i] + l.D[i];
            return g;
        };
        Loaded *skirt = find(rules.skirt), *under = find(rules.under), *plate = find(rules.plate);
        if (skirt && under) // the body under the skirt stays inside it
        {
            const std::vector<D3> sg = grown(*skirt);
            for (size_t i = 0; i < under->D.size(); ++i)
            {
                if (under->D[i].z >= -1e-5)
                    continue;
                const Ray r0 = rayOf(under->m.P[i]), r1 = rayOf(under->m.P[i] + under->D[i]);
                const double h0 = rayHit(r0.c, r0.u, skirt->m.P, skirt->m.T, skirt->m.triangles());
                const double h1 = rayHit(r1.c, r1.u, sg, skirt->m.T, skirt->m.triangles());
                if (!std::isfinite(h0) || !std::isfinite(h1))
                    continue;
                const double allowed = h1 - std::min(kMargin, h0 - r0.d);
                if (r1.d > allowed)
                    under->D[i] = r1.c + r1.u * allowed - under->m.P[i];
            }
        }
        // The skirt's waistband kept off the body under it (K, Nov 20: Olivia's belt z-fought above 120% Cash). At the
        // back of her waist (y ~1.09) the band's edge sits 3-4.5 mm off the dress, and the dress, whose triangles there
        // span heights the field grows differently, closed the gap to 0 at 130%. Each skirt record within 2 x kBand of
        // the body is pushed away from it (the direction to it at rest) at full growth by what keeps its gap, up to kBand,
        // at every size (growth is linear in Cash: checked at quarters). Records touching the body at rest stay so.
        if (skirt && under)
        {
            constexpr double kBand = 0.004;
            const size_t ut = under->m.triangles();
            const auto nearest = [&](D3 p, double f, D3 *at) {
                double best = 1e9;
                for (size_t t = 0; t < ut; ++t)
                {
                    const int a = under->m.T[3 * t], b = under->m.T[3 * t + 1], c = under->m.T[3 * t + 2];
                    const D3 A = under->m.P[a] + under->D[a] * f, B = under->m.P[b] + under->D[b] * f,
                             C = under->m.P[c] + under->D[c] * f;
                    const D3 lo{std::min({A.x, B.x, C.x}), std::min({A.y, B.y, C.y}), std::min({A.z, B.z, C.z})};
                    const D3 hi{std::max({A.x, B.x, C.x}), std::max({A.y, B.y, C.y}), std::max({A.z, B.z, C.z})};
                    if (p.x < lo.x - best || p.x > hi.x + best || p.y < lo.y - best || p.y > hi.y + best ||
                        p.z < lo.z - best || p.z > hi.z + best)
                        continue;
                    const D3 q = triClosest(p, A, B, C);
                    if (const double d = len(p - q); d < best)
                    {
                        best = d;
                        if (at)
                            *at = q;
                    }
                }
                return best;
            };
            for (size_t i = 0; i < skirt->D.size(); ++i)
            {
                const D3 p = skirt->m.P[i];
                D3 q;
                const double g0 = nearest(p, 0.0, &q);
                if (g0 > 2.0 * kBand || g0 < 1e-5)
                    continue;
                const double target = std::min(g0, kBand);
                double need = 0.0;
                for (double f : {0.25, 0.5, 0.75, 1.0})
                    need = std::max(need, (target - nearest(p + skirt->D[i] * f, f, nullptr)) / f);
                if (need > 0.0)
                    skirt->D[i] = skirt->D[i] + (p - q) * (need / g0);
            }
        }
        if (plate) // a belt plate over the skirt: pushed out so what is under it stays under it
        {
            D3 lo{1e9, 1e9, 1e9}, hi{-1e9, -1e9, -1e9};
            for (const D3 &p : plate->m.P)
                lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)},
                hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
            const std::vector<D3> pg = grown(*plate);
            double need = 0.0;
            for (Loaded *l : {skirt, under})
            {
                if (!l)
                    continue;
                for (size_t i = 0; i < l->D.size(); ++i)
                {
                    const D3 p = l->m.P[i];
                    if (p.x < lo.x - 0.012 || p.x > hi.x + 0.012 || p.y < lo.y - 0.012 || p.y > hi.y + 0.012 || p.z >= -0.05)
                        continue;
                    const Ray r0 = rayOf(p), r1 = rayOf(p + l->D[i]);
                    const double h0 = rayHit(r0.c, r0.u, plate->m.P, plate->m.T, plate->m.triangles());
                    const double h1 = rayHit(r1.c, r1.u, pg, plate->m.T, plate->m.triangles());
                    if (std::isfinite(h0) && std::isfinite(h1))
                        need = std::max(need, std::min(h0 - r0.d, kMargin) - (h1 - r1.d));
                }
            }
            if (need > 0.0)
                for (size_t i = 0; i < plate->D.size(); ++i)
                    plate->D[i] = plate->D[i] + rayOf(pg[i]).u * need;
        }
        for (Loaded &l : all)
        {
            const Mesh &m = l.m;
            double big = 0.0;
            for (const D3 &d : l.D)
                big = std::max({big, std::fabs(d.x), std::fabs(d.y), std::fabs(d.z)});
            if (big < 1e-3) // (boots near the thighs move under 1 mm: left alone)
                continue;
            const std::vector<D3> moved = grown(l);
            const std::vector<D3> sn = smoothNormals(m.P, m.T, m.triangles()), sn2 = smoothNormals(moved, m.T, m.triangles());
            GrownNode g;
            std::memcpy(g.name, l.ref.name, 8);
            g.mdb = l.ref.mdb;
            uint32_t end = l.ref.mdb + 32u + 4u * static_cast<uint32_t>(l.vbs.size());
            float most = 0.0f;
            size_t i = 0;
            for (const VbRef &b : l.vbs)
            {
                const uint32_t n = b.count;
                end = std::max({end, b.at + 32u, b.at + b.pos + 16u * n, b.at + b.uv + 4u * n, b.nrm ? b.at + b.nrm + 4u * n : 0u,
                                b.col ? b.at + b.col + 4u * n : 0u, b.wt ? b.at + b.wt + 8u * n : 0u});
                for (uint32_t k = 0; k < n; ++k, ++i)
                {
                    if (!b.nrm || len(l.D[i]) < 1e-5)
                        continue;
                    Change c;
                    c.pos = b.at + b.pos + 16u * k - l.ref.mdb;
                    c.nrm = b.at + b.nrm + 4u * k - l.ref.mdb;
                    std::memcpy(c.flat, scr + l.ref.mdb + c.pos, 12);
                    std::memcpy(c.detail, c.flat, 12);
                    std::memcpy(c.flatN, scr + l.ref.mdb + c.nrm, 3);
                    std::memcpy(c.detailN, c.flatN, 3);
                    c.grow[0] = static_cast<float>(l.D[i].x);
                    c.grow[1] = static_cast<float>(l.D[i].y);
                    c.grow[2] = static_cast<float>(l.D[i].z);
                    quantN(m.N[i] + (sn2[i] - sn[i]), true, c.growN);
                    c.region = 2;
                    const float gl = std::fabs(c.grow[0]) + std::fabs(c.grow[1]) + std::fabs(c.grow[2]);
                    if (gl > most)
                        most = gl, g.sentinel = static_cast<int>(g.changes.size());
                    g.changes.push_back(c);
                }
            }
            g.mdbSize = end - l.ref.mdb;
            if (g.sentinel >= 0 && end <= size)
                out.push_back(std::move(g));
        }
        return true;
    }

    bool seatTable(const uint8_t *scr, size_t size, const char *const *nodeNames, int nodeCount, float pelvisY, SeatTable &out)
    {
        out.ok = false;
        std::vector<NodeRef> nodes;
        if (!readNodes(scr, size, nodes))
            return false;
        std::vector<D3> V;
        std::vector<int> T;
        for (const NodeRef &r : nodes)
            for (int k = 0; k < nodeCount; ++k)
            {
                Mesh m;
                if (std::strncmp(r.name, nodeNames[k], 8) != 0 || (r.flags & 0x01000000u) || !loadMesh(scr, size, r.mdb, m))
                    continue;
                const int base = static_cast<int>(V.size());
                V.insert(V.end(), m.P.begin(), m.P.end());
                for (int i : m.T)
                    T.push_back(base + i);
            }
        if (T.empty())
            return false;
        int hits = 0;
        for (int iy = 0; iy < SeatTable::kNy; ++iy)
            for (int ix = 0; ix < SeatTable::kNx; ++ix)
            {
                const double x = SeatTable::kX0 + SeatTable::kStep * ix, y = pelvisY + SeatTable::kY0 + SeatTable::kStep * iy;
                const double h = rayHit({x, y, -1.0}, {0.0, 0.0, 1.0}, V, T, T.size() / 3); // the rearmost surface
                if (!std::isfinite(h) || h > 1.2)
                {
                    out.z0[iy][ix] = SeatTable::kNone, out.gz[iy][ix] = 0.0f;
                    continue;
                }
                const D3 p{x, y, -1.0 + h};
                out.z0[iy][ix] = static_cast<float>(p.z);
                out.gz[iy][ix] = static_cast<float>(cashField(p).z);
                ++hits;
            }
        out.ok = hits > 0;
        return out.ok;
    }

    float cashGrowthScale(float cash) { return std::clamp((cash - 1.0f) / 0.3f, 0.0f, 1.0f); }

    void wantedPosition(const Change &c, bool detailed, float grow, float out[3])
    {
        for (int k = 0; k < 3; ++k)
            out[k] = detailed ? c.detail[k] + grow * c.grow[k] : c.flat[k];
    }

    void apply(uint8_t *mdb, const std::vector<Change> &changes, int region, bool detailed, float grow)
    {
        for (const Change &c : changes)
        {
            if (c.region != region)
                continue;
            float p[3];
            wantedPosition(c, detailed, grow, p);
            std::memcpy(mdb + c.pos, p, 12);
            if (!detailed || grow <= 0.0f || (c.grow[0] == 0.0f && c.grow[1] == 0.0f && c.grow[2] == 0.0f))
            {
                std::memcpy(mdb + c.nrm, detailed ? c.detailN : c.flatN, 3);
                continue;
            }
            D3 n{c.detailN[0] * (1.0 - grow) + c.growN[0] * grow, c.detailN[1] * (1.0 - grow) + c.growN[1] * grow,
                 c.detailN[2] * (1.0 - grow) + c.growN[2] * grow};
            int8_t q[3];
            quantN(n, true, q);
            std::memcpy(mdb + c.nrm, q, 3);
        }
    }
}
