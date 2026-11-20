// Character model replacements (see character_mods.h). The files are served by the file layer (file_layer.h): any
// size, the disc image untouched.
#include "kamite/character_mods.h"
#include "kamite/builtin_characters.h"
#include "kamite/file_layer.h"
#include "kamite/ini_file.h"
#include "kamite/log.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fs = std::filesystem;

namespace kamite::characters
{
    namespace
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
    }

    Character describe(const fs::path &folder)
    {
        Character c;
        c.id = utf8(folder.filename());
        IniFile ini;
        if (ini.load(folder / "character.ini"))
        {
            c.name = ini.get("Character", "Name");
            c.credit = ini.get("Character", "Credit");
        }
        if (c.name.empty())
            c.name = c.id;
        return c;
    }

    std::vector<Character> list(const fs::path &exeDir)
    {
        std::vector<Character> out{{"gene", "Gene", "Vanilla Thrilla"}};
#if !defined(KAMITE_RELEASE)
        // Built from the disc at boot (olivia_player.h); testers only until K names her.
        out.push_back({"olivia", "Olivia", "Pre-Alpha", {{"dress", "Dress"}, {"bunny", "Bunny Suit"}}});
#endif
        // Built from the disc at boot (builtin_characters.h): Xavier.
        for (const fs::path &folder : builtin_characters::folders(exeDir))
            out.push_back(describe(folder));
        std::error_code error;
        const fs::path root = exeDir / "mods" / "characters";
        std::vector<fs::path> folders;
        for (const auto &entry : fs::directory_iterator(root, error))
            if (entry.is_directory(error))
                folders.push_back(entry.path());
        std::sort(folders.begin(), folders.end());
        for (const fs::path &folder : folders)
            if (const std::string id = lower(utf8(folder.filename()));
                id != "gene" && id != "olivia" && !builtin_characters::isBuiltIn(exeDir, id))
                out.push_back(describe(folder));
        return out;
    }

    namespace
    {
        // Every character's full-resolution textures (PCSX2 replacement images named after its in-game textures), not
        // only the chosen one's: a save state made as one character and loaded while another is chosen brings that
        // character's model and 16-colour textures back with it, and its images must still be found (Oct 27: Xavier
        // loaded from a state while Gene was chosen showed his 16-colour body, an olive sleeve and olive buttons).
        // The names are hashes of the textures' own pixels and palettes, so another character's images never match.
        void registerTextures(const fs::path &exeDir)
        {
            std::error_code error;
            std::string folders;
            std::vector<fs::path> all;
            for (const auto &entry : fs::directory_iterator(exeDir / "mods" / "characters", error))
                if (entry.is_directory(error) && fs::is_directory(entry.path() / "textures", error))
                    all.push_back(entry.path() / "textures");
            std::sort(all.begin(), all.end());
            // The built-in characters' own images (their painted textures' images: builtin_characters.cpp adds those).
            for (const fs::path &folder : builtin_characters::folders(exeDir))
                if (fs::is_directory(folder / "textures", error))
                    all.push_back(folder / "textures");
            for (const fs::path &textures : all)
            {
                const std::u8string native = fs::path(textures).lexically_normal().make_preferred().u8string();
                folders += (folders.empty() ? "" : "|") + std::string(native.begin(), native.end());
                KM_INFO(Boot, "character textures: " << std::string(native.begin(), native.end()));
            }
            if (folders.empty())
                return;
#if defined(_WIN32)
            _putenv_s("PS2X_PCSX2_TEXTURE_EXTRA", folders.c_str());
#else
            setenv("PS2X_PCSX2_TEXTURE_EXTRA", folders.c_str(), 1);
#endif
        }
    }

    namespace
    {
        Hair g_hair;

        bool vec3(const std::string &text, float out[3]) { return std::sscanf(text.c_str(), "%f %f %f", &out[0], &out[1], &out[2]) == 3; }

        void readHair(const fs::path &folder)
        {
            IniFile ini;
            if (!ini.load(folder / "character.ini") || ini.get("Hair", "BackBone").empty())
                return;
            Hair h;
            auto strand = [&](const char *prefix, HairStrand &s, bool hinge) {
                const std::string p(prefix);
                s.bone = std::atoi(ini.get("Hair", p + "Bone", "-1").c_str());
                s.length = static_cast<float>(std::atof(ini.get("Hair", p + "Length", "0.2").c_str()));
                s.hinge = hinge;
                if (s.bone < 0 || s.bone > 31 || !vec3(ini.get("Hair", p + "Pivot"), s.pivot) || !vec3(ini.get("Hair", p + "Dir"), s.dir))
                    s.bone = -1;
            };
            strand("Back", h.back, false);
            strand("Side", h.side, true);
            auto num = [&](const char *key, float &v) {
                const std::string t = ini.get("Hair", key);
                if (!t.empty())
                    v = static_cast<float>(std::atof(t.c_str()));
            };
            num("Stiffness", h.stiffness);
            num("Damping", h.damping);
            num("Gravity", h.gravity);
            num("Wind", h.wind);
            num("BackForward", h.backForward);
            num("BackBack", h.backBack);
            num("BackSide", h.backSide);
            num("SideSwing", h.sideSwing);
            g_hair = h;
            KM_INFO(Boot, "character: hair on bones " << h.back.bone << " and " << h.side.bone);
        }
    }

    const Hair &hair() { return g_hair; }

    void useHair(const fs::path &folder) { readHair(folder); }

    void install(const fs::path &exeDir)
    {
        registerTextures(exeDir);
        const char *chosen = std::getenv("KAMITE_CHARACTER");
        const std::string id = chosen ? lower(chosen) : std::string();
        if (id.empty() || id == "gene" || id == "olivia") // Olivia: olivia_player.h
            return;
        const fs::path folder = exeDir / "mods" / "characters" / id;
        std::error_code error;
        if (builtin_characters::isBuiltIn(exeDir, id)) // built from the disc and served by builtin_characters.h
        {
            if (fs::is_directory(folder, error))
                KM_INFO(Boot, "character '" << id << "': " << utf8(folder) << " is left alone: the built-in " << id << " wins");
            return;
        }
        if (!fs::is_directory(folder, error))
        {
            KM_WARN(Boot, "character '" << id << "': no folder " << utf8(folder) << "; playing as Gene");
            return;
        }
        readHair(folder);
        const int replaced = file_layer::replaceFolder(folder, "character '" + id + "'");
        if (replaced == 0)
        {
            KM_WARN(Boot, "character '" << id << "': no replacement files in " << utf8(folder) << "; playing as Gene");
            return;
        }
        KM_INFO(Boot, "playing as " << describe(folder).name << " (" << replaced << " files)");
        // Its full-resolution textures were registered with every character's (registerTextures).
    }
}
