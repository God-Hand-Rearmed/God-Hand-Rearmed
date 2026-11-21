// The file layer: game files replaced whole, served at the read (see file_layer.h).
#include "kamite/file_layer.h"
#include "kamite/disc_files.h"
#include "kamite/function_hooks.h"
#include "kamite/log.h"
#include "kamite/xram_heap.h"
#include "runtime/ps2_cd_image.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_map>

namespace fs = std::filesystem;

namespace kamite::file_layer
{
    namespace
    {
        constexpr uint32_t kFnameRange = 0x00324640u;  // ADXF_GetFnameRangeEx
        constexpr uint32_t kStreamBind = 0x00327E88u;  // ADXSTM's bind (the fields; under ADXSTM_BindFile / Nw)
        constexpr uint32_t kFstSize = 0x001FFD98u;     // BuildPackedAddr_1FFD98: a path table entry's size
        constexpr uint32_t kReadToBuffer = 0x00200B88u; // cDvd_ReadToBuffer
        constexpr uint32_t kLoadEntry = 0x001FFD00u;    // LoadEntryByValue_1FFD00 (FileBuf_init's fonts)
        constexpr uint32_t kMemoryAlloc = 0x0030EE08u;  // _MemoryAlloc(a0 size, a1 alignment, a2 cHeap *)
        // _MemoryAlloc's callers whose buffer holds one file: cDvd_ReadAlloc (jal 0x200D88) and ReadAllocSized
        // (0x200EC0) keep their job in s0 (+8 its archive index); FileBuf_init (0x2E1378) allocates 4 KB for a font.
        constexpr uint32_t kReadAllocRa = 0x00200D90u, kReadAllocSizedRa = 0x00200EC8u, kFontBufferRa = 0x002E1380u;
        // The DLL slot heaps (function_hooks.cpp): a room DLL's read stays where that redirect put it.
        constexpr uint32_t kSlotDescBegin = 0x00798000u, kSlotDescEnd = 0x00798030u;
        // Buffers made once that a read fills (cDvd_ReadToBuffer from these returns), and where the game keeps them:
        // the player's model and motion files (func_0030D8E0; heap A blocks of 0x1E6800 / 0x19800 from func_0030D5C8).
        struct FixedBuffer
        {
            uint32_t ra, pointer;
        };
        constexpr FixedBuffer kFixedBuffers[] = {{0x0030D994u, 0x00752C38u}, {0x0030D9DCu, 0x00752C3Cu}};
        constexpr uint32_t kFstTable = 0x00580D40u;    // the path table (func_001FF858)
        constexpr uint32_t kDvdRomName = 0x004545B0u;  // "DVD-ROM" in DVCI's data: the whole disc
        constexpr uint32_t kAfsPartition = 0u;         // _DATA.AFS's CRI partition
        constexpr uint32_t kPartitionTable = 0x003EB308u; // CRI's partition pointers by id (+0x10 the AFS file's name)

        std::map<std::string, Replaced> g_replaced;          // disc path -> replacement
        std::unordered_map<uint32_t, Replaced> g_byIndex;    // archive index -> replacement
        PS2Runtime *g_runtime = nullptr;
        bool g_hooked = false;
        GuestFunction g_origFnameRange = nullptr, g_origStreamBind = nullptr, g_origMemoryAlloc = nullptr;

        std::string lower(std::string_view s)
        {
            std::string out(s);
            for (char &c : out)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }

        std::string utf8(const fs::path &p)
        {
            const std::u8string s = p.generic_u8string();
            return std::string(s.begin(), s.end());
        }

