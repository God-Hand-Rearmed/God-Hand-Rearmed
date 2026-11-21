#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// A disc image file read as the disc's 2048-byte sectors: a plain .iso, or a CHD (MAME's compressed image, libchdr:
// chdman createdvd, or createcd's 2448-byte frames with MODE1 / MODE2 data). Not thread-safe (PS2CdImage locks).
class PS2DiscImageFile
{
public:
    PS2DiscImageFile();
    ~PS2DiscImageFile();
    PS2DiscImageFile(const PS2DiscImageFile &) = delete;
    PS2DiscImageFile &operator=(const PS2DiscImageFile &) = delete;

    // False when the file can't be opened or holds no disc this reads; error says why.
    bool open(const std::filesystem::path &path, std::string &error);
    void close();
    bool isOpen() const;
    bool isChd() const;
    uint64_t sizeBytes() const; // the disc's data, 2048 bytes a sector (a .chd's file is smaller)
    // False past the end or when a compressed hunk can't be decoded.
    bool read(uint64_t offset, void *destination, size_t size);

    // .iso or .chd (the disc images a folder search picks up).
    static bool hasImageExtension(const std::filesystem::path &path);

private:
    struct Chd;
    std::FILE *m_file = nullptr;
    std::unique_ptr<Chd> m_chd;
    uint64_t m_size = 0;
};

class PS2CdImage
{
public:
    struct Entry
    {
        uint32_t lba = 0;
        uint64_t size = 0;
        bool directory = false;
    };

    static constexpr uint32_t kSectorSize = 2048u;

    static PS2CdImage &instance();

    bool open(const std::filesystem::path &imagePath);
    void close();
    bool isOpen() const;
    bool ensureOpenFromIoPaths();

    bool find(std::string_view ps2Path, Entry &out) const;
    // Names of the files (not directories) in the image's root directory, upper case.
    std::vector<std::string> rootFileNames() const;
    bool read(uint64_t offset, void *destination, size_t size) const;
    bool readSectors(uint32_t lba, uint32_t count, void *destination) const;
    uint64_t sizeBytes() const;
    std::filesystem::path path() const;

    static std::string normalizeKey(std::string_view ps2Path);

private:
    PS2CdImage() = default;
    bool parseDirectory(uint32_t lba, uint32_t size, const std::string &prefix, int depth);
    bool readUnlocked(uint64_t offset, void *destination, size_t size) const;

    mutable std::mutex m_mutex;
    mutable PS2DiscImageFile m_image;
    std::filesystem::path m_path;
    uint64_t m_size = 0;
    bool m_openAttempted = false;
    std::unordered_map<std::string, Entry> m_entries;
};

// Byte patches over the disc image, applied to every read of it (PS2CdImage, the EE sceCdRead stub, the IOP's image
// reads). Set once before the game starts (load-time patches of a file's own bytes, same size: menu buttons, body
// detail); empty = the disc as it is. The image never changes shape: a file replaced whole is served (PS2ServedFiles).
class PS2DiscOverlay
{
public:
    using Patch = std::pair<uint64_t, std::vector<uint8_t>>; // image byte offset, bytes

    static void set(std::vector<Patch> patches);
    // Adds patches to the ones already set (several sources: file replacements, text patches). Before the game runs.
    static void add(std::vector<Patch> patches);
    static bool active();
    // Overlays the patches on [offset, offset + size) of the image, already read into destination.
    static void apply(uint64_t offset, void *destination, size_t size);
};

// Files served past the end of the disc (whole-file replacements of any size; src/kamite/file_layer.cpp): each a run
// of whole sectors from an LBA no disc reaches (kFirstLba up, one free sector between files), read from a host file
// or from bytes in memory. Only the EE sceCdRead stub sees them, at those LBAs; the image, its directory and every
// table in it stay as they are. The game reaches them through the file layer's hooks. Before the game runs.
class PS2ServedFiles
{
public:
    static constexpr uint32_t kFirstLba = 0x00400000u; // 8 GB in: past any DVD-9

    // A part of a file built from pieces (an archive with some entries swapped): `size` bytes at `offset` in the
    // served file, from the disc image (PS2CdImage's bytes at `from`, the overlay included), a host file (from `from`
    // in it) or `bytes`. Pieces don't overlap; bytes no piece covers read as zero.
    struct Piece
    {
        enum class Source
        {
            Disc,
            Host,
            Bytes,
        };
        uint64_t offset = 0, size = 0;
        Source source = Source::Bytes;
        uint64_t from = 0;
        std::filesystem::path host;
        std::vector<uint8_t> bytes;
    };

    // The new file's first LBA; 0 when the file can't be read (or is empty).
    static uint32_t serve(const std::filesystem::path &hostFile);
    static uint32_t serve(std::vector<uint8_t> bytes);
    // A file of `size` bytes made of pieces (sorted by offset here).
    static uint32_t serve(std::vector<Piece> pieces, uint64_t size);
    static bool any();
    // Reads [lba, lba + sectors) into destination (size bytes) when lba lies in a served file: true, bytes past the
    // file's end zero. False: lba is not served (or the host file can't be read).
    static bool read(uint32_t lba, uint32_t sectors, void *destination, size_t size);
    static bool isServed(uint32_t lba);
};
