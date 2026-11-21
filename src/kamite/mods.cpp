// The mods at boot (see mods.h).
#include "kamite/mods.h"
#include "kamite/adx_codec.h"
#include "kamite/afs_archive.h"
#include "kamite/disc_files.h"
#include "kamite/file_layer.h"
#include "kamite/log.h"
#include "kamite/mod_audio.h"
#include "kamite/mod_list.h"
#include "runtime/ps2_cd_image.h"
#include "runtime/ps2_game_menu.h"
#include "runtime/ps2_pnach.h"
#include "runtime/ps2_save_state.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

namespace fs = std::filesystem;

namespace kamite::mods
{
    namespace
    {
        std::vector<mod_list::Mod> g_mods;            // every mod, in load order
        std::map<std::string, std::string> g_replaced; // game file -> the winning mod's name
        std::vector<mod_list::Entry> g_now;            // the enabled mods, as a state records them

        // A state's record, read by the extension while it loads; compared once it has loaded (afterLoad).
        std::mutex g_stateMutex;
        std::vector<mod_list::Entry> g_stateMods;

        void setEnv(const char *name, const std::string &value)
        {
#if defined(_WIN32)
            _putenv_s(name, value.c_str());
#else
            setenv(name, value.c_str(), 1);
#endif
        }

        std::string env(const char *name)
        {
            const char *value = std::getenv(name);
            return value ? value : "";
        }

        // Replays and scripted tests run the vanilla game, whatever mods this machine has.
        const char *offReason()
        {
            if (!env("KAMITE_REPLAY_CASE").empty())
                return "a test-case replay runs without mods";
            if (!env("KAMITE_TEST").empty())
                return "a test run runs without mods";
            if (env("KAMITE_MODS") == "0")
                return "KAMITE_MODS=0";
            return nullptr;
        }

        std::string native(const fs::path &p)
        {
            const std::u8string s = fs::path(p).lexically_normal().make_preferred().u8string();
            return std::string(s.begin(), s.end());
        }

        void saveState(ps2x::state::Writer &out)
        {
            std::vector<std::string> folders, versions;
            for (const mod_list::Entry &e : g_now)
            {
                folders.push_back(e.folder);
                versions.push_back(e.version);
            }
            ps2x::state::fields(out, folders, versions);
        }

        void loadState(ps2x::state::Reader &in)
        {
            std::vector<std::string> folders, versions;
            ps2x::state::fields(in, folders, versions);
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_stateMods.clear();
            for (size_t i = 0; i < folders.size(); ++i)
                g_stateMods.push_back({folders[i], i < versions.size() ? versions[i] : std::string()});
        }

        // A mod's file standing for a game file or an archive entry; order = its place in the load order.
        struct Source
        {
            const mod_list::Mod *mod = nullptr;
            size_t order = 0;
            std::string file; // relative to the mod's folder, lower case
        };

        fs::path g_exeDir;