        uint32_t r32(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0;
            std::memcpy(&v, rdram + (a & (PS2_RAM_MASK & ~3u)), 4);
            return v;
        }
        void w32(uint8_t *rdram, uint32_t a, uint32_t v)
        {
            if (a != 0u)
                std::memcpy(rdram + (a & (PS2_RAM_MASK & ~3u)), &v, 4);
        }
        // A string in guest memory, RAM or the scratchpad (some of the game's threads keep their stacks there).
        std::string guestString(const uint8_t *rdram, uint32_t a, size_t max = 260)
        {
            std::string s;
            for (uint32_t i = 0; s.size() < max; ++i)
            {
                const uint8_t *c = getConstMemPtr(rdram, a + i);
                if (!c || *c == 0u)
                    break;
                s.push_back(static_cast<char>(*c));
            }
            return s;
        }

        // Partition 0's file name, as CRI hands it out: "_DATA.AFS" (a device, folder or ";1" around it allowed).
        bool isDataAfs(const std::string &name)
        {
            std::string leaf = lower(name);
            if (const size_t semi = leaf.find(';'); semi != std::string::npos)
                leaf.resize(semi);
            if (const size_t cut = leaf.find_last_of("/\\:"); cut != std::string::npos)
                leaf.erase(0, cut + 1);
            return leaf == "_data.afs";
        }

        // ADXF_GetFnameRangeEx: a replaced file of _DATA.AFS lies where it is served.
        void onFnameRange(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t partition = GPR_U32(ctx, 4), index = GPR_U32(ctx, 5);
            const uint32_t first = GPR_U32(ctx, 8), sectors = GPR_U32(ctx, 9), bytes = GPR_U32(ctx, 10);
            g_origFnameRange(rdram, ctx, runtime);
            if (partition != kAfsPartition || static_cast<int32_t>(GPR_U32(ctx, 2)) < 0)
                return;
            const auto it = g_byIndex.find(index);
            if (it == g_byIndex.end())
                return;
            // The partition's own name (its fname out can be a buffer guest RAM doesn't hold, e.g. on the scratchpad).
            if (const std::string afs = guestString(rdram, r32(rdram, kPartitionTable) + 0x10u); !isDataAfs(afs))
            {
                static bool told = false;
                if (!told)
                    KM_WARN(File, "file layer: partition 0 is \"" << afs << "\", not _DATA.AFS; replacements not served");
                told = true;
                return;
            }
            w32(rdram, first, it->second.lba);
            w32(rdram, sectors, (it->second.size + PS2CdImage::kSectorSize - 1u) / PS2CdImage::kSectorSize);
            w32(rdram, bytes, it->second.size);
            static std::unordered_map<uint32_t, bool> told; // the first read of each, so a test shows what the game loads
            if (!told[index])
            {
                told[index] = true;
                for (const auto &[path, r] : g_replaced)
                    if (r.index == index)
                        KM_INFO(File, "file layer: first read of " << path);
            }
        }

        // ADXSTM's bind: a range in a served file is read from the whole disc ("DVD-ROM": LBA = the range's sector).
        void onStreamBind(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (PS2ServedFiles::isServed(GPR_U32(ctx, 7)))
            {
                SET_GPR_U32(ctx, 5, kDvdRomName);
                SET_GPR_U32(ctx, 6, 0u);
            }
            g_origStreamBind(rdram, ctx, runtime);
        }

        // The path table's size of a file (the original's arithmetic), the replacement's for a replaced one.
        void onFstSize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t table = GPR_U32(ctx, 4), entry = GPR_U32(ctx, 5) & PS2_RAM_MASK;
            uint32_t size = 0u;
            if (entry + 12u <= PS2_RDRAM_SIZE)
            {
                uint16_t low = 0;
                std::memcpy(&low, rdram + entry + 2u, 2);
                size = ((static_cast<uint32_t>(rdram[entry + 1u]) << 16) + low) << 4;
                if (table == kFstTable && rdram[entry] == 2u)
                    if (const auto it = g_byIndex.find(r32(rdram, entry + 8u)); it != g_byIndex.end())
                        size = (it->second.size + 15u) & ~15u;
            }
            SET_GPR_U32(ctx, 2, size);
            ctx->pc = GPR_U32(ctx, 31);
        }

        uint32_t sectorBytes(uint32_t size) { return (size + 0x7FFu) & ~0x7FFu; }

