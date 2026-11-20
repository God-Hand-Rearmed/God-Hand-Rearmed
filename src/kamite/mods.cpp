// The mods at boot (see mods.h).
#include "kamite/mods.h"
#include "kamite/disc_files.h"
#include "kamite/file_layer.h"
#include "kamite/log.h"
#include "kamite/mod_list.h"
#include "runtime/ps2_game_menu.h"
#include "runtime/ps2_pnach.h"
#include "runtime/ps2_save_state.h"

#include <cstdlib>
#include <map>
#include <mutex>
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
            std::string parts = std::to_string(m.files.size()) + " game file(s)";
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
        std::map<std::string, const mod_list::Mod *> winner; // the last enabled mod with the file
        for (const mod_list::Mod &m : g_mods)
            if (m.enabled)
                for (const std::string &file : m.files)
                {
                    if (!disc_files::paths().count(file))
                    {
                        KM_WARN(Boot, "mod " << m.folder << ": " << file << " is not a file of the game; left out");
                        continue;
                    }
                    if (const auto it = winner.find(file); it != winner.end())
                        KM_INFO(Boot, "mods: " << file << ": " << m.folder << " wins over " << it->second->folder);
                    winner[file] = &m;
                }
        int served = 0;
        for (const auto &[file, m] : winner) // sorted by path: the same LBAs on every run with the same mods
        {
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