        // An archive with the mods' entries swapped in (step 2), served whole in its place. base: a mod's whole copy
        // of the archive, when one has it; entries of mods above that mod lose to it (the lower mod wins, as for files).
        bool serveArchive(const std::string &archivePath, const Source *base, const std::map<std::string, Source> &wanted)
        {
            const auto disc = disc_files::find(archivePath);
            if (!disc)
                return false;
            std::map<std::string, Source> list;
            for (const auto &[key, source] : wanted)
            {
                if (base && source.order < base->order)
                    KM_INFO(Boot, "mods: " << archivePath << " entry " << key << " of " << source.mod->folder << " loses to "
                                           << base->mod->folder << "'s whole " << archivePath);
                else
                    list[key] = source;
            }
            if (list.empty())
                return false; // the whole archive (if any) is served as a file
            // The archive the entries go into: the disc's, or the mod's whole copy.
            const fs::path baseFile = base ? base->mod->path / mod_list::fromUtf8(base->file) : fs::path();
            std::shared_ptr<std::ifstream> baseStream;
            uint64_t baseSize = disc->size;
            if (base)
            {
                std::error_code error;
                baseSize = fs::file_size(baseFile, error);
                baseStream = std::make_shared<std::ifstream>(baseFile, std::ios::binary);
                if (error || !*baseStream)
                {
                    KM_WARN(Boot, "mod " << base->mod->folder << ": " << archivePath << " can't be read; its entries from other mods are left out");
                    return false;
                }
            }
            const afs::Reader reader = [&](uint64_t offset, uint32_t size, std::vector<uint8_t> &out) {
                if (!base)
                    return offset <= disc->size && disc_files::readRange(*disc, static_cast<uint32_t>(offset), size, out);
                out.resize(size);
                baseStream->clear();
                baseStream->seekg(static_cast<std::streamoff>(offset));
                baseStream->read(reinterpret_cast<char *>(out.data()), size);
                return static_cast<uint32_t>(baseStream->gcount()) == size;
            };
            afs::Archive archive;
            std::string error;
            if (!afs::read(reader, baseSize, archive, error))
            {
                KM_WARN(Boot, "mods: " << archivePath << (base ? " (" + base->mod->folder + "'s)" : std::string()) << ": " << error
                                       << "; its entries from the mods are left out");
                return false;
            }
            const fs::path cache = g_exeDir / "cache" / "mod-audio";
            std::map<int, uint64_t> sizes;
            std::map<int, fs::path> files;
            std::map<int, const Source *> from;
            bool listed = false;
            for (const auto &[key, source] : list)
            {
                const int index = afs::find(archive, key);
                const fs::path file = source.mod->path / mod_list::fromUtf8(source.file);
                if (index < 0)
                {
                    KM_WARN(Boot, "mod " << source.mod->folder << ": " << archivePath << " has no entry \"" << key
                                         << "\" (an entry's name or its number, 0 to " << archive.entries.size() - 1 << "); left out");
                    if (!listed)
                    {
                        std::string names;
                        for (size_t i = 0; i < archive.entries.size(); ++i)
                            names += (i ? ", " : "") + std::to_string(i) + " " + (archive.entries[i].name.empty() ? "-" : archive.entries[i].name);
                        KM_INFO(Boot, "mods: " << archivePath << "'s entries: " << names);
                        listed = true;
                    }
                    continue;
                }
                const afs::Entry &entry = archive.entries[static_cast<size_t>(index)];
                const std::string what = archivePath + " entry " + std::to_string(index) + (entry.name.empty() ? "" : " (" + entry.name + ")");
                fs::path served = file;
                std::string note = "as it is";
                if (mod_audio::isAudio(file))
                {
                    // In the shape of the entry it replaces: its channels, rate and whether it loops.
                    std::vector<uint8_t> head;
                    const adx::Info info = reader(entry.offset, std::min<uint32_t>(entry.size, 0x800u), head)
                                               ? adx::readInfo(head.data(), head.size())
                                               : adx::Info{};
                    if (!info.valid)
                    {
                        KM_WARN(Boot, "mod " << source.mod->folder << ": " << what << " is not an ADX sound; " << source.file << " left out");
                        continue;
                    }
                    mod_audio::Target target;
                    target.channels = info.channels;
                    target.rate = info.rate;
                    target.loops = info.loops;
                    std::string why;
                    served = mod_audio::adxFor(file, target, cache, note, why);
                    if (served.empty())
                    {
                        KM_WARN(Boot, "mod " << source.mod->folder << ": " << source.file << " " << why << "; " << what << " stays the game's");
                        continue;
                    }
                }
                std::error_code ec;
                const uint64_t size = fs::file_size(served, ec);
                if (ec || size == 0 || size > 0xFFFFFFFFull)
                {
                    KM_WARN(Boot, "mod " << source.mod->folder << ": " << source.file << " can't be read or is empty; " << what << " stays the game's");
                    continue;
                }
                sizes[index] = size;
                files[index] = served;
                from[index] = &source;
                KM_INFO(Boot, "mod " << source.mod->folder << ": " << what << " <- " << source.file << ": " << note);
            }
            if (sizes.empty())
                return false;
            const afs::Layout layout = afs::layout(archive, sizes);
            std::vector<PS2ServedFiles::Piece> pieces;
            PS2ServedFiles::Piece header;
            header.offset = 0;
            header.size = layout.header.size();
            header.bytes = layout.header;
            pieces.push_back(std::move(header));
            for (const afs::Part &part : layout.parts)
            {
                PS2ServedFiles::Piece piece;
                piece.offset = part.offset;
                piece.size = part.size;
                if (part.replaced)
                {
                    piece.source = PS2ServedFiles::Piece::Source::Host;
                    piece.host = files[part.entry];
                }
                else if (base)
                {
                    piece.source = PS2ServedFiles::Piece::Source::Host;
                    piece.host = baseFile;
                    piece.from = archive.entries[static_cast<size_t>(part.entry)].offset;
                }
                else
                {
                    piece.source = PS2ServedFiles::Piece::Source::Disc;
                    piece.from = disc->imageOffset + archive.entries[static_cast<size_t>(part.entry)].offset;
                }
                pieces.push_back(std::move(piece));
            }
            if (archive.namesSize != 0)
            {
                PS2ServedFiles::Piece names;
                names.offset = layout.namesOffset;
                names.size = archive.names.size();
                names.bytes = archive.names;
                pieces.push_back(std::move(names));
            }
            std::set<std::string> who;
            for (const auto &[index, source] : from)
                who.insert(source->mod->folder);
            std::string mods;
            for (const std::string &w : who)
                mods += (mods.empty() ? "" : ", ") + w;
            if (!file_layer::replace(archivePath, std::move(pieces), layout.size,
                                     std::to_string(sizes.size()) + " entr" + (sizes.size() == 1 ? "y" : "ies") + " from " + mods +
                                         (base ? " in " + base->mod->folder + "'s copy" : std::string())))
            {
                KM_WARN(Boot, "mods: " << archivePath << " with the mods' entries could not be served");
                return false;
            }
            g_replaced[archivePath] = from.begin()->second->mod->name;
            return true;
        }

