#include "kamite/mapping_page.h"

#include "kamite/ini_file.h"
#include "kamite/settings_table.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

namespace kamite::mapping_page
{
    namespace
    {
        constexpr const char *kPlaceNames[PlaceCount] = {"south", "east", "west", "north", "l1", "r1", "l2", "r2", "l3",
                                                         "r3", "start", "select", "dpad", "lstick", "rstick", "l4", "r4",
                                                         "l5", "r5"};

        // The original page's wording (claude/button-mapping.md), per PS2 button (prompts::Input 0..9).
        struct Words
        {
            const char *line[2];
            bool wrapped; // the second line continues the first (one function, no underline between)
        };
        constexpr Words kActions[kRemappable] = {
            {{"ATTACK", "THROW ITEM"}, false},   // cross: two functions
            {{"ACTION BUTTON", nullptr}, false}, // circle
            {{"COMBO", nullptr}, false},         // square
            {{"ATTACK", nullptr}, false},        // triangle
            {{"180\xC2\xB0 TURN", nullptr}, false},
            {{"OPEN", "ROULETTE WHEEL"}, true},
            {{"TAUNT", nullptr}, false},
            {{"UNLEASH", "THE GODHAND"}, true},
            {{"DASH", nullptr}, false},          // L3: with Click-Stick Dash
            {{"BRAWL / FREECAM", nullptr}, false}, // R3: with Modern
        };
        constexpr const char *kEmpty = "\xE2\x80\x94"; // an em dash: nothing on this button
        // The drum's short names (K, Nov 14, second pass: short enough to turn round the pad on the left too); an arrow
        // after the two ATTACKs tells them apart by their button's place (K: the game's own arrow shape,
        // tools/options/make_drum_arrows.py, not the font's thin one).
        constexpr const char *kDrumNames[kRemappable] = {"ATTACK", "ACTION", "COMBO", "ATTACK", "180\xC2\xB0 TURN",
                                                         "ROULETTE", "TAUNT", "UNLEASH", "DASH", "FREECAM"};
        constexpr const char *kDrumArrows[kRemappable] = {"arrow_down", nullptr, nullptr, "arrow_up"};
        // The commands' words (K, Nov 14): on the page and on the drum (SAVE / LOAD's page label is two lines, its
        // second with the Start button in it: \x03, MappingLine::inlineIcon).
        constexpr const char *kQuickSave = "QUICK SAVE", *kQuickLoad = "QUICK LOAD", *kSaveLoadDrum = "SAVE / LOAD",
                             *kSaveLoadSecond = "+ \x03 LOAD";

        // Xelu's pack per set (resources/prompts/xelu/<folder>), by place; the sticks are ours (no click arrow).
        struct SetFiles
        {
            const char *folder;
            const char *file[13]; // South .. DPad
        };
        constexpr SetFiles kSets[] = {
            {"xbox360", {"360_A", "360_B", "360_X", "360_Y", "360_LB", "360_RB", "360_LT", "360_RT", "360_Left_Stick_Click",
                         "360_Right_Stick_Click", "360_Start_Alt", "360_Back_Alt", "360_Dpad"}},
            {"ps3", {"PS3_Cross", "PS3_Circle", "PS3_Square", "PS3_Triangle", "PS3_L1", "PS3_R1", "PS3_L2", "PS3_R2",
                     "PS3_Left_Stick_Click", "PS3_Right_Stick_Click", "PS3_Start", "PS3_Select", "PS3_Dpad"}},
            // Nintendo's letters by place: B at the bottom, A on the right, Y on the left, X at the top.
            {"switch", {"Switch_B", "Switch_A", "Switch_Y", "Switch_X", "Switch_LB", "Switch_RB", "Switch_LT", "Switch_RT",
                        "Switch_Left_Stick_Click", "Switch_Right_Stick_Click", "Switch_Plus", "Switch_Minus", "Switch_Dpad"}},
            {"steamdeck", {"SteamDeck_A", "SteamDeck_B", "SteamDeck_X", "SteamDeck_Y", "SteamDeck_L1", "SteamDeck_R1",
                           "SteamDeck_L2", "SteamDeck_R2", "SteamDeck_Left_Stick_Click", "SteamDeck_Right_Stick_Click",
                           "SteamDeck_Menu", "SteamDeck_Inventory", "SteamDeck_Dpad"}},
        };
        const SetFiles &setFiles(const std::string &folder)
        {
            for (const SetFiles &s : kSets)
                if (folder == s.folder)
                    return s;
            return kSets[0];
        }

