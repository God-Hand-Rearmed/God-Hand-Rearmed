#pragma once

// The OPTIONS screen's text and cursor as a list for the renderer's overlay (pcsx2gs_set_screen). No game state here:
// options_screen.cpp fills a View each frame and sends what build() makes. The look of the game's own Options screen,
// measured from the disc (claude/rearmed-options.md "Style of the original"), in the layout's units: x in 512ths of a
// 4:3 picture (one unit = 7/6 of a line), y in its 448 lines.

#include <cstdint>
#include <string>
#include <vector>

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define KAMITE_HAS_SCREEN_OVERLAY 1
#endif

namespace kamite::options_overlay
{
    struct Row
    {
        const char *name;
        const char *help; // two lines at most ('\n')
    };

    // A category's page: its options under headers (the launcher's category names, which the cursor skips).
    struct PageRow
    {
        const char *name = "";
        const char *help = "";                // two lines at most
        bool header = false;
        bool sub = false;                     // a sub-option, joined to the option above by an elbow. The caller
                                              // lists a sub-option only while its parent is on (K)
        const char *const *choices = nullptr; // up to 3 are shown in a row; more as < value >
        int choiceCount = 0;
        int choice = 0;
        bool list = false;                    // < value > even with 3 or fewer (long words: Display Mode)
        // A slider instead (valueCount > 0): choice indexes these numbers, shown with the unit beside a bar drawn like
        // the enemies' health bar: the number's share of the last (the largest).
        const int *values = nullptr;
        int valueCount = 0;
        const char *unit = "%";
    };

    struct Page
    {
        const char *name = ""; // the category (its title replaces OPTIONS while the page is open)
        const PageRow *rows = nullptr;
        int rowCount = 0;
    };

    // CONTROLS > BUTTON MAPPING (mapping_page.cpp fills it): the pad's picture in its flames and, in a column either
    // side, a label for each button joined to it by a line, in the look of the game's own controller page
    // (claude/button-mapping.md: its panels, lines and underscores, measured from the disc).
    struct MappingLine
    {
        float px = 0.0f, py = 0.0f; // the button on the pad's picture (fractions of its width and height)
        float reach = 0.0f;         // the line stops this far short of it (a fraction of the picture's height)
        bool via = false;           // the line bends at (vx, vy) on its way (fractions of the picture)
        float vx = 0.0f, vy = 0.0f;
        bool right = false;         // the right-hand column (lines are listed left column first, each top to bottom)
        // The back-button block under the pad (stage 4: a paddle; no leader, px / py unused): its column (0 left,
        // 1 right) and row (0 top). Listed after the columns' lines.
        bool block = false;
        int blockX = 0, blockY = 0;
        bool fixed = false;         // not remappable (Start, the sticks, the d-pad): drawn muted
        int mirror = -1;            // left column: the right-hand line it lines up with (L2 with R2 ...), -1 none
        const char *icon = "";      // the button's prompt image
        // The label's lines: the action's, then the menu's word. A line in `ends` (bit i) ends a function, so an
        // underline runs under it (the original: ATTACK / THROW ITEM / CONFIRM each underlined, UNLEASH / THE GODHAND one
        // wrapped phrase); the last line's is the panel's underscore.
        const char *text[4] = {"", "", "", ""};
        int count = 0;
        uint8_t ends = 0;
        const char *inlineIcon = ""; // a \x03 in a text line is drawn as this picture (SAVE / LOAD's "+ [Start] LOAD")
    };
    // An item of the BUTTON MAPPING page's drum: the icon of the button that has the action now, the action's short name.
    struct MappingDrumItem
    {
        const char *icon = "";
        const char *text = "";
        const char *arrow = "", *arrowLit = ""; // a picture after the text (the ATTACKs' arrows), the hovered item's look
    };
    struct MappingView
    {
        const char *pad = "", *flames = ""; // the pad's picture and its flame outline (round it, flamesMargin out)
        const char *leader = "", *leaderLit = "";         // the lines from the buttons (tapering to the button)...
        const char *underscore = "", *underscoreLit = ""; // ...and along the panels' bottoms (normal, selected)
        const char *panel = "";             // the panels' wash (opaque at the outer end)
        float padAspect = 4.0f / 3.0f;      // the picture's width / height
        float flamesMargin = 0.0f;          // a fraction of the picture's height
        const float *silhouette = nullptr;  // the pad's left and right ends per band from the top (fractions of the
        int bands = 0;                      // width; negative: an empty band)
        const MappingLine *lines = nullptr;
        int lineCount = 0;
        const char *padNote = ""; // one help line under the pad ("Back Buttons: Use Steam Input")
        int cursor = -1;         // the selected line (-1: none, the read-only layout)
        const char *notice = ""; // the help band's text; \x01 / \x02 in it are drawn as noticeIcons[0] / [1]
        const char *noticeIcons[2] = {"", ""};
        // The drum, open over the cursor's line (roulette style: the hovered item full size, two each way smaller and
        // fading, curving toward the pad): its items and where it has turned to (an item index; between two while it
        // turns; any number, taken round the item count).
        const MappingDrumItem *drum = nullptr;
        int drumCount = 0;
        float drumPos = 0.0f;
        // The hints (K, Nov 14): Reset on a line of its own above the others (so it never moves), then confirm / cancel
        // with these words over View::confirmHint / cancelHint's art ("" = that hint left out).
        const char *resetArt = "", *resetWord = "";
        const char *confirmWord = "", *cancelWord = "";
    };
    // CONTROLS > BUTTON MAPPING with the keyboard (keys_page.cpp fills it; K, Nov 16): two columns of rows, each the
    // action's key picture and its name, in the BUTTON MAPPING labels' look (their panels and underscores).
    struct KeysRow
    {
        const char *icon = "";    // the key's picture ("" = none: keyText is written in its place)
        const char *keyText = ""; // the key's name when there is no picture ("" with no icon: unbound, an em dash)
        const char *text = "";    // the action
        const char *arrow = "", *arrowLit = ""; // a picture after the text (the ATTACKs' arrows), the selected look
        bool fixed = false;       // not rebindable here: drawn muted
    };
    struct KeysView
    {
        const char *underscore = "", *underscoreLit = "", *panel = ""; // the BUTTON MAPPING page's
        const KeysRow *rows = nullptr;
        int rowCount = 0;
        int leftCount = 0;        // rows in the left column (the rest on the right)
        int cursor = -1;
        bool capture = false;     // the cursor's row waits for a key (its picture blinks out)
        const char *notice = "";  // the help band's text; \x01 / \x02 drawn as noticeIcons[0] / [1]
        const char *noticeIcons[2] = {"", ""};
        const char *resetArt = "", *resetWord = "";
        const char *confirmWord = "", *cancelWord = "";
    };

