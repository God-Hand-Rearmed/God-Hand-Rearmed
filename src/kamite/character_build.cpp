// A built-in character's player files (see character_build.h). Every step mirrors the Python it names, operation for
// operation, so the bytes come out the same: change both together.
#include "kamite/character_build.h"
#include "kamite/model_files.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <cfenv>
#include <tuple>
#include <unordered_map>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#endif

// The float steps must round as Python's / numpy's do: no fused multiply-adds.
#if defined(_MSC_VER) && !defined(__clang__)
#pragma fp_contract(off)
#elif defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

namespace kamite::character_build
{
    namespace
    {
        using namespace model_files;

        // The default floating-point environment (round to nearest, no flush to zero) for the build, as Python has it:
        // the game thread runs with the EE's (round toward zero, denormals flushed), whatever thread builds.
        struct DefaultFloatEnv
        {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
            unsigned saved = _mm_getcsr();
            DefaultFloatEnv() { _mm_setcsr(0x1F80u); }
            ~DefaultFloatEnv() { _mm_setcsr(saved); }
#else
            int saved = std::fegetround();
            DefaultFloatEnv() { std::fesetround(FE_TONEAREST); }
            ~DefaultFloatEnv() { std::fesetround(saved); }
#endif
        };

        // ---- a little JSON (character.json) --------------------------------------------------------------------------
        struct Json
        {
            enum class Kind
            {
                Null,
                Bool,
                Number,
                String,
                Array,
                Object,
            } kind = Kind::Null;
            bool boolean = false;
            double number = 0.0;
            std::string text;
            std::vector<Json> items;
            std::vector<std::string> keys; // an object's members: keys[i] -> items[i]

            const Json *get(std::string_view key) const
            {
                const Json *found = nullptr;
                for (size_t i = 0; i < keys.size(); ++i)
                    if (keys[i] == key)
                        found = &items[i]; // the last one, as Python's json keeps
                return found;
            }
        };

        struct JsonParser
        {
            std::string_view s;
            size_t i = 0;
            std::string error;

            void space()
            {
                while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
                    ++i;
            }
            bool fail(const char *why)
            {
                if (error.empty())
                    error = std::string(why) + " at byte " + std::to_string(i);
                return false;
            }
            bool string(std::string &out)
            {
                if (i >= s.size() || s[i] != '"')
                    return fail("expected a string");
                ++i;
                while (i < s.size() && s[i] != '"')
                {
                    char c = s[i++];
                    if (c == '\\')
                    {
                        if (i >= s.size())
                            return fail("bad escape");
                        const char e = s[i++];
                        switch (e)
                        {
                        case 'n': c = '\n'; break;
                        case 't': c = '\t'; break;
                        case 'r': c = '\r'; break;
                        case 'b': c = '\b'; break;
                        case 'f': c = '\f'; break;
                        case 'u':
                        {
                            if (i + 4 > s.size())
                                return fail("bad escape");
                            unsigned v = 0;
                            if (std::from_chars(s.data() + i, s.data() + i + 4, v, 16).ptr != s.data() + i + 4)
                                return fail("bad escape");
                            i += 4;
                            if (v < 0x80)
                                out += static_cast<char>(v);
                            else if (v < 0x800)
                                out += static_cast<char>(0xC0 | (v >> 6)), out += static_cast<char>(0x80 | (v & 0x3F));
                            else
                                out += static_cast<char>(0xE0 | (v >> 12)), out += static_cast<char>(0x80 | ((v >> 6) & 0x3F)),
                                    out += static_cast<char>(0x80 | (v & 0x3F));
                            continue;
                        }
                        default: c = e; break;
                        }
                    }
                    out += c;
                }
                if (i >= s.size())
                    return fail("unterminated string");
                ++i;
                return true;
            }
            bool value(Json &out, int depth = 0)
            {
                if (depth > 32)
                    return fail("too deep");
                space();
                if (i >= s.size())
                    return fail("unexpected end");
                const char c = s[i];
                if (c == '{')
                {
                    out.kind = Json::Kind::Object;
                    ++i;
                    space();
                    if (i < s.size() && s[i] == '}')
                        return ++i, true;
                    for (;;)
                    {
                        space();
                        std::string key;
                        if (!string(key))
                            return false;
                        space();
                        if (i >= s.size() || s[i] != ':')
                            return fail("expected ':'");
                        ++i;
                        Json v;
                        if (!value(v, depth + 1))
                            return false;
                        out.keys.push_back(std::move(key));
                        out.items.push_back(std::move(v));
                        space();
                        if (i < s.size() && s[i] == ',')
                        {
                            ++i;
                            continue;
                        }
                        if (i < s.size() && s[i] == '}')
                            return ++i, true;
                        return fail("expected ',' or '}'");
                    }
                }
                if (c == '[')
                {
                    out.kind = Json::Kind::Array;
                    ++i;
                    space();
                    if (i < s.size() && s[i] == ']')
                        return ++i, true;
                    for (;;)
                    {
                        Json v;
                        if (!value(v, depth + 1))
                            return false;
                        out.items.push_back(std::move(v));
                        space();
                        if (i < s.size() && s[i] == ',')
                        {
                            ++i;
                            continue;
                        }
                        if (i < s.size() && s[i] == ']')
                            return ++i, true;
                        return fail("expected ',' or ']'");
                    }
                }
                if (c == '"')
                {
                    out.kind = Json::Kind::String;
                    return string(out.text);
                }
                if (s.substr(i, 4) == "true" || s.substr(i, 5) == "false")
                {
                    out.kind = Json::Kind::Bool;
                    out.boolean = s[i] == 't';
                    i += out.boolean ? 4 : 5;
                    return true;
                }
                if (s.substr(i, 4) == "null")
                {
                    i += 4;
                    return true;
                }
                // A number (correctly rounded, as Python's float()).
                size_t end = i;
                while (end < s.size() && (std::isdigit(static_cast<unsigned char>(s[end])) || s[end] == '-' || s[end] == '+' ||
                                          s[end] == '.' || s[end] == 'e' || s[end] == 'E'))
                    ++end;
                const char *first = s.data() + i;
                if (end > i && *first == '+')
                    return fail("bad number");
                const auto r = std::from_chars(first, s.data() + end, out.number);
                if (r.ec != std::errc() || r.ptr != s.data() + end)
                    return fail("bad number");
                out.kind = Json::Kind::Number;
                i = end;
                return true;
            }
        };

        bool numbers(const Json *j, double *out, size_t count)
        {
            if (!j || j->kind != Json::Kind::Array || j->items.size() != count)
                return false;
            for (size_t k = 0; k < count; ++k)
            {
                if (j->items[k].kind != Json::Kind::Number)
                    return false;
                out[k] = j->items[k].number;
            }
            return true;
        }
        bool number(const Json *j, double &out)
        {
            if (!j || j->kind != Json::Kind::Number)
                return false;
            out = j->number;
            return true;
        }

        // ---- models (gamemodel.py) -------------------------------------------------------------------------------------
        const char *const kKeep[] = {"GH_00", "GHA_00", "GHA_01", "RH_00", "RH_01", "RH_02", "RH_03"}; // gamemodel.KEEP

        struct Vb
        {
            std::vector<std::array<double, 3>> pos;
            std::vector<int64_t> flags;
            bool hasNrm = false, hasCol = false, hasWt = false;
            std::vector<std::array<int, 3>> nrm;
            std::vector<std::array<int, 2>> uv;
            std::vector<std::array<int, 4>> col;
            std::vector<std::array<uint8_t, 8>> wt;
            uint32_t mat = 0;
            uint8_t extra[8] = {};
        };
        struct Mdb
        {
            std::vector<Vb> vbs;
            uint8_t hdr[20] = {};
            uint32_t boneOff = 0, bones = 0;
        };
        struct ScrNode
        {
            std::string name;
            uint32_t nodeOff = 0, mdbOff = 0, unk4 = 0;
            uint8_t srt[36] = {};
            uint8_t tail[60] = {};
        };

        int16_t s16(const Bytes &b, size_t o) { return static_cast<int16_t>(r16(b, o)); }
        int32_t s32(const Bytes &b, size_t o) { return static_cast<int32_t>(r32(b, o)); }