        // The columns, top to bottom, the same for every pad (K, Nov 14: the labels stay put, the lines reach the
        // buttons): the right as the original's page, the left with Select on top (level with R2: the title sits where
        // Start's row would be mirrored) and the sticks at the bottom like the right's.
        constexpr uint8_t kLeft[] = {Select, L2, L1, DPad, L3, LStick};
        constexpr uint8_t kRight[] = {Start, R2, R1, North, East, South, West, R3, RStick};

        int placeOf(const std::string &name)
        {
            for (int i = 0; i < PlaceCount; ++i)
                if (name == kPlaceNames[i])
                    return i;
            return -1;
        }
    }

    const char *placeName(Place place) { return place < PlaceCount ? kPlaceNames[place] : ""; }

    int extraIndex(uint8_t place)
    {
        for (int i = 0; i < kExtraCount; ++i)
            if (kExtraPlaces[i] == place)
                return i;
        return -1;
    }

    const char *padFor(const std::string &setFolder)
    {
        if (setFolder == "ps3")
            return "playstation";
        if (setFolder == "switch")
            return "nintendo";
        if (setFolder == "steamdeck")
            return "steam";
        return "xbox";
    }

    bool loadArt(const std::string &path, PadArt &out, std::string &error)
    {
        out = PadArt{};
        std::ifstream file(path);
        if (!file)
        {
            error = "cannot open " + path;
            return false;
        }
        std::string line;
        while (std::getline(file, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            std::istringstream in(line);
            std::string word;
            if (!(in >> word) || word[0] == '#')
                continue;
            if (word == "image")
                in >> out.image >> out.width >> out.height;
            else if (word == "flames")
                in >> out.flames >> out.flamesMargin;
            else if (word == "point")
            {
                std::string name;
                PadArt::Point p;
                in >> name >> p.x >> p.y >> p.reach;
                const int place = placeOf(name);
                if (place < 0 || in.fail())
                {
                    error = "bad point line: " + line;
                    return false;
                }
                p.set = true;
                out.points[static_cast<size_t>(place)] = p;
            }
            else if (word == "via")
            {
                std::string name;
                float x = 0.0f, y = 0.0f;
                in >> name >> x >> y;
                const int place = placeOf(name);
                if (place < 0 || in.fail())
                {
                    error = "bad via line: " + line;
                    return false;
                }
                PadArt::Point &p = out.points[static_cast<size_t>(place)];
                p.viaX = x;
                p.viaY = y;
                p.hasVia = true;
            }
            else if (word == "left" || word == "right")
                continue; // older data files listed the columns: the columns are fixed now (kLeft / kRight)
            else if (word == "silhouette")
            {
                int bands = 0;
                in >> bands;
                float v = 0.0f;
                while (in >> v)
                    out.silhouette.push_back(v);
                if (bands <= 0 || out.silhouette.size() != static_cast<size_t>(bands) * 2u)
                    out.silhouette.clear();
            }
        }
        if (out.image.empty() || out.width <= 0.0f || out.height <= 0.0f)
        {
            error = "no image line in " + path;
            return false;
        }
        out.ok = true;
        return true;
    }

    Options optionsFrom(const IniFile &ini)
    {
        Options o;
        const auto on = [&](const char *id) {
            const settings::Setting *s = settings::find(id);
            return s && settings::on(ini, *s);
        };
        o.clickStickDash = on("L3Dash");
        o.presets = on("StyleSwitching");
        if (const settings::Setting *s = settings::find("ControlStyle"))
            o.modern = settings::value(ini, *s) == "modern";
        if (const settings::Setting *s = settings::find("MenuButtons"))
        {
            const std::string v = settings::value(ini, *s);
            o.menuStyle = v == "classic" ? 0 : v == "japanese" ? 2 : 1;
        }
        return o;
    }

    State defaultState()
    {
        State s;
        for (int i = 0; i < kRemappable; ++i)
            s.onPlace[static_cast<size_t>(i)] = static_cast<uint8_t>(i);
        s.extras[0].command = Command::SaveLoad; // Select (K, Nov 14)
        return s;
    }

    uint8_t placeOfInput(const State &state, uint8_t input)
    {
        for (uint8_t place = 0; place < kRemappable; ++place)
            if (state.onPlace[place] == input)
                return place;
        return kNone;
    }

    std::vector<DrumEntry> drumEntries(const State &state, const Options &options, uint8_t place, int &start)
    {
        std::vector<DrumEntry> out;
        start = 0;
        const int extra = extraIndex(place);
        if (place >= kRemappable && extra < 0)
            return out;
        const Extra held = extra >= 0 ? state.extras[static_cast<size_t>(extra)] : Extra{};
        if (extra >= 0)
            out.push_back(DrumEntry{kNone, Command::None}); // nothing (an em dash)
        const uint8_t own = place < kRemappable ? state.onPlace[place] : held.command == Command::None ? held.copy : kNone;
        for (uint8_t input = 0; input < kRemappable; ++input)
        {
            if (placeOfInput(state, input) == kNone)
                continue; // not on a button the page edits
            // An extra button copies only what does something (the stick presses' actions come with their options).
            if (extra >= 0 && ((input == L3 && !options.clickStickDash) || (input == R3 && !options.modern)))
                continue;
            if (input == own)
                start = static_cast<int>(out.size());
            out.push_back(DrumEntry{input, Command::None});
        }
        if (extra >= 0)
            for (const Command command : {Command::QuickSave, Command::QuickLoad, Command::SaveLoad})
            {
                if (held.command == command)
                    start = static_cast<int>(out.size());
                out.push_back(DrumEntry{kNone, command});
            }
        return out;
    }

    void build(PageData &d, const PadArt &art, const std::string &artDir, const std::string &root, const std::string &setFolder,
               const State &state, const Options &options, uint8_t cursor, const Edit &edit)
    {
        d.text.clear();
        d.lines.clear();
        d.drum.clear();
        d.places.clear();
        d.view = options_overlay::MappingView{};
        if (!art.ok)
            return;
        const SetFiles &set = setFiles(setFolder);
        const std::string icons = root + "/resources/prompts/xelu/" + set.folder + "/";
        d.view.pad = d.keep(artDir + "/" + art.image);
        d.view.flames = art.flames.empty() ? "" : d.keep(artDir + "/" + art.flames);
        d.view.leader = d.keep(artDir + "/leader.png");
        d.view.leaderLit = d.keep(artDir + "/leader_lit.png");
        d.view.underscore = d.keep(artDir + "/underscore.png");
        d.view.underscoreLit = d.keep(artDir + "/underscore_lit.png");
        d.view.panel = d.keep(artDir + "/panel.png");
        d.view.padAspect = art.width / art.height;
        d.view.flamesMargin = art.flamesMargin / art.height;
        d.silhouette = art.silhouette;
        d.view.silhouette = d.silhouette.empty() ? nullptr : d.silhouette.data();
        d.view.bands = static_cast<int>(d.silhouette.size() / 2u);

        // The menus' words by place (menu_buttons: Western south confirms, east cancels; Japanese the other way round;
        // Classic as the US release: south and east confirm, north cancels).
        const auto menuWord = [&](uint8_t place) -> const char * {
            switch (options.menuStyle)
            {
            case 0: return place == South || place == East ? "CONFIRM" : place == North ? "CANCEL" : nullptr;
            case 2: return place == East ? "CONFIRM" : place == South ? "CANCEL" : nullptr;
            default: return place == South ? "CONFIRM" : place == East ? "CANCEL" : nullptr;
            }
        };

        // A line's label line; `end`: it ends a function (an underline under it).
        const auto addTo = [](options_overlay::MappingLine &line, const char *text, bool end) {
            if (!text || line.count >= 4)
                return;
            if (end)
                line.ends = static_cast<uint8_t>(line.ends | (1u << line.count));
            line.text[line.count++] = text;
        };
        const auto shown = [&](uint8_t input) {
            return input < kRemappable && !(input == L3 && !options.clickStickDash) && !(input == R3 && !options.modern);
        };
        // An extra button's label (K, Nov 14): a copy reads as its action, a command by name; SAVE / LOAD two lines,
        // the second with the Start button in it; nothing an em dash.
        const std::string startIcon = icons + set.file[Start] + ".png";
        const auto extraWords = [&](options_overlay::MappingLine &line, const Extra &e) {
            switch (e.command)
            {
            case Command::QuickSave: addTo(line, kQuickSave, true); break;
            case Command::QuickLoad: addTo(line, kQuickLoad, true); break;
            case Command::SaveLoad:
                addTo(line, kQuickSave, true);
                addTo(line, kSaveLoadSecond, true);
                line.inlineIcon = d.keep(startIcon);
                break;
            default:
                if (shown(e.copy))
                {
                    const Words &w = kActions[e.copy];
                    addTo(line, w.line[0], !w.wrapped || !w.line[1]);
                    addTo(line, w.line[1], true);
                }
                else
                    addTo(line, kEmpty, true);
                break;
            }
        };

        for (int side = 0; side < 2; ++side)
            for (const uint8_t place : side == 0 ? std::vector<uint8_t>(std::begin(kLeft), std::end(kLeft))
                                                 : std::vector<uint8_t>(std::begin(kRight), std::end(kRight)))
            {
                const PadArt::Point &p = art.points[place];
                if (!p.set)
                    continue;
                options_overlay::MappingLine line;
                line.px = p.x / art.width;
                line.py = p.y / art.height;
                line.reach = p.reach / art.height;
                if (p.hasVia)
                {
                    line.via = true;
                    line.vx = p.viaX / art.width;
                    line.vy = p.viaY / art.height;
                }
                line.right = side == 1;
                const auto add = [&](const char *text, bool end) { addTo(line, text, end); };
                if (place < kRemappable)
                {
                    const uint8_t input = state.onPlace[place];
                    if (shown(input))
                    {
                        const Words &w = kActions[input];
                        add(w.line[0], !w.wrapped || !w.line[1]);
                        add(w.line[1], true);
                    }
                    else
                        add(kEmpty, true); // nothing on it
                    if (const char *word = menuWord(place))
                        add(word, true);
                }
                else if (place == Start)
                    add("MENU", true);
                else if (place == Select)
                    extraWords(line, state.extras[0]);
                else if (place == LStick)
                    add("MOVE", true);
                else if (place == RStick)
                {
                    if (options.modern)
                    {
                        add("EVADE (BRAWLING)", true);
                        add("CAMERA (FREECAM)", true);
                    }
                    else
                        add("EVADE", true);
                }
                else if (place == DPad && options.presets)
                {
                    add("STYLE", false); // one phrase on two lines: the left column is narrow (K, Nov 14)
                    add("SWITCH", true);
                }
                line.fixed = place >= kRemappable && place != Select; // Select is an extra button (stage 4)
                if (line.count == 0)
                    continue; // nothing in play (the d-pad without Moveset Presets): no line, as the original
                if (place == LStick || place == RStick)
                    line.icon = d.keep(artDir + "/stick_" + set.folder + (place == LStick ? "_left.png" : "_right.png"));
                else
                    line.icon = d.keep(icons + set.file[place] + ".png");
                d.lines.push_back(line);
                d.places.push_back(place);
            }
        // The back paddles the controller has (K, Nov 14): a block centred under the pad, the left ones on the left and
        // the right ones on the right, two in a row, four 2x2 (L4 over L5, R4 over R5); K's own icons on every pad.
        int blockRows[2] = {0, 0};
        for (const uint8_t place : {L4, L5, R4, R5})
        {
            const int extra = extraIndex(place);
            if ((state.paddles & (1u << extra)) == 0u)
                continue;
            options_overlay::MappingLine line;
            line.block = true;
            line.right = place == R4 || place == R5;
            line.blockX = line.right ? 1 : 0;
            line.blockY = blockRows[line.blockX]++;
            extraWords(line, state.extras[static_cast<size_t>(extra)]);
            line.icon = d.keep(artDir + "/paddle_" + kPlaceNames[place] + ".png");
            d.lines.push_back(line);
            d.places.push_back(place);
        }
        // A Steam pad with no paddles reported: Steam Input has them (K's wording).
        d.view.padNote = setFolder == "steamdeck" && state.paddles == 0u ? kSteamPaddleNote : "";

        // The left column's rows sit level with the right's (the original's L2 / R2, L1 / R1 and stick rows share
        // their heights; the d-pad sits by the middle face row).
        for (size_t i = 0; i < d.lines.size(); ++i)
        {
            if (d.lines[i].right || d.lines[i].block)
                continue;
            const uint8_t p = d.places[i];
            const uint8_t want = p == Select ? uint8_t{R2} : p == L2 ? uint8_t{R1} : p == L1 ? uint8_t{North} : p == L3 ? uint8_t{R3}
                                 : p == LStick ? uint8_t{RStick} : p == DPad ? uint8_t{East} : kNone;
            for (size_t j = 0; j < d.lines.size(); ++j)
                if (d.lines[j].right && !d.lines[j].block && d.places[j] == want)
                    d.lines[i].mirror = static_cast<int>(j);
        }
        d.view.lines = d.lines.data();
        d.view.lineCount = static_cast<int>(d.lines.size());
        d.view.cursor = -1;
        if (!state.readOnly)
            for (size_t i = 0; i < d.places.size(); ++i)
                if (d.places[i] == cursor)
                    d.view.cursor = static_cast<int>(i);
        // The help: the buttons it names drawn as their icons (K, Nov 14): \x01 the page's own reset key (the left face
        // button), \x02 the menus' confirm button by Menu Controls (Japanese: the right one; Classic and Western: the
        // bottom one).
        d.view.noticeIcons[0] = d.keep(icons + set.file[West] + ".png");
        d.view.noticeIcons[1] = d.keep(icons + set.file[options.menuStyle == 2 ? East : South] + ".png");
        const std::string again = "Press \x01 again to confirm and reset.";
        if (state.readOnly)
            d.view.notice = d.keep(std::string(kReadOnlyNotice) + "\n" +
                                   (edit.armed == Armed::Reset ? again : std::string("Change it there or press \x01 to reset.")));
        else if (edit.armed == Armed::Reset)
            d.view.notice = d.keep(again);
        else
            d.view.notice = "";

        // The drum over the cursor's line.
        const bool extra = extraIndex(cursor) >= 0;
        if (edit.drum && !state.readOnly && d.view.cursor >= 0)
        {
            int start = 0;
            for (const DrumEntry &entry : drumEntries(state, options, cursor, start))
            {
                options_overlay::MappingDrumItem item;
                if (entry.input < kRemappable)
                {
                    const uint8_t input = entry.input, at = placeOfInput(state, input);
                    item.icon = at < kRemappable ? d.keep(icons + set.file[at] + ".png") : "";
                    item.text = shown(input) ? kDrumNames[input] : kEmpty;
                    if (shown(input) && kDrumArrows[input])
                    {
                        item.arrow = d.keep(artDir + "/" + kDrumArrows[input] + ".png");
                        item.arrowLit = d.keep(artDir + "/" + kDrumArrows[input] + "_lit.png");
                    }
                }
                else
                    item.text = entry.command == Command::QuickSave ? kQuickSave
                                : entry.command == Command::QuickLoad ? kQuickLoad
                                : entry.command == Command::SaveLoad  ? kSaveLoadDrum
                                                                      : kEmpty;
                d.drum.push_back(item);
            }
            d.view.drum = d.drum.data();
            d.view.drumCount = static_cast<int>(d.drum.size());
            d.view.drumPos = edit.pos;
        }

        // The hints (K, Nov 14): Reset on its own line above (always there, so it never moves), then confirm / cancel.
        d.view.resetArt = d.keep(icons + set.file[West] + ".png");
        if (d.view.drum)
        {
            d.view.resetWord = "";
            d.view.confirmWord = extra ? kHintAssign : kHintSwap;
            d.view.cancelWord = kHintCancel;
        }
        else
        {
            d.view.resetWord = kHintReset;
            d.view.confirmWord = state.readOnly ? "" : kHintSelect;
            d.view.cancelWord = kHintBack;
        }
    }

    uint8_t firstLine(const PageData &d)
    {
        for (const uint8_t place : d.places)
            if (place < kRemappable)
                return place;
        return kNone;
    }

    uint8_t move(const PageData &d, uint8_t cursor, int dx, int dy)
    {
        int at = -1;
        for (size_t i = 0; i < d.places.size(); ++i)
            if (d.places[i] == cursor)
                at = static_cast<int>(i);
        if (at < 0)
            return firstLine(d);
        const options_overlay::MappingLine &from = d.lines[static_cast<size_t>(at)];
        if (dy != 0)
        {
            // Up / down its side: the column, then that side's paddles under the pad (the lines are listed left column,
            // right column, then the block's left and right paddles).
            for (int i = at + dy; i >= 0 && i < static_cast<int>(d.places.size()); i += dy)
                if (d.lines[static_cast<size_t>(i)].right == from.right && !d.lines[static_cast<size_t>(i)].fixed)
                    return d.places[static_cast<size_t>(i)];
            return cursor;
        }
        if (dx != 0 && (dx > 0) != from.right)
        {
            // Across: a column line to the other column's line whose button is nearest in height; a paddle to the
            // other side's paddle in the nearest row.
            int best = -1;
            float bestDistance = 0.0f;
            for (size_t i = 0; i < d.places.size(); ++i)
            {
                const options_overlay::MappingLine &l = d.lines[i];
                if (l.right == from.right || l.fixed || l.block != from.block)
                    continue;
                const float distance = from.block ? std::fabs(static_cast<float>(l.blockY - from.blockY)) : std::fabs(l.py - from.py);
                if (best < 0 || distance < bestDistance)
                {
                    best = static_cast<int>(i);
                    bestDistance = distance;
                }
            }
            if (best >= 0)
                return d.places[static_cast<size_t>(best)];
        }
        return cursor;
    }
}