    // OPTIONS > PRESETS (K, Nov 16; claude/presets-design.md): a carousel. "< NAME >" over the preset's photo (the
    // pause menu's Olivia pictures, decoded from the disc) at the pause menu's own size, "IN USE" under the name when it
    // is the one in effect, the tagline on one line under the photo and the preset's text in the help band. Left / right
    // turn it: the name and photo slide out streaked and the next ones slide in.
    struct PresetsView
    {
        const char *name = "";
        const char *art = "";            // the photo (PNG path; "" = none)
        float artAspect = 295.0f / 203.0f;
        bool inUse = false;
        const char *inUseWord = "IN USE";
        const char *tagline = "";
        const char *help = "";           // the help band (two lines at most: fitPresetText wraps it)
        // Text sizes in lines (0 = the help text's): fitPresetText's.
        float helpCap = 0.0f, taglineCap = 0.0f;
        bool asking = false;             // "Replace your custom settings?": YES / NO in place of the tagline
        bool yes = false;
        const char *question = "";
        // A turn under way: the preset turned away from (its name and photo), dir +1 = turned right (the new one comes
        // in from the right), t 0..1 through kPresetTurnSeconds. dir 0 = still.
        const char *fromName = "", *fromArt = "";
        int turnDir = 0;
        float turnT = 1.0f;
    };
    inline constexpr float kPresetTurnSeconds = 0.24f;

    // K's fit rule (Nov 16): the band's text shrinks from the help size until it wraps to two lines at the help's
    // width (wrapped: its lines joined by '\n'), the tagline until it fits on one line across the carousel. Measured
    // with the help font's own widths.
    struct PresetFit
    {
        std::string band;
        float bandCap = 0.0f, taglineCap = 0.0f;
    };