        // mdb_read
        bool readMdb(const Bytes &b, size_t o, Mdb &m)
        {
            if (!inside(b, o, 32) || std::memcmp(b.data() + o, "mdb", 3) != 0)
                return false;
            m.boneOff = static_cast<uint32_t>(o + r32(b, o + 4));
            m.bones = r16(b, o + 8);
            const uint32_t nv = r16(b, o + 10);
            std::memcpy(m.hdr, b.data() + o + 12, 20);
            const bool room = r16(b, o + 30) == 0;
            if (!inside(b, m.boneOff, 16u * m.bones) || !inside(b, o + 32, 4u * nv))
                return false;
            for (uint32_t i = 0; i < nv; ++i)
            {
                const size_t vo = o + r32(b, o + 32 + 4 * i);
                if (!inside(b, vo, 32))
                    return false;
                const uint32_t po = r32(b, vo), no = r32(b, vo + 4), uo = r32(b, vo + 8), co = r32(b, vo + 12), wo = r32(b, vo + 16);
                const uint32_t cnt = r16(b, vo + 20);
                Vb vb;
                vb.mat = r16(b, vo + 22);
                std::memcpy(vb.extra, b.data() + vo + 24, 8);
                vb.hasNrm = no != 0;
                vb.hasCol = co != 0;
                vb.hasWt = wo != 0;
                if (!inside(b, vo + po, (room ? 8u : 16u) * cnt) || !inside(b, vo + uo, 4u * cnt) ||
                    (vb.hasNrm && !inside(b, vo + no, 4u * cnt)) || (vb.hasCol && !inside(b, vo + co, 4u * cnt)) ||
                    (vb.hasWt && !inside(b, vo + wo, 8u * cnt)))
                    return false;
                for (uint32_t k = 0; k < cnt; ++k)
                {
                    if (room)
                    {
                        const size_t p = vo + po + 8 * k;
                        vb.pos.push_back({s16(b, p) * 0.01, s16(b, p + 2) * 0.01, s16(b, p + 4) * 0.01});
                        vb.flags.push_back(r16(b, p + 6));
                    }
                    else
                    {
                        const size_t p = vo + po + 16 * k;
                        vb.pos.push_back({static_cast<double>(rf(b, p)), static_cast<double>(rf(b, p + 4)), static_cast<double>(rf(b, p + 8))});
                        vb.flags.push_back(s32(b, p + 12));
                    }
                    if (vb.hasNrm)
                    {
                        const size_t p = vo + no + 4 * k;
                        vb.nrm.push_back({static_cast<int8_t>(b[p]), static_cast<int8_t>(b[p + 1]), static_cast<int8_t>(b[p + 2])});
                    }
                    vb.uv.push_back({s16(b, vo + uo + 4 * k), s16(b, vo + uo + 4 * k + 2)});
                    if (vb.hasCol)
                    {
                        const size_t p = vo + co + 4 * k;
                        vb.col.push_back({b[p], b[p + 1], b[p + 2], b[p + 3]});
                    }
                    if (vb.hasWt)
                    {
                        std::array<uint8_t, 8> w{};
                        std::memcpy(w.data(), b.data() + vo + wo + 8 * k, 8);
                        vb.wt.push_back(w);
                    }
                }
                m.vbs.push_back(std::move(vb));
            }
            return true;
        }

        // scr_read
        bool readScr(const Bytes &b, uint32_t &kind, std::vector<ScrNode> &nodes)
        {
            if (!inside(b, 0, 16) || std::memcmp(b.data(), "scr", 3) != 0)
                return false;
            kind = r32(b, 4);
            const uint32_t n = r32(b, 8);
            if (n == 0 || n > 4096 || !inside(b, 16, 4u * n))
                return false;
            for (uint32_t i = 0; i < n; ++i)
            {
                ScrNode nd;
                nd.nodeOff = r32(b, 16 + 4 * i);
                if (!inside(b, nd.nodeOff, 112))
                    return false;
                nd.mdbOff = static_cast<uint32_t>(static_cast<int64_t>(nd.nodeOff) + s32(b, nd.nodeOff));
                nd.unk4 = r32(b, nd.nodeOff + 4);
                if (kind != 0)
                    nd.name = fieldName(b.data() + nd.nodeOff + 8, 8);
                std::memcpy(nd.srt, b.data() + nd.nodeOff + 16, 36);
                std::memcpy(nd.tail, b.data() + nd.nodeOff + 52, 60);
                nodes.push_back(std::move(nd));
            }
            return true;
        }

        // vb_write
        bool writeVb(const Vb &vb, Bytes &out)
        {
            const size_t n = vb.pos.size();
            if (n > 0xFFFF)
                return false;
            Bytes o(32, 0);
            const uint32_t po = static_cast<uint32_t>(o.size());
            for (size_t k = 0; k < n; ++k)
            {
                const size_t at = o.size();
                o.resize(at + 16);
                for (int c = 0; c < 3; ++c)
                    putf(o, at + 4 * c, static_cast<float>(vb.pos[k][c]));
                if (vb.flags[k] < INT32_MIN || vb.flags[k] > INT32_MAX)
                    return false;
                put32(o, at + 12, static_cast<uint32_t>(static_cast<int32_t>(vb.flags[k])));
            }
            pad(o, 16);
            uint32_t no = 0, co = 0, wo = 0;
            if (vb.hasNrm)
            {
                no = static_cast<uint32_t>(o.size());
                for (const auto &t : vb.nrm)
                {
                    for (int c = 0; c < 3; ++c)
                    {
                        if (t[c] < -128 || t[c] > 127)
                            return false;
                        o.push_back(static_cast<uint8_t>(static_cast<int8_t>(t[c])));
                    }
                    o.push_back(0);
                }
                pad(o, 16);
            }
            const uint32_t uo = static_cast<uint32_t>(o.size());
            for (const auto &t : vb.uv)
                for (int c = 0; c < 2; ++c)
                {
                    if (t[c] < -32768 || t[c] > 32767)
                        return false;
                    const uint16_t v = static_cast<uint16_t>(static_cast<int16_t>(t[c]));
                    o.push_back(static_cast<uint8_t>(v)), o.push_back(static_cast<uint8_t>(v >> 8));
                }
            pad(o, 16);
            if (vb.hasCol)
            {
                co = static_cast<uint32_t>(o.size());
                for (const auto &c : vb.col)
                    for (int q = 0; q < 4; ++q)
                    {
                        if (c[q] < 0 || c[q] > 255)
                            return false;
                        o.push_back(static_cast<uint8_t>(c[q]));
                    }
                pad(o, 16);
            }
            if (vb.hasWt)
            {
                wo = static_cast<uint32_t>(o.size());
                for (const auto &w : vb.wt)
                    o.insert(o.end(), w.begin(), w.end());
                pad(o, 16);
            }
            put32(o, 0, po), put32(o, 4, no), put32(o, 8, uo), put32(o, 12, co), put32(o, 16, wo);
            put16(o, 20, static_cast<uint16_t>(n));
            if (vb.mat > 0xFFFF)
                return false;
            put16(o, 22, static_cast<uint16_t>(vb.mat));
            std::memcpy(o.data() + 24, vb.extra, 8);
            out.insert(out.end(), o.begin(), o.end());
            return true;
        }

        // mdb_write
        bool writeMdb(const Mdb &m, const Bytes &boneTable, Bytes &out)
        {
            const size_t nv = m.vbs.size(), nb = boneTable.size() / 16;
            if (nv > 0xFFFF || nb > 0xFFFF)
                return false;
            Bytes head{'m', 'd', 'b', 0, 0, 0, 0, 0};
            head.push_back(static_cast<uint8_t>(nb)), head.push_back(static_cast<uint8_t>(nb >> 8));
            head.push_back(static_cast<uint8_t>(nv)), head.push_back(static_cast<uint8_t>(nv >> 8));
            head.insert(head.end(), m.hdr, m.hdr + 20);
            const size_t tabAt = head.size();
            head.resize(head.size() + 4 * nv, 0);
            pad(head, 16);
            const size_t boneAt = head.size();
            head.insert(head.end(), boneTable.begin(), boneTable.end());
            pad(head, 16);
            for (size_t i = 0; i < nv; ++i)
            {
                put32(head, tabAt + 4 * i, static_cast<uint32_t>(head.size()));
                if (!writeVb(m.vbs[i], head))
                    return false;
                pad(head, 16);
            }
            put32(head, 4, static_cast<uint32_t>(boneAt));
            out.insert(out.end(), head.begin(), head.end());
            return true;
        }

        struct OutNode
        {
            const ScrNode *game = nullptr;
            Mdb mdb;
        };
        // scr_write
        bool writeScr(uint32_t kind, const std::vector<OutNode> &nodes, const Bytes &boneTable, uint32_t anchor, Bytes &out)
        {
            const uint32_t n = static_cast<uint32_t>(nodes.size());
            out.assign({'s', 'c', 'r', 0});
            out.resize(16, 0);
            put32(out, 4, kind);
            put32(out, 8, n);
            const size_t tab = out.size();
            out.resize(out.size() + 4u * n, 0);
            pad(out, 16);
            std::vector<uint32_t> mdbOffs;
            for (const OutNode &nd : nodes)
            {
                mdbOffs.push_back(static_cast<uint32_t>(out.size()));
                if (!writeMdb(nd.mdb, boneTable, out))
                    return false;
                pad(out, 16);
            }
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint32_t no = static_cast<uint32_t>(out.size());
                put32(out, tab + 4 * i, no);
                out.resize(out.size() + 112, 0);
                put32(out, no, mdbOffs[i] - no);
                put32(out, no + 4, anchor - no);
                const std::string &name = nodes[i].game->name;
                std::memcpy(out.data() + no + 8, name.data(), std::min<size_t>(8, name.size()));
                std::memcpy(out.data() + no + 16, nodes[i].game->srt, 36);
                std::memcpy(out.data() + no + 52, nodes[i].game->tail, 60);
            }
            return true;
        }

        // empty_mdb
        Mdb emptyMdb(int bone, uint32_t mat = 0)
        {
            Mdb m;
            Vb vb;
            vb.pos.assign(3, {0.0, 0.0, 0.0});
            vb.flags = {0x8000, 0x8000, 0};
            vb.hasNrm = vb.hasCol = vb.hasWt = true;
            vb.nrm.assign(3, {0, 127, 0});
            vb.uv.assign(3, {0, 0});
            vb.col.assign(3, {128, 128, 128, 128});
            vb.wt.assign(3, {0, static_cast<uint8_t>(bone * 4), 0, 0, 100, 0, 0, 0});
            vb.mat = mat;
            m.vbs.push_back(std::move(vb));
            const float one = 1.0f;
            std::memcpy(m.hdr + 16, &one, 4);
            return m;
        }

        // strip_tris
        using Tri = std::array<int, 3>;
        std::vector<Tri> stripTris(const std::vector<int64_t> &flags)
        {
            std::vector<Tri> tris;
            for (size_t k = 2; k < flags.size(); ++k)
            {
                if (flags[k] == 0x8000)
                    continue;
                const int a = static_cast<int>(k);
                tris.push_back(flags[k] == 0 ? Tri{a - 2, a - 1, a} : Tri{a - 1, a - 2, a});
            }
            return tris;
        }