        // After every load: the state's mods (none for a state without the record: made before mods, or by a run
        // without them) against the ones on now. Shown after the "State loaded" line (2.5 s), for a few seconds.
        void afterLoad()
        {
            std::vector<mod_list::Entry> state;
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                state.swap(g_stateMods);
            }
            const std::string difference = mod_list::describeDifference(state, g_now);
            if (difference.empty())
                return;
            std::string was, now;
            for (const mod_list::Entry &e : state)
                was += (was.empty() ? "" : ", ") + e.folder + (e.version.empty() ? "" : " " + e.version);
            for (const mod_list::Entry &e : g_now)
                now += (now.empty() ? "" : ", ") + e.folder + (e.version.empty() ? "" : " " + e.version);
            KM_WARN(Boot, "mods: the state loaded was made with [" << was << "], on now [" << now << "]: " << difference);
            ps2x::menu::notify(difference, 9.0f);
        }
    }

    size_t prepare(const fs::path &exeDir)
    {
        // The record goes into every state, with or without mods (a state made without them then says so).
        ps2x::savestate::registerExtension("kamite.mods", &saveState, &loadState);
        ps2x::savestate::onLoaded(&afterLoad);
        const fs::path modsDir = exeDir / "mods";
        g_exeDir = exeDir;
        if (const char *why = offReason())
        {
            if (!mod_list::folderNames(modsDir).empty())
                KM_INFO(Boot, "mods: off (" << why << ")");
            return 0;
        }
        g_mods = mod_list::scan(modsDir, env("KAMITE_MOD_ORDER"), env("KAMITE_MODS_DISABLED"), false);
        g_now = mod_list::enabledEntries(g_mods);
        if (g_mods.empty())
            return 0;
        KM_INFO(Boot, "mods: " << g_mods.size() << " in " << mod_list::utf8(modsDir) << ", " << g_now.size() << " on (load order, the last wins)");
        std::string first;
        size_t patches = 0;
        for (const mod_list::Mod &m : g_mods)
        {
            const size_t entries = static_cast<size_t>(
                std::count_if(m.files.begin(), m.files.end(), [](const std::string &f) { return !mod_list::archiveOf(f).empty(); }));
            std::string parts = std::to_string(m.files.size() - entries) + " game file(s)";
            if (entries != 0)
                parts += ", " + std::to_string(entries) + " archive entr" + (entries == 1 ? "y" : "ies");
            if (m.hasTextures)
                parts += ", textures/";
            if (!m.patches.empty())
                parts += ", " + std::to_string(m.patches.size()) + " patch file(s)";
            if (m.hasCharacters)
                parts += ", characters/ (not loaded yet)";
            KM_INFO(Boot, "mod " << (m.enabled ? "on " : "off") << ": " << m.folder << (m.name != m.folder ? " (" + m.name + ")" : "")
                                 << (m.version.empty() ? "" : " " + m.version) << (m.isNew ? " [new]" : "") << ": " << parts);
            if (!m.enabled)
                continue;
            for (const fs::path &file : m.patches)
            {
                const size_t added = ps2x::pnach::loadFile(native(file));
                KM_INFO(Boot, "mod " << m.folder << ": " << added << " patch line(s) from " << mod_list::utf8(file.filename()));
                patches += added;
            }
        }
        // Textures: the lowest mod first; then the player's pack; then the extra folders (characters, the port's own).
        for (auto it = g_mods.rbegin(); it != g_mods.rend(); ++it)
            if (it->enabled && it->hasTextures)
                first += (first.empty() ? "" : "|") + native(it->path / "textures");
        if (!first.empty())
        {
            setEnv("PS2X_PCSX2_TEXTURE_FIRST", first);
            KM_INFO(Boot, "mods: texture folders ahead of the texture pack: " << first);
        }
        return patches;
    }

    void installFiles()
    {
        if (g_now.empty())
            return;
        if (!disc_files::open())
        {
            KM_WARN(Boot, "mods: the disc image has no file archive / path table; the mods' game files are not served");
            return;
        }
        // The last enabled mod with each game file, and with each entry of an archive (by its key: name or number
        // without the extension).
        std::map<std::string, Source> winner;
        std::map<std::string, std::map<std::string, Source>> entries; // archive -> entry key -> the mod's file
        for (size_t order = 0; order < g_mods.size(); ++order)
        {
            const mod_list::Mod &m = g_mods[order];
            if (!m.enabled)
                continue;
            for (const std::string &file : m.files)
            {
                if (disc_files::paths().count(file))
                {
                    if (const auto it = winner.find(file); it != winner.end())
                        KM_INFO(Boot, "mods: " << file << ": " << m.folder << " wins over " << it->second.mod->folder);
                    winner[file] = Source{&m, order, file};
                    continue;
                }
                const std::string archive = mod_list::archiveOf(file);
                if (!archive.empty() && disc_files::paths().count(archive))
                {
                    const std::string key = mod_list::fileKey(file).substr(archive.size() + 1);
                    auto &slot = entries[archive];
                    if (const auto it = slot.find(key); it != slot.end())
                        KM_INFO(Boot, "mods: " << archive << " entry " << key << ": " << m.folder << " wins over " << it->second.mod->folder);
                    slot[key] = Source{&m, order, file};
                    continue;
                }
                KM_WARN(Boot, "mod " << m.folder << ": " << file << " is not a file of the game"
                                     << (archive.empty() ? "" : " (nor an entry of one of its .afs archives)") << "; left out");
            }
        }
        std::set<std::string> paths;
        for (const auto &[file, source] : winner)
            paths.insert(file);
        for (const auto &[archive, list] : entries)
            paths.insert(archive);
        int served = 0;
        for (const std::string &file : paths) // sorted by path: the same LBAs on every run with the same mods
        {
            const auto whole = winner.find(file);
            const auto parts = entries.find(file);
            if (parts != entries.end() && serveArchive(file, whole != winner.end() ? &whole->second : nullptr, parts->second))
            {
                ++served;
                continue;
            }
            if (whole == winner.end())
                continue;
            const mod_list::Mod *m = whole->second.mod;
            const fs::path source = m->path / mod_list::fromUtf8(file);
            if (file_layer::replace(file, source))
            {
                g_replaced[file] = m->name;
                ++served;
            }
            else
                KM_WARN(Boot, "mod " << m->folder << ": " << file << " could not be served (unreadable or empty)");
        }
        KM_INFO(Boot, "mods: " << served << " game file(s) served from the mods");
    }

    const std::string *replacing(std::string_view path)
    {
        const auto it = g_replaced.find(mod_list::lower(std::string(path)));
        return it == g_replaced.end() ? nullptr : &it->second;
    }
}
