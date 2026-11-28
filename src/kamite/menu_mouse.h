#pragma once

#include <cstdint>

// Mouse in the game's own menus (K, Nov 28; claude/menu-mouse.md). The menu keeps its own cursor: hovering an item
// moves the game's cursor there (its own highlight and move sound), a left click begun over an item is the OK button,
// a right click the cancel button, the wheel steps the d-pad. The input side (clicks, wheel) is ps2_host_input's
// setMenuPointer and the pointer itself K's hand (ps2_host_cursor, everywhere); this is the game side: where the
// pointer is in the game's 2D and which item it is over. Each menu keeps its own table of item rectangles (a menu
// rebuilt natively later swaps only its table). Off while a test case records or replays (the mouse is not in a case).
namespace kamite::menu_mouse
{
    // A rectangle in the game's 2D: x 0..the screen width (0x3C1180, 512), y 0..448.
    struct Rect
    {
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    };

    // main.cpp, once, before the launcher: K's hand as the window's pointer (resources/images/cursor_hand.png).
    void install();

    struct Hover
    {
        int item = -1;     // the item under the pointer, -1 = none
        bool take = false; // the menu should make it its cursor now (the mouse moved, or the left button is down)
        float x = 0.0f, y = 0.0f; // the pointer in the game's 2D
        bool active = false, leftDown = false; // ps2_host_input::menuPointer's
    };
    // A menu's move, each time it runs (game thread), with its items. Keeps the input side's mouse on for ~100 ms.
    Hover hover(const uint8_t *rdram, const Rect *items, int count);

    // Drawn entries' extents, for menus whose items are where the game draws them (game thread): watch(entries) names
    // up to 8 cIDBaseEnts each frame; hud_layout reports each 2D draw of them (onDraw, screen units); drawn(entry)
    // gives the extent of the last frame it was drawn in, relative to its draw position (+0x50 / +0x54). False until
    // it has been drawn.
    void watch(const uint32_t *entries, int count);
    void onDraw(const uint8_t *rdram, uint32_t entry, float x0, float x1, float y0, float y1);
    bool drawn(uint32_t entry, Rect &relative);
}
