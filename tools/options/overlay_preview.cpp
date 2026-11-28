// Preview of the OPTIONS overlay without a Windows build: the DLL's own screen-drawing code (cut from
// thirdparty/pcsx2-gs/compat/HostStubs.cpp into screen_snippet.inc) and options_overlay.cpp, drawn with ImGui and
// rasterised in software over the wall. Built and run by overlay_preview.py.
#include "imgui.h"
#include "imgui_internal.h"
#include "pcsx2gs_api.h"
#include "kamite/ini_file.h"
#include "kamite/options_overlay.h"
#include "kamite/options_pages.h"
#include "kamite/mapping_page.h"
#include "kamite/keys_page.h"
#include "kamite/presets.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "runtime/ps2_hotkeys.h"

// options_pages.cpp names the textures hotkey in its help text (6a74a6b); the real ps2_hotkeys.cpp needs the host
// backend, so the preview shows the default.
namespace ps2x::hotkeys
{
    Binding binding(Action) { return Binding{84}; } // KEY_T
    std::string text(Binding) { return "T"; }
}

namespace
{
    float g_presentRect[4] = {};
    std::mutex g_screenMutex;
    std::vector<Pcsx2GsScreenItem> g_screenItems;
    std::string g_screenStrings;
    uint32_t g_screenStale = 0;
    constexpr uint32_t kScreenStaleLimit = 8;
    struct ScreenFont
    {
        ImFont *font = nullptr;
        float capRatio = 0.7f;
    };
    std::vector<std::pair<std::string, ScreenFont>> g_screenFonts;
    std::vector<std::pair<std::string, ImTextureData *>> g_screenImages;
    // The DLL decodes PNGs with WIC; here overlay_preview.py hands over "<path>.rgba" (width, height, then RGBA rows).
    bool LoadScreenImage(const char *path, std::vector<uint8_t> &rgba, int &width, int &height)
    {
        FILE *f = fopen((std::string(path) + ".rgba").c_str(), "rb");
        if (!f)
            return false;
        uint32_t size[2] = {};
        bool ok = fread(size, 4, 2, f) == 2 && size[0] && size[1];
        if (ok)
        {
            width = int(size[0]), height = int(size[1]);
            rgba.resize(size_t(width) * height * 4);
            ok = fread(rgba.data(), 1, rgba.size(), f) == rgba.size();
        }
        fclose(f);
        return ok;
    }
}
#include "screen_snippet.inc"

static float edge(float ax, float ay, float bx, float by, float px, float py) { return (bx - ax) * (py - ay) - (by - ay) * (px - ax); }