    // A button hint in the strip under the help band ("[A] Confirm"): the button's images (PNG paths: one, or two for
    // Classic's two confirm buttons; null = none) and the word.
    struct Hint
    {
        const char *art[2] = {nullptr, nullptr};
        const char *word = "";
        float squeeze = 1.0f; // the word narrower than the help's (Button Mapping's Reset over Back)
    };

    struct View
    {
        float aspect = 16.0f / 9.0f; // the picture's
        float fade = 1.0f;           // the screen's fades (its backdrop's alpha, the game's fade to and from black)
        bool pause = true;           // the pause menu's screen (the title's has fewer rows, spaced wider)
        const Row *rows = nullptr;
        int rowCount = 0;
        int cursor = 0;
        int confirmRow = -1;         // a Yes / No beside this row (-1 = none)
        bool confirmYes = false;
        const char *question = "";   // the help box while asking
        // The category under the list's cursor: on 16:10 and wider it is shown beside the list, joined to it by a
        // bracket while open; narrower, an open page replaces the list.
        const Page *page = nullptr;
        bool inPage = false; // the cursor is in the page
        int pageCursor = 0;  // its row
        int pageScroll = 0;  // the first row shown
        // The menu buttons' hints, laid out from the strip's right end: confirm, then cancel (K, Nov 13: as the game's
        // load menu words them, in the help text's look, on the list, in pages and while asking Yes / No).
        Hint confirmHint, cancelHint;
        // While a page is open: L1 / R1 beside its title, each with the neighbouring category's name outside it
        // (K, Nov 13). A side with no neighbour (word "") shows nothing.
        Hint prevHint, nextHint;
        // CONTROLS > BUTTON MAPPING open: the whole picture is that page (title CONTROLS, no list, no L1 / R1).
        const MappingView *mapping = nullptr;
        // The keyboard's page instead (the same frame: title CONTROLS, no list).
        const KeysView *keys = nullptr;
        // The PRESETS category's carousel in place of `page`'s rows (page still names the category).
        const PresetsView *presets = nullptr;
        // L1 / R1 just switched the open category (K, Nov 14: as the title screen's menu words): the old title
        // (`titleFrom`) streaks off blurred and the new one streaks in from the other side and sharpens; titleDir +1 =
        // R1 (the next category, coming in from the right), titleT 0..1 through it. titleDir 0 = still.
        const char *titleFrom = "";
        int titleDir = 0;
        float titleT = 0.0f;
    };
    // The page sits beside the list from this aspect on (16:10 and wider); narrower, an open page replaces the list.
    inline constexpr float kSplitAspect = 1.595f;
    // How long a title switch (View::titleDir) takes, in seconds.
    inline constexpr float kTitleSwitchSeconds = 0.24f;

    // The fonts (resources/fonts) and the titles' folder (resources/options: title_options.png, title_<category>.png),
    // set once at start.
    void setAssets(const std::string &labelFont, const std::string &helpFont, const std::string &titleDir);
    // The scroll bar's pieces (resources/options/scroll_track.png, scroll_thumb.png: tools/options/make_scrollbar.py).
    void setScrollBar(const std::string &track, const std::string &thumb);
    // The sliders' fill (resources/options/slider_fill.png: tools/options/make_slider.py).
    void setSliderFill(const std::string &fill);
    // A title image's letters (title_<name>.png, name in lower case): their left and right pixel columns in its canvas,
    // measured once at start (the glow left out), so the L1 / R1 hints sit beside the letters whatever their width.
    void setTitleLetters(const std::string &name, float left, float right);
    // The rows a page shows at once (scrolling keeps the cursor among them).
    int pageRowsShown(const Page &page, int scroll);

    // The PRESETS band text fitted to the picture. Without the renderer's overlay (no pcsx2gs_api.h: the built-in GS)
    // nothing draws it, and the band comes back unwrapped at the help text's size.
    PresetFit fitPresetText(const char *tagline, const char *band, float aspect);
#if defined(KAMITE_HAS_SCREEN_OVERLAY)
    void build(const View &view, std::vector<Pcsx2GsScreenItem> &items, std::string &strings);
    // A line of the help font's text at this cap height (lines), in layout units (calibrated against the renderer).
    float helpTextWidth(const char *text, size_t length, float cap);
    // Black over the whole picture at this strength (0..1): the fade behind an OPTIONS change.
    void appendBlackout(float alpha, std::vector<Pcsx2GsScreenItem> &items);
#endif
}
