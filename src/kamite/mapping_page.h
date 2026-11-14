#pragma once

// CONTROLS > BUTTON MAPPING: what the page shows (options_overlay::MappingView), without game state, so the preview tool
// (tools/options/overlay_preview.py --page mapping) builds the page the game shows. The design, the original
// controller page's measurements and the stages: claude/button-mapping.md.
//
// The page is positional: each line is a physical button of the controller (its place on the default layout: South is
// where the PS2 cross is). Its label is the play action of the PS2 button bound there (controls.ini, as the prompts
// read it: the first controller binding), then the menus' word for that place (Menu Controls: the menus are pinned
// to physical buttons and never move with the mapping).

#include "kamite/options_overlay.h"

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace kamite
{
    class IniFile;
}

namespace kamite::mapping_page
{
    // The places. The first ten are the remappable ones, in ps2_host_input::prompts::Input's order (place i is where
    // that PS2 button sits on the default layout). Select and the back paddles are the extra buttons (stage 4, K Nov 14:
    // each holds nothing, a copy of an action or a save-state command); the paddles sit in a block under the pad.
    enum Place : uint8_t
    {
        South, East, West, North, L1, R1, L2, R2, L3, R3, // remappable
        Start, Select, DPad, LStick, RStick,
        L4, R4, L5, R5, // the back paddles (SDL's left / right paddle 1 and 2)
        PlaceCount
    };
    constexpr int kRemappable = 10;
    constexpr uint8_t kNone = 0xFFu;
    const char *placeName(Place place); // "south", "l1"... (the data files' names)
    // The extra buttons in ps2_host_input::extra::Button's order: Select, L4, R4, L5, R5.
    constexpr int kExtraCount = 5;
    constexpr Place kExtraPlaces[kExtraCount] = {Select, L4, R4, L5, R5};
    int extraIndex(uint8_t place); // -1: not an extra button

    // A pad's picture and where its buttons are (resources/options/mapping/pad_<name>.txt, tools/options/make_pad_art.py).
    // The labels never move with the pad (K, Nov 14): the columns' rows are the same for every pad (kLeft / kRight in
    // mapping_page.cpp) and only the lines change to reach its buttons.
    struct PadArt
    {
        std::string image, flames; // file names beside the data file
        float width = 0.0f, height = 0.0f, flamesMargin = 0.0f; // the render's pixels
        struct Point
        {
            float x = 0.0f, y = 0.0f, reach = 0.0f;
            float viaX = 0.0f, viaY = 0.0f; // a bend on the line's way (via), when hasVia
            bool set = false, hasVia = false;
        };
        std::array<Point, PlaceCount> points{};
        std::vector<float> silhouette;    // left, right per band
        bool ok = false;
    };
    bool loadArt(const std::string &path, PadArt &out, std::string &error);
    // The picture a prompt set's pad gets (prompt_hd's folders: "xbox360", "ps3", "switch", "steamdeck"): "xbox",
    // "playstation", "nintendo", "steam" (K's Steam Controller render, for the Steam Controller and the Deck).
    const char *padFor(const std::string &setFolder);

    // What the labels depend on besides the mapping.
    struct Options
    {
        bool clickStickDash = false; // [Game] L3Dash: L3 dashes
        bool modern = false;         // [Game] ControlStyle=modern: R3 switches brawling / free camera, the right stick
        bool presets = false;        // [Game] StyleSwitching: the d-pad switches styles
        int menuStyle = 1;           // 0 Classic, 1 Western, 2 Japanese (menu_buttons::Style)
    };
    Options optionsFrom(const IniFile &ini);

    // An extra button's command (as ps2_host_input::extra::Command).
    enum class Command : uint8_t
    {
        None,
        QuickSave,
        QuickLoad,
        SaveLoad
    };
    struct Extra
    {
        Command command = Command::None;
        uint8_t copy = kNone; // the PS2 button (prompts::Input 0..9) it is a copy of (when no command)
    };

    struct State
    {
        // The PS2 button (prompts::Input 0..9) whose first controller binding is each remappable place; kNone: none.
        std::array<uint8_t, kRemappable> onPlace{};
        std::array<Extra, kExtraCount> extras{}; // kExtraPlaces' order; Select holds SaveLoad by default
        uint8_t paddles = 0;   // the back paddles the controller has (bit = extraIndex: 1 L4, 2 R4, 3 L5, 4 R5)
        bool readOnly = false; // a layout the page cannot edit (set in the launcher)
    };
    State defaultState();

    // Stage 3, editing (K, Nov 14): the drum on A over a line lists what its button can take, each with the icon of the
    // button that has it now. On a standard button: every remappable PS2 button's action, in prompts::Input order;
    // picking one swaps the two buttons' actions. On an extra button (stage 4): nothing, the actions as copies (the
    // action stays on its own button too), then the commands; picking one assigns it. Reset (the left face button)
    // takes a second press; any other press disarms it.
    enum class Armed : uint8_t
    {
        None,
        Reset
    };
    struct Edit
    {
        bool drum = false;
        int target = 0;   // the drum's item (any number: taken round the item count)
        float pos = 0.0f; // where it has turned to (eases toward target)
        Armed armed = Armed::None;
    };
    // The drum's items on `place`, in order: an action (input < kRemappable: a swap on a standard button, a copy on an
    // extra one) or, on an extra button, a command (input kNone; Command::None = nothing). `start`: the item it opens on
    // (what the button holds now).
    struct DrumEntry
    {
        uint8_t input = kNone;
        Command command = Command::None;
    };
    std::vector<DrumEntry> drumEntries(const State &state, const Options &options, uint8_t place, int &start);
    // The place that has a PS2 button now (kNone: none).
    uint8_t placeOfInput(const State &state, uint8_t input);

    // The page for the overlay; the strings and lines live here.
    struct PageData
    {
        std::deque<std::string> text;
        std::vector<options_overlay::MappingLine> lines;
        std::vector<uint8_t> places; // each line's place
        std::vector<float> silhouette;
        std::vector<options_overlay::MappingDrumItem> drum;
        options_overlay::MappingView view;

        const char *keep(std::string value)
        {
            text.push_back(std::move(value));
            return text.back().c_str();
        }
    };
    // root: the folder resources/ is in (the exe's). setFolder: the prompt set's folder (prompt_hd). cursor: a place,
    // kNone for none.
    void build(PageData &d, const PadArt &art, const std::string &artDir, const std::string &root, const std::string &setFolder,
               const State &state, const Options &options, uint8_t cursor, const Edit &edit = {});
    // The cursor's next place among the remappable lines: up / down within its column, left / right across to the line
    // nearest its height. The same place when there is none that way.
    uint8_t move(const PageData &d, uint8_t cursor, int dx, int dy);
    // The first remappable line (top of the left column).
    uint8_t firstLine(const PageData &d);

    // The read-only layout's help (K): "This layout was set in the launcher." / "Change it there or press X to reset."
    // (X: the left face button's name in the prompt set); after one press (stage 3) the second line reads
    // "Press X again to confirm and reset."
    constexpr const char *kReadOnlyNotice = "This layout was set in the launcher.";
    // A Steam pad without reported paddles (Steam Input gives the game a plain pad), under the pad (K, Nov 14).
    constexpr const char *kSteamPaddleNote = "Back Buttons: Use Steam Input";
    constexpr const char *kHintReset = "Reset", *kHintSelect = "Select", *kHintBack = "Back", *kHintSwap = "Swap",
                         *kHintAssign = "Assign", *kHintCancel = "Cancel";
}
