// Stubs: everything launcher.cpp calls outside the launcher's own modules (a Linux harness only).
#include <cstdlib>
#include "kamite/character_mods.h"
#include "kamite/disc_files.h"
#include "kamite/game_disc.h"
#include "kamite/launcher_platform.h"
#include "kamite/launcher_update.h"
#include "kamite/log.h"
#include "ps2_host_gamepads.h"
#include "ps2_host_input.h"
#include "runtime/ps2_display_options.h"
#include "runtime/ps2_hotkeys.h"
#include "raylib.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
namespace ps2_host_input::controls {
static std::string empty;
const std::string &path() { return empty; }
bool dirty() { return false; }
const std::string &fileError() { return empty; }
bool envOverride(uint32_t) { return false; }
std::vector<std::string> bindings(uint32_t, std::string_view key) {
  // the new-install defaults (claude/keyboard.md), so the Controls pages have something to show
  static const std::pair<const char *, std::vector<std::string>> kDefaults[] = {
      {"Cross", {"Keyboard/Space", "SDL-0/FaceSouth"}}, {"Circle", {"Keyboard/E", "SDL-0/FaceEast"}},
      {"Square", {"Pointer-0/LeftButton", "SDL-0/FaceWest"}}, {"Triangle", {"Pointer-0/RightButton", "SDL-0/FaceNorth"}},
      {"L1", {"Keyboard/Q", "SDL-0/LeftShoulder"}}, {"R1", {"Keyboard/R", "SDL-0/RightShoulder"}},
      {"L2", {"Keyboard/F", "SDL-0/+LeftTrigger"}}, {"R2", {"Keyboard/G", "SDL-0/+RightTrigger"}},
      {"L3", {"SDL-0/LeftStick"}}, {"R3", {"Pointer-0/MiddleButton", "SDL-0/RightStick"}},
      {"Start", {"Keyboard/Escape", "SDL-0/Start"}}, {"Select", {"SDL-0/Back"}},
      {"Up", {"Keyboard/1", "SDL-0/DPadUp"}}, {"Down", {"Keyboard/3", "SDL-0/DPadDown"}},
      {"Left", {"Keyboard/4", "SDL-0/DPadLeft"}}, {"Right", {"Keyboard/2", "SDL-0/DPadRight"}},
      {"LUp", {"Keyboard/W", "SDL-0/-LeftY"}}, {"LDown", {"Keyboard/S", "SDL-0/+LeftY"}},
      {"LLeft", {"Keyboard/A", "SDL-0/-LeftX"}}, {"LRight", {"Keyboard/D", "SDL-0/+LeftX"}},
      {"RUp", {"SDL-0/-RightY"}}, {"RDown", {"SDL-0/+RightY"}}, {"RLeft", {"SDL-0/-RightX"}}, {"RRight", {"SDL-0/+RightX"}},
      {"DodgeModifier", {"Keyboard/LeftShift"}}};
  for (const auto &[name, list] : kDefaults)
    if (key == name) return list;
  return {};
}
void setBindings(uint32_t, std::string_view, std::vector<std::string>) {}
void resetDefaults(uint32_t) {}
bool save(std::string &) { return true; }
Kind kindOf(std::string_view b) { return b.rfind("SDL-", 0) == 0 ? Kind::Controller : Kind::Keys; }
bool sameBinding(std::string_view a, std::string_view b) { return a == b; }
std::string displayName(std::string_view b) { return std::string(b); }
std::string keyBinding(int) { return {}; }
const std::vector<int> &bindableKeys() { static std::vector<int> v; return v; }
std::string mouseBinding(int) { return {}; }
std::string padButtonBinding(int, int) { return {}; }
std::string padAxisBinding(int, int, bool) { return {}; }
std::string keyboardKeyUse(uint32_t, int) { return {}; }
}
namespace ps2_host_display {
void openHostWindow(const char *title) {
  const char *w = std::getenv("LH_W"), *h = std::getenv("LH_H");
  InitWindow(w ? std::atoi(w) : 1600, h ? std::atoi(h) : 900, title);
  SetExitKey(KEY_NULL);
}
bool vsyncEnabled() { return false; }
}
namespace ps2_host_gamepads {
void update() {}
bool connected(int) { return false; }
bool button(int, Button) { return false; }
float axis(int, Axis) { return 0.0f; }
}
namespace ps2x::hotkeys {
const Info &info(Action a) {
  static const Info kInfo[] = {{"SaveState", "Save state", {}}, {"LoadState", "Load state", {}}, {"Fullscreen", "Fullscreen", {}},
                               {"DevMenu", "F2 menu", {}}, {"Screenshot", "Screenshot", {}}, {"RecordCase", "Record case", {}}};
  static const Info kOther{"Other", "Something else", {}};
  const int i = static_cast<int>(a);
  return i >= 0 && i < 6 ? kInfo[i] : kOther;
}
Binding binding(Action a) { return Binding{static_cast<int>(a) < 4 ? 290 + static_cast<int>(a) : 0, false, false}; }
bool available(Action a) { return static_cast<int>(a) < 6; }
void setBinding(Action, Binding) {}
Action conflict(Action a, Binding) { return a; }
Action plainKeyHolder(int) { return Action{}; }
std::string text(Binding b) { return b.key ? "F" + std::to_string(b.key - 289) : std::string(); }
bool parse(std::string_view, Binding &) { return false; }
int capturePressed() { return 0; }
}
namespace kamite {
namespace characters { std::vector<Character> list(const std::filesystem::path &) { return {}; } }
namespace disc_files {
bool readFromImage(const std::filesystem::path &, std::string_view path, std::vector<uint8_t> &out) {
  std::ifstream in("/mnt/user-data/uploads/Claude/God Hand Extract/" + std::string(path), std::ios::binary);
  if (!in) return false; out.assign(std::istreambuf_iterator<char>(in), {}); return true;
}
}
DiscCheckResult verifyDiscAndPrepare(const std::filesystem::path &, const std::filesystem::path &) { DiscCheckResult r; r.ok = true; return r; }
namespace log {
void write(Category, Level level, std::string_view message) { std::fprintf(stderr, "[%d] %.*s\n", (int)level, (int)message.size(), message.data()); }
bool traceEnabled(Category) { return false; }
}
namespace launcher::update {
std::string versionLabel() { return "v0.6.9 Beta (dev abc1234)"; }
std::string editionName() { return "DEV EDITION"; }
std::string devCommit() { return std::getenv("LH_RELEASE") ? "" : "abc1234"; }
// LH_UPDATE: v0.7.0 is out; LH_OFFLINE: the list could not be read; else up to date (this version's notes).
void startCheck(Checker &c, bool, bool) {
  const char *notes = "- Restored the alpha's Hold to Brawl control scheme: Control Style Modern: Hold (hold L1 to brawl, double-tap for a 180\xC2\xB0 turn); the old Modern is now Modern: Toggle.\n"
                      "- Modern: Hold: Auto's brawl mode ends by itself once no enemy is near.\n"
                      "- Button Mapping: with Modern: Hold, R3 can take a single-press 180\xC2\xB0 turn (off by default).\n"
                      "- Remastered, Rearmed and Sleazy Mode presets use Modern: Hold, Modern: Toggle stays an option (installs already on them show Custom until you pick the preset again).\n"
                      "- With several controllers connected, the one that pressed a button last controls Gene (and its button prompts show).\n"
                      "- Launcher: with a controller, Y / triangle eats a piece of fruit and X / square takes from the smoothie.\n"
                      "- 128 MB of extra RAM above the game's own 32 MB, for mods and bigger models; older save states and test cases still load.";
  c.current = Edition{"v0.6.9.1", "NOV. 19, 2026", notes};
  c.hasCurrent = true;
  if (std::getenv("LH_OFFLINE")) { c.state.store((int)CheckState::Failed); return; }
  if (std::getenv("LH_UPDATE")) {
    c.release.tag = "v0.7.0";
    c.release.editions.push_back(Edition{"v0.7.0", "NOV. 24, 2026", "- Mods: any game file can be replaced by a loose file in a mod folder.\n- Olivia (pre-alpha) in the Mods tab.\n- The newspaper in the launcher's corner."});
    c.release.editions.push_back(Edition{"v0.6.9.1", "NOV. 19, 2026", notes});
    c.state.store((int)CheckState::Available);
    return;
  }
  c.state.store((int)CheckState::UpToDate);
}
// LH_FAIL: the download fails; else it sits at 45%.
void startInstall(Installer &i, const Release &, const std::filesystem::path &) {
  if (std::getenv("LH_FAIL")) { i.error = "The download did not finish. Check your connection and try again."; i.stage = (int)Stage::Failed; return; }
  i.total = 100; i.received = 45;
}
void cleanup(const std::filesystem::path &) {}
}
namespace launcher::platform {
std::filesystem::path pick(void *, bool) { return {}; }
// LH_ZIP: the zip "picked" for Install from zip.
std::filesystem::path pickZip(void *) { const char *z = std::getenv("LH_ZIP"); return z ? std::filesystem::path(z) : std::filesystem::path(); }
void open(const std::filesystem::path &) {}
bool shiftHeld() { return false; }
bool relaunch(const std::filesystem::path &) { return false; }
std::filesystem::path thisExe() { return "/tmp/lh/GHR"; }
}
}
#include "kamite/launcher_steam.h"
namespace kamite::launcher::steam {
Status probe() { Status s; s.available = true; return s; }
bool running() { return false; }
Result addOrUpdate(const std::filesystem::path &, std::atomic<int> &stage) { stage = (int)Stage::Done; Result r; r.ok = true; r.message = "Added."; return r; }
}
