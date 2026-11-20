// recomplauncher.json's settings (gen_manifest.py): every setting with the GHR.ini keys each of its choices writes, and
// the keys each preset sets, as JSON on stdout. Built by gen_manifest.py from src/kamite/settings_table.cpp,
// presets.cpp and ini_file.cpp; not part of the game.
#include "kamite/ini_file.h"
#include "kamite/presets.h"
#include "kamite/settings_table.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace kamite;

namespace
{
    struct Key
    {
        std::string section, key, value;
    };

    void str(const std::string &text)
    {
        std::putchar('"');
        for (const char c : text)
        {
            const unsigned char u = static_cast<unsigned char>(c);
            if (c == '"' || c == '\\')
                std::printf("\\%c", c);
            else if (c == '\n')
                std::printf("\\n");
            else if (u < 0x20)
                std::printf("\\u%04x", u);
            else
                std::putchar(c);
        }
        std::putchar('"');
    }

    bool same(const std::string &a, const char *b)
    {
        if (a.size() != std::strlen(b))
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    }

    // The keys a value of this setting writes, as the launcher and the game write them (Display Mode and Camera & FOV
    // write several). Camera & FOV's Custom keeps the four values as they are (the rows under it set them).
    std::vector<Key> keysOf(const settings::Setting &s, const std::string &value)
    {
        if (std::strcmp(s.id, "DisplayMode") == 0)
        {
            IniFile ini;
            settings::writeDisplayMode(ini, value);
            return {{"Video", "StartFullscreen", ini.get("Video", "StartFullscreen")},
                    {"Video", "StartMaximized", ini.get("Video", "StartMaximized")}};
        }
        if (std::strcmp(s.id, "CameraPreset") == 0 && !same(value, "custom"))
        {
            IniFile ini;
            settings::writeCameraPreset(ini, value);
            std::vector<Key> keys;
            for (const char *key : {"CameraPreset", "FieldOfView", "FightFieldOfView", "CameraZoomOut", "CameraTilt"})
                keys.push_back({"Video", key, ini.get("Video", key)});
            return keys;
        }
        if (!*s.key)
            return {};
        return {{s.section, s.key, value}};
    }

    void keys(const std::vector<Key> &list)
    {
        std::putchar('[');
        for (size_t i = 0; i < list.size(); ++i)
        {
            std::printf(i ? ",[" : "[");
            str(list[i].section), std::putchar(','), str(list[i].key), std::putchar(','), str(list[i].value);
            std::putchar(']');
        }
        std::putchar(']');
    }

    // Whether a preset's match looks at this setting (presets.cpp counts(): a sub-option only while the preset turns
    // its parent on; HUD Layout not at 4:3), from the values the preset wrote.
    bool counts(const IniFile &ini, const settings::Setting &s)
    {
        if (std::strcmp(s.id, "HudLayout") == 0)
            return !same(settings::value(ini, *settings::find("AspectRatio")), "4:3");
        if (!s.parent)
            return true;
        const settings::Setting *parent = settings::find(s.parent);
        if (!parent || !presets::controls(*parent))
            return true;
        const std::string value = settings::value(ini, *parent);
        return s.parentValue ? same(value, s.parentValue) : value != "0";
    }
}

int main()
{
    std::printf("{\"presets\":[");
    for (int p = 0; p < presets::kCount; ++p)
    {
        IniFile ini;
        presets::write(ini, p);
        std::vector<Key> list;
        for (size_t i = 0; i < settings::kSettingCount; ++i)
        {
            const settings::Setting &s = settings::kSettings[i];
            if (!presets::controls(s) || !*s.key || !counts(ini, s))
                continue;
            for (const Key &k : keysOf(s, settings::value(ini, s)))
                list.push_back(k);
        }
        std::printf(p ? ",\n{" : "\n{");
        std::printf("\"name\":"), str(presets::kText[p].name);
        std::printf(",\"tagline\":"), str(presets::kText[p].tagline);
        std::printf(",\"keys\":"), keys(list), std::putchar('}');
    }
    std::printf("],\"custom\":{\"name\":"), str(presets::kCustomText.name);
    std::printf(",\"tagline\":"), str(presets::kCustomText.tagline), std::putchar('}');
    std::printf(",\"settings\":[");
    for (size_t i = 0; i < settings::kSettingCount; ++i)
    {
        const settings::Setting &s = settings::kSettings[i];
        std::printf(i ? ",\n{" : "\n{");
        std::printf("\"id\":"), str(s.id);
        std::printf(",\"tab\":"), str(settings::tabName(s.tab));
        std::printf(",\"category\":"), str(s.category);
        std::printf(",\"name\":"), str(s.name);
        std::printf(",\"description\":"), str(s.description);
        std::printf(",\"kind\":%d", static_cast<int>(s.kind));
        std::printf(",\"section\":"), str(s.section);
        std::printf(",\"key\":"), str(s.key);
        std::printf(",\"fallback\":"), str(s.fallback);
        std::printf(",\"fallbackKeys\":"), keys(keysOf(s, s.fallback));
        std::printf(",\"parent\":"), str(s.parent ? s.parent : "");
        std::printf(",\"parentValue\":"), str(s.parentValue ? s.parentValue : "");
        std::printf(",\"choices\":[");
        for (int c = 0; c < s.choiceCount; ++c)
        {
            std::printf(c ? ",{" : "{");
            std::printf("\"value\":"), str(s.choices[c].value);
            std::printf(",\"label\":"), str(s.choices[c].label);
            std::printf(",\"keys\":"), keys(keysOf(s, s.choices[c].value)), std::putchar('}');
        }
        std::printf("],\"values\":[");
        for (int v = 0; v < s.valueCount; ++v)
            std::printf(v ? ",%d" : "%d", s.values[v]);
        std::printf("]}");
    }
    std::printf("]}\n");
    return 0;
}