        // ---- strip.py ------------------------------------------------------------------------------------------------------
        // Python's random.Random(seed) for a small non-negative int seed: MT19937 seeded by init_by_array([seed]).
        struct PyRandom
        {
            uint32_t mt[624];
            int index = 624;
            explicit PyRandom(uint32_t seed)
            {
                mt[0] = 19650218u;
                for (int i = 1; i < 624; ++i)
                    mt[i] = 1812433253u * (mt[i - 1] ^ (mt[i - 1] >> 30)) + static_cast<uint32_t>(i);
                const uint32_t key[1] = {seed};
                int i = 1, j = 0;
                for (int k = 624; k; --k)
                {
                    mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1664525u)) + key[j] + static_cast<uint32_t>(j);
                    ++i, ++j;
                    if (i >= 624)
                        mt[0] = mt[623], i = 1;
                    if (j >= 1)
                        j = 0;
                }
                for (int k = 623; k; --k)
                {
                    mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1566083941u)) - static_cast<uint32_t>(i);
                    ++i;
                    if (i >= 624)
                        mt[0] = mt[623], i = 1;
                }
                mt[0] = 0x80000000u;
            }
            uint32_t next()
            {
                if (index >= 624)
                {
                    for (int k = 0; k < 624; ++k)
                    {
                        const uint32_t y = (mt[k] & 0x80000000u) | (mt[(k + 1) % 624] & 0x7fffffffu);
                        mt[k] = mt[(k + 397) % 624] ^ (y >> 1) ^ ((y & 1u) ? 0x9908b0dfu : 0u);
                    }
                    index = 0;
                }
                uint32_t y = mt[index++];
                y ^= y >> 11;
                y ^= (y << 7) & 0x9d2c5680u;
                y ^= (y << 15) & 0xefc60000u;
                y ^= y >> 18;
                return y;
            }
            // _randbelow_with_getrandbits
            uint32_t below(uint32_t n)
            {
                int k = 0;
                while (k < 32 && (n >> k))
                    ++k;
                uint32_t r = next() >> (32 - k);
                while (r >= n)
                    r = next() >> (32 - k);
                return r;
            }
        };

        // A CPython set of small non-negative ints (hash = value): its iteration order is its table's slot order, which
        // the first try's min(left, key=degree) depends on (setobject.c: linear probes of 9, perturb shift 5, the table
        // grown to used x 4 when 3/5 full).
        struct PyIntSet
        {
            std::vector<int> table = std::vector<int>(8, -1);
            size_t fill = 0;
            static void insertClean(std::vector<int> &t, int key)
            {
                const size_t mask = t.size() - 1;
                size_t perturb = static_cast<size_t>(key), i = static_cast<size_t>(key) & mask;
                for (;;)
                {
                    if (t[i] < 0)
                        return void(t[i] = key);
                    if (i + 9 <= mask)
                        for (size_t j = 1; j <= 9; ++j)
                            if (t[i + j] < 0)
                                return void(t[i + j] = key);
                    perturb >>= 5;
                    i = (i * 5 + 1 + perturb) & mask;
                }
            }
            void add(int key) // a key not in the set
            {
                insertClean(table, key);
                ++fill;
                const size_t mask = table.size() - 1;
                if (fill * 5 < mask * 3)
                    return;
                const size_t minused = fill > 50000 ? fill * 2 : fill * 4;
                size_t size = 8;
                while (size <= minused)
                    size <<= 1;
                std::vector<int> grown(size, -1);
                for (const int k : table)
                    if (k >= 0)
                        insertClean(grown, k);
                table.swap(grown);
            }
            bool empty() const { return fill == 0; }
        };

        struct Stripper
        {
            const std::vector<Tri> &tris;
            std::vector<std::array<const std::vector<int> *, 3>> edgesOf; // each triangle's three edges' triangle lists
            std::unordered_map<uint64_t, std::vector<int>> edgeTris;
            std::vector<char> used;
            const std::vector<int> kNone;

            static uint64_t key(int p, int q)
            {
                const uint32_t lo = static_cast<uint32_t>(std::min(p, q)), hi = static_cast<uint32_t>(std::max(p, q));
                return (static_cast<uint64_t>(hi) << 32) | lo;
            }
            explicit Stripper(const std::vector<Tri> &t) : tris(t)
            {
                for (size_t i = 0; i < tris.size(); ++i)
                {
                    const auto [a, b, c] = tris[i];
                    for (const auto &[p, q] : {std::pair{a, b}, std::pair{b, c}, std::pair{c, a}})
                        edgeTris[key(p, q)].push_back(static_cast<int>(i));
                }
                for (const Tri &tr : tris)
                    edgesOf.push_back({&edgeTris[key(tr[0], tr[1])], &edgeTris[key(tr[1], tr[2])], &edgeTris[key(tr[2], tr[0])]});
            }
            const std::vector<int> &edge(int p, int q) const
            {
                const auto it = edgeTris.find(key(p, q));
                return it == edgeTris.end() ? kNone : it->second;
            }
            // across: the unused triangle over edge p-q (not t) and its third vertex
            bool across(int t, int p, int q, int &u, int &x) const
            {
                for (const int v : edge(p, q))
                {
                    if (v == t || used[static_cast<size_t>(v)])
                        continue;
                    int third = 0, count = 0;
                    for (const int w : tris[static_cast<size_t>(v)])
                        if (w != p && w != q)
                            third = w, ++count;
                    if (count == 1)
                        return u = v, x = third, true;
                }
                return false;
            }
            int degree(int t) const
            {
                int n = 0;
                for (const auto *list : edgesOf[static_cast<size_t>(t)])
                    for (const int u : *list)
                        if (u != t && !used[static_cast<size_t>(u)])
                            ++n;
                return n;
            }
            using Seq = std::vector<std::pair<int, int>>; // (vertex, the triangle it closes or -1)
            std::vector<int> grow(Seq &seq, int t)
            {
                std::vector<int> taken;
                int u, x;
                while (across(t, seq[seq.size() - 2].first, seq.back().first, u, x))
                {
                    t = u;
                    used[static_cast<size_t>(t)] = 1;
                    taken.push_back(t);
                    seq.emplace_back(x, t);
                }
                return taken;
            }
            Seq build(int start, int rot, bool keep, int &count)
            {
                const Tri &tr = tris[static_cast<size_t>(start)];
                const int a = tr[static_cast<size_t>(rot)], b = tr[static_cast<size_t>((rot + 1) % 3)], c = tr[static_cast<size_t>((rot + 2) % 3)];
                used[static_cast<size_t>(start)] = 1;
                Seq back{{c, -1}, {b, -1}, {a, start}};
                const std::vector<int> tookB = grow(back, start);
                const size_t n = back.size();
                Seq seq(n);
                for (size_t i = 0; i < n; ++i) // _reverse
                    seq[i] = {back[n - 1 - i].first, i >= 2 ? back[n + 1 - i].second : -1};
                const std::vector<int> tookF = grow(seq, start);
                if (!keep)
                {
                    for (const int u : tookB)
                        used[static_cast<size_t>(u)] = 0;
                    for (const int u : tookF)
                        used[static_cast<size_t>(u)] = 0;
                    used[static_cast<size_t>(start)] = 0;
                }
                count = 1 + static_cast<int>(tookB.size() + tookF.size());
                return seq;
            }
            std::vector<Seq> run(PyRandom *rng)
            {
                used.assign(tris.size(), 0);
                std::vector<Seq> strips;
                PyIntSet left;
                for (size_t t = 0; t < tris.size(); ++t)
                    left.add(static_cast<int>(t));
                while (!left.empty())
                {
                    int start = -1;
                    if (!rng)
                    {
                        int best = 0;
                        for (const int t : left.table)
                            if (t >= 0)
                                if (const int d = degree(t); start < 0 || d < best)
                                    start = t, best = d;
                    }
                    else
                    {
                        std::vector<std::pair<int, int>> d; // (triangle, degree)
                        int m = 0;
                        for (const int t : left.table)
                            if (t >= 0)
                            {
                                d.emplace_back(t, degree(t));
                                m = d.size() == 1 ? d.back().second : std::min(m, d.back().second);
                            }
                        std::vector<int> cands;
                        for (const auto &[t, k] : d)
                            if (k == m)
                                cands.push_back(t);
                        std::sort(cands.begin(), cands.end());
                        start = cands[rng->below(static_cast<uint32_t>(cands.size()))];
                    }
                    int rots[3] = {0, 1, 2};
                    if (rng)
                        for (int i = 2; i >= 1; --i)
                            std::swap(rots[i], rots[rng->below(static_cast<uint32_t>(i + 1))]);
                    int bestRot = rots[0], bestN = -1;
                    size_t bestLen = 0;
                    for (const int r : rots)
                    {
                        int n;
                        const Seq s = build(start, r, false, n);
                        if (bestN < 0 || n > bestN || (n == bestN && s.size() < bestLen))
                            bestRot = r, bestN = n, bestLen = s.size();
                    }
                    int n;
                    strips.push_back(build(start, bestRot, true, n));
                    PyIntSet rest;
                    for (const int t : left.table)
                        if (t >= 0 && !used[static_cast<size_t>(t)])
                            rest.add(t);
                    left = std::move(rest);
                }
                return strips;
            }
        };

        // build_buffers(tris, max_verts): [(vertex, flag)] per buffer
        using Buffer = std::vector<std::pair<int, int64_t>>;
        std::vector<Buffer> buildBuffers(const std::vector<Tri> &tris, size_t maxVerts = 1000)
        {
            Stripper st(tris);
            std::vector<Stripper::Seq> best;
            size_t bestCost = 0;
            for (uint32_t k = 0; k < 40; ++k) // stripify, tries=40
            {
                std::vector<Stripper::Seq> strips;
                if (k == 0)
                    strips = st.run(nullptr);
                else
                {
                    PyRandom rng(k);
                    strips = st.run(&rng);
                }
                size_t cost = 0;
                for (const auto &s : strips)
                    cost += s.size();
                if (k == 0 || cost < bestCost)
                    best = std::move(strips), bestCost = cost;
            }
            std::vector<Buffer> bufs;
            Buffer cur;
            for (const auto &s : best)
            {
                if (cur.size() + 3 > maxVerts)
                    bufs.push_back(std::move(cur)), cur.clear();
                int last[2] = {0, 0};
                int have = 0;
                for (const auto &[v, tid] : s)
                {
                    if (tid >= 0)
                    {
                        if (cur.size() + 1 > maxVerts)
                        {
                            bufs.push_back(std::move(cur));
                            cur = {{last[0], 0x8000}, {last[1], 0x8000}};
                        }
                        const Tri &t = tris[static_cast<size_t>(tid)];
                        const bool same = (last[0] == t[0] && last[1] == t[1] && v == t[2]) || (last[0] == t[1] && last[1] == t[2] && v == t[0]) ||
                                          (last[0] == t[2] && last[1] == t[0] && v == t[1]);
                        cur.emplace_back(v, same ? 0 : 1);
                    }
                    else
                        cur.emplace_back(v, 0x8000);
                    if (have < 2)
                        last[have++] = v;
                    else
                        last[0] = last[1], last[1] = v;
                }
            }
            if (!cur.empty())
                bufs.push_back(std::move(cur));
            return bufs;
        }

        // ---- trim_mdb / restrip_mdb ---------------------------------------------------------------------------------------
        struct Vert
        {
            std::array<double, 3> pos{};
            std::array<int, 3> nrm{};
            std::array<int, 2> uv{};
            std::array<int, 4> col{};
            std::array<uint8_t, 8> wt{};
        };
        Vert vertOf(const Vb &vb, size_t k)
        {
            Vert v;
            v.pos = vb.pos[k];
            if (vb.hasNrm)
                v.nrm = vb.nrm[k];
            v.uv = vb.uv[k];
            if (vb.hasCol)
                v.col = vb.col[k];
            if (vb.hasWt)
                v.wt = vb.wt[k];
            return v;
        }
        int pyRound(double x) { return static_cast<int>(std::nearbyint(x)); } // round(): half to even
        // Python 3.12+ sum() of floats (Neumaier), from int 0.
        double pySum3(double a, double b, double c)
        {
            double s = a, comp = 0.0;
            for (const double x : {b, c})
            {
                const double t = s + x;
                if (std::fabs(s) >= std::fabs(x))
                    comp += (s - t) + x;
                else
                    comp += (x - t) + s;
                s = t;
            }
            if (comp != 0.0 && std::isfinite(comp))
                s += comp;
            return s;
        }
        // _lerp_vert
        Vert lerpVert(const Vb &vb, size_t i, size_t j, double t)
        {
            Vert out;
            for (int k = 0; k < 3; ++k)
                out.pos[k] = vb.pos[i][k] + (vb.pos[j][k] - vb.pos[i][k]) * t;
            if (vb.hasNrm)
            {
                double n[3];
                for (int k = 0; k < 3; ++k)
                    n[k] = vb.nrm[i][k] + static_cast<double>(vb.nrm[j][k] - vb.nrm[i][k]) * t;
                double l = std::sqrt(pySum3(n[0] * n[0], n[1] * n[1], n[2] * n[2]));
                if (l == 0.0)
                    l = 1.0;
                for (int k = 0; k < 3; ++k)
                    out.nrm[k] = pyRound(n[k] / l * 127);
            }
            for (int k = 0; k < 2; ++k)
                out.uv[k] = pyRound(vb.uv[i][k] + static_cast<double>(vb.uv[j][k] - vb.uv[i][k]) * t);
            if (vb.hasCol)
                for (int k = 0; k < 4; ++k)
                    out.col[k] = pyRound(vb.col[i][k] + static_cast<double>(vb.col[j][k] - vb.col[i][k]) * t);
            if (vb.hasWt)
                out.wt = t > 0.5 ? vb.wt[j] : vb.wt[i];
            return out;
        }
        Vb vbFrom(const Vb &like, const std::vector<Vert> &verts, const Buffer &buf)
        {
            Vb out;
            out.hasNrm = like.hasNrm, out.hasCol = like.hasCol, out.hasWt = like.hasWt;
            out.mat = like.mat;
            std::memcpy(out.extra, like.extra, 8);
            for (const auto &[id, flag] : buf)
            {
                const Vert &v = verts[static_cast<size_t>(id)];
                out.flags.push_back(flag);
                out.pos.push_back(v.pos);
                if (out.hasNrm)
                    out.nrm.push_back(v.nrm);
                out.uv.push_back(v.uv);
                if (out.hasCol)
                    out.col.push_back(v.col);
                if (out.hasWt)
                    out.wt.push_back(v.wt);
            }
            return out;
        }

        Mdb trimMdb(const Mdb &mdb, const Trim &tr)
        {
            Mdb result;
            std::memcpy(result.hdr, mdb.hdr, 20);
            for (const Vb &vb : mdb.vbs)
            {
                const size_t cnt = vb.pos.size();
                std::vector<double> d(cnt);
                for (size_t k = 0; k < cnt; ++k)
                    d[k] = vb.pos[k][0] * tr.axis[0] + vb.pos[k][1] * tr.axis[1] + vb.pos[k][2] * tr.axis[2] - tr.minimum;
                std::vector<Vert> verts;
                for (size_t k = 0; k < cnt; ++k)
                    verts.push_back(vertOf(vb, k));
                std::vector<double> dist = d;
                std::map<std::pair<int, int>, int> cut;
                auto cutv = [&](int i, int j) {
                    const auto key = std::make_pair(std::min(i, j), std::max(i, j));
                    if (const auto it = cut.find(key); it != cut.end())
                        return it->second;
                    const double t = dist[static_cast<size_t>(i)] / (dist[static_cast<size_t>(i)] - dist[static_cast<size_t>(j)]);
                    const int n = static_cast<int>(verts.size());
                    verts.push_back(lerpVert(vb, static_cast<size_t>(i), static_cast<size_t>(j), t));
                    dist.push_back(0.0);
                    cut[key] = n;
                    return n;
                };
                std::vector<Tri> tris;
                for (const Tri &t : stripTris(vb.flags))
                {
                    int in = 0;
                    for (const int i : t)
                        in += d[static_cast<size_t>(i)] >= 0;
                    if (in == 3)
                    {
                        tris.push_back(t);
                        continue;
                    }
                    if (in == 0)
                        continue;
                    std::vector<int> poly;
                    for (int e = 0; e < 3; ++e)
                    {
                        const int a = t[static_cast<size_t>(e)], b = t[static_cast<size_t>((e + 1) % 3)];
                        const bool ia = d[static_cast<size_t>(a)] >= 0, ib = d[static_cast<size_t>(b)] >= 0;
                        if (ia)
                            poly.push_back(a);
                        if (ia != ib)
                            poly.push_back(cutv(a, b));
                    }
                    for (size_t k = 1; k + 1 < poly.size(); ++k)
                        tris.push_back({poly[0], poly[k], poly[k + 1]});
                }
                if (tris.empty())
                    continue;
                if (tr.inset)
                {
                    const double start = tr.insetStart, factor = tr.insetFactor, cy = tr.insetCentre[0], cz = tr.insetCentre[1];
                    for (size_t k = 0; k < verts.size(); ++k)
                    {
                        const double dk = dist[k] + tr.minimum;
                        if (dk < start)
                        {
                            const double q = (dk - tr.minimum) / (start - tr.minimum);
                            const double f = factor + (1.0 - factor) * (q > 0.0 ? q : 0.0);
                            auto &p = verts[k].pos;
                            p = {p[0], cy + (p[1] - cy) * f, cz + (p[2] - cz) * f};
                        }
                    }
                }
                if (tr.tint && vb.hasCol)
                {
                    for (size_t k = 0; k < verts.size(); ++k)
                    {
                        double f = 1.0;
                        if (tr.tintLength > 0)
                        {
                            const double q = (dist[k] + tr.minimum - tr.tintStart) / tr.tintLength;
                            f = std::min(1.0, q > 0.0 ? q : 0.0);
                        }
                        auto &c = verts[k].col;
                        for (int q = 0; q < 3; ++q)
                            c[q] = pyRound(c[q] * (tr.tintRgb[q] / 128.0 + (1.0 - tr.tintRgb[q] / 128.0) * f));
                    }
                }
                for (const Buffer &buf : buildBuffers(tris))
                    result.vbs.push_back(vbFrom(vb, verts, buf));
            }
            if (result.vbs.empty())
                result.vbs = emptyMdb(0).vbs;
            return result;
        }

        Mdb restripMdb(const Mdb &mdb)
        {
            Mdb out;
            std::memcpy(out.hdr, mdb.hdr, 20);
            size_t before = 0, after = 0;
            for (const Vb &vb : mdb.vbs)
            {
                before += vb.flags.size();
                // Identical vertices (every field equal, as Python's tuples compare: -0.0 == 0.0) merged.
                const auto less = [&](size_t a, size_t b) {
                    if (vb.pos[a] != vb.pos[b])
                        return vb.pos[a] < vb.pos[b];
                    if (vb.hasNrm && vb.nrm[a] != vb.nrm[b])
                        return vb.nrm[a] < vb.nrm[b];
                    if (vb.uv[a] != vb.uv[b])
                        return vb.uv[a] < vb.uv[b];
                    if (vb.hasCol && vb.col[a] != vb.col[b])
                        return vb.col[a] < vb.col[b];
                    if (vb.hasWt && vb.wt[a] != vb.wt[b])
                        return vb.wt[a] < vb.wt[b];
                    return false;
                };
                std::map<size_t, int, decltype(less)> uniq(less);
                std::vector<size_t> first;
                auto vid = [&](int i) {
                    const auto [it, added] = uniq.emplace(static_cast<size_t>(i), static_cast<int>(first.size()));
                    if (added)
                        first.push_back(static_cast<size_t>(i));
                    return it->second;
                };
                std::vector<Tri> tris;
                for (const Tri &t : stripTris(vb.flags))
                {
                    const int a = vid(t[0]), b = vid(t[1]), c = vid(t[2]);
                    if (a != b && b != c && a != c)
                        tris.push_back({a, b, c});
                }
                if (tris.empty())
                    continue;
                std::vector<Vert> verts;
                for (const size_t i : first)
                    verts.push_back(vertOf(vb, i));
                for (const Buffer &buf : buildBuffers(tris))
                {
                    out.vbs.push_back(vbFrom(vb, verts, buf));
                    after += buf.size();
                }
            }
            if (out.vbs.empty() || after >= before)
                return mdb;
            return out;
        }

        // ---- textures (gametex.py, paint.py) ----------------------------------------------------------------------------
        constexpr int kBlock32[32] = {0, 1, 4, 5, 16, 17, 20, 21, 2, 3, 6, 7, 18, 19, 22, 23, 8, 9, 12, 13, 24, 25, 28, 29, 10, 11, 14, 15, 26, 27, 30, 31};
        constexpr int kColumnWord32[16] = {0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15};
        constexpr int kBlock4[32] = {0, 2, 8, 10, 1, 3, 9, 11, 4, 6, 12, 14, 5, 7, 13, 15, 16, 18, 24, 26, 17, 19, 25, 27, 20, 22, 28, 30, 21, 23, 29, 31};
        int columnWord4(int col, int i) // ColumnWord4[col & 1][i]
        {
            static const int a[8] = {0, 1, 4, 5, 8, 9, 12, 13}, b[8] = {2, 3, 6, 7, 10, 11, 14, 15};
            static const int c[8] = {8, 9, 12, 13, 0, 1, 4, 5}, e[8] = {10, 11, 14, 15, 2, 3, 6, 7};
            // _cw4a = a x 4 + b x 4, _cw4b = c x 4 + e x 4; [_cw4a + _cw4b, _cw4b + _cw4a]
            const bool second = (i >= 64) != ((col & 1) != 0);
            const int r = i % 64;
            return second ? (r < 32 ? c[r % 8] : e[r % 8]) : (r < 32 ? a[r % 8] : b[r % 8]);
        }
        int columnByte4(int i)
        {
            const int r = i % 32, half = i / 64;
            return 2 * (r / 8) + half;
        }
        size_t addr32(int x, int y, int dbw)
        {
            const int page = x / 64 + (y / 32) * dbw, px = x % 64, py = y % 32;
            const int block = kBlock32[px / 8 + (py / 8) * 8], bx = px % 8, by = py % 8, col = by / 2, cy = by % 2;
            return static_cast<size_t>(page) * 2048 + static_cast<size_t>(block) * 64 + static_cast<size_t>(col) * 16 + static_cast<size_t>(kColumnWord32[bx + cy * 8]);
        }
        size_t addr4(int x, int y, int dbw)
        {
            dbw >>= 1;
            const int page = x / 128 + (y / 128) * dbw, px = x % 128, py = y % 128;
            const int block = kBlock4[px / 32 + (py / 16) * 4], bx = px % 32, by = py % 16, col = by / 4, cy = by % 4;
            const size_t word = static_cast<size_t>(page) * 2048 + static_cast<size_t>(block) * 64 + static_cast<size_t>(col) * 16 +
                                static_cast<size_t>(columnWord4(col, bx + cy * 32));
            return word * 8 + static_cast<size_t>(columnByte4(bx + cy * 32));
        }

        struct Tim3
        {
            int w = 0, h = 0, imageType = 0;
            size_t dataAt = 0, dataBytes = 0, clutAt = 0, clutBytes = 0;
        };
        bool tim3Info(const Bytes &t, Tim3 &out)
        {
            if (!inside(t, 0, 16 + 48))
                return false;
            const uint32_t clut = r32(t, 20), img = r32(t, 24);
            const uint32_t hs = r16(t, 28);
            out.imageType = t[35];
            out.w = r16(t, 36), out.h = r16(t, 38);
            out.dataAt = 16 + hs, out.dataBytes = img, out.clutAt = out.dataAt + img, out.clutBytes = clut;
            return inside(t, out.dataAt, img) && inside(t, out.clutAt, clut) && out.w > 0 && out.h > 0;
        }
        // unswizzle4 / the plain nibbles: the texture's indices, row-major
        bool indices4(const Bytes &t, const Tim3 &info, std::vector<uint8_t> &ind)
        {
            const int w = info.w, h = info.h;
            if (info.imageType != 4 || info.dataBytes < static_cast<size_t>(w) * h / 2)
                return false;
            ind.assign(static_cast<size_t>(w) * h, 0);
            if (w >= 128 && h >= 128)
            {
                const int rw = w / 2, rh = h / 4, wdbw = std::max(1, rw / 64), ndbw = std::max(1, w / 64);
                std::vector<uint32_t> mem(std::max<size_t>(info.dataBytes, 4u * 1024 * 1024) / 4 + 1, 0);
                for (int y = 0; y < rh; ++y)
                    for (int x = 0; x < rw; ++x)
                    {
                        uint32_t v;
                        std::memcpy(&v, t.data() + info.dataAt + (static_cast<size_t>(y) * rw + x) * 4, 4);
                        const size_t a = addr32(x, y, wdbw);
                        if (a >= mem.size())
                            return false;
                        mem[a] = v;
                    }
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                    {
                        const size_t n = addr4(x, y, ndbw);
                        if (n / 8 >= mem.size())
                            return false;
                        ind[static_cast<size_t>(y) * w + x] = static_cast<uint8_t>((mem[n / 8] >> (4 * (n % 8))) & 15u);
                    }
            }
            else
                for (size_t i = 0; i < ind.size(); ++i)
                {
                    const uint8_t b = t[info.dataAt + i / 2];
                    ind[i] = (i & 1) ? (b >> 4) : (b & 15);
                }
            return true;
        }
        // tim3_decode (4-bit): RGBA, alpha doubled (0x80 = 255)
        bool decode4(const Bytes &t, Image &out)
        {
            Tim3 info;
            std::vector<uint8_t> ind;
            if (!tim3Info(t, info) || !indices4(t, info, ind))
                return false;
            out.width = info.w, out.height = info.h, out.channels = 4;
            out.px.resize(ind.size() * 4);
            for (size_t i = 0; i < ind.size(); ++i)
            {
                if (static_cast<size_t>(ind[i]) * 4 + 4 > info.clutBytes)
                    return false;
                const uint8_t *c = t.data() + info.clutAt + ind[i] * 4u;
                out.px[i * 4 + 0] = c[0], out.px[i * 4 + 1] = c[1], out.px[i * 4 + 2] = c[2];
                out.px[i * 4 + 3] = static_cast<uint8_t>(std::min(255, c[3] * 2));
            }
            return true;
        }
        // tim3_encode (4-bit, the game's layout)
        Bytes encode4(const std::vector<uint8_t> &idx, const std::array<std::array<uint8_t, 4>, 16> &pal, int w, int h)
        {
            Bytes data;
            int fid = 0;
            if (w >= 128 && h >= 128)
            {
                const int rw = w / 2, rh = h / 4, wdbw = std::max(1, rw / 64), ndbw = std::max(1, w / 64);
                std::vector<uint32_t> mem(4u * 1024 * 1024 / 4, 0);
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                    {
                        const size_t n = addr4(x, y, ndbw);
                        uint32_t &word = mem[n / 8];
                        word = (word & ~(15u << (4 * (n % 8)))) | (static_cast<uint32_t>(idx[static_cast<size_t>(y) * w + x] & 15) << (4 * (n % 8)));
                    }
                data.resize(static_cast<size_t>(rw) * rh * 4);
                for (int y = 0; y < rh; ++y)
                    for (int x = 0; x < rw; ++x)
                        std::memcpy(data.data() + (static_cast<size_t>(y) * rw + x) * 4, &mem[addr32(x, y, wdbw)], 4);
                fid = 6;
            }
            else
            {
                data.resize(idx.size() / 2);
                for (size_t i = 0; i + 1 < idx.size(); i += 2)
                    data[i / 2] = static_cast<uint8_t>(idx[i] | (idx[i + 1] << 4));
            }
            Bytes clut;
            for (const auto &c : pal)
                clut.insert(clut.end(), {c[0], c[1], c[2], static_cast<uint8_t>(pyRound(c[3] * 128 / 255.0))});
            const uint32_t tbw = static_cast<uint32_t>(std::max(2, w / 64));
            int tw = 0, th = 0;
            while ((1 << (tw + 1)) <= w)
                ++tw;
            while ((1 << (th + 1)) <= h)
                ++th;
            const uint64_t tex0 = (static_cast<uint64_t>(tbw) << 14) | (20ull << 20) | (static_cast<uint64_t>(tw) << 26) | (static_cast<uint64_t>(th) << 30);
            Bytes o{'T', 'I', 'M', '3', 4, static_cast<uint8_t>(fid), 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            Bytes pic(48, 0);
            put32(pic, 0, static_cast<uint32_t>(48 + clut.size() + data.size()));
            put32(pic, 4, static_cast<uint32_t>(clut.size()));
            put32(pic, 8, static_cast<uint32_t>(data.size()));
            put16(pic, 12, 48), put16(pic, 14, 16);
            pic[16] = 0, pic[17] = 1, pic[18] = 3, pic[19] = 4;
            put16(pic, 20, static_cast<uint16_t>(w)), put16(pic, 22, static_cast<uint16_t>(h));
            std::memcpy(pic.data() + 24, &tex0, 8);
            const uint64_t tex1 = 0x260;
            std::memcpy(pic.data() + 32, &tex1, 8);
            o.insert(o.end(), pic.begin(), pic.end());
            o.insert(o.end(), data.begin(), data.end());
            o.insert(o.end(), clut.begin(), clut.end());
            return o;
        }

        // Pillow's Lanczos resampling (Resample.c: precompute_coeffs, the 8-bit and the 32-bit float passes).
        double sinc(double x)
        {
            if (x == 0.0)
                return 1.0;
            x = x * 3.14159265358979323846;
            return std::sin(x) / x;
        }
        double lanczos(double x) { return (-3.0 <= x && x < 3.0) ? sinc(x) * sinc(x / 3) : 0.0; }
        int coeffs(int inSize, int outSize, std::vector<int> &bounds, std::vector<double> &kk)
        {
            const float in0 = 0.0f, in1 = static_cast<float>(inSize);
            double scale = static_cast<double>(in1 - in0) / outSize, filterscale = scale;
            if (filterscale < 1.0)
                filterscale = 1.0;
            const double support = 3.0 * filterscale;
            const int ksize = static_cast<int>(std::ceil(support)) * 2 + 1;
            kk.assign(static_cast<size_t>(outSize) * ksize, 0.0);
            bounds.assign(static_cast<size_t>(outSize) * 2, 0);
            for (int xx = 0; xx < outSize; ++xx)
            {
                const double center = in0 + (xx + 0.5) * scale, ss = 1.0 / filterscale;
                double ww = 0.0;
                int xmin = static_cast<int>(center - support + 0.5);
                if (xmin < 0)
                    xmin = 0;
                int xmax = static_cast<int>(center + support + 0.5);
                if (xmax > inSize)
                    xmax = inSize;
                xmax -= xmin;
                double *k = &kk[static_cast<size_t>(xx) * ksize];
                for (int x = 0; x < xmax; ++x)
                {
                    const double w = lanczos((x + xmin - center + 0.5) * ss);
                    k[x] = w;
                    ww += w;
                }
                for (int x = 0; x < xmax; ++x)
                    if (ww != 0.0)
                        k[x] /= ww;
                bounds[static_cast<size_t>(xx) * 2] = xmin;
                bounds[static_cast<size_t>(xx) * 2 + 1] = xmax;
            }
            return ksize;
        }
        // A float plane (mode "F") resized.
        std::vector<float> resizeF(const std::vector<float> &in, int w, int h, int ow, int oh)
        {
            std::vector<int> bh, bv;
            std::vector<double> kh, kv;
            const int ksh = coeffs(w, ow, bh, kh), ksv = coeffs(h, oh, bv, kv);
            const int first = bv[0], last = bv[static_cast<size_t>(oh) * 2 - 2] + bv[static_cast<size_t>(oh) * 2 - 1];
            const bool needH = ow != w, needV = oh != h;
            std::vector<float> tmp = in;
            int tw = w, th = h;
            if (needH)
            {
                for (int i = 0; i < oh; ++i)
                    bv[static_cast<size_t>(i) * 2] -= first;
                th = last - first;
                tmp.assign(static_cast<size_t>(ow) * th, 0.0f);
                for (int yy = 0; yy < th; ++yy)
                    for (int xx = 0; xx < ow; ++xx)
                    {
                        const int xmin = bh[static_cast<size_t>(xx) * 2], xmax = bh[static_cast<size_t>(xx) * 2 + 1];
                        const double *k = &kh[static_cast<size_t>(xx) * ksh];
                        double ss = 0.0;
                        for (int x = 0; x < xmax; ++x)
                            ss += static_cast<double>(in[static_cast<size_t>(yy + first) * w + static_cast<size_t>(x + xmin)]) * k[x];
                        tmp[static_cast<size_t>(yy) * ow + xx] = static_cast<float>(ss);
                    }
                tw = ow;
            }
            if (!needV)
                return tmp;
            std::vector<float> out(static_cast<size_t>(tw) * oh, 0.0f);
            for (int yy = 0; yy < oh; ++yy)
            {
                const int ymin = bv[static_cast<size_t>(yy) * 2], ymax = bv[static_cast<size_t>(yy) * 2 + 1];
                const double *k = &kv[static_cast<size_t>(yy) * ksv];
                for (int xx = 0; xx < tw; ++xx)
                {
                    double ss = 0.0;
                    for (int y = 0; y < ymax; ++y)
                        ss += static_cast<double>(tmp[static_cast<size_t>(y + ymin) * tw + xx]) * k[y];
                    out[static_cast<size_t>(yy) * tw + xx] = static_cast<float>(ss);
                }
            }
            return out;
        }
        // An RGBA image resized as Pillow does for mode RGBA (premultiplied, 8-bit fixed point coefficients).
        Image resizeRgba(const Image &src, int ow, int oh)
        {
            constexpr int kBits = 22;
            const int w = src.width, h = src.height;
            std::vector<uint8_t> pre(src.px.size());
            for (size_t i = 0; i < src.px.size(); i += 4)
            {
                const unsigned a = src.px[i + 3];
                for (int c = 0; c < 3; ++c)
                {
                    const unsigned tmp = src.px[i + static_cast<size_t>(c)] * a + 128;
                    pre[i + static_cast<size_t>(c)] = static_cast<uint8_t>(((tmp >> 8) + tmp) >> 8);
                }
                pre[i + 3] = static_cast<uint8_t>(a);
            }
            auto fixed = [&](std::vector<double> &kk) {
                std::vector<int> out(kk.size());
                for (size_t x = 0; x < kk.size(); ++x)
                    out[x] = kk[x] < 0 ? static_cast<int>(-0.5 + kk[x] * (1 << kBits)) : static_cast<int>(0.5 + kk[x] * (1 << kBits));
                return out;
            };
            auto clip8 = [](int v) { return static_cast<uint8_t>(std::clamp(v >> kBits, 0, 255)); };
            std::vector<int> bh, bv;
            std::vector<double> khd, kvd;
            const int ksh = coeffs(w, ow, bh, khd), ksv = coeffs(h, oh, bv, kvd);
            const std::vector<int> kh = fixed(khd), kv = fixed(kvd);
            const int first = bv[0], last = bv[static_cast<size_t>(oh) * 2 - 2] + bv[static_cast<size_t>(oh) * 2 - 1];
            std::vector<uint8_t> tmp = pre;
            int tw = w;
            if (ow != w)
            {
                for (int i = 0; i < oh; ++i)
                    bv[static_cast<size_t>(i) * 2] -= first;
                const int th = last - first;
                tmp.assign(static_cast<size_t>(ow) * th * 4, 0);
                for (int yy = 0; yy < th; ++yy)
                    for (int xx = 0; xx < ow; ++xx)
                    {
                        const int xmin = bh[static_cast<size_t>(xx) * 2], xmax = bh[static_cast<size_t>(xx) * 2 + 1];
                        const int *k = &kh[static_cast<size_t>(xx) * ksh];
                        for (int c = 0; c < 4; ++c)
                        {
                            int ss = 1 << (kBits - 1);
                            for (int x = 0; x < xmax; ++x)
                                ss += pre[(static_cast<size_t>(yy + first) * w + static_cast<size_t>(x + xmin)) * 4 + static_cast<size_t>(c)] * k[x];
                            tmp[(static_cast<size_t>(yy) * ow + xx) * 4 + static_cast<size_t>(c)] = clip8(ss);
                        }
                    }
                tw = ow;
            }
            Image out;
            out.width = tw, out.height = oh, out.channels = 4;
            out.px.assign(static_cast<size_t>(tw) * oh * 4, 0);
            for (int yy = 0; yy < oh; ++yy)
            {
                const int ymin = bv[static_cast<size_t>(yy) * 2], ymax = bv[static_cast<size_t>(yy) * 2 + 1];
                const int *k = &kv[static_cast<size_t>(yy) * ksv];
                for (int xx = 0; xx < tw; ++xx)
                    for (int c = 0; c < 4; ++c)
                    {
                        int ss = 1 << (kBits - 1);
                        for (int y = 0; y < ymax; ++y)
                            ss += tmp[(static_cast<size_t>(y + ymin) * tw + xx) * 4 + static_cast<size_t>(c)] * k[y];
                        out.px[(static_cast<size_t>(yy) * tw + xx) * 4 + static_cast<size_t>(c)] = clip8(ss);
                    }
            }
            for (size_t i = 0; i < out.px.size(); i += 4) // back from premultiplied
            {
                const unsigned a = out.px[i + 3];
                if (a != 255 && a != 0)
                    for (int c = 0; c < 3; ++c)
                        out.px[i + static_cast<size_t>(c)] = static_cast<uint8_t>(std::min(255u, 255u * out.px[i + static_cast<size_t>(c)] / a));
            }
            return out;
        }

        // paint.overlay: the layer (RGBA, any size: scaled to the base, premultiplied) over the base (RGBA) by its alpha.
        Image overlay(const Image &base, const Image &layer)
        {
            const int w = base.width, h = base.height;
            const size_t n = static_cast<size_t>(w) * h;
            std::vector<float> a(n), rgb(n * 3);
            if (layer.width != w || layer.height != h)
            {
                const size_t ln = static_cast<size_t>(layer.width) * layer.height;
                std::vector<std::vector<float>> pre(4, std::vector<float>(ln));
                for (size_t i = 0; i < ln; ++i)
                {
                    const float al = static_cast<float>(layer.px[i * 4 + 3]);
                    const float f = al / 255.0f;
                    for (int c = 0; c < 3; ++c)
                        pre[static_cast<size_t>(c)][i] = static_cast<float>(layer.px[i * 4 + static_cast<size_t>(c)]) * f;
                    pre[3][i] = al;
                }
                std::vector<std::vector<float>> small(4);
                for (int c = 0; c < 4; ++c)
                    small[static_cast<size_t>(c)] = resizeF(pre[static_cast<size_t>(c)], layer.width, layer.height, w, h);
                for (size_t i = 0; i < n; ++i)
                {
                    const float ai = std::clamp(small[3][i], 0.0f, 255.0f) / 255.0f;
                    a[i] = ai;
                    for (int c = 0; c < 3; ++c)
                        rgb[i * 3 + static_cast<size_t>(c)] = ai > 1e-4f ? small[static_cast<size_t>(c)][i] / std::max(ai, 1e-4f) : 0.0f;
                }
            }
            else
                for (size_t i = 0; i < n; ++i)
                {
                    a[i] = static_cast<float>(layer.px[i * 4 + 3]) / 255.0f;
                    for (int c = 0; c < 3; ++c)
                        rgb[i * 3 + static_cast<size_t>(c)] = static_cast<float>(layer.px[i * 4 + static_cast<size_t>(c)]);
                }
            Image out;
            out.width = w, out.height = h, out.channels = 4;
            out.px.resize(n * 4);
            for (size_t i = 0; i < n; ++i)
            {
                for (int c = 0; c < 4; ++c)
                {
                    float v = static_cast<float>(base.px[i * 4 + static_cast<size_t>(c)]);
                    if (c < 3)
                    {
                        const float keep = v * (1.0f - a[i]);
                        const float add = std::clamp(rgb[i * 3 + static_cast<size_t>(c)], 0.0f, 255.0f) * a[i];
                        v = keep + add;
                    }
                    out.px[i * 4 + static_cast<size_t>(c)] = static_cast<uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f));
                }
            }
            return out;
        }

        // paint.quantize16: a median cut (the box with the largest squared error split at its population's median along
        // its widest channel; each box's colour its rounded mean), then each pixel's nearest colour (the first on ties).
        void quantize16(const Image &img, std::vector<uint8_t> &idx, std::array<std::array<uint8_t, 4>, 16> &pal)
        {
            const size_t n = static_cast<size_t>(img.width) * img.height;
            std::vector<uint32_t> keys(n);
            for (size_t i = 0; i < n; ++i)
                keys[i] = (static_cast<uint32_t>(img.px[i * 4]) << 16) | (static_cast<uint32_t>(img.px[i * 4 + 1]) << 8) | img.px[i * 4 + 2];
            std::sort(keys.begin(), keys.end());
            struct Colour
            {
                int c[3];
                int64_t count;
            };
            std::vector<Colour> cols;
            for (size_t i = 0; i < n;)
            {
                size_t j = i;
                while (j < n && keys[j] == keys[i])
                    ++j;
                cols.push_back({{static_cast<int>(keys[i] >> 16), static_cast<int>((keys[i] >> 8) & 255), static_cast<int>(keys[i] & 255)},
                                static_cast<int64_t>(j - i)});
                i = j;
            }
            std::vector<std::vector<Colour>> boxes{cols};
            while (boxes.size() < 16)
            {
                double best = -1;
                int bi = -1;
                for (size_t i = 0; i < boxes.size(); ++i)
                {
                    const auto &b = boxes[i];
                    if (b.size() < 2)
                        continue;
                    int64_t cnt = 0;
                    for (const Colour &c : b)
                        cnt += c.count;
                    double s = 0;
                    for (int q = 0; q < 3; ++q)
                    {
                        int64_t s1 = 0, s2 = 0;
                        for (const Colour &c : b)
                            s1 += c.c[q] * c.count, s2 += static_cast<int64_t>(c.c[q]) * c.c[q] * c.count;
                        s += static_cast<double>(s2) - static_cast<double>(s1 * s1) / static_cast<double>(cnt);
                    }
                    if (s > best)
                        best = s, bi = static_cast<int>(i);
                }
                if (bi < 0)
                    break;
                std::vector<Colour> b = boxes[static_cast<size_t>(bi)];
                int64_t cnt = 0;
                int range[3];
                for (int q = 0; q < 3; ++q)
                {
                    int lo = 255, hi = 0;
                    for (const Colour &c : b)
                        lo = std::min(lo, c.c[q]), hi = std::max(hi, c.c[q]);
                    range[q] = hi - lo;
                }
                for (const Colour &c : b)
                    cnt += c.count;
                const int ch = range[0] >= range[1] && range[0] >= range[2] ? 0 : range[1] >= range[2] ? 1 : 2;
                std::sort(b.begin(), b.end(), [&](const Colour &x, const Colour &y) {
                    return std::make_tuple(x.c[ch], x.c[0], x.c[1], x.c[2]) < std::make_tuple(y.c[ch], y.c[0], y.c[1], y.c[2]);
                });
                size_t cut = b.size() - 1;
                int64_t cum = 0;
                for (size_t i = 1; i < b.size(); ++i)
                {
                    cum += b[i - 1].count;
                    if (2 * cum >= cnt)
                    {
                        cut = i;
                        break;
                    }
                }
                boxes[static_cast<size_t>(bi)].assign(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(cut));
                boxes.emplace_back(b.begin() + static_cast<std::ptrdiff_t>(cut), b.end());
            }
            size_t k = 0;
            for (; k < boxes.size(); ++k)
            {
                int64_t cnt = 0, s[3] = {};
                for (const Colour &c : boxes[k])
                    for (int q = 0; q < 3; ++q)
                        s[q] += c.c[q] * c.count;
                for (const Colour &c : boxes[k])
                    cnt += c.count;
                for (int q = 0; q < 3; ++q)
                    pal[k][static_cast<size_t>(q)] = static_cast<uint8_t>((s[q] + cnt / 2) / cnt);
                pal[k][3] = 255;
            }
            for (; k < 16; ++k)
                pal[k] = pal[k - 1];
            idx.resize(n);
            for (size_t i = 0; i < n; ++i)
            {
                int bestD = INT32_MAX, bestK = 0;
                for (int p = 0; p < 16; ++p)
                {
                    int d = 0;
                    for (int q = 0; q < 3; ++q)
                    {
                        const int e = img.px[i * 4 + static_cast<size_t>(q)] - pal[static_cast<size_t>(p)][static_cast<size_t>(q)];
                        d += e * e;
                    }
                    if (d < bestD)
                        bestD = d, bestK = p;
                }
                idx[i] = static_cast<uint8_t>(bestK);
            }
        }
    }

    bool readSpec(std::string_view json, std::vector<FileSpec> &out, std::string &error)
    {
        JsonParser p;
        p.s = json;
        Json root;
        if (!p.value(root))
            return error = "character.json: " + p.error, false;
        const Json *files = root.get("files");
        if (!files || files->kind != Json::Kind::Object || files->keys.empty())
            return error = "character.json: no \"files\"", false;
        for (size_t fi = 0; fi < files->keys.size(); ++fi)
        {
            const std::string &path = files->keys[fi];
            const Json &how = files->items[fi];
            if (how.kind != Json::Kind::Object)
                return error = "character.json: " + path + " is not an object", false;
            FileSpec f;
            f.path = path;
            if (const Json *e = how.get("empty"); e && ((e->kind == Json::Kind::Bool && e->boolean) || (e->kind == Json::Kind::Number && e->number != 0)))
            {
                f.empty = true;
                out.push_back(std::move(f));
                continue;
            }
            const Json *nodes = how.get("nodes"), *textures = how.get("textures");
            if (!nodes || nodes->kind != Json::Kind::String || !textures || textures->kind != Json::Kind::String)
                return error = "character.json: " + path + " needs \"nodes\" and \"textures\"", false;
            f.nodes = nodes->text;
            f.textures = textures->text;
            if (const Json *keep = how.get("keep"))
            {
                if (keep->kind != Json::Kind::Array)
                    return error = "character.json: " + path + " \"keep\" is not a list", false;
                for (const Json &k : keep->items)
                    f.keep.push_back(k.text);
            }
            else
                f.keep.assign(std::begin(kKeep), std::end(kKeep));
            if (const Json *a = how.get("append_textures"); a && a->kind == Json::Kind::Bool)
                f.appendTextures = a->boolean;
            if (const Json *trim = how.get("trim"); trim && trim->kind == Json::Kind::Object)
                for (size_t ti = 0; ti < trim->keys.size(); ++ti)
                {
                    const std::string &node = trim->keys[ti];
                    const Json &t = trim->items[ti];
                    Trim tr;
                    if (!numbers(t.get("axis"), tr.axis, 3) || !number(t.get("min"), tr.minimum))
                        return error = "character.json: " + path + " trim " + node + " needs \"axis\" and \"min\"", false;
                    if (const Json *tint = t.get("tint"); tint && tint->kind == Json::Kind::Object)
                    {
                        tr.tint = true;
                        if (!number(tint->get("start"), tr.tintStart) || !number(tint->get("length"), tr.tintLength) || !numbers(tint->get("rgb"), tr.tintRgb, 3))
                            return error = "character.json: " + path + " trim " + node + ": bad \"tint\"", false;
                    }
                    if (const Json *inset = t.get("inset"); inset && inset->kind == Json::Kind::Object)
                    {
                        tr.inset = true;
                        if (!number(inset->get("start"), tr.insetStart) || !number(inset->get("factor"), tr.insetFactor) ||
                            !numbers(inset->get("centre"), tr.insetCentre, 2))
                            return error = "character.json: " + path + " trim " + node + ": bad \"inset\"", false;
                    }
                    f.trim.emplace_back(node, tr);
                }
            if (const Json *ov = how.get("overlay"); ov && ov->kind == Json::Kind::Object)
                for (size_t oi = 0; oi < ov->keys.size(); ++oi)
                    f.overlay.emplace_back(ov->keys[oi], ov->items[oi].text);
            out.push_back(std::move(f));
        }
        return true;
    }

    bool mergeFile(const FileSpec &spec, const Bytes &game, const Bytes &nodesScr, const Bytes &modTm3,
                   const std::map<std::string, Image> &layers, Bytes &out, std::vector<Overlay> &overlays, std::string &error)
    {
        const DefaultFloatEnv floatEnv;
        auto fail = [&](const std::string &why) {
            error = spec.path + ": " + why;
            return false;
        };
        std::vector<Entry> ents;
        if (!readDat(game, ents))
            return fail("not a dat");
        // The painted textures (paint.overlay_textures over every TM3 of the game's file).
        std::vector<std::pair<std::string, Bytes>> painted; // name -> new TIM3, the last of a name wins
        auto paintedFind = [&](const std::string &name) -> Bytes * {
            for (auto &[n, t] : painted)
                if (n == name)
                    return &t;
            return nullptr;
        };
        if (!spec.empty && !spec.overlay.empty())
            for (const Entry &e : ents)
            {
                if (!entryIs(e, "TM3"))
                    continue;
                Textures texs;
                if (!readTm3(e.bytes, texs))
                    return fail("a TM3 that can't be read");
                for (const auto &[name, tim] : texs)
                {
                    const auto layer = layers.find(name);
                    if (layer == layers.end())
                        continue;
                    Image rgba;
                    if (!decode4(tim, rgba))
                        return fail("texture " + name + " is not a 4-bit TIM3");
                    if (rgba.width > 512 || rgba.height > 512)
                        return fail("texture " + name + " is larger than 512");
                    const Image on = overlay(rgba, layer->second);
                    std::vector<uint8_t> idx;
                    std::array<std::array<uint8_t, 4>, 16> pal{};
                    quantize16(on, idx, pal);
                    Bytes encoded = encode4(idx, pal, rgba.width, rgba.height);
                    if (Bytes *old = paintedFind(name))
                        *old = encoded;
                    else
                        painted.emplace_back(name, encoded);
                    bool known = false;
                    for (Overlay &o : overlays)
                        if (o.name == name)
                            o.gameTim3 = tim, o.newTim3 = encoded, o.layer = &layer->second, known = true;
                    if (!known)
                        overlays.push_back({name, tim, encoded, &layer->second});
                }
            }

        uint32_t modKind = 0;
        std::vector<ScrNode> modNodes;
        std::map<std::string, Mdb> mods;
        Textures modTex;
        if (!spec.empty)
        {
            if (!readScr(nodesScr, modKind, modNodes))
                return fail(spec.nodes + " is not a model");
            for (const ScrNode &nd : modNodes)
            {
                Mdb m;
                if (!readMdb(nodesScr, nd.mdbOff, m))
                    return fail(spec.nodes + ": node " + nd.name + " can't be read");
                mods[nd.name] = std::move(m);
            }
            if (!readTm3(modTm3, modTex))
                return fail(spec.textures + " is not a TM3");
        }
        const auto kept = [&](const std::string &name) { return std::find(spec.keep.begin(), spec.keep.end(), name) != spec.keep.end(); };
        const Trim *trimOf = nullptr;
        for (Entry &e : ents)
        {
            if (entryIs(e, "MD") && e.bytes.size() >= 3 && std::memcmp(e.bytes.data(), "scr", 3) == 0)
            {
                uint32_t kind = 0;
                std::vector<ScrNode> nodes;
                if (!readScr(e.bytes, kind, nodes))
                    return fail("a model that can't be read");
                const uint32_t anchor = nodes[0].nodeOff + nodes[0].unk4;
                Mdb m0;
                if (!readMdb(e.bytes, nodes[0].mdbOff, m0))
                    return fail("a model node that can't be read");
                const Bytes bones(e.bytes.begin() + m0.boneOff, e.bytes.begin() + m0.boneOff + 16 * m0.bones);
                std::vector<OutNode> outNodes;
                for (const ScrNode &nd : nodes)
                {
                    OutNode on;
                    on.game = &nd;
                    if (spec.empty)
                        on.mdb = emptyMdb(0);
                    else if (const auto it = mods.find(nd.name); it != mods.end())
                        on.mdb = it->second;
                    else if (kept(nd.name))
                    {
                        Mdb m;
                        if (!readMdb(e.bytes, nd.mdbOff, m))
                            return fail("node " + nd.name + " can't be read");
                        trimOf = nullptr;
                        for (const auto &[name, tr] : spec.trim)
                            if (name == nd.name)
                                trimOf = &tr;
                        if (trimOf)
                            m = trimMdb(m, *trimOf);
                        on.mdb = restripMdb(m);
                    }
                    else
                        on.mdb = emptyMdb(m0.bones > 4 ? 4 : 0);
                    outNodes.push_back(std::move(on));
                }
                Bytes scr;
                if (!writeScr(kind, outNodes, bones, anchor, scr))
                    return fail("the merged model can't be written");
                e.bytes = std::move(scr);
            }
            else if (!spec.empty && entryIs(e, "TM3") && (!modTex.empty() || !painted.empty()))
            {
                Textures texs;
                if (!readTm3(e.bytes, texs))
                    return fail("a TM3 that can't be read");
                auto find = [&](const std::string &name) {
                    for (size_t i = 0; i < texs.size(); ++i)
                        if (texs[i].first == name)
                            return static_cast<int>(i);
                    return -1;
                };
                for (const auto &[name, tim] : modTex)
                {
                    if (const int i = find(name); i >= 0)
                        texs[static_cast<size_t>(i)].second = tim;
                    else if (spec.appendTextures)
                        texs.emplace_back(name, tim);
                }
                for (const auto &[name, tim] : painted)
                    if (const int i = find(name); i >= 0)
                        texs[static_cast<size_t>(i)].second = tim;
                e.bytes = writeTm3(texs);
            }
        }
        out = writeDat(ents);
        return true;
    }

    bool hdImage(const Bytes &gameTim3, const Image *packBase, const Image &layer, Image &out)
    {
        const DefaultFloatEnv floatEnv;
        Image base;
        if (packBase && packBase->channels == 4 && packBase->width > 0 && packBase->height > 0)
            base = *packBase;
        else
        {
            Image small;
            if (!decode4(gameTim3, small))
                return false;
            base = resizeRgba(small, small.width * 4, small.height * 4);
        }
        const Image on = overlay(base, layer);
        out.width = on.width, out.height = on.height, out.channels = 3;
        out.px.resize(static_cast<size_t>(on.width) * on.height * 3);
        for (size_t i = 0, n = static_cast<size_t>(on.width) * on.height; i < n; ++i)
            std::memcpy(&out.px[i * 3], &on.px[i * 4], 3);
        return true;
    }

    bool readDds(const Bytes &b, Image &out)
    {
        if (!inside(b, 0, 128) || std::memcmp(b.data(), "DDS ", 4) != 0)
            return false;
        const uint32_t h = r32(b, 12), w = r32(b, 16);
        if (r32(b, 88) != 32 || w == 0 || h == 0 || w > 8192 || h > 8192 || !inside(b, 128, static_cast<size_t>(w) * h * 4))
            return false;
        out.width = static_cast<int>(w), out.height = static_cast<int>(h), out.channels = 4;
        out.px.resize(static_cast<size_t>(w) * h * 4);
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
        {
            const uint8_t *p = b.data() + 128 + i * 4;
            out.px[i * 4] = p[2], out.px[i * 4 + 1] = p[1], out.px[i * 4 + 2] = p[0], out.px[i * 4 + 3] = p[3];
        }
        return true;
    }

    bool findTexture(const Bytes &dat, const std::string &name, Bytes &tim3)
    {
        std::vector<Entry> ents;
        if (!readDat(dat, ents))
            return false;
        bool found = false;
        for (const Entry &e : ents)
        {
            Textures texs;
            if (entryIs(e, "TM3") && readTm3(e.bytes, texs))
                for (const auto &[n, t] : texs)
                    if (n == name)
                        tim3 = t, found = true;
        }
        return found;
    }

    bool tim3Payload128(const Bytes &tim3, const uint8_t *&payload, const uint8_t *&clut16)
    {
        Tim3 info;
        if (!tim3Info(tim3, info) || info.imageType != 4 || info.w != 128 || info.h != 128 || info.dataBytes < 0x2000 || info.clutBytes < 64)
            return false;
        payload = tim3.data() + info.dataAt;
        clut16 = tim3.data() + info.clutAt;
        return true;
    }
}
