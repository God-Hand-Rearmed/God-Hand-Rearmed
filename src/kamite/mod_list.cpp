// The mods under <exe>/mods and their load order (see mod_list.h).
#include "kamite/mod_list.h"
#include "kamite/ini_file.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string_view>

namespace fs = std::filesystem;

namespace kamite::mod_list
{
    std::string lower(std::string s)
    {
        for (char &c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    std::string utf8(const fs::path &p)
    {
        const std::u8string s = p.generic_u8string();
        return std::string(s.begin(), s.end());
    }

    fs::path fromUtf8(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }

    std::string archiveOf(const std::string &file)
    {
        const size_t slash = file.find_last_of('/');
        if (slash == std::string::npos || slash < 4 || slash + 1 == file.size())
            return {};
        const std::string archive = file.substr(0, slash);
        return lower(archive.substr(archive.size() - 4)) == ".afs" ? archive : std::string();
    }

    std::string fileKey(const std::string &file)
    {
        const std::string archive = archiveOf(file);
        if (archive.empty())
            return file;
        std::string entry = file.substr(archive.size() + 1);
        if (const size_t dot = entry.find_last_of('.'); dot != std::string::npos && dot != 0)
            entry.resize(dot);
        return archive + "/" + entry;
    }

    namespace
    {
        std::string trim(const std::string &s)
        {
            size_t a = 0, b = s.size();
            while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
                ++a;
            while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
                --b;
            return s.substr(a, b - a);
        }

        // "a|b|c" -> its names, as written (empty ones dropped).
        std::vector<std::string> split(const std::string &text)
        {
            std::vector<std::string> out;
            size_t start = 0;
            while (start <= text.size())
            {
                const size_t bar = text.find('|', start);
                const std::string part = trim(text.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
                if (!part.empty())
                    out.push_back(part);
                if (bar == std::string::npos)
                    break;
                start = bar + 1;
            }
            return out;
        }

        bool isModFolder(const fs::directory_entry &entry)
        {
            std::error_code error;
            if (!entry.is_directory(error))
                return false;
            const std::string name = utf8(entry.path().filename());
            return !name.empty() && name[0] != '.' && lower(name) != "characters";
        }

        // The top folder of a path relative to the mod ("" for a file at the top).
        std::string topFolder(const std::string &rel)
        {
            const size_t slash = rel.find('/');
            return slash == std::string::npos ? std::string() : rel.substr(0, slash);
        }

        void readIni(Mod &m)
        {
            IniFile ini;
            std::error_code error;
            fs::path file = m.path / "mod.ini";
            if (!fs::is_regular_file(file, error))
            {
                // any case (a zip made on another system)
                for (const auto &entry : fs::directory_iterator(m.path, error))
                    if (entry.is_regular_file(error) && lower(utf8(entry.path().filename())) == "mod.ini")
                        file = entry.path();
            }
            if (ini.load(file))
            {
                m.hasIni = true;
                m.name = trim(ini.get("Mod", "Name"));
                m.author = trim(ini.get("Mod", "Author"));
                m.version = trim(ini.get("Mod", "Version"));
                m.description = trim(ini.get("Mod", "Description"));
                for (size_t at = 0; (at = m.description.find("\\n", at)) != std::string::npos;)
                    m.description.replace(at, 2, "\n");
            }
            if (m.name.empty())
                m.name = m.folder;
        }

        void readContents(Mod &m, bool details)
        {
            std::error_code error;
            for (auto it = fs::recursive_directory_iterator(m.path, fs::directory_options::skip_permission_denied, error);
                 !error && it != fs::recursive_directory_iterator(); it.increment(error))
            {
                const std::string rel = lower(utf8(it->path().lexically_relative(m.path)));
                const std::string top = topFolder(rel);
                if (it->is_directory(error))
                {
                    if (it.depth() == 0 && (rel == "textures" || rel == "patches" || rel == "characters"))
                    {
                        m.hasTextures = m.hasTextures || rel == "textures";
                        m.hasCharacters = m.hasCharacters || rel == "characters";
                        if (rel == "characters" || (rel == "textures" && !details))
                            it.disable_recursion_pending(); // not walked: characters are step 4's; the boot only needs the folder
                    }
                    continue;
                }
                if (!it->is_regular_file(error) || top.empty())
                    continue;
                if (top == "textures")
                {
                    const std::string ext = lower(utf8(it->path().extension()));
                    if (ext == ".png" || ext == ".dds")
                        m.textures.push_back(lower(utf8(it->path().stem())));
                }
                else if (top == "patches")
                {
                    if (lower(utf8(it->path().extension())) == ".pnach")
                        m.patches.push_back(it->path());
                }
                else if (top != "characters")
                    m.files.push_back(rel);
            }
            std::sort(m.files.begin(), m.files.end());
            std::sort(m.textures.begin(), m.textures.end());
            m.textures.erase(std::unique(m.textures.begin(), m.textures.end()), m.textures.end());
            m.textureCount = m.textures.size();
            std::sort(m.patches.begin(), m.patches.end(), [](const fs::path &a, const fs::path &b) {
                return lower(utf8(a.filename())) < lower(utf8(b.filename()));
            });
        }
    }

    std::vector<std::string> folderNames(const fs::path &modsDir)
    {
        std::vector<std::string> out;
        std::error_code error;
        for (const auto &entry : fs::directory_iterator(modsDir, error))
            if (isModFolder(entry))
                out.push_back(utf8(entry.path().filename()));
        std::sort(out.begin(), out.end(), [](const std::string &a, const std::string &b) { return lower(a) < lower(b); });
        return out;
    }

    std::vector<Mod> scan(const fs::path &modsDir, const std::string &loadOrder, const std::string &disabled, bool details)
    {
        const std::vector<std::string> folders = folderNames(modsDir);
        std::map<std::string, std::string> byKey; // lower case -> as on disk
        for (const std::string &f : folders)
            byKey.emplace(lower(f), f);
        std::set<std::string> off;
        for (const std::string &name : split(disabled))
            off.insert(lower(name));
        std::vector<std::string> placed; // lower case, in order
        std::set<std::string> seen;
        const auto place = [&](const std::string &name) {
            const std::string key = lower(name);
            if (byKey.count(key) && seen.insert(key).second)
                placed.push_back(key);
        };
        for (const std::string &name : split(loadOrder))
            place(name);
        for (const std::string &name : split(disabled)) // off and not in the order (written by hand): after the ordered ones
            place(name);
        const size_t known = placed.size();
        for (const std::string &f : folders) // new: on, at the bottom, A-Z
            place(f);
        std::vector<Mod> mods;
        for (size_t i = 0; i < placed.size(); ++i)
        {
            Mod m;
            m.folder = byKey[placed[i]];
            m.path = modsDir / fromUtf8(m.folder);
            m.isNew = i >= known;
            m.enabled = m.isNew || !off.count(placed[i]);
            readIni(m);
            readContents(m, details);
            mods.push_back(std::move(m));
        }
        return mods;
    }

    std::string orderText(const std::vector<Mod> &mods)
    {
        std::string out;
        for (const Mod &m : mods)
            out += (out.empty() ? "" : "|") + m.folder;
        return out;
    }

    std::string disabledText(const std::vector<Mod> &mods)
    {
        std::string out;
        for (const Mod &m : mods)
            if (!m.enabled)
                out += (out.empty() ? "" : "|") + m.folder;
        return out;
    }

    std::string folderName(const std::string &wanted)
    {
        std::string out;
        for (const char c : wanted)
            if (static_cast<unsigned char>(c) >= 32u && std::string_view("<>:\"/\\|?*;").find(c) == std::string_view::npos)
                out.push_back(c);
        out = trim(out);
        while (!out.empty() && (out.back() == '.' || out.back() == ' '))
            out.pop_back();
        while (!out.empty() && out.front() == '.')
            out.erase(0, 1);
        if (out.empty())
            out = "mod";
        if (lower(out) == "characters")
            out += " mod";
        return out;
    }

    std::vector<Overrides> overrides(const std::vector<Mod> &mods)
    {
        std::vector<Overrides> out(mods.size());
        for (size_t below = 0; below < mods.size(); ++below)
        {
            if (!mods[below].enabled)
                continue;
            for (size_t above = 0; above < below; ++above)
            {
                if (!mods[above].enabled)
                    continue;
                Conflict c;
                c.other = above;
                // By key (an archive entry's name without its extension: "x.ogg" and "x.adx" are one entry), and a whole
                // archive against the entries of it.
                std::set<std::string> keysAbove, keysBelow;
                for (const std::string &f : mods[above].files)
                    keysAbove.insert(fileKey(f));
                for (const std::string &f : mods[below].files)
                    keysBelow.insert(fileKey(f));
                std::set<std::string> clash;
                for (const std::string &k : keysBelow)
                    if (keysAbove.count(k) || keysAbove.count(archiveOf(k)))
                        clash.insert(k);
                for (const std::string &k : keysAbove)
                    if (keysBelow.count(archiveOf(k)))
                        clash.insert(archiveOf(k));
                c.files.assign(clash.begin(), clash.end());
                std::vector<std::string> shared;
                std::set_intersection(mods[above].textures.begin(), mods[above].textures.end(), mods[below].textures.begin(),
                                      mods[below].textures.end(), std::back_inserter(shared));
                c.textures = shared.size();
                if (c.files.empty() && c.textures == 0)
                    continue;
                out[below].wins.push_back(c);
                c.other = below;
                out[above].loses.push_back(std::move(c));
            }
        }
        return out;
    }

    std::vector<Entry> enabledEntries(const std::vector<Mod> &mods)
    {
        std::vector<Entry> out;
        for (const Mod &m : mods)
            if (m.enabled)
                out.push_back({m.folder, m.version});
        return out;
    }

    std::string describeDifference(const std::vector<Entry> &state, const std::vector<Entry> &now)
    {
        if (state == now)
            return {};
        const auto find = [](const std::vector<Entry> &list, const std::string &folder) -> const Entry * {
            for (const Entry &e : list)
                if (lower(e.folder) == lower(folder))
                    return &e;
            return nullptr;
        };
        std::vector<std::string> gone, added, versions;
        for (const Entry &e : state)
        {
            const Entry *n = find(now, e.folder);
            if (!n)
                gone.push_back(e.folder);
            else if (n->version != e.version)
                versions.push_back(e.folder + " (" + (e.version.empty() ? "no version" : e.version) + ", now " +
                                   (n->version.empty() ? "no version" : n->version) + ")");
        }
        for (const Entry &e : now)
            if (!find(state, e.folder))
                added.push_back(e.folder);
        const auto names = [](const std::vector<std::string> &list) {
            std::string out;
            const size_t shown = std::min<size_t>(list.size(), 3);
            for (size_t i = 0; i < shown; ++i)
                out += (i ? ", " : "") + list[i];
            if (list.size() > shown)
                out += " +" + std::to_string(list.size() - shown) + " more";
            return out;
        };
        std::string text;
        if (!gone.empty())
            text += " Not on now: " + names(gone) + ".";
        if (!added.empty())
            text += " On now: " + names(added) + ".";
        if (!versions.empty())
            text += " Another version: " + names(versions) + ".";
        if (text.empty())
            text = " The same mods in another order.";
        return "This state was made with other mods." + text;
    }
}
