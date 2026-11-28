// Mouse in the game's own menus: the game side (menu_mouse.h; K, Nov 28, claude/menu-mouse.md).
#include "kamite/menu_mouse.h"

#include "kamite/log.h"
#include "kamite/paths.h"
#include "kamite/test_mode.h"
#include "ps2_host_input.h"
#include "runtime/gs/gs_pcsx2_backend.h"
#include "runtime/ps2_memory.h" // PS2_RAM_MASK

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

        uint32_t g_watch = 0u;     // the entry watched
        Rect g_drawn;              // this frame's draws of it (y relative to its +0x3C)
        bool g_drawnValid = false;
        Rect g_shape;              // the last frame's
        bool g_shapeValid = false;

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
                                                                    << ": the menus' mouse works without a visible pointer");
            return;
        }
        const std::u8string path = file.u8string();
        ps2_host_input::setMenuPointerImage(std::string(path.begin(), path.end()).c_str(), kHotU, kHotV, kHeight);
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

    void watch(uint32_t entry)
    {
        if (entry != g_watch)
        {
            g_watch = entry;
            g_drawnValid = g_shapeValid = false;
        }
        else if (g_drawnValid)
        {
            // The draws since the last call become its shape (none: the last one stays).
            g_shape = g_drawn;
            g_shapeValid = true;
            g_drawnValid = false;
        }
    }

    void onDraw(const uint8_t *rdram, uint32_t entry, float x0, float x1, float y0, float y1)
    {
        if (g_watch == 0u || entry != g_watch)
            return;
        float ey;
        std::memcpy(&ey, rdram + ((entry + 0x3Cu) & PS2_RAM_MASK), sizeof(ey));
        if (!std::isfinite(ey) || !std::isfinite(x0) || !std::isfinite(x1) || !std::isfinite(y0) || !std::isfinite(y1))
            return;
        const Rect r{x0, y0 - ey, x1, y1 - ey};
        if (!g_drawnValid)
        {
            g_drawn = r;
            g_drawnValid = true;
            return;
        }
        g_drawn.x0 = std::fmin(g_drawn.x0, r.x0);
        g_drawn.y0 = std::fmin(g_drawn.y0, r.y0);
        g_drawn.x1 = std::fmax(g_drawn.x1, r.x1);
        g_drawn.y1 = std::fmax(g_drawn.y1, r.y1);
    }

    bool shape(Rect &out)
    {
        if (!g_shapeValid || g_shape.x1 - g_shape.x0 < 1.0f || g_shape.y1 - g_shape.y0 < 1.0f)
            return false;
        out = g_shape;
        return true;
    }
}