int main(int argc, char **argv)
{
    // args: wall.rgb W H out.rgb pause(1/0) cursor confirmRow
    const char *wallPath = argv[1];
    const int W = atoi(argv[2]), H = atoi(argv[3]);
    const char *outPath = argv[4];
    std::vector<float> img(size_t(W) * H * 3);
    {
        std::vector<unsigned char> raw(size_t(W) * H * 3);
        FILE *f = fopen(wallPath, "rb");
        if (fread(raw.data(), 1, raw.size(), f) != raw.size())
            return 1;
        fclose(f);
        for (size_t i = 0; i < raw.size(); ++i) img[i] = raw[i] / 255.0f;
    }
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    io.DisplaySize = ImVec2(float(W), float(H));
    io.Fonts->AddFontDefault();
    ImGui::NewFrame();
    g_presentRect[0] = 0; g_presentRect[1] = 0; g_presentRect[2] = float(W); g_presentRect[3] = float(H);

    using namespace kamite::options_overlay;
    // args 5..: pause cursor confirmRow labelFont helpFont title [scrollTrack scrollThumb page open
    // pageCursor pageScroll sliderFill]
    setAssets(argv[8], argv[9], argc > 10 ? argv[10] : "");
    if (argc > 12)
        setScrollBar(argv[11], argv[12]);
    static const Row pauseRows[] = {{"CONTROLS", "Display controls"}, {"DISPLAY", "Window and screen settings."},
        {"GRAPHICS", "Rendering and graphics settings."},
        {"INTERFACE", "The HUD, button prompts and gauges."}, {"GAMEPLAY", "Techniques, enemies and new\nways to play."},
        {"SOUND", "Sound mode and volume levels."}, {"LOAD GAME", "Select a save file and\nrestart the game."},
        {"RESET GAME", "Return to title screen"}, {"RESTORE DEFAULTS", "Set every option back to\nits default."}, {"EXIT", ""}};
    static const Row titleRows[] = {pauseRows[0], pauseRows[1], pauseRows[2], pauseRows[3], pauseRows[4], pauseRows[5], pauseRows[8], pauseRows[9]};
    constexpr int kCategories = 6; // CONTROLS .. SOUND

    // The real pages: the shared settings list over the repo's GHR.ini (with --set overrides), as the game builds them.
    static kamite::IniFile ini;
    ini.load(argc > 18 ? argv[18] : "GHR.ini");
    for (int i = 19; i < argc; ++i) // Section.Key=Value
    {
        const std::string arg = argv[i];
        const size_t dot = arg.find('.'), eq = arg.find('=');
        if (dot != std::string::npos && eq != std::string::npos && dot < eq)
            ini.set(arg.substr(0, dot), arg.substr(dot + 1, eq - dot - 1), arg.substr(eq + 1));
    }
    View v;
    v.aspect = float(W) / float(H);
    v.pause = atoi(argv[5]) != 0;
    v.rows = v.pause ? pauseRows : titleRows;
    v.rowCount = v.pause ? 10 : 8;
    v.cursor = atoi(argv[6]);
    v.confirmRow = atoi(argv[7]);
    v.confirmYes = false;
    v.question = "Would you really like to reset?";
    static kamite::options_pages::PageData data;
    const int page = argc > 13 ? atoi(argv[13]) : -1; // a tab: 0 controls .. 5 sound
    if (page >= 0 && page < kCategories)
    {
        kamite::options_pages::buildPage(data, kamite::settings::kTabs[page], ini, v.aspect);
        v.page = &data.page;
        v.cursor = page; // the list's cursor on that category
    }
    v.inPage = argc > 14 && atoi(argv[14]) != 0;
    v.pageCursor = argc > 15 ? atoi(argv[15]) : 0;
    v.pageScroll = argc > 16 ? atoi(argv[16]) : 0;
    if (argc > 17)
        setSliderFill(argv[17]);
    // The button hints: KAMITE_PREVIEW_HINTS="confirm|confirm2|cancel|l1|r1" (image paths, overlay_preview.py); the
    // titles' letters: KAMITE_PREVIEW_TITLES="name:left:right;...".
    if (const char *titles = getenv("KAMITE_PREVIEW_TITLES"))
    {
        std::string all = titles;
        while (!all.empty())
        {
            const size_t end = all.find(';');
            const std::string one = all.substr(0, end);
            all = end == std::string::npos ? std::string() : all.substr(end + 1);
            const size_t a = one.find(':'), b = one.rfind(':');
            if (a != std::string::npos && b > a)
                setTitleLetters(one.substr(0, a), float(atof(one.substr(a + 1, b - a - 1).c_str())), float(atof(one.substr(b + 1).c_str())));
        }
    }
    static std::string hintPaths[5];
    if (const char *hints = getenv("KAMITE_PREVIEW_HINTS"))
    {
        std::string all = hints;
        for (int i = 0; i < 5; ++i)
        {
            const size_t bar = all.find('|');
            hintPaths[i] = all.substr(0, bar);
            all = bar == std::string::npos ? std::string() : all.substr(bar + 1);
        }
        v.confirmHint.art[0] = hintPaths[0].c_str();
        v.confirmHint.art[1] = hintPaths[1].c_str();
        v.confirmHint.word = "Confirm";
        v.cancelHint.art[0] = hintPaths[2].c_str();
        v.cancelHint.word = "Cancel";
        // An open page: L1 / R1 and the neighbouring categories (the list's first five rows).
        if (v.inPage && page >= 0 && page < kCategories)
        {
            if (page > 0)
            {
                v.prevHint.art[0] = hintPaths[3].c_str();
                v.prevHint.word = pauseRows[page - 1].name;
            }
            if (page < kCategories - 1)
            {
                v.nextHint.art[0] = hintPaths[4].c_str();
                v.nextHint.word = pauseRows[page + 1].name;
            }
        }
    }
    // OPTIONS > PRESETS: KAMITE_PREVIEW_PRESETS="N[:inuse[:ask[:dir[:t]]]]" (N 0..3 = Classic .. Sleazy Mode, 4 =
    // Custom; inuse the preset in effect; ask 1 = "Replace your custom settings?"; dir / t a turn under way from the
    // neighbour), the list's cursor on PRESETS, as options_screen fills it (presets.cpp's text, fitPresetText). The
    // photos: KAMITE_PREVIEW_PRESET_ART/NN.png (+ .rgba).
    static Row presetRows[16];
    static Page presetPage;
    static PresetsView pv;
    static std::string presetArt, presetFromArt, presetBand;
    if (const char *pp = getenv("KAMITE_PREVIEW_PRESETS"))
    {
        int n = 0, inUse = -1, ask = 0, dir = 0;
        float t = 1.0f;
        sscanf(pp, "%d:%d:%d:%d:%f", &n, &inUse, &ask, &dir, &t);
        n = std::clamp(n, 0, 4);
        const auto preset = [](int i) { return i >= 4 ? kamite::presets::kCustom : i; };
        const kamite::presets::Text &text = kamite::presets::text(preset(n));
        const char *artDir = getenv("KAMITE_PREVIEW_PRESET_ART");
        const auto art = [&](const kamite::presets::Text &tx) {
            char path[32];
            snprintf(path, sizeof(path), "/%02d.png", tx.photo);
            return artDir ? std::string(artDir) + path : std::string();
        };
        presetArt = art(text);
        const kamite::options_overlay::PresetFit fit = kamite::options_overlay::fitPresetText(text.tagline, text.band, v.aspect, n != 4);
        presetBand = fit.band;
        pv.name = text.title;
        pv.art = presetArt.c_str();
        pv.artAspect = kamite::presets::kPhotoAspect;
        pv.inUse = inUse == n;
        pv.tagline = text.tagline;
        pv.help = presetBand.c_str();
        pv.helpCap = fit.bandCap;
        pv.taglineCap = fit.taglineCap;
        pv.asking = ask != 0;
        pv.question = "Replace your custom settings?";
        if (dir != 0)
        {
            const int from = ((n - dir) % 4 + 4) % 4;
            const kamite::presets::Text &old = kamite::presets::text(preset(from));
            presetFromArt = art(old);
            pv.fromName = old.title;
            pv.fromArt = presetFromArt.c_str();
            pv.turnDir = dir;
            pv.turnT = t;
        }
        fprintf(stderr, "preset %s: band cap %.2f, tagline cap %.2f\n%s\n", text.title, fit.bandCap, fit.taglineCap, fit.band.c_str());
        presetRows[0] = {"PRESETS", "Ready-made setups, from the PS2 original\nto everything Rearmed has."};
        for (int i = 0; i < v.rowCount && i < 15; ++i)
            presetRows[i + 1] = v.rows[i];
        v.rows = presetRows;
        v.rowCount += 1;
        v.cursor = 0;
        presetPage.name = "PRESETS";
        presetPage.rows = nullptr;
        presetPage.rowCount = 0;
        v.page = &presetPage;
        v.presets = &pv;
        if (v.inPage)
        {
            v.prevHint = {};
            v.nextHint.art[0] = hintPaths[4].c_str();
            v.nextHint.word = "CONTROLS";
        }
    }
    // An L1 / R1 switch under way: KAMITE_PREVIEW_SWITCH="FROM:DIR:T" (the category left, +1 = R1, 0..1 through it).
    static std::string switchFrom;
    if (const char *sw = getenv("KAMITE_PREVIEW_SWITCH"); sw && *sw && v.inPage)
    {
        const std::string all = sw;
        const size_t a = all.find(':'), b = all.find(':', a + 1);
        switchFrom = all.substr(0, a);
        v.titleFrom = switchFrom.c_str();
        v.titleDir = atoi(all.substr(a + 1, b - a - 1).c_str());
        v.titleT = static_cast<float>(atof(all.substr(b + 1).c_str()));
    }
    // BUTTON MAPPING: KAMITE_PREVIEW_MAPPING="root|art|setFolder|cursor|readOnly|swaps|extras|drum|armed|paddles|r3turn"
    // (overlay_preview.py): swaps "south:west,..." exchange two places' PS2 buttons, extras "select=saveload;l4=west;..."
    // (none / quicksave / quickload / saveload, or a place: a copy of the action whose default place that is), drum the
    // open drum's turn ("" closed; items from the one it opens on: 1.5 = half-way from the next to the one after),
    // armed "reset", paddles 0 / 2 (L4, R4) / 4.
    static kamite::mapping_page::PageData mapping;
    if (const char *spec = getenv("KAMITE_PREVIEW_MAPPING"))
    {
        std::vector<std::string> f;
        std::string all = spec;
        for (size_t at = 0;;)
        {
            const size_t bar = all.find('|', at);
            f.push_back(all.substr(at, bar == std::string::npos ? std::string::npos : bar - at));
            if (bar == std::string::npos)
                break;
            at = bar + 1;
        }
        f.resize(11);
        using namespace kamite::mapping_page;
        static PadArt art;
        std::string error;
        if (!loadArt(f[1], art, error))
            fprintf(stderr, "mapping art: %s\n", error.c_str());
        const auto place = [](const std::string &name) {
            for (int i = 0; i < PlaceCount; ++i)
                if (name == placeName(static_cast<Place>(i)))
                    return static_cast<uint8_t>(i);
            return kNone;
        };
        State state = defaultState();
        state.readOnly = f[4] == "1";
        for (size_t at = 0; at < f[5].size();)
        {
            size_t comma = f[5].find(',', at);
            const std::string pair = f[5].substr(at, comma == std::string::npos ? std::string::npos : comma - at);
            const size_t colon = pair.find(':');
            const uint8_t a = place(pair.substr(0, colon)), b = colon == std::string::npos ? kNone : place(pair.substr(colon + 1));
            if (a < kRemappable && b < kRemappable)
                std::swap(state.onPlace[a], state.onPlace[b]);
            at = comma == std::string::npos ? f[5].size() : comma + 1;
        }
        for (size_t at = 0; at < f[6].size();)
        {
            size_t semi = f[6].find(';', at);
            const std::string one = f[6].substr(at, semi == std::string::npos ? std::string::npos : semi - at);
            const size_t eq = one.find('=');
            const int extra = eq == std::string::npos ? -1 : extraIndex(place(one.substr(0, eq)));
            if (extra >= 0)
            {
                const std::string value = one.substr(eq + 1);
                Extra &e = state.extras[static_cast<size_t>(extra)];
                e = Extra{};
                if (value == "quicksave")
                    e.command = Command::QuickSave;
                else if (value == "quickload")
                    e.command = Command::QuickLoad;
                else if (value == "saveload")
                    e.command = Command::SaveLoad;
                else if (const uint8_t from = place(value); from < kRemappable)
                    e.copy = from; // the action that sits there by default (its PS2 button)
            }
            at = semi == std::string::npos ? f[6].size() : semi + 1;
        }
        const int paddles = atoi(f[9].c_str());
        state.paddles = paddles >= 4 ? 0x1E : paddles >= 2 ? 0x06 : 0x00; // bits 1 L4, 2 R4, 3 L5, 4 R5
        state.r3QuickTurn = f[10] == "1"; // Modern: Hold's R3 turns
        const std::string artDir = f[1].substr(0, f[1].find_last_of('/'));
        const uint8_t cursor = f[3].empty() ? kNone : place(f[3]);
        Edit edit;
        if (!f[7].empty() && cursor != kNone)
        {
            int start = 0;
            drumEntries(state, optionsFrom(ini), cursor, start);
            edit.drum = true;
            edit.pos = static_cast<float>(start) + static_cast<float>(atof(f[7].c_str()));
            edit.target = static_cast<int>(std::lround(edit.pos));
        }
        edit.armed = f[8] == "reset" ? Armed::Reset : Armed::None;
        build(mapping, art, artDir, f[0], f[2], state, optionsFrom(ini), cursor, edit);
        v.mapping = &mapping.view;
        v.prevHint = v.nextHint = Hint{};
    }
    // The keyboard's page: KAMITE_PREVIEW_KEYS="mappingArtDir|keyArtDir|cursor|mode|labels" (overlay_preview.py);
    // mode browse / capture / reset; labels "Cross=Spc;..." over the default layout's; a label's picture is
    // keyArtDir/<file>_Key_Light.png by prompt_hd's names for the default layout's keys.
    static kamite::keys_page::PageData keys;
    if (const char *spec = getenv("KAMITE_PREVIEW_KEYS"))
    {
        std::vector<std::string> f;
        std::string all = spec;
        for (size_t at = 0;;)
        {
            const size_t bar = all.find('|', at);
            f.push_back(all.substr(at, bar == std::string::npos ? std::string::npos : bar - at));
            if (bar == std::string::npos)
                break;
            at = bar + 1;
        }
        f.resize(5);
        std::map<std::string, std::string> labels{
            {"LUp", "W"},   {"LDown", "S"},  {"LLeft", "A"},  {"LRight", "D"}, {"DodgeModifier", "Shft"}, {"L3", ""},
            {"Cross", "Spc"}, {"Square", "M1"}, {"Triangle", "M2"}, {"Circle", "E"}, {"L1", "Q"}, {"R1", "R"},
            {"L2", "F"},    {"R2", "G"},     {"R3", "M3"},    {"Up", "1"},     {"Right", "2"}, {"Down", "3"}, {"Left", "4"},
            {"Start", "Esc"}};
        for (size_t at = 0; at < f[4].size();)
        {
            const size_t semi = f[4].find(';', at);
            const std::string one = f[4].substr(at, semi == std::string::npos ? std::string::npos : semi - at);
            if (const size_t eq = one.find('='); eq != std::string::npos)
                labels[one.substr(0, eq)] = one.substr(eq + 1);
            at = semi == std::string::npos ? f[4].size() : semi + 1;
        }
        const std::string keyDir = f[1];
        const auto art = [&](const std::string &label) -> std::string {
            static const std::map<std::string, std::string> kFiles{{"Spc", "Space"}, {"Shft", "Shift"}, {"M1", "Mouse_Left"},
                                                                   {"M2", "Mouse_Right"}, {"M3", "Mouse_Middle"}, {"Del", "Del"},
                                                                   {"Esc", "Esc"}, {"Ent", "Enter"}, {"Ctrl", "Ctrl"}, {"Tab", "Tab"}};
            const auto found = kFiles.find(label);
            std::string file = found != kFiles.end() ? found->second : label.size() == 1 ? label : std::string();
            if (file.empty())
                return {};
            const std::string path = keyDir + "/" + file + "_Key_Light.png";
            FILE *probe = fopen((path + ".rgba").c_str(), "rb");
            if (!probe)
                return {};
            fclose(probe);
            return path;
        };
        const kamite::keys_page::Mode mode = f[3] == "capture" ? kamite::keys_page::Mode::Capture
                                             : f[3] == "reset" ? kamite::keys_page::Mode::ResetArmed
                                                               : kamite::keys_page::Mode::Browse;
        kamite::keys_page::build(keys, labels, art, f[0], kamite::keys_page::optionsFrom(ini), atoi(f[2].c_str()), mode, art("Del"));
        v.keys = &keys.view;
        v.prevHint = v.nextHint = Hint{};
    }
    std::vector<Pcsx2GsScreenItem> items;
    std::string strings;
    build(v, items, strings);
    // BUTTON MAPPING checks (stderr): leaders that cross, and leaders passing over another line's button.
    if (v.mapping)
    {
        const Pcsx2GsScreenItem *pad = nullptr;
        std::vector<const Pcsx2GsScreenItem *> leaders;
        for (const Pcsx2GsScreenItem &it : items)
        {
            const char *path = strings.c_str() + it.text;
            if (it.kind == PCSX2GS_SCREEN_IMAGE && strstr(path, "/pad_"))
                pad = &it;
            // Leaders, and the second leg of a bent one (an underscore image that is not level).
            if (it.kind == PCSX2GS_SCREEN_LINE && (strstr(path, "/leader") || (strstr(path, "/underscore") && std::fabs(it.y1 - it.y) > 1e-4f)))
                leaders.push_back(&it);
        }
        const auto P = [&](float x, float y) { return std::pair<float, float>(x * W, y * H); };
        const auto cross = [](std::pair<float, float> a, std::pair<float, float> b, std::pair<float, float> c) {
            return (b.first - a.first) * (c.second - a.second) - (b.second - a.second) * (c.first - a.first);
        };
        for (size_t i = 0; i < leaders.size(); ++i)
            for (size_t j = i + 1; j < leaders.size(); ++j)
            {
                const auto a = P(leaders[i]->x, leaders[i]->y), b = P(leaders[i]->x1, leaders[i]->y1);
                const auto c = P(leaders[j]->x, leaders[j]->y), d = P(leaders[j]->x1, leaders[j]->y1);
                if (cross(a, b, c) * cross(a, b, d) < 0 && cross(c, d, a) * cross(c, d, b) < 0)
                    fprintf(stderr, "CROSS: leader %zu (from %.0f,%.0f) x leader %zu (from %.0f,%.0f)\n", i, a.first, a.second, j, c.first, c.second);
            }
        // Leaders through the paddle block (its labels' underscores mark each one's bottom and width; a label's top is
        // taken 30 lines over it).
        for (const Pcsx2GsScreenItem &it : items)
        {
            const char *path = strings.c_str() + it.text;
            if (it.kind != PCSX2GS_SCREEN_LINE || !strstr(path, "/underscore") || std::fabs(it.y1 - it.y) > 1e-4f)
                continue;
            const float x0 = std::min(it.x, it.x1) * W, x1 = std::max(it.x, it.x1) * W, y1 = it.y * H, y0 = y1 - 30.0f / 448.0f * H;
            bool block = false;
            for (int k = 0; k < mapping.view.lineCount; ++k)
                block = block || mapping.view.lines[k].block;
            if (!block || x0 < 0.25f * W || x1 > 0.75f * W || y1 < 0.55f * H)
                continue; // the columns' underscores (the block sits in the middle, under the pad)
            for (size_t i = 0; i < leaders.size(); ++i)
            {
                const auto a = P(leaders[i]->x, leaders[i]->y), b = P(leaders[i]->x1, leaders[i]->y1);
                for (int step = 0; step <= 64; ++step)
                {
                    const float t = step / 64.0f, x = a.first + t * (b.first - a.first), y = a.second + t * (b.second - a.second);
                    if (x > x0 && x < x1 && y > y0 && y < y1)
                    {
                        fprintf(stderr, "BLOCK: leader %zu (from %.0f,%.0f) runs through a paddle label (%.0f..%.0f, %.0f..%.0f)\n", i,
                                a.first, a.second, x0, x1, y0, y1);
                        break;
                    }
                }
            }
        }
        if (pad)
            for (size_t i = 0; i < leaders.size(); ++i)
                for (int k = 0; k < mapping.view.lineCount; ++k)
                {
                    const auto &l = mapping.view.lines[k];
                    if (l.block)
                        continue; // a paddle has no point on the pad
                    const float bx = (pad->x + l.px * (pad->x1 - pad->x)) * W, by = (pad->y + l.py * (pad->y1 - pad->y)) * H;
                    const auto a = P(leaders[i]->x, leaders[i]->y), b = P(leaders[i]->x1, leaders[i]->y1);
                    const float dx = b.first - a.first, dy = b.second - a.second, len2 = dx * dx + dy * dy;
                    const float t = std::clamp(((bx - a.first) * dx + (by - a.second) * dy) / std::max(len2, 1.0f), 0.0f, 1.0f);
                    const float ex = a.first + t * dx - bx, ey = a.second + t * dy - by, dist = std::sqrt(ex * ex + ey * ey);
                    const float r = l.reach * (pad->y1 - pad->y) * H;
                    if (t > 0.02f && dist < std::max(r, 0.012f * H) && std::hypot(a.first - bx, a.second - by) > r + 2.0f)
                        fprintf(stderr, "OVER: leader %zu (from %.0f,%.0f) passes %.0f px from line %d's button (%s)\n", i, a.first,
                                a.second, dist, k, l.text[0]);
                }
    }
    g_screenItems = items;
    g_screenStrings = strings;
    g_screenStrings.push_back('\0');
    DrawScreen();
    ImGui::Render();
    ImDrawData *dd = ImGui::GetDrawData();
    // textures
    for (ImDrawList *list : dd->CmdLists)
    {
        for (const ImDrawCmd &cmd : list->CmdBuffer)
        {
            ImTextureData *tex = cmd.TexRef._TexData;
            if (!tex || !tex->Pixels) { fprintf(stderr, "no tex data\n"); continue; }
            const int tw = tex->Width, th = tex->Height, bpp = tex->BytesPerPixel;
            auto sample = [&](float u, float v, int ch) -> float {
                float x = u * tw - 0.5f, y = v * th - 0.5f;
                int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
                float fx = x - x0, fy = y - y0;
                auto at = [&](int xx, int yy) -> float {
                    xx = std::clamp(xx, 0, tw - 1); yy = std::clamp(yy, 0, th - 1);
                    const unsigned char *p = (const unsigned char *)tex->Pixels + (size_t(yy) * tw + xx) * bpp;
                    return (bpp == 4 ? p[ch] : (ch == 3 ? p[0] : 255)) / 255.0f;
                };
                return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
            };
            for (unsigned int i = 0; i < cmd.ElemCount; i += 3)
            {
                const ImDrawVert *v3[3];
                for (int k = 0; k < 3; ++k) v3[k] = &list->VtxBuffer[cmd.VtxOffset + list->IdxBuffer[cmd.IdxOffset + i + k]];
                float area = edge(v3[0]->pos.x, v3[0]->pos.y, v3[1]->pos.x, v3[1]->pos.y, v3[2]->pos.x, v3[2]->pos.y);
                if (std::fabs(area) < 1e-9f) continue;
                int minx = std::max(0, (int)std::floor(std::min({v3[0]->pos.x, v3[1]->pos.x, v3[2]->pos.x})));
                int maxx = std::min(W - 1, (int)std::ceil(std::max({v3[0]->pos.x, v3[1]->pos.x, v3[2]->pos.x})));
                int miny = std::max(0, (int)std::floor(std::min({v3[0]->pos.y, v3[1]->pos.y, v3[2]->pos.y})));
                int maxy = std::min(H - 1, (int)std::ceil(std::max({v3[0]->pos.y, v3[1]->pos.y, v3[2]->pos.y})));
                for (int y = miny; y <= maxy; ++y)
                    for (int x = minx; x <= maxx; ++x)
                    {
                        const float px = x + 0.5f, py = y + 0.5f;
                        float w0 = edge(v3[1]->pos.x, v3[1]->pos.y, v3[2]->pos.x, v3[2]->pos.y, px, py) / area;
                        float w1 = edge(v3[2]->pos.x, v3[2]->pos.y, v3[0]->pos.x, v3[0]->pos.y, px, py) / area;
                        float w2 = 1 - w0 - w1;
                        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                        float col[4] = {0, 0, 0, 0};
                        for (int c = 0; c < 4; ++c)
                            for (int k = 0; k < 3; ++k)
                                col[c] += (w0 * (k == 0) + w1 * (k == 1) + w2 * (k == 2)) * ((v3[k]->col >> (8 * c)) & 255) / 255.0f;
                        const float u = w0 * v3[0]->uv.x + w1 * v3[1]->uv.x + w2 * v3[2]->uv.x;
                        const float vv = w0 * v3[0]->uv.y + w1 * v3[1]->uv.y + w2 * v3[2]->uv.y;
                        const float a = col[3] * sample(u, vv, 3);
                        float *d = &img[(size_t(y) * W + x) * 3];
                        for (int c = 0; c < 3; ++c) d[c] = d[c] * (1 - a) + col[c] * sample(u, vv, c) * a;
                    }
            }
        }
    }
    std::vector<unsigned char> out(img.size());
    for (size_t i = 0; i < img.size(); ++i) out[i] = (unsigned char)std::lround(std::clamp(img[i], 0.0f, 1.0f) * 255);
    FILE *f = fopen(outPath, "wb");
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    fprintf(stderr, "items %zu, vertices %d\n", items.size(), dd->TotalVtxCount);
    return 0;
}
