#include "kamite/presets.h"

#include "kamite/ini_file.h"
#include "kamite/settings_table.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace kamite::presets
{
    // K's text (Nov 16 night; claude/presets-design.md). The same strings in game, in the launcher and on the posters.
    const Text kText[kCount] = {
        {"CLASSIC", "Classic", "Clover's Vanilla Thrilla", "Cinetastic 30FPS, 4:3, Classic Camera & Controls, as handed by God.",
         13},
        {"REMASTERED", "Remastered", "Spitshine Edition",
         "Smooth Sexy Silky 60FPS, Fills your screen - edge to edge, Modern Controls, etc.", 17},
        {"REARMED", "Rearmed", "That's the Spice!",
         "Adds: New Cam Angle, Roulette Refinements, Moveset Presets, Buck Breaker, Busta-Chan Jive, and "
         "Misqu\xC3\xA9 & Risqu\xC3\xA9.",
         18},
        {"SLEAZY MODE", "Sleazy Mode", "...is Now Selectable! For the girl who just wants to have fun.",
         "Adds: Cunt Punter, Tension - Off the Cuff, and Wild Curry Funk.", 15},
    };
    const Text kCustomText = {"CUSTOM", "Custom", "You know what you like.", "Resets with a preset.", 1};

    const Text &text(int preset) { return preset >= 0 && preset < kCount ? kText[preset] : kCustomText; }

    namespace
    {
        // Each option a preset sets, with its value in Classic, Remastered, Rearmed and Sleazy Mode (each includes the
        // one before it). Sub-options count only where the preset's own value of their parent shows them (Classic's
        // controller style has no Modern: Auto); HUD Layout not at 4:3 (Classic: its row is not shown there).
        // Menu Width and Roulette Timer stay the player's (K: Stretched and Double time are in no preset).
        struct Entry
        {
            const char *id;
            const char *value[kCount];
        };
        constexpr Entry kTable[] = {
            {"Fps60", {"0", "1", "1", "1"}},
            {"ControlStyle", {"classic", "hold", "hold", "hold"}}, // Remastered up: Modern: Hold (K, Nov 19; Toggle optional)
            {"ModernAuto", {"1", "1", "1", "1"}},     // Toggle's Auto: no preset uses Toggle, so never counted
            {"ModernAutoHold", {"1", "1", "1", "1"}}, // the same [Game] ModernAuto key under Hold
            {"HoldMode", {"brawl", "brawl", "brawl", "brawl"}}, // Hold with Auto = Hold to Brawl (K, Nov 20)
            {"KbmControlStyle", {"classic", "modern", "modern", "modern"}},
            {"KbmModernAuto", {"0", "0", "0", "0"}},
            {"AspectRatio", {"4:3", "auto", "auto", "auto"}},
            {"CameraPreset", {"ps2", "wider", "action", "action"}},
            {"HudLayout", {"4:3", "16:9", "16:9", "16:9"}},
            {"SkipIntro", {"0", "1", "1", "1"}},
            {"UnlockHard", {"0", "1", "1", "1"}},
            {"RouletteRatchet", {"0", "1", "1", "1"}},
            {"L3Dash", {"0", "1", "1", "1"}}, // Remastered since Nov 16 (K)
            {"RouletteTargetCycle", {"0", "0", "1", "1"}},
            {"RouletteNoOrbs", {"0", "0", "1", "1"}},
            {"SwayBar", {"0", "0", "1", "1"}},
            {"StyleSwitching", {"0", "0", "1", "1"}},
            {"StyleSignal", {"1", "1", "1", "1"}},
            {"BuckBreaker", {"0", "0", "1", "1"}},
            {"NpcPoses", {"0", "0", "1", "1"}},
            {"SeeThrough", {"0", "0", "1", "1"}},
            {"SprintTraversal", {"0", "0", "1", "1"}},
            {"TensionPlacable", {"0", "0", "0", "1"}},
            {"OffTheCuff", {"1", "1", "1", "1"}},
            {"BallBusterWomen", {"0", "0", "0", "1"}},
            {"LiftChairs", {"0", "0", "0", "1"}},
            {"Cash", {"100", "100", "115", "115"}},
            {"Money", {"100", "100", "115", "115"}},
        };

        bool same(const std::string &a, const char *b)
        {
            if (a.size() != std::strlen(b))
                return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                    return false;
            return true;
        }

        const Entry *entryOf(const char *id)
        {
            for (const Entry &e : kTable)
                if (std::strcmp(e.id, id) == 0)
                    return &e;
            return nullptr;
        }

        // Whether the preset's own values show this option (its parent's value; HUD Layout: a wide aspect).
        bool counts(const settings::Setting &s, int preset)
        {
            if (std::strcmp(s.id, "HudLayout") == 0)
                return !same(entryOf("AspectRatio")->value[preset], "4:3");
            if (!s.parent)
                return true;
            const Entry *parent = entryOf(s.parent);
            if (!parent)
                return true;
            const char *value = parent->value[preset];
            return s.parentValue ? same(value, s.parentValue) : std::strcmp(value, "0") != 0;
        }

        // A stored value against the preset's: numbers (Money, Cash) as numbers, words as words.
        bool matches(const std::string &stored, const char *wanted)
        {
            if (same(stored, wanted))
                return true;
            char *end = nullptr;
            const double a = std::strtod(stored.c_str(), &end);
            if (stored.empty() || *end)
                return false;
            const double b = std::strtod(wanted, &end);
            return !*end && std::fabs(a - b) < 0.01;
        }
    }

    bool controls(const settings::Setting &setting) { return entryOf(setting.id) != nullptr; }

    int current(const IniFile &ini)
    {
        for (int preset = 0; preset < kCount; ++preset)
        {
            bool all = true;
            for (const Entry &e : kTable)
            {
                const settings::Setting *s = settings::find(e.id);
                if (!s || !counts(*s, preset))
                    continue;
                if (!matches(settings::value(ini, *s), e.value[preset]))
                {
                    all = false;
                    break;
                }
            }
            if (all)
                return preset;
        }
        return kCustom;
    }

    std::vector<const settings::Setting *> write(IniFile &ini, int preset)
    {
        std::vector<const settings::Setting *> changed;
        if (preset < 0 || preset >= kCount)
            return changed;
        for (const Entry &e : kTable)
        {
            const settings::Setting *s = settings::find(e.id);
            if (!s || !*s->key)
                continue;
            // A hidden sub-option keeps the player's value (Classic leaves Modern: Auto as it was); HUD Layout is
            // written at 4:3 too (it shows again at a wider aspect).
            if (s->parent && !counts(*s, preset))
                continue;
            if (matches(settings::value(ini, *s), e.value[preset]))
                continue;
            if (std::strcmp(s->key, "CameraPreset") == 0)
                settings::writeCameraPreset(ini, e.value[preset]);
            else
                ini.set(s->section, s->key, e.value[preset]);
            changed.push_back(s);
        }
        return changed;
    }

    std::string photoPath(int photo)
    {
        char path[48];
        std::snprintf(path, sizeof(path), "id/photo/orivia_tex%03d.tm2", photo);
        return path;
    }

    bool decodePhoto(const std::vector<uint8_t> &tm2, int &width, int &height, std::vector<uint8_t> &rgba)
    {
        // TIM2: "TIM2", u8 version, u8 alignment (0: 16 bytes, 1: 128), u16 pictures; the first picture's header at
        // 0x10 (0x80 aligned): u32 total, clut, image sizes, u16 header size, u16 CLUT colours, u8 format, mipmaps,
        // CLUT type, image type (5 = 8 bits), u16 width, height; the pixels after the header, the CLUT after them.
        const auto u8 = [&](size_t o) { return o < tm2.size() ? tm2[o] : uint8_t{0}; };
        const auto u16 = [&](size_t o) { return static_cast<uint32_t>(u8(o) | (u8(o + 1) << 8)); };
        const auto u32 = [&](size_t o) { return u16(o) | (u16(o + 2) << 16); };
        if (tm2.size() < 0x40 || std::memcmp(tm2.data(), "TIM2", 4) != 0)
            return false;
        const size_t header = u8(5) == 1 ? 0x80u : 0x10u;
        const uint32_t imageSize = u32(header + 8), headerSize = u16(header + 12), colours = u16(header + 14);
        const uint8_t clutType = u8(header + 18), imageType = u8(header + 19);
        const uint32_t w = u16(header + 20), h = u16(header + 22);
        constexpr uint32_t kLeft = 0, kTop = 13, kRight = 295, kBottom = 216; // the picture in the texture
        if (imageType != 5 || w < kRight || h < kBottom || colours < 256 || imageSize < w * h)
            return false;
        const size_t pixels = header + headerSize, clut = pixels + imageSize;
        const uint32_t clutFormat = clutType & 0x3Fu; // 1: 16 bits (A1B5G5R5), 2: 24 bits, 3: 32 bits
        const size_t entry = clutFormat == 1 ? 2u : clutFormat == 2 ? 3u : clutFormat == 3 ? 4u : 0u;
        if (!entry || clut + 256u * entry > tm2.size())
            return false;
        uint8_t palette[256][4];
        for (uint32_t i = 0; i < 256; ++i)
        {
            // CSM1 (bit 7 clear): stored in the GS's order, blocks of 8 entries 8..15 and 16..23 swapped in each 32.
            const uint32_t at = (clutType & 0x80u) ? i : ((i & 0xE7u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1));
            const size_t o = clut + at * entry;
            if (entry == 2)
            {
                const uint32_t c = u16(o);
                palette[i][0] = static_cast<uint8_t>(((c & 0x1Fu) * 255u) / 31u);
                palette[i][1] = static_cast<uint8_t>((((c >> 5) & 0x1Fu) * 255u) / 31u);
                palette[i][2] = static_cast<uint8_t>((((c >> 10) & 0x1Fu) * 255u) / 31u);
            }
            else
                for (int c = 0; c < 3; ++c)
                    palette[i][c] = u8(o + static_cast<size_t>(c));
            palette[i][3] = 255u; // a photo: opaque
        }
        width = static_cast<int>(kRight - kLeft);
        height = static_cast<int>(kBottom - kTop);
        rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
        uint8_t *out = rgba.data();
        for (uint32_t y = kTop; y < kBottom; ++y)
            for (uint32_t x = kLeft; x < kRight; ++x, out += 4)
                std::memcpy(out, palette[u8(pixels + static_cast<size_t>(y) * w + x)], 4);
        return true;
    }
}
