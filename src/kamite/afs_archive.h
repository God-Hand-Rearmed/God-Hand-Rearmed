#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

// An AFS archive inside the game's files (bgm/bgm00.afs, se/*.afs, se/ev/*.afs) with some entries swapped, for the
// mods' single entries (claude/mod-support.md "Step 2"). The format, as the game's archives have it: "AFS\0", u32
// count, count x (u32 offset, u32 size), then (u32 offset, u32 size) of the name table; entries one after another from
// the first offset, each on a 2048-byte boundary (CRI's partitions keep only the sizes and add them up: an entry that
// grows moves every later one); the name table after the last entry, 48 bytes an entry (name[32], six u16 date and time,
// u32). The swapped archive keeps the header's bytes, the order and the names; only the offsets and sizes change.
// Plain std: the boot and the host tests.
namespace kamite::afs
{
    struct Entry
    {
        uint64_t offset = 0;
        uint32_t size = 0;
        std::string name; // "" when the archive has no name table
    };

    struct Archive
    {
        std::vector<Entry> entries;
        std::vector<uint8_t> header; // [0, the first entry's offset)
        uint32_t namesPointerAt = 0; // where the header keeps the name table's offset and size (0: none)
        uint64_t namesOffset = 0;
        uint32_t namesSize = 0;
        std::vector<uint8_t> names;
        uint64_t size = 0; // the archive file's size
    };

    // Reads size bytes at offset of the archive file into out.
    using Reader = std::function<bool(uint64_t offset, uint32_t size, std::vector<uint8_t> &out)>;
    bool read(const Reader &reader, uint64_t fileSize, Archive &out, std::string &error);

    // The entry a mod's file stands for: its name without the extension = an entry's stored name (any case, with or
    // without its extension), else a whole number = the entry's position (0 = the first). -1 when neither.
    int find(const Archive &archive, const std::string &wanted);

    // The swapped archive: newSizes maps an entry's index to its replacement's size.
    struct Part
    {
        uint64_t offset = 0, size = 0; // in the new archive
        int entry = -1;
        bool replaced = false; // from the replacement, else the original entry's bytes
    };
    struct Layout
    {
        std::vector<uint8_t> header; // [0, header size): the table updated
        std::vector<Part> parts;
        uint64_t namesOffset = 0;    // the name table's bytes (Archive::names) go there
        uint64_t size = 0;
    };
    Layout layout(const Archive &archive, const std::map<int, uint64_t> &newSizes);
}
