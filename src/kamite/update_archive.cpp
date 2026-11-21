// Release downloads and the tar.gz (update_archive.h).
#include "kamite/update_archive.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string_view>

// raylib's own inflate (rcore.c builds sinfl.h with SUPPORT_COMPRESSION_API): the zip's and the gzip's deflate streams.
extern "C" int sinflate(void *out, int cap, const void *in, int size);

namespace kamite::launcher::update::archive
{
    namespace
    {
        std::string lower(std::string s)
        {
            for (char &c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        bool endsWith(const std::string &text, std::string_view suffix)
        {
            return text.size() > suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        // A tar number: octal digits (spaces or NULs around them), or GNU's base-256 (first byte's high bit set) for
        // sizes past 8 GB. False when it is neither.
        bool tarNumber(const uint8_t *field, size_t width, uint64_t &value)
        {
            value = 0;
            if (field[0] & 0x80u)
            {
                if (field[0] != 0x80u) // a negative base-256 number
                    return false;
                for (size_t i = 1; i < width; ++i)
                {
                    if (value >> 56)
                        return false;
                    value = (value << 8) | field[i];
                }
                return true;
            }
            size_t i = 0;
            while (i < width && (field[i] == ' ' || field[i] == 0))
                ++i;
            bool digits = false;
            for (; i < width && field[i] >= '0' && field[i] <= '7'; ++i)
            {
                value = (value << 3) | static_cast<uint64_t>(field[i] - '0');
                digits = true;
            }
            for (; i < width; ++i)
                if (field[i] != ' ' && field[i] != 0)
                    return false;
            return digits;
        }

        std::string cString(const uint8_t *field, size_t width)
        {
            size_t n = 0;
            while (n < width && field[n] != 0)
                ++n;
            return std::string(reinterpret_cast<const char *>(field), n);
        }

        // "path" from a pax extended header ("<length> path=<value>\n" records); "" when it has none.
        std::string paxPath(const uint8_t *data, size_t size)
        {
            size_t at = 0;
            std::string path;
            while (at < size)
            {
                size_t length = 0, p = at;
                while (p < size && data[p] >= '0' && data[p] <= '9')
                    length = length * 10 + static_cast<size_t>(data[p++] - '0');
                if (length == 0 || p >= size || data[p] != ' ' || at + length > size)
                    break;
                const std::string record(reinterpret_cast<const char *>(data) + p + 1, at + length - p - 1);
                if (record.rfind("path=", 0) == 0)
                {
                    path = record.substr(5);
                    if (!path.empty() && path.back() == '\n')
                        path.pop_back();
                }
                at += length;
            }
            return path;
        }

        // Nothing may leave the game's folder: no absolute path, no "..". Also cleans "./" parts and doubled slashes.
        bool safeName(std::string &name)
        {
            if (name.empty() || name[0] == '/' || name.find('\\') != std::string::npos)
                return false;
            std::string clean;
            size_t at = 0;
            while (at <= name.size())
            {
                const size_t slash = std::min(name.find('/', at), name.size());
                const std::string part = name.substr(at, slash - at);
                at = slash + 1;
                if (part.empty() || part == ".")
                    continue;
                if (part == "..")
                    return false;
                clean += (clean.empty() ? "" : "/") + part;
            }
            name = clean;
            return true;
        }
    }

    Package current()
    {
#if defined(_WIN32)
        return Package::WindowsZip;
#else
        std::error_code error;
        const std::filesystem::path link = std::filesystem::read_symlink("/proc/self/exe", error);
        const std::string exe = error ? std::string() : link.string();
        const bool flatpakInfo = std::filesystem::exists("/.flatpak-info", error);
        return linuxPackage(std::getenv("APPIMAGE"), flatpakInfo, exe);
#endif
    }

    Package linuxPackage(const char *appImage, bool flatpakInfo, const std::string &exe)
    {
        if (flatpakInfo && exe.rfind("/app/", 0) == 0)
            return Package::LinuxFlatpak;
        if (appImage && *appImage)
            return Package::LinuxAppImage;
        return Package::LinuxFolder;
    }

    const char *name(Package package)
    {
        switch (package)
        {
        case Package::WindowsZip:
            return "zip";
        case Package::LinuxFolder:
            return "tar.gz";
        case Package::LinuxAppImage:
            return "AppImage";
        case Package::LinuxFlatpak:
            return "Flatpak";
        }
        return "?";
    }

    int pickAsset(const std::vector<std::string> &assetNames, Package package)
    {
        for (size_t i = 0; i < assetNames.size(); ++i)
        {
            const std::string n = lower(assetNames[i]);
            bool fits = false;
            switch (package)
            {
            case Package::WindowsZip:
                // The Windows zip: any .zip, as before, except one a Linux build might ship.
                fits = endsWith(n, ".zip") && n.find("linux") == std::string::npos && n.find("appimage") == std::string::npos;
                break;
            case Package::LinuxFolder:
                fits = endsWith(n, "-linux-x64.tar.gz");
                break;
            case Package::LinuxAppImage:
                fits = endsWith(n, "-linux-x64.appimage");
                break;
            case Package::LinuxFlatpak:
                fits = endsWith(n, "-linux-x64.flatpak");
                break;
            }
            if (fits)
                return static_cast<int>(i);
        }
        return -1;
    }

    uint32_t crc32(const uint8_t *data, size_t size)
    {
        static const auto table = []
        {
            struct Table
            {
                uint32_t v[256];
            } t{};
            for (uint32_t n = 0; n < 256; ++n)
            {
                uint32_t c = n;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t.v[n] = c;
            }
            return t;
        }();
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t i = 0; i < size; ++i)
            crc = table.v[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
        return ~crc;
    }

    bool gunzip(const std::vector<uint8_t> &gz, std::vector<uint8_t> &out, size_t maxBytes, std::string &error)
    {
        out.clear();
        error = "the download is not a valid .tar.gz";
        // RFC 1952: ID1 ID2 CM FLG MTIME(4) XFL OS, the optional fields FLG names, the deflate data, CRC32, ISIZE.
        if (gz.size() < 18 || gz[0] != 0x1F || gz[1] != 0x8B || gz[2] != 8 || (gz[3] & 0xE0u) != 0)
            return false;
        const uint8_t flags = gz[3];
        size_t at = 10;
        if (flags & 0x04u) // FEXTRA
        {
            if (at + 2 > gz.size())
                return false;
            at += 2 + static_cast<size_t>(gz[at] | (gz[at + 1] << 8));
        }
        for (const uint8_t text : {uint8_t{0x08u}, uint8_t{0x10u}}) // FNAME, FCOMMENT: zero-terminated
            if (flags & text)
            {
                while (at < gz.size() && gz[at] != 0)
                    ++at;
                ++at;
            }
        if (flags & 0x02u) // FHCRC
            at += 2;
        if (at + 8 > gz.size())
            return false;
        const uint8_t *trailer = &gz[gz.size() - 8];
        const uint32_t crc = static_cast<uint32_t>(trailer[0] | (trailer[1] << 8) | (trailer[2] << 16)) | (static_cast<uint32_t>(trailer[3]) << 24);
        const uint32_t size = static_cast<uint32_t>(trailer[4] | (trailer[5] << 8) | (trailer[6] << 16)) | (static_cast<uint32_t>(trailer[7]) << 24);
        const size_t packed = gz.size() - 8 - at;
        // sinflate counts in int; ISIZE is the size modulo 4 GB, so anything near that is refused rather than guessed.
        if (size > maxBytes || size > 0x7FFFFF00u || packed > 0x7FFFFFFFu)
        {
            error = "the update is too big";
            return false;
        }
        out.assign(static_cast<size_t>(size) + 64u, 0u);
        const int got = sinflate(out.data(), static_cast<int>(out.size()), &gz[at], static_cast<int>(packed));
        if (got < 0 || static_cast<uint32_t>(got) != size)
        {
            out.clear();
            error = "the update is damaged (it unpacks to the wrong size)";
            return false;
        }
        out.resize(size);
        if (crc32(out.data(), out.size()) != crc)
        {
            out.clear();
            error = "the update is damaged (checksum)";
            return false;
        }
        error.clear();
        return true;
    }

    bool readTar(const std::vector<uint8_t> &tar, std::vector<TarEntry> &entries, size_t &skipped, std::string &error)
    {
        entries.clear();
        skipped = 0;
        error = "the update's tar is damaged";
        std::string longName; // GNU 'L' or pax 'x': the next entry's name
        size_t at = 0;
        bool ended = false;
        while (at + 512 <= tar.size())
        {
            const uint8_t *h = &tar[at];
            if (std::all_of(h, h + 512, [](uint8_t b) { return b == 0; }))
            {
                ended = true; // the end: one (or the usual two) zero blocks
                break;
            }
            uint64_t sum = 0, stored = 0;
            for (size_t i = 0; i < 512; ++i)
                sum += (i >= 148 && i < 156) ? ' ' : h[i];
            uint64_t size = 0;
            if (!tarNumber(h + 148, 8, stored) || stored != sum || !tarNumber(h + 124, 12, size))
                return false;
            const size_t data = at + 512;
            if (size > tar.size() || data + size > tar.size())
                return false;
            const char type = static_cast<char>(h[156]);
            at = data + ((static_cast<size_t>(size) + 511u) & ~static_cast<size_t>(511u));
            if (type == 'L')
            {
                longName = cString(&tar[data], static_cast<size_t>(size));
                continue;
            }
            if (type == 'x')
            {
                longName = paxPath(&tar[data], static_cast<size_t>(size));
                continue;
            }
            if (type == 'g' || type == 'K') // pax global header, GNU long link name: nothing GHR needs
                continue;
            std::string entryName = longName;
            longName.clear();
            if (entryName.empty())
            {
                entryName = cString(h, 100);
                if (std::memcmp(h + 257, "ustar", 5) == 0 && h[345] != 0) // ustar's prefix field
                    entryName = cString(h + 345, 155) + "/" + entryName;
            }
            const bool folder = type == '5';
            if (!(folder || type == '0' || type == '\0' || type == '7')) // 7: contiguous file, a regular file
            {
                ++skipped;
                continue;
            }
            std::string clean = entryName;
            if (!safeName(clean))
            {
                error = "the update names a file outside the game's folder (" + entryName + ")";
                return false;
            }
            if (clean.empty())
                continue; // "./" itself
            uint64_t mode = 0;
            tarNumber(h + 100, 8, mode);
            TarEntry e;
            e.name = clean;
            e.folder = folder;
            e.executable = !folder && (mode & 0111u) != 0;
            e.data = data;
            e.size = folder ? 0 : static_cast<size_t>(size);
            entries.push_back(std::move(e));
        }
        if (!ended && at != tar.size())
            return false; // a header cut off
        // One top folder for everything ("God Hand Rearmed/..."): stripped, as the zip's is.
        std::string top;
        bool shared = !entries.empty();
        for (const TarEntry &e : entries)
        {
            const size_t slash = e.name.find('/');
            const std::string first = slash == std::string::npos ? (e.folder ? e.name : std::string()) : e.name.substr(0, slash);
            if (first.empty() || (!top.empty() && first != top))
            {
                shared = false;
                break;
            }
            top = first;
        }
        if (shared)
        {
            for (TarEntry &e : entries)
                e.name = e.name.size() > top.size() ? e.name.substr(top.size() + 1) : std::string();
            entries.erase(std::remove_if(entries.begin(), entries.end(), [](const TarEntry &e) { return e.name.empty(); }), entries.end());
        }
        error.clear();
        return true;
    }
}
