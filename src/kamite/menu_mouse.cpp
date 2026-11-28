// Mouse in the game's own menus: the game side (menu_mouse.h; K, Nov 28, claude/menu-mouse.md).
#include "kamite/menu_mouse.h"

#include "kamite/log.h"
#include "kamite/paths.h"
#include "kamite/test_mode.h"
#include "ps2_host_cursor.h"
#include "ps2_host_input.h"
#include "runtime/gs/gs_pcsx2_backend.h"
#include "runtime/ps2_memory.h" // PS2_RAM_MASK

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>

namespace kamite::menu_mouse
{
    namespace
    {
        constexpr uint32_t kScreenWidth = 0x3C1180u; // the game's 2D width (512; hud_layout.cpp)
        constexpr float kScreenHeight = 448.0f;       // the game's 2D height (NTSC)
        // K's hand (cursor_hand.png, 474 x 556): the hot spot at the index fingertip ~(80, 8) (K, Nov 28), drawn this
        // share of the picture's height (~79 px at 1440).
        constexpr float kHotU = 80.0f / 474.0f, kHotV = 8.0f / 556.0f, kHeight = 0.055f;

        uint32_t g_lastMoves = 0u;
        bool g_warnedRect = false;

        // The watched entries (watch / onDraw / drawn), relative to each one's draw position.
        struct Watched
        {
            uint32_t entry = 0u;
            Rect now, last;          // this frame's draws, the last frame's with any
            bool nowValid = false, lastValid = false;
        };
        constexpr int kWatchMax = 8;
        Watched g_watched[kWatchMax];
        int g_watchCount = 0;

        bool caseRun()
        {
            return test_mode::recording() || test_mode::replayFrame() != test_mode::kReplayIdle;
        }

        float screenWidth(const uint8_t *rdram)
        {
            int32_t width;
            std::memcpy(&width, rdram + (kScreenWidth & PS2_RAM_MASK), sizeof(width));
            return width > 0 && width < 4096 ? static_cast<float>(width) : 640.0f;
        }
    }

    void install()
    {
        const std::filesystem::path file = paths::resources() / "images" / "cursor_hand.png";
        std::error_code error;
        if (!std::filesystem::is_regular_file(file, error))
        {
            KM_LOG(log::Category::Input, log::Level::Warn, "[menu mouse] no pointer image at " << file.string()
                                                                    << ": the OS arrow stays");
            return;
        }
        const std::u8string path = file.u8string();
        ps2_host_cursor::setImage(std::string(path.begin(), path.end()), kHotU, kHotV, kHeight);
    }

    Hover hover(const uint8_t *rdram, const Rect *items, int count)
    {
        if (caseRun())
            return {};
        const ps2_host_input::MenuPointer pointer = ps2_host_input::menuPointer();
        const bool moved = pointer.moves != g_lastMoves;
        g_lastMoves = pointer.moves;
        float rect[4];
        if (!GSPcsx2Backend::PresentRect(rect))
        {
            if (!g_warnedRect)
            {
                g_warnedRect = true;
                KM_LOG(log::Category::Input, log::Level::Warn, "[menu mouse] the GS gives no picture rectangle: no hover");
            }
            ps2_host_input::setMenuPointer(false);
            return {};
        }
        // Window pixels -> the game's 2D through the rectangle the picture is shown in (4:3 with bars, or stretched:
        // either way the menu's 2D spans the whole picture).
        const float x = (pointer.x - rect[0]) / (rect[2] - rect[0]) * screenWidth(rdram);
        const float y = (pointer.y - rect[1]) / (rect[3] - rect[1]) * kScreenHeight;
        Hover hover;
        hover.x = x, hover.y = y;
        hover.active = pointer.active, hover.leftDown = pointer.leftDown;
        for (int i = 0; items && i < count; ++i)
            if (x >= items[i].x0 && x < items[i].x1 && y >= items[i].y0 && y < items[i].y1)
            {
                hover.item = i;
                break;
            }
        hover.take = pointer.active && hover.item >= 0 && (moved || pointer.leftDown);
        ps2_host_input::setMenuPointer(pointer.active && hover.item >= 0);
        return hover;
    }

    void watch(const uint32_t *entries, int count)
    {
        count = std::clamp(count, 0, kWatchMax);
        Watched kept[kWatchMax];
        for (int i = 0; i < count; ++i)
        {
            kept[i].entry = entries[i];
            for (int j = 0; j < g_watchCount; ++j)
                if (g_watched[j].entry == entries[i])
                {
                    kept[i] = g_watched[j];
                    break;
                }
            // The draws since the last call become the entry's last drawn extent (none: the one before stays).
            if (kept[i].nowValid)
            {
                kept[i].last = kept[i].now;
                kept[i].lastValid = true;
                kept[i].nowValid = false;
            }
        }
        for (int i = 0; i < count; ++i)
            g_watched[i] = kept[i];
        g_watchCount = count;
    }

    void onDraw(const uint8_t *rdram, uint32_t entry, float x0, float x1, float y0, float y1)
    {
        for (int i = 0; i < g_watchCount; ++i)
        {
            Watched &w = g_watched[i];
            if (w.entry == 0u || w.entry != entry)
                continue;
            float dx, dy;
            std::memcpy(&dx, rdram + ((entry + 0x50u) & PS2_RAM_MASK), sizeof(dx)); // IDBaseEnt kDrawX / kDrawY
            std::memcpy(&dy, rdram + ((entry + 0x54u) & PS2_RAM_MASK), sizeof(dy));
            if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(x0) || !std::isfinite(x1) || !std::isfinite(y0) ||
                !std::isfinite(y1))
                return;
            const Rect r{x0 - dx, y0 - dy, x1 - dx, y1 - dy};
            if (!w.nowValid)
            {
                w.now = r;
                w.nowValid = true;
                return;
            }
            w.now.x0 = std::fmin(w.now.x0, r.x0);
            w.now.y0 = std::fmin(w.now.y0, r.y0);
            w.now.x1 = std::fmax(w.now.x1, r.x1);
            w.now.y1 = std::fmax(w.now.y1, r.y1);
            return;
        }
    }

    bool drawn(uint32_t entry, Rect &relative)
    {
        for (int i = 0; i < g_watchCount; ++i)
            if (g_watched[i].entry == entry && g_watched[i].lastValid)
            {
                const Rect &r = g_watched[i].last;
                if (r.x1 - r.x0 < 1.0f || r.y1 - r.y0 < 1.0f)
                    return false;
                relative = r;
                return true;
            }
        return false;
    }
}
