#pragma once

// The game's model containers, read and written byte for byte as tools/models/gamemodel.py does (dat_read /
// dat_build, tm3_read / tm3_write): a dat is (u32 count, u32 offsets[count] (0 = none), char type[4][count]), entries
// 32-byte aligned; a TM3 is ("TM3\0", count, 4, 0, offsets, padding to 8, 8-byte names), TIM3s 64-byte aligned. An
// entry or texture runs from its offset to the next larger offset of its container (or its end), padding included.
// Pure file layout, no runtime: olivia_player_build.cpp and character_build.cpp share it.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace kamite::model_files
{
    using Bytes = std::vector<uint8_t>;

    inline uint32_t r32(const Bytes &b, size_t o)
    {
        uint32_t v = 0;
        std::memcpy(&v, b.data() + o, 4);
        return v;
    }
    inline uint16_t r16(const Bytes &b, size_t o)
    {
        uint16_t v = 0;
        std::memcpy(&v, b.data() + o, 2);
        return v;
    }
    inline float rf(const Bytes &b, size_t o)
    {
        float v = 0.0f;
        std::memcpy(&v, b.data() + o, 4);
        return v;
    }
    inline void put32(Bytes &b, size_t o, uint32_t v) { std::memcpy(b.data() + o, &v, 4); }
    inline void put16(Bytes &b, size_t o, uint16_t v) { std::memcpy(b.data() + o, &v, 2); }
    inline void putf(Bytes &b, size_t o, float v) { std::memcpy(b.data() + o, &v, 4); }
    inline void pad(Bytes &b, size_t align)
    {
        while (b.size() % align)
            b.push_back(0);
    }
    inline bool inside(const Bytes &b, size_t at, size_t bytes) { return at <= b.size() && bytes <= b.size() - at; }
    // A fixed-size name field as gamemodel.py reads it (trailing zero bytes stripped).
    inline std::string fieldName(const uint8_t *p, size_t size)
    {
        while (size && p[size - 1] == 0)
            --size;
        return std::string(reinterpret_cast<const char *>(p), size);
    }

    // ---- dat ------------------------------------------------------------------------------------------------------------
    struct Entry
    {
        char type[4] = {};
        Bytes bytes;
        bool present = false;
    };
    inline bool readDat(const Bytes &dat, std::vector<Entry> &out)
    {
        if (dat.size() < 4)
            return false;
        const uint32_t n = r32(dat, 0);
        if (n == 0 || n > 4096 || !inside(dat, 4, 8u * n))
            return false;
        std::vector<uint32_t> starts;
        for (uint32_t i = 0; i < n; ++i)
            if (const uint32_t o = r32(dat, 4 + 4 * i); o)
                starts.push_back(o);
        std::sort(starts.begin(), starts.end());
        out.assign(n, Entry{});
        for (uint32_t i = 0; i < n; ++i)
        {
            Entry &e = out[i];
            std::memcpy(e.type, dat.data() + 4 + 4 * n + 4 * i, 4);
            const uint32_t at = r32(dat, 4 + 4 * i);
            if (at == 0)
                continue;
            if (at >= dat.size())
                return false;
            const auto next = std::upper_bound(starts.begin(), starts.end(), at);
            const uint32_t end = next == starts.end() ? static_cast<uint32_t>(dat.size()) : *next;
            e.bytes.assign(dat.begin() + at, dat.begin() + end);
            e.present = true;
        }
        return true;
    }
    inline Bytes writeDat(const std::vector<Entry> &entries)
    {
        const uint32_t n = static_cast<uint32_t>(entries.size());
        Bytes o(4 + 8u * n, 0);
        put32(o, 0, n);
        for (uint32_t i = 0; i < n; ++i)
            std::memcpy(o.data() + 4 + 4 * n + 4 * i, entries[i].type, 4);
        for (uint32_t i = 0; i < n; ++i)
        {
            if (!entries[i].present)
                continue;
            pad(o, 0x20);
            put32(o, 4 + 4 * i, static_cast<uint32_t>(o.size()));
            o.insert(o.end(), entries[i].bytes.begin(), entries[i].bytes.end());
        }
        pad(o, 0x20);
        return o;
    }
    // The entry's type as gamemodel.py compares it ("MD\0\0" is "MD").
    inline bool entryIs(const Entry &e, const char *type)
    {
        return e.present && fieldName(reinterpret_cast<const uint8_t *>(e.type), 4) == type;
    }

    // ---- TM3 ------------------------------------------------------------------------------------------------------------
    using Textures = std::vector<std::pair<std::string, Bytes>>;
    inline bool readTm3(const Bytes &t, Textures &out)
    {
        if (!inside(t, 0, 16) || std::memcmp(t.data(), "TM3", 3) != 0)
            return false;
        const uint32_t n = r32(t, 4);
        const size_t names = 16 + 4u * n + ((n & 1) ? 4 : 0);
        if (n == 0 || n > 1024 || !inside(t, names, 8u * n))
            return false;
        for (uint32_t i = 0; i < n; ++i)
        {
            const uint32_t at = r32(t, 16 + 4 * i);
            uint32_t end = static_cast<uint32_t>(t.size());
            for (uint32_t k = 0; k < n; ++k)
                if (const uint32_t o = r32(t, 16 + 4 * k); o > at && o < end)
                    end = o;
            if (at >= end)
                return false;
            out.emplace_back(fieldName(t.data() + names + 8 * i, 8), Bytes(t.begin() + at, t.begin() + end));
        }
        return true;
    }
    inline Bytes writeTm3(const Textures &texs)
    {
        const uint32_t n = static_cast<uint32_t>(texs.size());
        Bytes o{'T', 'M', '3', 0};
        o.resize(16, 0);
        put32(o, 4, n);
        put32(o, 8, 4);
        o.resize(16 + 4u * n + ((n & 1) ? 4 : 0), 0);
        for (const auto &[name, tim] : texs)
        {
            const size_t at = o.size();
            o.resize(at + 8, 0);
            std::memcpy(o.data() + at, name.data(), std::min<size_t>(8, name.size()));
        }
        for (uint32_t i = 0; i < n; ++i)
        {
            pad(o, 64);
            put32(o, 16 + 4 * i, static_cast<uint32_t>(o.size()));
            o.insert(o.end(), texs[i].second.begin(), texs[i].second.end());
        }
        pad(o, 64);
        return o;
    }
}
