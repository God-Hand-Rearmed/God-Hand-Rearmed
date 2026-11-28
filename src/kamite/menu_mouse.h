#pragma once

#include <cstdint>

// Mouse in the game's own menus (K, Nov 28; claude/menu-mouse.md). The menu keeps its own cursor: hovering an item
// moves the game's cursor there (its own highlight and move sound), a left click begun over an item is the OK button,
// a right click the cancel button, the wheel steps the d-pad. The input side (clicks, wheel, hiding the OS arrow,
// drawing K's hand) is ps2_host_input's setMenuPointer; this is the game side: where the pointer is in the game's 2D
// and which item it is over. Each menu keeps its own table of item rectangles (a menu rebuilt natively later swaps
// only its table). Off while a test case records or replays (the mouse is not in a case).
namespace kamite::menu_mouse
{
    // A rectangle in the game's 2D: x 0..the screen width (0x3C1180, 512), y 0..448.
    struct Rect
    {
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    };

    // main.cpp, once: the pointer's image (resources/images/cursor_hand.png).
    void install();

    struct Hover
    {
        int item = -1;     // the item under the pointer, -1 = none
        bool take = false; // the menu should make it its cursor now (the mouse moved, or the left button is down)
    };
    // A menu's move, each time it runs (game thread), with its items. Keeps the input side's mouse on for ~100 ms.
    Hover hover(const uint8_t *rdram, const Rect *items, int count);

    // A drawn entry's shape, for menus whose item rectangles follow a drawn highlight (game thread): watch(entry) names
    // the cIDBaseEnt to measure; hud_layout reports each 2D draw of it (onDraw, screen units); shape() gives the last
    // frame's extent, x as drawn, y relative to the entry's y (+0x3C). False until it has been drawn.
    void watch(uint32_t entry);
    void onDraw(const uint8_t *rdram, uint32_t entry, float x0, float x1, float y0, float y1);
    bool shape(Rect &out);
}
