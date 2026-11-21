// Recorded footprints (native_function.h Recording): the shadow harness's fill-on-first-touch for natives that walk
// guest data (rung 3, claude/native-next.md). Dev replays only use it, but it is plain code with no runtime calls, so
// it builds everywhere (ShadowEnv is instantiated in every build).

#include "kamite/native/native_function.h"

#include <cstring>

namespace kamite::native
{
    namespace
    {
        // One bit per byte of the RAM backing: present in the current recording. Allocated on first use (16 MB of
        // zero pages, touched only where footprints land); every run unmarks what it marked.
        uint8_t *bits()
        {
            static std::vector<uint8_t> map;
            if (map.empty())
                map.resize((PS2_RAM_MASK + 1u) / 8u);
            return map.data();
        }
        bool present(const uint8_t *map, uint32_t at) { return (map[at >> 3] >> (at & 7u)) & 1u; }
        void setRange(uint8_t *map, uint32_t begin, uint32_t size, bool on)
        {
            for (uint32_t at = begin, end = begin + size; at < end; ++at)
            {
                if ((at & 7u) == 0u && end - at >= 8u)
                {
                    map[at >> 3] = on ? 0xFFu : 0u;
                    at += 7u;
                    continue;
                }
                if (on)
                    map[at >> 3] = static_cast<uint8_t>(map[at >> 3] | (1u << (at & 7u)));
                else
                    map[at >> 3] = static_cast<uint8_t>(map[at >> 3] & ~(1u << (at & 7u)));
            }
        }
    }

    void Recording::touch(uint8_t *scratch, uint32_t offset, uint32_t size)
    {
        uint8_t *const map = bits();
        uint32_t at = offset;
        const uint32_t end = offset + size;
        while (at < end)
        {
            if (present(map, at))
            {
                ++at;
                continue;
            }
            uint32_t run = at + 1u;
            while (run < end && !present(map, run))
                ++run;
            const uint32_t length = run - at;
            setRange(map, at, length, true);
            if (real)
            {
                std::memcpy(scratch + at, real + at, length);
                in.insert(in.end(), real + at, real + run);
                if (!ranges.empty() && ranges.back().address + ranges.back().size == at)
                    ranges.back().size += length;
                else
                    ranges.push_back(Footprint::Range{at, length});
                bytes += length;
                if (ranges.size() > kMaxRecordedRanges || bytes > (maxBytes ? maxBytes : 64u * 1024u))
                    overflow = true;
            }
            else
            {
                // A rerun: the scratch holds zeros there; remember the range so it gets unmarked and zeroed.
                strayed = true;
                ranges.push_back(Footprint::Range{at, length});
            }
            at = run;
        }
    }

    void Recording::mark() const
    {
        uint8_t *const map = bits();
        for (const Footprint::Range &r : ranges)
            setRange(map, r.address, r.size, true);
    }

    void Recording::unmark() const
    {
        uint8_t *const map = bits();
        for (const Footprint::Range &r : ranges)
            setRange(map, r.address, r.size, false);
    }
}