        // cDvd_ReadToBuffer(a0 cDvd, a1 path, a2 buffer, a3 id, t0 size or 0): a replaced file read whole into a buffer
        // the caller made, in whole sectors. A heap block's header sits 0x20 before its data (+0x10 used bytes). Too
        // small: a buffer the game keeps at a known place (kFixedBuffers) is swapped for an extension one big enough
        // (the pointer and the read follow it); any other read is cut (t0) so it cannot run past the block into the
        // next one, which hung the game (Oct 27, Gene's buffer from a save state made with a smaller file).
        void onReadToBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            std::string path;
            const Replaced *r = replacedPath(rdram, GPR_U32(ctx, 5), &path);
            if (!r)
                return;
            const uint32_t buffer = GPR_U32(ctx, 6), asked = GPR_U32(ctx, 8), ra = GPR_U32(ctx, 31);
            const uint32_t need = sectorBytes(r->size), room = xram_heap::blockRoom(rdram, buffer);
            if (asked != 0u)
            {
                if (asked < r->size)
                    KM_WARN(Game, "file layer: " << path << " read into a buffer with " << asked << " bytes asked; the replacement is "
                                                 << r->size << " (cut short)");
                return;
            }
            if (room == 0u || room >= need)
                return;
            for (const FixedBuffer &f : kFixedBuffers)
            {
                if (ra != f.ra || (r32(rdram, f.pointer) & PS2_RAM_MASK) != (buffer & PS2_RAM_MASK))
                    continue;
                const uint32_t header = (buffer & PS2_RAM_MASK) - 0x20u;
                const uint32_t heap = xram_heap::owns(buffer) ? r32(rdram, header + 0x14u) : r32(rdram, header);
                const uint32_t swapped = xram_heap::alloc(rdram, need, 0x80u, heap);
                if (swapped == 0u)
                    break;
                if (xram_heap::owns(buffer))
                    xram_heap::free(rdram, buffer);
                w32(rdram, f.pointer, swapped);
                SET_GPR_U32(ctx, 6, swapped);
                KM_INFO(Game, "file layer: " << path << " (" << r->size << " bytes) gets a buffer of " << need << " in the extension (0x"
                                             << std::hex << swapped << ", was 0x" << buffer << std::dec << ", " << room << " bytes)");
                return;
            }
            const uint32_t cut = room & ~0x7FFu;
            SET_GPR_U32(ctx, 8, cut);
            KM_ERROR(Game, "file layer: " << path << " (" << r->size << " bytes) does not fit the game's buffer of " << room
                                          << " bytes (ra 0x" << std::hex << ra << std::dec << "); read cut to " << cut
                                          << ". Was a save state made with another build or other mods? Restart the game.");
        }

        // _MemoryAlloc for a buffer that will hold one file: a replacement bigger than the game's file gets its block in
        // the extension heap, standing in for a block of the heap the game asked (freed with it, trimmed and freed by the
        // game's own code). The game's heaps keep the layout they have in vanilla.
        void onMemoryAlloc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t size = GPR_U32(ctx, 4), align = GPR_U32(ctx, 5), heap = GPR_U32(ctx, 6), ra = GPR_U32(ctx, 31);
            uint32_t need = 0u;
            std::string what;
            if ((ra == kReadAllocRa || ra == kReadAllocSizedRa) && !(heap >= kSlotDescBegin && heap < kSlotDescEnd))
            {
                const uint32_t job = GPR_U32(ctx, 16);
                if (const auto it = g_byIndex.find(r32(rdram, job + 8u)); it != g_byIndex.end() && it->second.size > it->second.gameSize)
                {
                    need = size; // the job's sectors: the replacement's, through the path table's size (onFstSize)
                    what = "AFS " + std::to_string(it->first);
                }
            }
            else if (ra == kFontBufferRa)
            {
                for (const char *font : {"etc/moji8.tm2", "etc/moji81.tm2"})
                    if (const auto it = g_replaced.find(font); it != g_replaced.end() && sectorBytes(it->second.size) > size)
                    {
                        need = std::max(need, sectorBytes(it->second.size));
                        what = "fonts";
                    }
            }
            if (need != 0u)
            {
                if (const uint32_t data = xram_heap::alloc(rdram, need, align, heap))
                {
                    KM_INFO(Game, "file layer: " << what << ": " << need << " bytes in the extension at 0x" << std::hex << data << " for heap 0x"
                                                 << heap << std::dec << " (" << size << " asked)");
                    SET_GPR_U32(ctx, 2, data);
                    ctx->pc = ra;
                    return;
                }
                KM_ERROR(Game, "file layer: " << what << ": no room for " << need << " bytes in the extension; the game's heap is asked");
            }
            g_origMemoryAlloc(rdram, ctx, runtime);
        }

        // LoadEntryByValue_1FFD00(a0 path table, a1 path, a2 buffer) reads the whole file (FileBuf_init: etc/moji8.tm2 or
        // moji81.tm2 into a 4 KB heap block, made big enough for a replaced font by onMemoryAlloc). It can't be cut; a
        // replacement larger than the block is reported.
        void onLoadEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            std::string path;
            const Replaced *r = replacedPath(rdram, GPR_U32(ctx, 5), &path);
            const uint32_t room = r ? xram_heap::blockRoom(rdram, GPR_U32(ctx, 6)) : 0u;
            if (room != 0u && room < sectorBytes(r->size))
                KM_ERROR(Game, "file layer: " << path << " (" << r->size << " bytes) is read whole into the game's buffer of " << room
                                              << " bytes; the read runs past it");
        }

        void hook()
        {
            if (g_hooked || !g_runtime || g_byIndex.empty())
                return;
            uint8_t *rdram = g_runtime->memory().getRDRAM();
            if (std::memcmp(rdram + kDvdRomName, "DVD-ROM", 8) != 0 || !g_runtime->hasFunction(kFnameRange) ||
                !g_runtime->hasFunction(kStreamBind) || !g_runtime->hasFunction(kFstSize) || !g_runtime->hasFunction(kMemoryAlloc) ||
                !(g_origFnameRange = g_runtime->lookupFunction(kFnameRange)) || !(g_origStreamBind = g_runtime->lookupFunction(kStreamBind)) ||
                !(g_origMemoryAlloc = g_runtime->lookupFunction(kMemoryAlloc)))
            {
                KM_ERROR(File, "file layer: this game's CRI file system is not the one expected; replaced files are not served");
                return;
            }
            FunctionHooks &hooks = FunctionHooks::instance();
            const bool ok = hooks.replace(kFnameRange, "ADXF_GetFnameRangeEx (file layer)", &onFnameRange, ReplacementStatus::Native,
                                          "replaced files lie where they are served") &&
                            hooks.replace(kStreamBind, "ADXSTM bind (file layer)", &onStreamBind, ReplacementStatus::Native,
                                          "a served file is read from the whole disc") &&
                            hooks.replace(kFstSize, "path table file size (file layer)", &onFstSize, ReplacementStatus::Native,
                                          "a replaced file's own size") &&
                            hooks.replace(kMemoryAlloc, "_MemoryAlloc (file layer)", &onMemoryAlloc, ReplacementStatus::Native,
                                          "a bigger replaced file's buffer from the extension heap");
            if (!ok)
            {
                KM_ERROR(File, "file layer: the hooks could not be installed; replaced files are not served");
                return;
            }
            hooks.observeEntry(kReadToBuffer, "file_layer_read_to_buffer", &onReadToBuffer);
            hooks.observeEntry(kLoadEntry, "file_layer_load_entry", &onLoadEntry);
            xram_heap::install(*g_runtime);
            g_hooked = true;
            KM_INFO(File, "file layer: " << g_byIndex.size() << " replaced file(s) served");
        }

        bool add(std::string_view path, uint32_t lba, uint32_t size, const std::string &from)
        {
            const auto file = disc_files::find(path);
            if (!file)
                return false;
            const std::string key = lower(path);
            if (lba == 0u)
            {
                KM_WARN(File, "file layer: " << key << ": " << from << " can't be read or is empty; the game's file stays");
                return false;
            }
            const Replaced r{file->index, file->size, size, lba};
            g_replaced[key] = r;
            g_byIndex[file->index] = r;
            KM_INFO(File, "file layer: " << key << " (AFS " << file->index << ", " << file->size << " -> " << size << " bytes) from " << from);
            hook();
            return true;
        }
    }

    bool replace(std::string_view path, const fs::path &looseFile)
    {
        if (!disc_files::open() || !disc_files::find(path))
            return false;
        std::error_code error;
        const uint64_t size = fs::file_size(looseFile, error);
        if (error || size > 0xFFFFFFFFull)
            return add(path, 0u, 0u, utf8(looseFile));
        return add(path, PS2ServedFiles::serve(looseFile), static_cast<uint32_t>(size), utf8(looseFile));
    }

    bool replace(std::string_view path, std::vector<uint8_t> bytes)
    {
        if (!disc_files::open() || !disc_files::find(path) || bytes.size() > 0xFFFFFFFFull)
            return false;
        const uint32_t size = static_cast<uint32_t>(bytes.size());
        return add(path, PS2ServedFiles::serve(std::move(bytes)), size, "memory");
    }

    bool replace(std::string_view path, std::vector<PS2ServedFiles::Piece> pieces, uint64_t size, const std::string &from)
    {
        if (!disc_files::open() || !disc_files::find(path) || size > 0xFFFFFFFFull)
            return false;
        return add(path, PS2ServedFiles::serve(std::move(pieces), size), static_cast<uint32_t>(size), from);
    }

    int replaceFolder(const fs::path &folder, const std::string &who)
    {
        if (!disc_files::open())
        {
            KM_WARN(File, who << ": the disc image has no file archive / path table; nothing replaced");
            return 0;
        }
        // Sorted, so the same files are served at the same LBAs on every run (a save state stays valid).
        std::vector<std::pair<std::string, fs::path>> files;
        std::error_code error;
        for (const auto &entry : fs::recursive_directory_iterator(folder, error))
            if (entry.is_regular_file(error))
            {
                const std::string rel = lower(utf8(fs::relative(entry.path(), folder, error)));
                if (disc_files::paths().count(rel))
                    files.emplace_back(rel, entry.path());
            }
        std::sort(files.begin(), files.end());
        int count = 0;
        for (const auto &[rel, file] : files)
            count += replace(rel, file) ? 1 : 0;
        if (count != 0)
            KM_INFO(File, who << ": " << count << " game file(s) replaced");
        return count;
    }

    const std::map<std::string, Replaced> &replaced() { return g_replaced; }

    const Replaced *replacedPath(const uint8_t *rdram, uint32_t pathAddress, std::string *pathOut)
    {
        if (g_replaced.empty())
            return nullptr;
        std::string path = lower(guestString(rdram, pathAddress, 128));
        for (char &c : path)
            if (c == '\\')
                c = '/';
        if (const size_t semi = path.find(';'); semi != std::string::npos)
            path.resize(semi);
        if (const size_t colon = path.find(':'); colon != std::string::npos)
            path.erase(0, colon + 1);
        while (!path.empty() && (path[0] == '/' || path[0] == '.'))
            path.erase(0, 1);
        // The full path, or (when the game names a folder the table doesn't) what follows any '/' in it.
        for (size_t from = 0;;)
        {
            if (const auto it = g_replaced.find(path.substr(from)); it != g_replaced.end())
            {
                if (pathOut)
                    *pathOut = it->first;
                return &it->second;
            }
            const size_t slash = path.find('/', from);
            if (slash == std::string::npos)
                return nullptr;
            from = slash + 1;
        }
    }

    void install(PS2Runtime &runtime)
    {
        g_runtime = &runtime;
        hook();
    }
}
