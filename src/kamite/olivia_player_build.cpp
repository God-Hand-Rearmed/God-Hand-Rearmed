// Playable Olivia's player files (see olivia_player_build.h). The formats: tools/models/gamemodel.py, mot.py.
#include "kamite/olivia_player_build.h"
#include "kamite/model_files.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>

namespace kamite::olivia_player
{
    namespace
    {
        using namespace model_files; // Bytes, r32 ..., readDat / writeDat, readTm3 / writeTm3

        // ---- scr: "scr\0", kind, node count, 0, node offsets; the mdbs; 112-byte node records (+0 mdb, +4 the anchor,
        // +8 name, +16 srt, +52 the rest). The nodes' mdbs are copied as they are (every offset in one is its own).
        struct Node
        {
            std::string name;
            uint8_t rec[112] = {};
            Bytes mdb;
        };
        struct Scr
        {
            uint32_t kind = 0, anchor = 0;
            std::vector<Node> nodes;
        };
        bool readScr(const Bytes &s, Scr &out)
        {
            if (!inside(s, 0, 16) || std::memcmp(s.data(), "scr", 3) != 0)
                return false;
            out.kind = r32(s, 4);
            const uint32_t n = r32(s, 8);
            if (n == 0 || n > 256 || !inside(s, 16, 4u * n))
                return false;
            std::vector<uint32_t> mdbs, recs;
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint32_t no = r32(s, 16 + 4 * i);
                if (!inside(s, no, 112))
                    return false;
                int32_t rel;
                std::memcpy(&rel, s.data() + no, 4);
                const int64_t m = static_cast<int64_t>(no) + rel;
                if (m < 16 || m >= static_cast<int64_t>(s.size()) || std::memcmp(s.data() + m, "mdb", 3) != 0)
                    return false;
                mdbs.push_back(static_cast<uint32_t>(m));
                recs.push_back(no);
            }
            std::vector<uint32_t> bounds = mdbs;
            bounds.insert(bounds.end(), recs.begin(), recs.end());
            bounds.push_back(static_cast<uint32_t>(s.size()));
            std::sort(bounds.begin(), bounds.end());
            int32_t anchorRel;
            std::memcpy(&anchorRel, s.data() + recs[0] + 4, 4);
            out.anchor = static_cast<uint32_t>(static_cast<int64_t>(recs[0]) + anchorRel);
            for (uint32_t i = 0; i < n; ++i)
            {
                Node node;
                std::memcpy(node.rec, s.data() + recs[i], 112);
                const char *name = reinterpret_cast<const char *>(node.rec + 8);
                node.name.assign(name, strnlen(name, 8));
                const uint32_t end = *std::upper_bound(bounds.begin(), bounds.end(), mdbs[i]);
                node.mdb.assign(s.begin() + mdbs[i], s.begin() + end);
                out.nodes.push_back(std::move(node));
            }
            return true;
        }
        Bytes writeScr(const Scr &scr)
        {
            const uint32_t n = static_cast<uint32_t>(scr.nodes.size());
            Bytes o{'s', 'c', 'r', 0};
            o.resize(16, 0);
            put32(o, 4, scr.kind);
            put32(o, 8, n);
            o.resize(16 + 4u * n, 0);
            pad(o, 16);
            std::vector<uint32_t> mdbs;
            for (const Node &node : scr.nodes)
            {
                mdbs.push_back(static_cast<uint32_t>(o.size()));
                o.insert(o.end(), node.mdb.begin(), node.mdb.end());
                pad(o, 16);
            }
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint32_t no = static_cast<uint32_t>(o.size());
                put32(o, 16 + 4 * i, no);
                o.insert(o.end(), scr.nodes[i].rec, scr.nodes[i].rec + 112);
                put32(o, no, mdbs[i] - no);
                put32(o, no + 4, scr.anchor - no);
                std::memset(o.data() + no + 8, 0, 8);
                std::memcpy(o.data() + no + 8, scr.nodes[i].name.c_str(), std::min<size_t>(8, scr.nodes[i].name.size()));
            }
            return o;
        }
        const Node *find(const Scr &s, const std::string &name)
        {
            for (const Node &n : s.nodes)
                if (n.name == name)
                    return &n;
            return nullptr;
        }

        // ---- an mdb: "mdb\0", +4 bone table (rel), +8 u16 bones, +10 u16 buffers, +32 buffer offsets (rel); a buffer:
        // +0 positions (16 bytes each: xyz, strip flag), +4 normals, +8 UVs, +12 colours, +16 weights (8 bytes: 0, bone
        // x 4 three times, percent three times, 0), +20 u16 count, +22 u16 material.
        bool mdbOk(const Bytes &m)
        {
            if (!inside(m, 0, 32) || std::memcmp(m.data(), "mdb", 3) != 0)
                return false;
            const uint16_t nv = r16(m, 10);
            if (!inside(m, 32, 4u * nv))
                return false;
            for (uint16_t i = 0; i < nv; ++i)
            {
                const uint32_t v = r32(m, 32 + 4 * i);
                if (!inside(m, v, 32))
                    return false;
                const uint32_t n = r16(m, v + 20), po = r32(m, v), wo = r32(m, v + 16);
                if (!inside(m, v + po, 16u * n) || (wo && !inside(m, v + wo, 8u * n)))
                    return false;
            }
            return inside(m, r32(m, 4), 16u * r16(m, 8));
        }
        // f(the buffer's offset in b) for each buffer of the mdb at b[base].
        void forEachBuffer(Bytes &b, uint32_t base, const std::function<void(uint32_t)> &f)
        {
            const uint16_t nv = r16(b, base + 10);
            for (uint16_t i = 0; i < nv; ++i)
                f(base + r32(b, base + 32 + 4 * i));
        }
        void forEachBuffer(Bytes &m, const std::function<void(uint32_t)> &f) { forEachBuffer(m, 0, f); }
        void shiftMaterials(Bytes &m, int by)
        {
            forEachBuffer(m, [&](uint32_t v) { put16(m, v + 22, static_cast<uint16_t>(r16(m, v + 22) + by)); });
        }
        // Every vertex at the model's origin on bone 0 alone: whatever the bones do, each triangle is a point.
        void collapse(Bytes &m, uint32_t base = 0)
        {
            forEachBuffer(m, base, [&](uint32_t v) {
                const uint32_t n = r16(m, v + 20), po = r32(m, v), wo = r32(m, v + 16);
                for (uint32_t k = 0; k < n; ++k)
                {
                    std::memset(m.data() + v + po + 16 * k, 0, 12);
                    if (wo)
                    {
                        uint8_t *w = m.data() + v + wo + 8 * k;
                        std::memset(w, 0, 8);
                        w[4] = 100;
                    }
                }
            });
        }
        float maxY(const Bytes &m)
        {
            float top = -1e9f;
            Bytes copy = m;
            forEachBuffer(copy, [&](uint32_t v) {
                const uint32_t n = r16(copy, v + 20), po = r32(copy, v);
                for (uint32_t k = 0; k < n; ++k)
                    top = std::max(top, rf(copy, v + po + 16 * k + 4));
            });
            return top;
        }
        // The bind position of a bone (its offsets summed up the chain), from an mdb's bone table.
        bool bindOf(const Bytes &m, int bone, float out[3])
        {
            const uint32_t table = r32(m, 4), nb = r16(m, 8);
            out[0] = out[1] = out[2] = 0.0f;
            for (int i = bone, guard = 0; i >= 0 && guard < 64; ++guard)
            {
                if (static_cast<uint32_t>(i) >= nb)
                    return false;
                for (int a = 0; a < 3; ++a)
                    out[a] += rf(m, table + 16u * static_cast<uint32_t>(i) + 4u * static_cast<uint32_t>(a));
                int16_t parent;
                std::memcpy(&parent, m.data() + table + 16u * static_cast<uint32_t>(i) + 14, 2);
                i = parent;
            }
            return true;
        }

        // ---- Gene's spread palm (02) on her open hand (00) -------------------------------------------------------------
        // Gene's 00 and 02 are one mesh in two poses (the same strips), so each of his vertices says where it goes.
        // Her hand (scaled to his by the hands' lengths, both about their hand bone) takes, per vertex, the rigid move of
        // his 12 nearest vertices (inverse-square weighted), which sends her fingers where his go; then an as-rigid-as-
        // possible fit (her mesh's own edges, pulled toward those targets with weight 0.1, 30 rounds) keeps every finger
        // her own shape and thickness (the per-vertex moves alone stretched them into sausages, K Nov 19).
        using V3 = std::array<double, 3>;
        using M3 = std::array<double, 9>; // row-major
        V3 sub3(const V3 &a, const V3 &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
        V3 apply3(const M3 &m, const V3 &v)
        {
            return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2], m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
        }
        void addOuter(M3 &m, const V3 &a, const V3 &b, double w)
        {
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    m[3 * i + j] += w * a[static_cast<size_t>(i)] * b[static_cast<size_t>(j)];
        }
        // The rotation nearest to m (R = U V^T of m = U S V^T, a reflection turned into a rotation): Kabsch.
        M3 nearestRotation(const M3 &m)
        {
            M3 a{}; // m^T m
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    for (int k = 0; k < 3; ++k)
                        a[3 * i + j] += m[3 * k + i] * m[3 * k + j];
            M3 v{1, 0, 0, 0, 1, 0, 0, 0, 1};
            for (int sweep = 0; sweep < 50; ++sweep) // Jacobi
            {
                const double off = a[1] * a[1] + a[2] * a[2] + a[5] * a[5];
                if (off < 1e-24)
                    break;
                for (const auto &[p, q] : {std::pair{0, 1}, std::pair{0, 2}, std::pair{1, 2}})
                {
                    const double apq = a[3 * p + q];
                    if (std::fabs(apq) < 1e-30)
                        continue;
                    const double theta = (a[3 * q + q] - a[3 * p + p]) / (2.0 * apq);
                    const double t = (theta >= 0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
                    const double c = 1.0 / std::sqrt(t * t + 1.0), sn = t * c;
                    for (int k = 0; k < 3; ++k) // a = J^T a J
                    {
                        const double akp = a[3 * k + p], akq = a[3 * k + q];
                        a[3 * k + p] = c * akp - sn * akq;
                        a[3 * k + q] = sn * akp + c * akq;
                    }
                    for (int k = 0; k < 3; ++k)
                    {
                        const double apk = a[3 * p + k], aqk = a[3 * q + k];
                        a[3 * p + k] = c * apk - sn * aqk;
                        a[3 * q + k] = sn * apk + c * aqk;
                    }
                    for (int k = 0; k < 3; ++k)
                    {
                        const double vkp = v[3 * k + p], vkq = v[3 * k + q];
                        v[3 * k + p] = c * vkp - sn * vkq;
                        v[3 * k + q] = sn * vkp + c * vkq;
                    }
                }
            }
            // columns of v = right singular vectors, sorted by eigenvalue (largest first)
            int order[3] = {0, 1, 2};
            std::sort(order, order + 3, [&](int x, int y) { return a[3 * x + x] > a[3 * y + y]; });
            V3 vc[3], uc[3];
            for (int i = 0; i < 3; ++i)
            {
                vc[i] = {v[order[i]], v[3 + order[i]], v[6 + order[i]]};
                const V3 mv = apply3(m, vc[i]);
                const double len = std::sqrt(mv[0] * mv[0] + mv[1] * mv[1] + mv[2] * mv[2]);
                uc[i] = len > 1e-12 ? V3{mv[0] / len, mv[1] / len, mv[2] / len} : V3{0, 0, 0};
            }
            const auto cross = [](const V3 &x, const V3 &y) { return V3{x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]}; };
            if (uc[1][0] == 0 && uc[1][1] == 0 && uc[1][2] == 0)
                return {1, 0, 0, 0, 1, 0, 0, 0, 1};
            uc[2] = cross(uc[0], uc[1]); // a right-handed U: R is a rotation (Kabsch's sign fix)
            const V3 vx = cross(vc[0], vc[1]);
            if (vx[0] * vc[2][0] + vx[1] * vc[2][1] + vx[2] * vc[2][2] < 0)
                vc[2] = {-vc[2][0], -vc[2][1], -vc[2][2]};
            M3 r{};
            for (int i = 0; i < 3; ++i)
                addOuter(r, uc[i], vc[i], 1.0);
            return r;
        }

        struct HandMesh
        {
            std::vector<V3> pos;
            std::vector<std::pair<uint32_t, uint32_t>> at; // (buffer offset, vertex) per vertex
            std::vector<std::array<uint32_t, 3>> tris;
        };
        HandMesh handMesh(Bytes &m)
        {
            HandMesh h;
            forEachBuffer(m, [&](uint32_t v) {
                const uint32_t n = r16(m, v + 20), po = r32(m, v), base = static_cast<uint32_t>(h.pos.size());
                for (uint32_t k = 0; k < n; ++k)
                {
                    h.pos.push_back({rf(m, v + po + 16 * k), rf(m, v + po + 16 * k + 4), rf(m, v + po + 16 * k + 8)});
                    h.at.emplace_back(v, k);
                    if (k >= 2 && r32(m, v + po + 16 * k + 12) != 0x8000u)
                        h.tris.push_back({base + k - 2, base + k - 1, base + k});
                }
            });
            return h;
        }

        bool spreadHand(Bytes &hers, const Bytes &herBones, const Bytes &geneOpenIn, const Bytes &geneSpreadIn, int handBone)
        {
            Bytes geneOpen = geneOpenIn, geneSpread = geneSpreadIn;
            HandMesh x = handMesh(hers), a = handMesh(geneOpen), b = handMesh(geneSpread);
            float hoF[3], goF[3];
            if (a.pos.size() != b.pos.size() || a.pos.size() < 12 || x.pos.size() < 3 || !bindOf(herBones, handBone, hoF) ||
                !bindOf(geneOpen, handBone, goF))
                return false;
            const V3 ho{hoF[0], hoF[1], hoF[2]}, go{goF[0], goF[1], goF[2]};
            const auto spanX = [](const std::vector<V3> &p) {
                double lo = 1e9, hi = -1e9;
                for (const V3 &q : p)
                    lo = std::min(lo, q[0]), hi = std::max(hi, q[0]);
                return hi - lo;
            };
            const double s = spanX(x.pos) / std::max(spanX(a.pos), 1e-6);
            for (size_t i = 0; i < a.pos.size(); ++i)
                for (int c = 0; c < 3; ++c)
                {
                    a.pos[i][static_cast<size_t>(c)] = (a.pos[i][static_cast<size_t>(c)] - go[static_cast<size_t>(c)]) * s;
                    b.pos[i][static_cast<size_t>(c)] = (b.pos[i][static_cast<size_t>(c)] - go[static_cast<size_t>(c)]) * s;
                }
            // Her vertices merged where they share a position (the strips repeat them), with the mesh's edges.
            std::vector<V3> u;
            std::vector<size_t> idx(x.pos.size());
            for (size_t i = 0; i < x.pos.size(); ++i)
            {
                const V3 p = sub3(x.pos[i], ho);
                size_t k = 0;
                while (k < u.size() && std::fabs(u[k][0] - p[0]) + std::fabs(u[k][1] - p[1]) + std::fabs(u[k][2] - p[2]) > 1e-6)
                    ++k;
                if (k == u.size())
                    u.push_back(p);
                idx[i] = k;
            }
            const size_t n = u.size();
            std::vector<std::vector<size_t>> nb(n);
            for (const auto &t : x.tris)
                for (int e = 0; e < 3; ++e)
                {
                    const size_t i = idx[t[static_cast<size_t>(e)]], j = idx[t[static_cast<size_t>((e + 1) % 3)]];
                    if (i != j && std::find(nb[i].begin(), nb[i].end(), j) == nb[i].end())
                        nb[i].push_back(j), nb[j].push_back(i);
                }
            // The targets: his 12 nearest vertices' rigid move.
            std::vector<V3> target(n);
            for (size_t i = 0; i < n; ++i)
            {
                std::vector<std::pair<double, size_t>> d(a.pos.size());
                for (size_t k = 0; k < a.pos.size(); ++k)
                {
                    const V3 e = sub3(a.pos[k], u[i]);
                    d[k] = {std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]), k};
                }
                std::partial_sort(d.begin(), d.begin() + 12, d.end());
                double wsum = 0.0;
                V3 ca{}, cb{};
                double w[12];
                for (int k = 0; k < 12; ++k)
                {
                    w[k] = 1.0 / ((d[static_cast<size_t>(k)].first + 1e-3) * (d[static_cast<size_t>(k)].first + 1e-3));
                    wsum += w[k];
                }
                for (int k = 0; k < 12; ++k)
                    for (int c = 0; c < 3; ++c)
                    {
                        ca[static_cast<size_t>(c)] += w[k] / wsum * a.pos[d[static_cast<size_t>(k)].second][static_cast<size_t>(c)];
                        cb[static_cast<size_t>(c)] += w[k] / wsum * b.pos[d[static_cast<size_t>(k)].second][static_cast<size_t>(c)];
                    }
                M3 h{};
                for (int k = 0; k < 12; ++k)
                    addOuter(h, sub3(b.pos[d[static_cast<size_t>(k)].second], cb), sub3(a.pos[d[static_cast<size_t>(k)].second], ca), w[k] / wsum);
                const V3 r = apply3(nearestRotation(h), sub3(u[i], ca));
                target[i] = {r[0] + cb[0], r[1] + cb[1], r[2] + cb[2]};
            }
            // As rigid as possible: (L + lambda I) X = lambda T + sum over edges 0.5 (R_i + R_j)(u_i - u_j).
            constexpr double kLambda = 0.1;
            std::vector<double> chol(n * n, 0.0);
            for (size_t i = 0; i < n; ++i)
            {
                chol[i * n + i] = static_cast<double>(nb[i].size()) + kLambda;
                for (const size_t j : nb[i])
                    chol[i * n + j] -= 1.0;
            }
            for (size_t j = 0; j < n; ++j) // Cholesky, lower triangle in place
            {
                double dsum = chol[j * n + j];
                for (size_t k = 0; k < j; ++k)
                    dsum -= chol[j * n + k] * chol[j * n + k];
                if (dsum <= 0.0)
                    return false;
                chol[j * n + j] = std::sqrt(dsum);
                for (size_t i = j + 1; i < n; ++i)
                {
                    double v = chol[i * n + j];
                    for (size_t k = 0; k < j; ++k)
                        v -= chol[i * n + k] * chol[j * n + k];
                    chol[i * n + j] = v / chol[j * n + j];
                }
            }
            const auto solve = [&](std::vector<double> &y) {
                for (size_t i = 0; i < n; ++i)
                {
                    for (size_t k = 0; k < i; ++k)
                        y[i] -= chol[i * n + k] * y[k];
                    y[i] /= chol[i * n + i];
                }
                for (size_t i = n; i-- > 0;)
                {
                    for (size_t k = i + 1; k < n; ++k)
                        y[i] -= chol[k * n + i] * y[k];
                    y[i] /= chol[i * n + i];
                }
            };
            std::vector<V3> xs = target;
            std::vector<M3> rot(n);
            for (int round = 0; round < 30; ++round)
            {
                for (size_t i = 0; i < n; ++i)
                {
                    M3 m{};
                    for (const size_t j : nb[i])
                        addOuter(m, sub3(xs[i], xs[j]), sub3(u[i], u[j]), 1.0);
                    rot[i] = nb[i].empty() ? M3{1, 0, 0, 0, 1, 0, 0, 0, 1} : nearestRotation(m);
                }
                for (int c = 0; c < 3; ++c)
                {
                    std::vector<double> y(n);
                    for (size_t i = 0; i < n; ++i)
                    {
                        y[i] = kLambda * target[i][static_cast<size_t>(c)];
                        for (const size_t j : nb[i])
                        {
                            const V3 e = sub3(u[i], u[j]);
                            const V3 ri = apply3(rot[i], e), rj = apply3(rot[j], e);
                            y[i] += 0.5 * (ri[static_cast<size_t>(c)] + rj[static_cast<size_t>(c)]);
                        }
                    }
                    solve(y);
                    for (size_t i = 0; i < n; ++i)
                        xs[i][static_cast<size_t>(c)] = y[i];
                }
            }
            // Back into her mesh: positions, and the normals turned with their vertex.
            for (size_t i = 0; i < x.pos.size(); ++i)
            {
                const auto [v, k] = x.at[i];
                const V3 &p = xs[idx[i]];
                for (int c = 0; c < 3; ++c)
                    putf(hers, v + r32(hers, v) + 16 * k + 4u * static_cast<uint32_t>(c), static_cast<float>(p[static_cast<size_t>(c)] + ho[static_cast<size_t>(c)]));
                if (const uint32_t no = r32(hers, v + 4); no)
                {
                    int8_t raw[3];
                    std::memcpy(raw, hers.data() + v + no + 4 * k, 3);
                    const V3 nr = apply3(rot[idx[i]], V3{static_cast<double>(raw[0]), static_cast<double>(raw[1]), static_cast<double>(raw[2])});
                    const double len = std::max(std::sqrt(nr[0] * nr[0] + nr[1] * nr[1] + nr[2] * nr[2]), 1e-9);
                    for (int c = 0; c < 3; ++c)
                        hers[v + no + 4 * k + static_cast<uint32_t>(c)] =
                            static_cast<uint8_t>(static_cast<int8_t>(std::lround(std::clamp(nr[static_cast<size_t>(c)] / len * 127.0, -127.0, 127.0))));
                }
            }
            return true;
        }

        // ---- skin: a TIM3's palette (+16: picture header; total, CLUT bytes, image bytes, header size; the CLUT after
        // the image, RGBA, alpha 0x80 = opaque). Skin-range colours (hue 8..50 deg or 345+, saturation 0.07..0.45,
        // value 0.28+: the range the previews used, olivia_skin_variants.png) turned by a hue / saturation / value shift;
        // warmth(t): t of the way to cutscene Olivia's (measured Nov 19, median HSV of the arms: hue -9.6 deg,
        // saturation x1.36, value x1.16).
        struct SkinShift
        {
            float hue = 0.0f, sat = 1.0f, val = 1.0f; // hue in turns, saturation and value as factors
        };
        // t of the way from her gameplay skin to cutscene Olivia's (t < 0 or > 1: past either end).
        SkinShift warmth(float t) { return {-0.0267f * t, 1.0f + 0.36f * t, 1.0f + 0.16f * t}; }
        int shiftSkin(Bytes &tim, const SkinShift &k)
        {
            constexpr size_t p = 16;
            if (!inside(tim, p, 32))
                return 0;
            const uint32_t clut = r32(tim, p + 4), img = r32(tim, p + 8);
            const uint16_t hs = r16(tim, p + 12);
            const size_t at = p + hs + img;
            if (clut == 0 || !inside(tim, at, clut))
                return 0;
            int changed = 0;
            for (size_t e = at; e + 4 <= at + clut; e += 4)
            {
                if (tim[e + 3] == 0)
                    continue;
                const float r = tim[e] / 255.0f, g = tim[e + 1] / 255.0f, b = tim[e + 2] / 255.0f;
                const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
                if (mx <= 0.0f || d <= 0.0f)
                    continue;
                float h = mx == r ? (g - b) / d : mx == g ? 2.0f + (b - r) / d : 4.0f + (r - g) / d;
                h = std::fmod(h / 6.0f + 1.0f, 1.0f);
                float s = d / mx, v = mx;
                const float deg = h * 360.0f;
                if (!((deg >= 8.0f && deg <= 50.0f) || deg >= 345.0f) || s < 0.07f || s > 0.45f || v < 0.28f)
                    continue;
                h = std::fmod(h + k.hue + 1.0f, 1.0f);
                s = std::min(1.0f, s * k.sat);
                v = std::min(1.0f, v * k.val);
                const float hh = h * 6.0f, f = hh - std::floor(hh), pp = v * (1 - s), q = v * (1 - s * f), u = v * (1 - s * (1 - f));
                float o[3];
                switch (static_cast<int>(hh) % 6)
                {
                case 0: o[0] = v, o[1] = u, o[2] = pp; break;
                case 1: o[0] = q, o[1] = v, o[2] = pp; break;
                case 2: o[0] = pp, o[1] = v, o[2] = u; break;
                case 3: o[0] = pp, o[1] = q, o[2] = v; break;
                case 4: o[0] = u, o[1] = pp, o[2] = v; break;
                default: o[0] = v, o[1] = pp, o[2] = q; break;
                }
                for (int c = 0; c < 3; ++c)
                    tim[e + static_cast<size_t>(c)] = static_cast<uint8_t>(std::lround(std::clamp(o[c], 0.0f, 1.0f) * 255.0f));
                ++changed;
            }
            return changed;
        }

        // ---- motions: "mtb3", u16 frames, u8 records, u8 loop; 12-byte records (u8 bone, u8 channel, u16 keys, u32
        // flags, u32 payload); channel: 0x00 constant (payload an f32), 0x10 Hermite (payload: the curve block's offset
        // from the motion's start; six half floats: value, value step, slope in, its step, slope out, its step, then the
        // keys' u8 steps). value = a * value + c: the curve's values move with it exactly (up to the halves' rounding).
        float halfToFloat(uint16_t v)
        {
            const int expo = (v >> 9) & 0x3F, mant = v & 0x1FF;
            if (expo == 0 && mant == 0)
                return 0.0f;
            const float value = std::ldexp(1.0f + mant / 512.0f, expo - 47);
            return (v & 0x8000u) ? -value : value;
        }
        uint16_t floatToHalf(float x)
        {
            if (x == 0.0f || !std::isfinite(x))
                return 0;
            const uint16_t sign = x < 0.0f ? 0x8000u : 0u;
            const float a = std::fabs(x);
            int e = static_cast<int>(std::floor(std::log2(a)));
            int m = static_cast<int>(std::lround((a / std::ldexp(1.0f, e) - 1.0f) * 512.0f));
            if (m >= 512)
                m = 0, ++e;
            if (e + 47 < 1)
                return 0;
            const int expo = std::min(e + 47, 62);
            return static_cast<uint16_t>(sign | (expo << 9) | (m & 0x1FF));
        }
        struct Affine
        {
            float a = 1.0f, c = 0.0f;
        };
        // affine[bone 0, 20, 24, 255][component 0..2]
        int editMotion(Bytes &m, const std::function<const Affine *(int bone, int component)> &edit)
        {
            if (!inside(m, 0, 8) || std::memcmp(m.data(), "mtb3", 4) != 0)
                return 0;
            const int records = m[6];
            int changed = 0;
            for (int i = 0; i < records && inside(m, 8 + 12u * static_cast<uint32_t>(i), 12); ++i)
            {
                const size_t r = 8 + 12u * static_cast<uint32_t>(i);
                const uint8_t bone = m[r], channel = m[r + 1];
                if (channel == 0xFF)
                    break;
                const int component = channel & 0x0F;
                if (component > 2)
                    continue;
                const Affine *f = edit(bone, component);
                if (!f)
                    continue;
                const uint32_t payload = r32(m, r + 8);
                if ((channel & 0xF0) == 0x00)
                {
                    float v;
                    std::memcpy(&v, m.data() + r + 8, 4);
                    putf(m, r + 8, f->a * v + f->c);
                    ++changed;
                }
                else if ((channel & 0xF0) == 0x10 && inside(m, payload, 12))
                {
                    uint16_t h[6];
                    std::memcpy(h, m.data() + payload, 12);
                    h[0] = floatToHalf(f->a * halfToFloat(h[0]) + f->c);
                    for (int k = 1; k < 6; ++k)
                        h[k] = floatToHalf(f->a * halfToFloat(h[k]));
                    std::memcpy(m.data() + payload, h, 12);
                    ++changed;
                }
            }
            return changed;
        }

        bool fail(Built &out, const char *why)
        {
            out.error = why;
            return false;
        }
    }

    bool build(const Inputs &in, Built &out)
    {
        std::vector<Entry> pl, ema0, elf4;
        if (!readDat(in.pl00, pl) || pl.size() < 3 || !entryIs(pl[0], "TM3") || !entryIs(pl[1], "MD") || !entryIs(pl[2], "TXE"))
            return fail(out, "pl/pl00.dat is not the expected dat (TM3, MD, TXE, motions)");
        if (!readDat(in.ema0, ema0) || ema0.size() < 2 || !entryIs(ema0[0], "TM3") || !entryIs(ema0[1], "MD"))
            return fail(out, "em/ema0.dat is not the expected dat (TM3, MD)");
        const bool bunny = in.costume == Costume::Bunny;
        if (bunny && (!readDat(in.elf4, elf4) || elf4.size() < 2 || !entryIs(elf4[0], "TM3") || !entryIs(elf4[1], "MD")))
            return fail(out, "el/elf4.dat is not the expected dat (TM3, MD)");

        Scr gene, her, suit;
        if (!readScr(pl[1].bytes, gene) || !readScr(in.ema0Md.empty() ? ema0[1].bytes : in.ema0Md, her) ||
            (bunny && !readScr(in.elf4Md.empty() ? elf4[1].bytes : in.elf4Md, suit)))
            return fail(out, "a model could not be read");
        for (const Scr *s : {&gene, &her, &suit})
            for (const Node &n : s->nodes)
                if (!mdbOk(n.mdb))
                    return fail(out, "a model's mesh could not be read");
        const Node *geneHead = find(gene, "HD_00"), *herHead = find(her, "HD_00");
        if (!geneHead || !herHead || !find(her, "RH_00") || !find(her, "RH_01") || !find(her, "LH_00") || !find(her, "LH_01") ||
            !find(her, "z_00") || r16(herHead->mdb, 8) != 30 || r16(geneHead->mdb, 8) < 29)
            return fail(out, "the models are not the ones this was made for");
        if (bunny && (!find(suit, "body") || r16(find(suit, "body")->mdb, 8) != 30))
            return fail(out, "the bunny suit is not the one this was made for");

        // Textures: hers (the skin warmed); the bunny suit's own sheets first, hers after them.
        Textures herTex, suitTex, texs;
        if (!readTm3(ema0[0].bytes, herTex) || (bunny && !readTm3(elf4[0].bytes, suitTex)))
            return fail(out, "a texture pack could not be read");
        const float skin = std::clamp(in.skin, 0.0f, 1.0f);
        if (skin > 0.0f)
            for (auto &[name, tim] : herTex)
                if (name == "ema0_01" || name == "ema0_03" || name == "ema0_06")
                    out.skinColours += shiftSkin(tim, warmth(skin));
        // The bunny suit's skin (emaf01) is cutscene Olivia's: brought back to the same warmth as her head and hands
        // (the cutscene's turned by the inverse of the rest of the way).
        if (bunny && skin < 1.0f)
        {
            const SkinShift full = warmth(1.0f), part = warmth(skin);
            for (auto &[name, tim] : suitTex)
                if (name == "emaf01")
                    out.skinColours += shiftSkin(tim, {part.hue - full.hue, part.sat / full.sat, part.val / full.val});
        }
        texs = suitTex;
        texs.insert(texs.end(), herTex.begin(), herTex.end());
        const int herBase = static_cast<int>(suitTex.size());

        // The model: her nodes (or the suit's body and her head, hair, lashes, hands), Gene's four hand shapes, and the
        // God Hand's two nodes empty.
        Scr made;
        made.kind = her.kind;
        made.anchor = her.anchor;
        const auto take = [&](const Node &n, const std::string &as, int matBase) {
            Node copy = n;
            copy.name = as;
            if (matBase)
                shiftMaterials(copy.mdb, matBase);
            made.nodes.push_back(std::move(copy));
        };
        if (bunny)
        {
            for (const Node &n : suit.nodes)
                take(n, n.name, 0);
            for (const Node &n : her.nodes)
            {
                const bool head = n.name == "HD_00" || n.name == "y_Eyl00" || (n.name.rfind("z_", 0) == 0 && n.name != "z_blt00");
                const bool hand = n.name.rfind("RH_", 0) == 0 || n.name.rfind("LH_", 0) == 0;
                if (head || hand)
                    take(n, n.name, herBase);
            }
        }
        else
            for (const Node &n : her.nodes)
                take(n, n.name, 0);
        const auto handOf = [&](const char *side, int which) { return find(her, std::string(side) + (which == 1 ? "_01" : "_00")); };
        for (const char *side : {"RH", "LH"})
        {
            const std::string s2 = std::string(side) + "_02", s3 = std::string(side) + "_03";
            take(*handOf(side, in.hand2 == 1 ? 1 : 0), s2, herBase);
            if (in.hand2 == 2)
            {
                const Node *open = find(gene, std::string(side) + "_00"), *spread = find(gene, s2);
                if (open && spread && spreadHand(made.nodes.back().mdb, herHead->mdb, open->mdb, spread->mdb, side[0] == 'R' ? 10 : 16))
                    ++out.spreadHands;
            }
            take(*handOf(side, in.hand3), s3, herBase);
        }
        for (const char *name : {"GH_00", "GHA_00"})
        {
            take(*find(her, "z_00"), name, herBase);
            collapse(made.nodes.back().mdb);
        }
        out.nodes = static_cast<int>(made.nodes.size());

        // Gene's motions on her skeleton.
        float geneHip[3], herHip[3], geneAnkle[3], herAnkle[3];
        if (!bindOf(geneHead->mdb, 0, geneHip) || !bindOf(herHead->mdb, 0, herHip) || !bindOf(geneHead->mdb, 20, geneAnkle) ||
            !bindOf(herHead->mdb, 20, herAnkle))
            return fail(out, "the skeletons could not be read");
        const float geneTop = maxY(geneHead->mdb), herTop = maxY(herHead->mdb);
        if (!(geneTop > 1.0f) || !(herTop > 1.0f))
            return fail(out, "the heads' heights could not be read");
        out.ratio = herTop / geneTop;
        out.hipDy = herHip[1] - geneHip[1];
        out.ankleDy = herAnkle[1] - geneAnkle[1];
        const Affine flat{out.ratio, 0.0f}, hipY{1.0f, out.hipDy}, ankleY{1.0f, out.ankleDy};
        const auto edit = [&](int bone, int component) -> const Affine * {
            if (bone == 255)
                return &flat;
            if (bone == 0)
                return component == 1 ? &hipY : &flat;
            if (bone == 20 || bone == 24)
                return component == 1 ? &ankleY : &flat;
            return nullptr;
        };
        for (size_t i = 3; i < pl.size(); ++i)
            if (entryIs(pl[i], "MOT"))
                if (const int n = editMotion(pl[i].bytes, edit))
                    ++out.motions, out.channels += n;

        pl[0].bytes = writeTm3(texs);
        pl[1].bytes = writeScr(made);
        out.textureBytes = static_cast<uint32_t>(pl[0].bytes.size());
        out.body = writeDat(pl);
        return true;
    }

    bool hideCoat(const std::vector<uint8_t> &coat, std::vector<uint8_t> &out)
    {
        std::vector<Entry> entries;
        if (!readDat(coat, entries))
            return false;
        int models = 0;
        for (Entry &e : entries)
        {
            if (!entryIs(e, "MD"))
                continue;
            Scr s;
            if (!readScr(e.bytes, s))
                return false;
            for (const Node &n : s.nodes)
                if (!mdbOk(n.mdb))
                    return false;
            // In place: the file keeps its layout.
            const uint32_t n = r32(e.bytes, 8);
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint32_t no = r32(e.bytes, 16 + 4 * i);
                int32_t rel;
                std::memcpy(&rel, e.bytes.data() + no, 4);
                collapse(e.bytes, static_cast<uint32_t>(static_cast<int64_t>(no) + rel));
            }
            ++models;
        }
        if (models == 0)
            return false;
        out = writeDat(entries);
        return true;
    }
}
