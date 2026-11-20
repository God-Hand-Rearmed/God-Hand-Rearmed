// Install from zip (see mod_zip.h).
#include "kamite/mod_zip.h"
#include "kamite/mod_list.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

// raylib's own inflate (rcore.c builds sinfl.h with SUPPORT_COMPRESSION_API): the zip's deflate streams (as the
// updater, launcher_update.cpp).
extern "C" int sinflate(void *out, int cap, const void *in, int size);

namespace fs = std::filesystem;

namespace kamite::mod_zip
{
    namespace
    {
        using mod_list::fromUtf8;
        using mod_list::lower;
        using mod_list::utf8;

        uint16_t u16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
        uint32_t u32(const uint8_t *p) { return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16)) | (static_cast<uint32_t>(p[3]) << 24); }
        uint64_t u64(const uint8_t *p) { return u32(p) | (static_cast<uint64_t>(u32(p + 4)) << 32); }

        uint32_t crc32(uint32_t crc, const uint8_t *data, size_t size)
        {
            static const std::array<uint32_t, 256> table = [] {
                std::array<uint32_t, 256> t{};
                for (uint32_t i = 0; i < 256; ++i)
                {
                    uint32_t c = i;
                    for (int k = 0; k < 8; ++k)
                        c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    t[i] = c;
                }
                return t;
            }();
            crc = ~crc;
            for (size_t i = 0; i < size; ++i)
                crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
            return ~crc;
        }

        struct Entry
        {
            std::string name; // '/' separated
            uint16_t method = 0, flags = 0;
            uint32_t crc = 0;
            uint64_t packed = 0, size = 0, local = 0;
        };

        bool readAt(std::ifstream &in, uint64_t at, void *out, size_t size)
        {
            in.clear();
            in.seekg(static_cast<std::streamoff>(at));
            in.read(static_cast<char *>(out), static_cast<std::streamsize>(size));
            return static_cast<size_t>(in.gcount()) == size;
        }

        // The central directory (ZIP64 too).
        bool readTable(std::ifstream &in, std::vector<Entry> &entries, std::string &error)
        {
            error = "not a zip file";
            in.seekg(0, std::ios::end);
            const uint64_t size = static_cast<uint64_t>(in.tellg());
            if (size < 22)
                return false;
            const uint64_t tail = std::min<uint64_t>(size, 22 + 0xFFFF + 20);
            std::vector<uint8_t> t(static_cast<size_t>(tail));
            if (!readAt(in, size - tail, t.data(), t.size()))
                return false;
            size_t end = t.size() - 22;
            while (u32(&t[end]) != 0x06054B50u)
            {
                if (end == 0)
                    return false;
                --end;
            }
            uint64_t count = u16(&t[end + 10]), tableSize = u32(&t[end + 12]), tableAt = u32(&t[end + 16]);
            if ((count == 0xFFFFu || tableAt == 0xFFFFFFFFu || tableSize == 0xFFFFFFFFu) && end >= 20 && u32(&t[end - 20]) == 0x07064B50u)
            {
                uint8_t z[56];
                if (!readAt(in, u64(&t[end - 20 + 8]), z, sizeof z) || u32(z) != 0x06064B50u)
                    return false;
                count = u64(z + 32);
                tableSize = u64(z + 40);
                tableAt = u64(z + 48);
            }
            if (tableAt + tableSize > size || tableSize > 256u * 1024u * 1024u)
                return false;
            std::vector<uint8_t> table(static_cast<size_t>(tableSize));
            if (!readAt(in, tableAt, table.data(), table.size()))
                return false;
            size_t at = 0;
            for (uint64_t i = 0; i < count; ++i)
            {
                if (at + 46 > table.size() || u32(&table[at]) != 0x02014B50u)
                    return false;
                const uint8_t *h = &table[at];
                Entry e;
                e.flags = u16(h + 8);
                e.method = u16(h + 10);
                e.crc = u32(h + 16);
                e.packed = u32(h + 20);
                e.size = u32(h + 24);
                e.local = u32(h + 42);
                const uint16_t nameLength = u16(h + 28), extra = u16(h + 30), comment = u16(h + 32);
                if (at + 46 + nameLength + extra > table.size())
                    return false;
                e.name.assign(reinterpret_cast<const char *>(h + 46), nameLength);
                std::replace(e.name.begin(), e.name.end(), '\\', '/');
                // ZIP64's sizes and offset (only the fields saturated above, in this order).
                for (size_t x = at + 46 + nameLength; x + 4 <= at + 46 + nameLength + extra;)
                {
                    const uint16_t id = u16(&table[x]), length = u16(&table[x + 2]);
                    if (id == 0x0001u)
                    {
                        size_t f = x + 4;
                        const size_t stop = x + 4 + length;
                        for (uint64_t *field : {&e.size, &e.packed, &e.local})
                            if (*field == 0xFFFFFFFFu && f + 8 <= stop)
                            {
                                *field = u64(&table[f]);
                                f += 8;
                            }
                    }
                    x += 4 + length;
                }
                entries.push_back(std::move(e));
                at += 46 + nameLength + extra + comment;
            }
            error.clear();
            return true;
        }

        bool isFolder(const Entry &e) { return !e.name.empty() && e.name.back() == '/'; }

        // "a/b/c" -> safe to put under a folder: no drive, no root, no "..".
        bool safe(const std::string &name)
        {
            if (name.empty() || name[0] == '/' || name.find(':') != std::string::npos)
                return false;
            return ("/" + name + "/").find("/../") == std::string::npos;
        }

        bool unpack(std::ifstream &in, const Entry &e, std::vector<uint8_t> &out, std::string &error)
        {
            uint8_t local[30];
            if (!readAt(in, e.local, local, sizeof local) || u32(local) != 0x04034B50u)
            {
                error = e.name + ": damaged zip";
                return false;
            }
            const uint64_t data = e.local + 30 + u16(local + 26) + u16(local + 28);
            if (e.size > 0x7FFFFF00u || e.packed > 0x7FFFFF00u)
            {
                error = e.name + ": over 2 GB";
                return false;
            }
            std::vector<uint8_t> packed(static_cast<size_t>(e.packed));
            if (!readAt(in, data, packed.data(), packed.size()))
            {
                error = e.name + ": damaged zip";
                return false;
            }
            if (e.method == 0)
                out = std::move(packed);
            else
            {
                out.assign(static_cast<size_t>(e.size) + 64u, 0u);
                const int got = e.size == 0 ? 0 : sinflate(out.data(), static_cast<int>(out.size()), packed.data(), static_cast<int>(packed.size()));
                if (got != static_cast<int>(e.size))
                {
                    error = e.name + ": damaged zip (unpacked " + std::to_string(got) + " of " + std::to_string(e.size) + " bytes)";
                    return false;
                }
                out.resize(static_cast<size_t>(e.size));
            }
            if (out.size() != e.size || crc32(0, out.data(), out.size()) != e.crc)
            {
                error = e.name + ": damaged zip (checksum)";
                return false;
            }
            return true;
        }

        // mod.ini's [Mod] key, from its text.
        std::string iniValue(const std::string &text, const std::string &key)
        {
            std::string section, line;
            size_t start = 0;
            while (start < text.size())
            {
                size_t end = text.find('\n', start);
                if (end == std::string::npos)
                    end = text.size();
                line = text.substr(start, end - start);
                start = end + 1;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                    line.pop_back();
                const size_t first = line.find_first_not_of(" \t");
                if (first == std::string::npos)
                    continue;
                line.erase(0, first);
                if (line[0] == '[' && line.back() == ']')
                {
                    section = lower(line.substr(1, line.size() - 2));
                    continue;
                }
                const size_t equals = line.find('=');
                if (section != "mod" || equals == std::string::npos)
                    continue;
                std::string k = line.substr(0, equals);
                while (!k.empty() && (k.back() == ' ' || k.back() == '\t'))
                    k.pop_back();
                if (lower(k) == lower(key))
                {
                    std::string v = line.substr(equals + 1);
                    const size_t f = v.find_first_not_of(" \t");
                    return f == std::string::npos ? std::string() : v.substr(f);
                }
            }
            return {};
        }

        std::string lastName(const std::string &prefix) // "a/b/" -> "b"
        {
            std::string p = prefix;
            if (!p.empty() && p.back() == '/')
                p.pop_back();
            const size_t slash = p.rfind('/');
            return slash == std::string::npos ? p : p.substr(slash + 1);
        }
    }

    Plan inspect(const fs::path &zip, const fs::path &modsDir)
    {
        Plan plan;
        plan.zip = zip;
        std::ifstream in(zip, std::ios::binary);
        std::vector<Entry> entries;
        if (!in || !readTable(in, entries, plan.error))
        {
            if (plan.error.empty())
                plan.error = "can't be read";
            return plan;
        }
        // The mods' folders: every folder holding a mod.ini, the outermost ones.
        std::vector<std::string> roots;
        std::map<std::string, const Entry *> ini;
        for (const Entry &e : entries)
        {
            if (isFolder(e) || lower(e.name).rfind("__macosx/", 0) == 0)
                continue;
            const size_t slash = e.name.rfind('/');
            const std::string leaf = slash == std::string::npos ? e.name : e.name.substr(slash + 1);
            if (lower(leaf) == "mod.ini")
            {
                const std::string root = slash == std::string::npos ? std::string() : e.name.substr(0, slash + 1);
                roots.push_back(root);
                ini[root] = &e;
            }
        }
        std::sort(roots.begin(), roots.end(), [](const std::string &a, const std::string &b) { return a.size() < b.size(); });
        std::vector<std::string> outer;
        for (const std::string &r : roots)
            if (std::none_of(outer.begin(), outer.end(), [&](const std::string &o) { return r.compare(0, o.size(), o) == 0; }))
                outer.push_back(r);
        if (outer.empty())
        {
            plan.error = "has no mod.ini: it isn't a GHR mod (a mod's zip holds a mod.ini beside its files)";
            return plan;
        }
        std::sort(outer.begin(), outer.end());
        std::map<std::string, std::string> installed; // lower case -> as on disk
        for (const std::string &f : mod_list::folderNames(modsDir))
            installed.emplace(lower(f), f);
        std::set<std::string> taken;
        for (const std::string &root : outer)
        {
            Found m;
            m.prefix = root;
            std::string folder = mod_list::folderName(root.empty() ? utf8(zip.stem()) : lastName(root));
            for (int n = 2; taken.count(lower(folder)); ++n)
                folder = mod_list::folderName((root.empty() ? utf8(zip.stem()) : lastName(root)) + " " + std::to_string(n));
            taken.insert(lower(folder));
            m.folder = folder;
            std::vector<uint8_t> text;
            std::string error;
            if (ini[root]->flags & 1u)
            {
                plan.error = "is encrypted (a password-protected zip)";
                return plan;
            }
            if (ini[root]->method != 0 && ini[root]->method != 8)
            {
                plan.error = "is packed in a way GHR can't unpack (save it as a plain zip)";
                return plan;
            }
            if (ini[root]->size < 1024u * 1024u && unpack(in, *ini[root], text, error))
            {
                const std::string t(text.begin(), text.end());
                m.name = iniValue(t, "Name");
                m.version = iniValue(t, "Version");
            }
            if (m.name.empty())
                m.name = m.folder;
            if (const auto it = installed.find(lower(m.folder)); it != installed.end())
            {
                m.exists = true;
                m.folder = it->second;
                std::ifstream old(modsDir / fromUtf8(m.folder) / "mod.ini", std::ios::binary);
                const std::string oldText((std::istreambuf_iterator<char>(old)), std::istreambuf_iterator<char>());
                m.oldVersion = iniValue(oldText, "Version");
            }
            plan.mods.push_back(std::move(m));
        }
        for (const Entry &e : entries)
        {
            if (isFolder(e))
                continue;
            for (Found &m : plan.mods)
            {
                if (e.name.compare(0, m.prefix.size(), m.prefix) != 0)
                    continue;
                if (!safe(e.name.substr(m.prefix.size())))
                {
                    plan.error = "names a file outside its folder (" + e.name + ")";
                    return plan;
                }
                if (e.flags & 1u)
                {
                    plan.error = "is encrypted (a password-protected zip)";
                    return plan;
                }
                if (e.method != 0 && e.method != 8)
                {
                    plan.error = "is packed in a way GHR can't unpack (save it as a plain zip)";
                    return plan;
                }
                ++m.files;
                m.bytes += e.size;
                break;
            }
        }
        return plan;
    }

    bool install(const Plan &plan, const fs::path &modsDir, std::string &error, Progress *progress)
    {
        error.clear();
        std::ifstream in(plan.zip, std::ios::binary);
        std::vector<Entry> entries;
        if (!plan.error.empty() || !in || !readTable(in, entries, error))
        {
            if (error.empty())
                error = plan.error.empty() ? "can't be read" : plan.error;
            return false;
        }
        std::error_code ec;
        fs::create_directories(modsDir, ec);
        if (progress)
        {
            uint64_t total = 0;
            for (const Found &m : plan.mods)
                total += m.bytes;
            progress->total = total;
            progress->done = 0;
        }
        for (const Found &m : plan.mods)
        {
            const fs::path temp = modsDir / fromUtf8(".installing-" + m.folder);
            fs::remove_all(temp, ec);
            fs::create_directories(temp, ec);
            const auto fail = [&](const std::string &why) {
                error = m.name + ": " + why;
                fs::remove_all(temp, ec);
                return false;
            };
            for (const Entry &e : entries)
            {
                if (isFolder(e) || e.name.compare(0, m.prefix.size(), m.prefix) != 0)
                    continue;
                const std::string rel = e.name.substr(m.prefix.size());
                // the entry belongs to a mod further in (another root starting with this one's prefix)
                bool other = false;
                for (const Found &o : plan.mods)
                    other = other || (&o != &m && o.prefix.size() > m.prefix.size() && e.name.compare(0, o.prefix.size(), o.prefix) == 0);
                if (other || !safe(rel))
                    continue;
                std::vector<uint8_t> bytes;
                std::string why;
                if (!unpack(in, e, bytes, why))
                    return fail(why);
                const fs::path target = temp / fromUtf8(rel);
                fs::create_directories(target.parent_path(), ec);
                std::ofstream out(target, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (!out)
                    return fail("can't write " + utf8(target));
                if (progress)
                    progress->done += e.size;
            }
            const fs::path place = modsDir / fromUtf8(m.folder);
            const fs::path aside = modsDir / fromUtf8(".replaced-" + m.folder);
            if (fs::exists(place, ec))
            {
                fs::remove_all(aside, ec);
                fs::rename(place, aside, ec);
                if (ec)
                    return fail("the installed mods/" + m.folder + " can't be moved (a file in it is open?)");
            }
            fs::rename(temp, place, ec);
            if (ec)
            {
                std::error_code back;
                fs::rename(aside, place, back);
                return fail("can't be put in mods/" + m.folder);
            }
            fs::remove_all(aside, ec);
        }
        return true;
    }
}
