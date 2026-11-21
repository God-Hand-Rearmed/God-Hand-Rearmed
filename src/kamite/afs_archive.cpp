// AFS archives with entries swapped (see afs_archive.h).
#include "kamite/afs_archive.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace kamite::afs
{
    namespace
    {
        constexpr uint64_t kAlign = 0x800u;
        constexpr uint32_t kMaxEntries = 65536u;

        uint64_t align(uint64_t v) { return (v + kAlign - 1u) & ~(kAlign - 1u); }

        uint32_t le32(const std::vector<uint8_t> &b, size_t at)
        {
            return at + 4 > b.size() ? 0u : uint32_t(b[at]) | (uint32_t(b[at + 1]) << 8) | (uint32_t(b[at + 2]) << 16) | (uint32_t(b[at + 3]) << 24);
        }
        void put32(std::vector<uint8_t> &b, size_t at, uint64_t v)
        {
            for (int i = 0; i < 4; ++i)
                b[at + i] = static_cast<uint8_t>(v >> (8 * i));
        }

        std::string lower(std::string s)
        {
            for (char &c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        std::string withoutExtension(const std::string &name)
        {
            const size_t dot = name.find_last_of('.');
            return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
        }
    }

    bool read(const Reader &reader, uint64_t fileSize, Archive &out, std::string &error)
    {
        out = Archive{};
        out.size = fileSize;
        std::vector<uint8_t> head;
        if (fileSize < 16 || !reader(0, 8, head) || head.size() < 8 || std::memcmp(head.data(), "AFS\0", 4) != 0)
        {
            error = "not an AFS archive";
            return false;
        }
        const uint32_t count = le32(head, 4);
        if (count == 0 || count > kMaxEntries || 16u + 8ull * count > fileSize)
        {
            error = "its table is damaged";
            return false;
        }
        std::vector<uint8_t> table;
        if (!reader(0, 16u + 8u * count, table))
        {
            error = "its table can't be read";
            return false;
        }
        uint64_t first = fileSize;
        out.entries.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            Entry &e = out.entries[i];
            e.offset = le32(table, 8 + 8u * i);
            e.size = le32(table, 12 + 8u * i);
            if (e.offset + e.size > fileSize)
            {
                error = "entry " + std::to_string(i) + " runs past its end";
                return false;
            }
            if (e.size != 0 || e.offset != 0)
                first = std::min(first, e.offset);
        }
        if (first < 16u + 8ull * count || first > 0x1000000u)
        {
            error = "its first entry lies inside its table";
            return false;
        }
        if (!reader(0, static_cast<uint32_t>(first), out.header) || out.header.size() < first)
        {
            error = "its header can't be read";
            return false;
        }
        // The name table's pointer: right after the entries (the game's archives), else at the header's end.
        for (const uint64_t at : {uint64_t{8u + 8ull * count}, uint64_t{first - 8u}})
        {
            const uint32_t offset = le32(out.header, static_cast<size_t>(at)), size = le32(out.header, static_cast<size_t>(at) + 4);
            if (offset != 0 && size != 0 && size % 48u == 0 && offset + static_cast<uint64_t>(size) <= fileSize && offset >= first)
            {
                out.namesPointerAt = static_cast<uint32_t>(at);
                out.namesOffset = offset;
                out.namesSize = size;
                break;
            }
        }
        if (out.namesPointerAt != 0 && reader(out.namesOffset, out.namesSize, out.names) && out.names.size() >= out.namesSize)
        {
            for (uint32_t i = 0; i < count && 48u * (i + 1) <= out.namesSize; ++i)
            {
                const char *name = reinterpret_cast<const char *>(out.names.data() + 48u * i);
                out.entries[i].name.assign(name, strnlen(name, 32));
            }
        }
        else
        {
            out.namesPointerAt = 0;
            out.namesOffset = 0;
            out.namesSize = 0;
            out.names.clear();
        }
        return true;
    }

    int find(const Archive &archive, const std::string &wantedIn)
    {
        const std::string wanted = lower(wantedIn);
        for (size_t i = 0; i < archive.entries.size(); ++i)
        {
            const std::string name = lower(archive.entries[i].name);
            if (!name.empty() && (name == wanted || withoutExtension(name) == wanted))
                return static_cast<int>(i);
        }
        if (wanted.empty() || wanted.size() > 9 || !std::all_of(wanted.begin(), wanted.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return -1;
        const unsigned long index = std::stoul(wanted);
        return index < archive.entries.size() ? static_cast<int>(index) : -1;
    }

    Layout layout(const Archive &archive, const std::map<int, uint64_t> &newSizes)
    {
        Layout out;
        out.header = archive.header;
        uint64_t at = archive.header.size();
        for (size_t i = 0; i < archive.entries.size(); ++i)
        {
            const Entry &e = archive.entries[i];
            const auto it = newSizes.find(static_cast<int>(i));
            Part part;
            part.entry = static_cast<int>(i);
            part.replaced = it != newSizes.end();
            part.size = part.replaced ? it->second : e.size;
            // An empty slot (offset and size 0) stays one: nothing to place.
            if (!part.replaced && e.size == 0 && e.offset == 0)
                continue;
            part.offset = at;
            put32(out.header, 8 + 8 * i, part.offset);
            put32(out.header, 12 + 8 * i, part.size);
            out.parts.push_back(part);
            at = align(part.offset + part.size);
        }
        if (archive.namesPointerAt != 0)
        {
            out.namesOffset = at;
            put32(out.header, archive.namesPointerAt, out.namesOffset);
            put32(out.header, archive.namesPointerAt + 4, archive.namesSize);
            at = align(out.namesOffset + archive.namesSize);
        }
        out.size = at;
        return out;
    }
}
